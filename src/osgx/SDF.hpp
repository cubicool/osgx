#pragma once

#include "Array.hpp"
#include "Core.hpp"
#include "Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/BufferIndexBinding>
#include <osg/Image>
#include <osg/StateAttribute>
#include <osg/Texture2D>
#include <osg/Vec4>

OSGX_ENABLE_WARNINGS

// osgx::SDF - two things sharing one namespace of GLSL: (1) a small catalog of pure, closed-form
// 2D signed-distance functions (negative = inside, matching every other SDF convention in this
// codebase), published as a `#pragma osgx::sdf SHAPES` GLSL shader-library entry - no CPU-side
// counterpart, unlike Projection.hpp's twins, since these are pure per-fragment shape tests; and
// (2) further down, the SDF StateAttribute + its SAMPLING/TEXTURE library entries for rendering
// BAKED distance-field textures (SDF/MSDF) produced elsewhere.
//
// Extracted 2026-09-18 from osgSlug's Atlas.shaders.cpp (SHADER_LIB_MASK), which originally
// defined these itself as `osgSlug_SDF_*` to back its own `slughorn::Mask` dispatch
// (Circle/Rect/Capsule/Arc/ArcBand/Hexagon/Octagon/Star - see slughorn.hpp's own Mask::Type
// enum). Every one of these functions was already pure math with zero dependency on
// osgSlug's own state (no Mask struct, no MSDF texture, no LayerBuffer) - a clean extraction
// boundary, not a forced one. osgSlug now consumes `#pragma osgx::sdf SHAPES` instead of
// defining its own copies; only the Mask-struct-specific dispatch/coverage/MSDF glue
// (osgSlug_Mask_CoverageFor, osgSlug_Mask_Coverage, osgSlug_Mask_DebugMSDF) stayed behind, since
// that part IS genuinely osgSlug-specific.
//
// A second batch (TILE below) was extracted 2026-10-12 from osgSlug's Atlas.shaders.cpp
// (SHADER_LIB_SDF, `#pragma osgSlug sdf_lib`): sampling/reconstructing a sub-rect ("tile") of a
// CALLER-OWNED shared atlas texture, addressed by a packed rect (texel-space) + frame (origin,
// scale, range) pair, in whatever 2D coordinate space the caller's own `coord` means - osgx has
// no opinion on that (osgSlug calls it "em space"; nothing here cares). Same clean-boundary
// extraction as SHAPES: pure math, zero osgSlug state, just parameterized over an explicit
// texture/rect/frame/coord instead of global uniforms. Renamed on the move, same convention as
// SHAPES: osgSlug_SDF_TileTexel -> osgx_SDF_TileTexel, osgSlug_SDF_TileSample ->
// osgx_SDF_TileSample. osgx_SDF_TileGradient is NEW (generalizes what used to be
// osgSlug_SDF_Gradient's inline central-difference math, which had no osgx counterpart before).
// osgx_SDF_BevelNormal's signature changed on the move: it now takes a precomputed gradient
// vector instead of a coord + computing the gradient itself, decoupling it entirely from tile
// addressing. Deliberately NOT moved: the "many tiles in one atlas, indexed per-fragment" SSBO
// scheme (osgSlug's own `sdfTiles` buffer/binding) - that indexing concept, and the per-fragment
// index that drives it, are osgSlug-specific; TILE's functions take an already-resolved single
// rect/frame, agnostic to how the caller found it.
//
// Two renames happened during the move, to match slughorn::Mask::Type's own vocabulary (the
// actual public data-model API) rather than the GLSL functions' own previously-independent
// names: osgSlug_SDF_Box -> osgx_SDF_Rect, osgSlug_SDF_Pie -> osgx_SDF_Arc. Every other name
// carried over unchanged (just the osgSlug_ -> osgx_ prefix swap).
//
// All eight shape functions are registered as ONE library entry ("SHAPES"), not split into
// individually-selectable pragma tags the way Projection.hpp's UNPROJECT/DEPTH are: unlike
// those two (independent, callers pick whichever they need), a Mask-style dispatch that branches
// on a runtime shape-type value needs every shape function compiled in regardless of which one
// is actually active per-fragment, so there is no real caller that would ever want a subset.
//
// Hexagon/Octagon/Star all call osgx_SDF_Rotate internally - it is part of the SAME "SHAPES"
// entry (not a separate tag a caller must remember to also pull in), since resolveShaderLibs()
// only does ONE pass over the CALLER's own source (see Shader.hpp's own comment) - a dependency
// between two SEPARATE registered entries would silently fail to expand rather than erroring.
//
// float osgx_SDF_Circle(vec2 p, vec2 center, float r)
// float osgx_SDF_Rect(vec2 p, vec2 center, vec2 halfExtents)
// float osgx_SDF_Capsule(vec2 p, vec2 a, vec2 b, float r)
// float osgx_SDF_Arc(vec2 p, vec2 center, float r, float angleStart, float angleEnd) - filled pie
//     sector; angles in radians, standard math convention (0 = +X, CCW positive).
// float osgx_SDF_ArcBand(vec2 p, vec2 center, float r, float angleStart, float angleEnd,
//     float strokeHalfWidth) - stroked arc (an annular band along an arc), not a filled sector.
// vec2 osgx_SDF_Rotate(vec2 p, float angle) - rotates p by angle (CCW, radians); every rotatable
//     shape below pre-rotates its query point by -rotation into the shape's own local frame.
// float osgx_SDF_Hexagon(vec2 p, vec2 center, float r, float rotation) - flat-top at rotation=0.
// float osgx_SDF_Octagon(vec2 p, vec2 center, float r, float rotation)
// float osgx_SDF_Star(vec2 p, vec2 center, float r, float points, float innerRatio,
//     float rotation) - points rounded to the nearest integer >= 3; innerRatio in [0, 1] (0 =
//     sharpest spikes, 1 = a regular n-gon).
namespace osgx {

// The `#pragma osgx::sdf` catalog (registered by osgx::Library, expanded by resolveShaderLibs() in
// Shader.hpp) has six entries:
//
//   SHAPES   - the nine closed-form functions listed above.
//   SAMPLING - pure, texture-agnostic reconstruction helpers for BAKED distance fields (SDF or
//              MSDF, from any producer - osgx never generates these, only consumes them):
//                float osgx_SDF_Median(vec3 msd) - median-of-three, the MSDF reconstruction.
//                float osgx_SDF_ScreenPixelRange(vec2 uv, vec2 texSize, float pixelRange) - how many
//                    SCREEN pixels the field's baked pixelRange spans at this fragment (Chlumsky's
//                    screenPixelRange; derivative-driven, so it is correct under magnification,
//                    minification, and rotation; clamped to >= 1). `uv` must be in whole-texture
//                    [0, 1] space and `texSize` the WHOLE texture's size, even when only a
//                    sub-rect (a tile) of it is being drawn.
//                float osgx_SDF_CoverageFromDistance(float d, float screenPixelRange) - `d` is the
//                    raw texture value (0.5 = edge, > 0.5 = inside); returns antialiased [0, 1]
//                    coverage.
//   SAMPLING_DECL - the same three function signatures as SAMPLING, declarations only. GLSL rejects
//              one function defined twice across the shader objects of one Program, so exactly ONE
//              object pulls in SAMPLING (or TEXTURE's dependency on it) and any other object in the
//              same Program that merely CALLS these lists SAMPLING_DECL instead - the same pattern
//              as PBR's *_DECL entries.
//   TEXTURE  - declares the SDF StateAttribute's own inputs (sampler + std140 block, see the class
//              below) and float osgx_SDF_Coverage(vec2 uv), the whole "sample + reconstruct +
//              antialias" pipeline in one call; `uv` is tile-local [0, 1] (mapped through
//              uvRect internally). REQUIRES `SAMPLING` listed BEFORE it:
//                #pragma osgx::sdf SAMPLING,TEXTURE
//              (resolveShaderLibs() does one pass, so TEXTURE's own source can't carry a nested
//              pragma - see the note on SHAPES above). Needs `#version 430` or later
//              (layout(binding=...) on both the sampler and the block).
//   TILE     - pure, caller-owns-everything helpers for ONE tile (a sub-rect) of a shared atlas
//              texture the caller packs many baked fields into. Unlike TEXTURE, this owns no
//              uniform/SSBO of its own: texture, rect, frame, and sdfType are all explicit
//              parameters, since how a caller indexes "which tile" (a fixed array, an SSBO keyed
//              by a per-fragment index, whatever) is entirely its own concern.
//                vec3 osgx_SDF_TileTexel(sampler2D tex, vec4 rect, vec4 frame, vec2 coord) - the
//                    tile's raw texel (.rgb for MSDF, .r meaningful for plain SDF) at `coord`,
//                    mapped via rect.xy + (coord - frame.xy) * frame.z and clamped to the tile's
//                    own texels (so bilinear filtering can never read a neighbouring tile).
//                float osgx_SDF_TileSample(sampler2D tex, vec4 rect, vec4 frame, vec2 coord, int
//                    sdfType) - the scalar every consumer wants regardless of type (0 = SDF, 1 =
//                    MSDF): 0.5 = edge, > 0.5 = inside.
//                vec2 osgx_SDF_TileGradient(sampler2D tex, vec4 rect, vec4 frame, vec2 coord, int
//                    sdfType) - central-difference gradient of the field, one tile texel wide,
//                    points toward the interior. Prefer this over dFdx/dFdy(sd): screen-space
//                    derivatives are constant per 2x2 hardware quad, quantizing anything built
//                    from them (a bevel normal, a reflection vector) into pixel-scale blocks.
//                vec3 osgx_SDF_BevelNormal(vec2 gradient, float sd, vec3 flatNormal, vec3
//                    tangentU, vec3 tangentV, float bevelWidth, float bevelStrength) - tilts
//                    flatNormal toward the shape's edge as sd approaches 0.5, for a smoothly
//                    curved "dome"/bevel look instead of a flat facet. `gradient` is whatever
//                    osgx_SDF_TileGradient returned (or your own equivalent); tangentU/tangentV
//                    are the world-space axes gradient.xy map onto. Returns flatNormal unchanged
//                    where sd < 0 or the bevel/gradient is negligible.
//              REQUIRES `SAMPLING` listed BEFORE it (osgx_SDF_TileSample calls osgx_SDF_Median):
//                #pragma osgx::sdf SAMPLING,TILE
//   TILE_DECL - the same four signatures as TILE, declarations only - same SAMPLING_DECL pattern,
//              for a second shader object in the same Program that only CALLS these.

// ================================================================================================
// SDF
//
// A real osg::StateAttribute carrying one baked distance-field texture (single-channel SDF or
// 3-channel MSDF) plus the metadata needed to reconstruct it: pixelRange (the distance range, in
// TEXELS, the field was baked with - msdfgen's `range`), an SDF type, and a uvRect selecting which
// part of the texture is drawn (default: all of it). Attach like any StateAttribute, then call
// `osgx_SDF_Coverage(uv)` from your own fragment shader (see the catalog comment above).
//
// Follows Material/GridSettings exactly: claims the reserved CAPABILITY Type with its own member
// number, keeps its parameters in a small std140 uniform block rewritten in place by every setter, and
// apply() is read-only. The texture binds at the "osgx::sdf.texture" slot (osgx::Bindings),
// referenced by the shader through `layout(binding=...)` so no osg::Uniform is needed.
//
// uvRect is (u0, v0, u1, v1) in whole-texture space. Because tile-local uv is mapped with a plain
// mix(), v0 > v1 flips the tile vertically - useful, since OSG flips images on load and
// external sidecars usually give top-down pixel rects.
//
// Deliberately NOT here: any knowledge of where the texture came from (a slughorn export, an
// msdfgen run, a hand-authored PNG) and no sidecar parsing - that's a separate, later layer
// (a glTF `osgx_sdf` extension) which would just construct one of these.
// ================================================================================================
class SDF: public osg::StateAttribute {
public:
	static constexpr Type SDF_TYPE = CAPABILITY;
	static constexpr unsigned int SDF_MEMBER = 3;

	// Named SDFType, not Type: inside this class `Type` already means osg::StateAttribute::Type
	// (SDF_TYPE below, and getType()'s return), which a nested enum of that name would shadow.
	enum class SDFType {
		SDF = 0, // single channel, read from .r
		MSDF = 1 // three channel, median-of-three
	};

	SDF();
	SDF(const SDF& sdf, const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY);

	OSGX_META_StateAttribute(osgx, SDF, SDF_TYPE)

	unsigned int getMember() const override { return SDF_MEMBER; }
	int compare(const osg::StateAttribute& sa) const override;
	void apply(osg::State& state) const override;

	// Builds a Texture2D configured the way a distance field must be sampled: bilinear (LINEAR both
	// ways), NO mipmaps (a mip of a distance field is not a distance field at that scale), and
	// CLAMP_TO_EDGE (nothing wraps past a tile's own edge). A non-power-of-two image keeps
	// its real size (OSG's default rescale would invalidate every tile rect). A single-channel 8-bit grayscale image
	// (what msdfgen writes for a plain SDF, loaded by OSG as GL_LUMINANCE - a format core-profile GL
	// removed, so `texel.r` would not be reliable) is relabeled in place as GL_RED / GL_R8; same
	// bytes, just a plain red channel. Returns null for a null image.
	static osg::ref_ptr<osg::Texture2D> makeTexture(osg::Image* image);

	void setTexture(osg::Texture2D* texture);
	osg::Texture2D* getTexture() const { return _texture.get(); }

	void setSDFType(SDFType sdfType);
	SDFType getSDFType() const { return _sdfType; }

	void setPixelRange(float pixelRange);
	float getPixelRange() const { return _pixelRange; }

	void setUVRect(const osg::Vec4& uvRect);
	const osg::Vec4& getUVRect() const { return _uvRect; }

protected:
	virtual ~SDF();

private:
	void _initBuffer();
	void _write();

	osg::ref_ptr<osg::Texture2D> _texture;
	SDFType _sdfType = SDFType::SDF;
	float _pixelRange = 4.0f;
	osg::Vec4 _uvRect{0.0f, 0.0f, 1.0f, 1.0f};

	osg::ref_ptr<osgx::FloatArray> _buffer;
	osg::ref_ptr<osg::UniformBufferBinding> _binding;
	mutable std::once_flag _bindingResolved;

	mutable unsigned int _unit = 0;
	mutable std::once_flag _unitResolved;
};

}
