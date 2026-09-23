#pragma once

#include "Array.hpp"
#include "Shader.hpp"
#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Vec4>

OSGX_ENABLE_WARNINGS

#include <string>

namespace osg {
	class ShaderStorageBufferBinding;
	class Texture2D;
}

namespace osgx {

// ================================================================================================
// PBR / IBL
//
// This file (the BRDF math itself - GGX distribution, Schlick Fresnel, Smith geometry term,
// independent of where the incoming light comes from: the same terms feed a direct point-light
// loop or an IBL environment term) and IBL.hpp (the environment-as-light-source pipeline: a
// prefiltered specular cubemap plus a split-sum BRDF LUT (Karis 2013), and eventually SH-9
// diffuse irradiance, calling back into this file for its Fresnel term) both live directly under
// `osgx::` - neither is a separate opt-in subsystem (its own #include outside the umbrella, its
// own CMake link target) the way osgx::debug/imgui/platform/gltf/ktx2 are, so neither earns its
// own namespace; see TODO.md's namespace-boundary decision. The `"osgx::pbr"`/`"osgx::ibl"`
// strings passed to registerPBRShaderLibs()/registerIBLShaderLibs() below are just the shader-lib
// registry's conventional catalog tags, unrelated to the (now-flat) C++ namespace.
//
// Ported from the STATIC path of OpenSceneGraph.py/examples/pyosg-lighting/09-ibl.py: a
// pre-baked .ktx2 prefiltered cubemap loaded once, plus a one-shot BRDF LUT bake. Deliberately
// does NOT include 10-dynamicprobes.py's live GPU re-bake - out of scope here.
//
// Direct lights (LightSet, LightType, the DIRECT_LIGHT*/DIRECT_LIGHTING_* family) split out to
// Light.hpp/Light.cpp 2026-09-23 - a separate domain that happened to live here by history, not by
// dependency. This file has no C++ dependency on Light.hpp;
// a consumer wanting both includes both, same as always via the osgx.hpp umbrella.
// ================================================================================================

// GLSL function-body snippets, not full shaders - concatenate the ones you need into a
// consuming fragment shader (same mechanism osgSlug's SHADER_LIB_FRAGMENT uses: paste the
// source in, above main()). All function names carry the osgx_ prefix to avoid collisions
// with whatever else is in the consuming shader.
//
// Contract: these assume `const float PI = 3.14159265359;` is already in scope. Not bundled
// here, since plenty of consuming shaders already define PI themselves and a duplicate
// `const float PI` is a compile error, not a harmless redefinition - the caller adds it once.
//
// Kept as `inline constexpr` header definitions (not moved to PBR.cpp): most are bound directly
// by name in ext/osgx-python.cpp (needs real external linkage), AND all eleven are referenced
// inside registerPBRShaderLibs()'s `static constexpr ShaderLib` array below, which needs a genuine
// constant expression - `inline constexpr` in a header is the one form satisfying both, same
// reasoning as osgx::ibl's shader-string constants.

// GGX/Trowbridge-Reitz normal distribution term (D). NdotH and roughness in [0,1];
// `roughness * roughness` is the standard Disney/Karis alpha remap.
inline constexpr const char* D_GGX = R"GLSL(
float osgx_D_GGX(float NdotH, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / (PI * d * d);
}
)GLSL";

// Schlick-GGX geometry term for a single direction (view OR light). Combine both via
// osgx_G_Smith (G_SMITH below) for the full geometric attenuation term.
inline constexpr const char* G_SCHLICK = R"GLSL(
float osgx_G_Schlick(float NdotX, float roughness) {
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return NdotX / (NdotX * (1.0 - k) + k);
}
)GLSL";

// Smith's method: visible geometric attenuation = product of the view-side and light-side
// Schlick-GGX terms. Requires osgx_G_Schlick (G_SCHLICK) already in scope.
inline constexpr const char* G_SMITH = R"GLSL(
float osgx_G_Smith(float NdotV, float NdotL, float roughness) {
	return osgx_G_Schlick(NdotV, roughness) * osgx_G_Schlick(NdotL, roughness);
}
)GLSL";

// Fresnel-Schlick: reflectance rises toward white (dielectrics) or the material's own tint
// (metals, via F0 = mix(vec3(0.04), albedo, metallic)) at grazing angles. For direct lights.
inline constexpr const char* F_SCHLICK = R"GLSL(
vec3 osgx_F_Schlick(float cosTheta, vec3 F0) {
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}
)GLSL";

// Roughness-aware Fresnel (Lagarde) - for IBL specular, so a rough surface's Fresnel rim
// doesn't stay mirror-sharp the way plain F_Schlick would. Direct lights use F_SCHLICK instead.
inline constexpr const char* F_SCHLICK_ROUGHNESS = R"GLSL(
vec3 osgx_F_Schlick_roughness(float cosTheta, vec3 F0, float roughness) {
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}
)GLSL";

// Plain PBR material bundle - source-agnostic (osgGLTF's optional renderer populates one from
// its loader-defined material interface, but nothing here assumes glTF): everything DIRECT_SPECULAR,
// F_MULTISCATTER/IBL_SPECULAR, and a hemisphere/SH ambient term need to shade a fragment.
inline constexpr const char* MATERIAL_STRUCT = R"GLSL(
struct osgx_Material {
	vec3 albedo;
	float ao;
	float roughness;
	float metallic;
	vec3 F0;
};
)GLSL";

// Binding point for the factor buffer osgx::Material (below) builds. Lives here (generic osgx::),
// not under osgx::gltf - osgx::gltf::shader::MATERIAL_BINDING (Shader.hpp) is now just an alias
// for this constant, so a caller reading GET_MATERIAL never has to care whether the buffer at this
// binding was populated by the glTF loader (Material.cpp) or by a hand-authored osgx::Material
// (see examples/osgx-gbuffer-blueprint.cpp's buildShapeNode() for a non-glTF consumer) - same
// binding, same buffer shape, either way. See TODO.md's "Generic vs. glTF-specific layering"
// section for the principle this is following.
inline constexpr unsigned int MATERIAL_BINDING = 0;

// Texture units osgx::Material's four maps bind at, and osgx::gltf's loader populates directly
// (Material.cpp) for the same reason MATERIAL_BINDING lives here rather than under osgx::gltf::
// shader:: - osgx::gltf::shader::BASE_COLOR_TEXTURE_UNIT etc. (Shader.hpp) are now just aliases.
inline constexpr int BASE_COLOR_TEXTURE_UNIT = 0;
inline constexpr int NORMAL_TEXTURE_UNIT = 1;
inline constexpr int ORM_TEXTURE_UNIT = 2;
inline constexpr int EMISSIVE_TEXTURE_UNIT = 3;

// Custom osg::StateAttribute wrapping a PBR material's scalar factors and up to four texture maps
// into ONE state-graph object: `stateSet.setAttributeAndModes(new osgx::Material(...))` replaces
// the old attachMaterialFactors() free function plus however many manual
// setTextureAttributeAndModes() calls a caller previously had to keep in sync with it by hand
// (osgx::gltf's own loader - Material.cpp - was the worst offender: the has*Map flags it built
// were a SEPARATE set of locals from whatever texture binds actually happened, so a load failure
// partway through could silently leave them lying about what was bound). Modeled on osg::Material
// (a StateAttribute wrapping glMaterial state) more than on osgEarth::PBRTexture (a StateAttribute
// wrapping just texture refs, paired with a separate plain PBRMaterial value struct) - osgx::
// Material owns BOTH the scalar factors and the maps together, since MATERIAL_INPUTS/GET_MATERIAL
// (Shader.hpp/PBRIBL.cpp) already treat them as one interface.
//
// has*Map (the flags GET_MATERIAL gates every texture read behind) are no longer separate bools a
// caller can drift out of sync with reality - they're derived directly from whether the
// corresponding ref_ptr is set, so binding a texture and marking "this material has that map" are
// literally the same operation, by construction. hasOcclusion is the one exception: occlusion is
// read from the metallic-roughness map's own R channel, not a dedicated texture/unit of its own
// (see Material.cpp's ORM-baking comments), so it stays an explicit flag - setHasOcclusion().
//
// Deliberately claims the reserved osg::StateAttribute::CAPABILITY Type rather than reusing an
// existing built-in one the way osgEarth::PBRTexture reuses TEXTURE. CAPABILITY/member 0 is this
// class's key; osg::LightSet below uses CAPABILITY/member 1. osg::State keys its per-context "last
// applied attribute" bookkeeping by that (Type, member) pair, so unrelated custom attributes must
// not share both parts of it (a real osg::Texture at unit 0 alongside a same-key custom attribute,
// for instance, would silently fight over one slot).
// osgEarth::PBRTexture only gets away with reusing TEXTURE because it also gives up on compare()
// (unconditionally returns -1, opting out of state-sorting dedup entirely) - osgx::Material does
// neither: real dedup means two drawables sharing an equal material (same texture pointers --
// osgx::gltf's TextureLoader already caches/shares those - and equal factors) skip a redundant
// apply() entirely.
//
// apply() is deliberately read-only over this object's state - it binds whatever's already set,
// nothing more. Every setter rebuilds the factor buffer's contents immediately (in place, via
// osg::Array::dirty() - no new GL buffer object, no custom per-context dirty flag) rather than
// leaving that work for apply() to discover lazily; apply() can then run concurrently from
// multiple graphics contexts (osgViewer::CompositeViewer, an offscreen bake pass alongside the
// main view) with no shared mutable state to race over, the same guarantee osg::Material::apply()
// gets for free by only ever touching glMaterialfv with already-known values.
class Material: public osg::StateAttribute {
	public:
		static constexpr Type MATERIAL_TYPE = CAPABILITY;

		Material();
		Material(const Material& material, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);

		OSGX_META_StateAttribute(osgx, Material, MATERIAL_TYPE)

		int compare(const osg::StateAttribute& sa) const override;
		void apply(osg::State& state) const override;

		void setBaseColor(const osg::Vec4& baseColor);
		const osg::Vec4& getBaseColor() const { return _baseColor; }

		void setRoughness(float roughness);
		float getRoughness() const { return _roughness; }

		void setMetallic(float metallic);
		float getMetallic() const { return _metallic; }

		// See the class comment - occlusion has no dedicated unit of its own, so unlike the four
		// map setters below, this doesn't derive from a ref_ptr.
		void setHasOcclusion(bool hasOcclusion);
		bool getHasOcclusion() const { return _hasOcclusion; }

		void setBaseColorMap(osg::Texture2D* texture);
		osg::Texture2D* getBaseColorMap() const { return _baseColorMap.get(); }

		void setNormalMap(osg::Texture2D* texture);
		osg::Texture2D* getNormalMap() const { return _normalMap.get(); }

		// glTF's combined occlusion/roughness/metallic texture, bound at ORM_TEXTURE_UNIT.
		void setMetallicRoughnessMap(osg::Texture2D* texture);
		osg::Texture2D* getMetallicRoughnessMap() const { return _metallicRoughnessMap.get(); }

		void setEmissiveMap(osg::Texture2D* texture);
		osg::Texture2D* getEmissiveMap() const { return _emissiveMap.get(); }

	protected:
		virtual ~Material();

	private:
		void _initBuffer();
		void _writeFactors();

		osg::Vec4 _baseColor{1.0f, 1.0f, 1.0f, 1.0f};
		float _roughness = 1.0f;
		float _metallic = 1.0f;
		bool _hasOcclusion = false;

		osg::ref_ptr<osg::Texture2D> _baseColorMap;
		osg::ref_ptr<osg::Texture2D> _normalMap;
		osg::ref_ptr<osg::Texture2D> _metallicRoughnessMap;
		osg::ref_ptr<osg::Texture2D> _emissiveMap;

		osg::ref_ptr<osgx::FloatArray> _buffer;
		osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
};

// All five snippets, concatenated in dependency order (G_SMITH calls osgx_G_Schlick, so
// G_SCHLICK must precede it). Convenience for callers that want the whole BRDF toolkit;
// reach for the individual constants instead if only part of it is needed.
std::string snippets();

// Multi-scattering energy-compensated Fresnel (Fdez-Aguera 2019, "A Multiple-Scattering
// Microfacet Model for Real-Time Image-based Lighting"), the same formula the official Khronos
// glTF-Sample-Viewer uses (ported from OpenSceneGraph.py/examples/pyosg-khronos-viewer.py's
// fresnel(), which was itself written to match that viewer's IBL.glsl exactly - confirmed
// pixel-parity against github.khronos.org/glTF-Sample-Viewer-Release/ on 2026-07-22).
//
// The classic single-scatter split-sum approximation (Karis 2013 - what IBL_SPECULAR below
// used before this was added) loses energy at higher roughness because it only accounts for
// light bouncing off the microfacet surface once; this adds back an estimate of what multiple
// internal bounces would have contributed, using the same split-sum LUT (ab.x/ab.y) the
// single-scatter term already samples - no extra texture reads, just more math on the same
// two numbers. Most visible on rough metals/dielectrics, which the single-scatter version
// renders measurably too dark/desaturated.
inline constexpr const char* F_MULTISCATTER = R"GLSL(
vec3 osgx_F_MultiScatter(vec3 N, vec3 V, float roughness, vec3 F0, sampler2D brdfLUT) {
	float NdotV = max(dot(N, V), 0.0);
	vec2 ab = texture(brdfLUT, clamp(vec2(NdotV, roughness), 0.0, 1.0)).rg;

	// Single-scatter Fresnel (Schlick, roughness-aware) and its split-sum-combined result.
	vec3 Fss = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
	vec3 FssCombined = Fss * ab.x + ab.y;

	// Energy lost to single-scatter, and the average Fresnel across all angles - both from
	// Fdez-Aguera's derivation; feeds a geometric-series estimate of the multi-bounce term.
	float Ems = 1.0 - (ab.x + ab.y);
	vec3 Favg = F0 + (1.0 - F0) / 21.0;

	return FssCombined + Ems * FssCombined * Favg / (1.0 - Favg * Ems);
}
)GLSL";

// Split-sum IBL specular: samples the prefiltered cubemap along the reflection vector and
// combines with the baked BRDF LUT via the multi-scatter energy-compensated Fresnel above (not
// the plain Karis 2013 single-scatter combine this used to be). Handles the OSG (Z-up) ->
// baked-cubemap (Y-up) face remap internally. Requires F_MULTISCATTER already in scope.
inline constexpr const char* IBL_SPECULAR = R"GLSL(
vec3 osgx_IBLSpecular(
	vec3 N,
	vec3 V,
	vec3 F0,
	float roughness,
	samplerCube envMap,
	sampler2D brdfLUT,
	float envMaxMip
) {
	vec3 R = reflect(-V, N);

	// OSG world space is Z-up; the baked cubemap's faces are Y-up - without this remap we'd
	// sample a direction that doesn't correspond to R at all.
	vec3 R_gl = vec3(R.x, R.z, -R.y);

	vec3 prefilt = textureLod(envMap, R_gl, roughness * envMaxMip).rgb;
	vec3 F = osgx_F_MultiScatter(N, V, roughness, F0, brdfLUT);

	return prefilt * F;
}
)GLSL";

// osgx_AmbientLighting() CONTRACT - same "hook" pattern as osgx_DirectLighting() above (see its
// own contract comment for the full rationale): a consumer's fragment shader only needs
// AMBIENT_LIGHTING_DECL spliced in (list MATERIAL_STRUCT earlier in the SAME pragma line) plus a
// call site (`color += osgx_AmbientLighting(N, V, mat, envMap, brdfLUT, envMaxMip, iblIntensity);`);
// it never touches osgx_IBLSpecular/osgx_F_MultiScatter directly. The DEFINITION lives in
// AMBIENT_LIGHTING_HOOK_DEFAULT below - specular-only (no SH-9 diffuse irradiance yet, see
// osgx::ibl TODO.md) - a consumer wanting real diffuse IBL (osgx_EvaluateIBL(), IBL.hpp's own
// EVALUATE_IBL - osgx::gltf::pbribl's PBRIBL.cpp is its own real consumer, bakes a Lambertian
// irradiance cubemap and blends diffuse/specular against two independent intensities) supplies
// its own shader object defining osgx_AmbientLighting() instead of adding
// AMBIENT_LIGHTING_HOOK_DEFAULT - same override mechanism, and why PBRIBL.cpp does not (yet) route
// through this hook itself.
inline constexpr const char* AMBIENT_LIGHTING_DECL = R"GLSL(
vec3 osgx_AmbientLighting(
	vec3 N,
	vec3 V,
	osgx_Material mat,
	samplerCube envMap,
	sampler2D brdfLUT,
	float envMaxMip,
	float intensity
);
)GLSL";

// Self-contained - carries its own #version/#pragma line so it compiles as a standalone
// osg::Shader object regardless of what the consumer's own fragment shader happens to have in
// scope. Add alongside DIRECT_LIGHTING_HOOK_DEFAULT (if also used) as another EXTRA shader object
// on the same Program - not spliced by name via #pragma osgx::pbr, so it is deliberately NOT in
// registerPBRShaderLibs()'s catalog.
inline constexpr const char* AMBIENT_LIGHTING_HOOK_DEFAULT = R"GLSL(
#version 460 core

#pragma osgx::pbr MATERIAL_STRUCT, F_MULTISCATTER, IBL_SPECULAR

vec3 osgx_AmbientLighting(
	vec3 N,
	vec3 V,
	osgx_Material mat,
	samplerCube envMap,
	sampler2D brdfLUT,
	float envMaxMip,
	float intensity
) {
	return osgx_IBLSpecular(N, V, mat.F0, mat.roughness, envMap, brdfLUT, envMaxMip) * intensity;
}
)GLSL";

// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019 / Filament's normal filtering):
// widens roughness where the shading normal changes rapidly across a pixel's screen-space
// footprint, so a low-roughness, high-curvature surface (a beveled metal trim is the case that
// motivated this) converges under low sample counts instead of showing per-pixel reflection
// sparkle/aliasing as its mirror-like reflection vector jitters between neighboring fragments.
// Works in alpha space (roughness^2, the actual GGX parameter) since the added variance term is
// only meaningful there, then converts back to the perceptual roughness this codebase otherwise
// passes around (IBL_SPECULAR/F_MULTISCATTER's LOD selection and BRDF LUT lookups both expect
// perceptual roughness, not alpha). `N` should be the final shading normal (post normal-map),
// evaluated in any space - dFdx/dFdy operate on screen-space fragment neighbors regardless.
inline constexpr const char* SPECULAR_AA = R"GLSL(
float osgx_SpecularAA(vec3 N, float roughness) {
	vec3 dndx = dFdx(N);
	vec3 dndy = dFdy(N);
	float variance = 0.15 * (dot(dndx, dndx) + dot(dndy, dndy));
	float kernelAlpha = min(variance, 0.18);
	float alpha = roughness * roughness;

	return sqrt(clamp(alpha + kernelAlpha, 0.0, 1.0));
}
)GLSL";

// Khronos PBR Neutral tonemap - hue-preserving (no ACES orange shift), for compressing HDR
// specular (routinely > 1.0 off a near-mirror surface under a bright environment) into LDR
// without hard-clipping to solid white. Ported verbatim from 09-ibl.py's tonemapPBRNeutral().
// Caller still applies its own gamma afterward if not rendering to an sRGB framebuffer.
inline constexpr const char* TONEMAP_PBR_NEUTRAL = R"GLSL(
vec3 osgx_TonemapPBRNeutral(vec3 color) {
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;
	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;
	float peak = max(color.r, max(color.g, color.b));
	if(peak >= startCompression) {
		float d = 1.0 - startCompression;
		float newPeak = 1.0 - d * d / (peak + d - startCompression);
		color *= newPeak / peak;
		float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
		color = mix(color, vec3(newPeak), g);
	}
	return clamp(color, 0.0, 1.0);
}
)GLSL";

// osgx_Tonemap() CONTRACT - same "hook" pattern as osgx_DirectLighting()/osgx_AmbientLighting()
// above: a consumer's fragment shader only needs TONEMAP_DECL spliced in plus a call site
// (`color = osgx_Tonemap(color);`) on its final linear color, before gamma. The DEFINITION lives
// in TONEMAP_HOOK_DEFAULT below (osgx_TonemapPBRNeutral, unchanged) - a consumer wanting a
// different tone curve (ACES, a flat clamp, a look-specific LUT) supplies its own shader object
// defining osgx_Tonemap() instead of adding TONEMAP_HOOK_DEFAULT.
inline constexpr const char* TONEMAP_DECL = R"GLSL(
vec3 osgx_Tonemap(vec3 color);
)GLSL";

// Self-contained, same shape as DIRECT_LIGHTING_HOOK_DEFAULT/AMBIENT_LIGHTING_HOOK_DEFAULT above --
// not spliced by name via #pragma osgx::pbr, so deliberately NOT in registerPBRShaderLibs()'s catalog.
inline constexpr const char* TONEMAP_HOOK_DEFAULT = R"GLSL(
#version 460 core

#pragma osgx::pbr TONEMAP_PBR_NEUTRAL

vec3 osgx_Tonemap(vec3 color) {
	return osgx_TonemapPBRNeutral(color);
}
)GLSL";

// Pass-through, for a consumer that tonemaps somewhere else entirely (a post-processing chain
// ending in its own exposure/tonemap pass, e.g. pyosg-lighting/11-sketchfab.py).
//
// **Attach this rather than attaching no tonemap hook at all.** A consumer that skips the hook and
// #ifdef's the osgx_Tonemap() CALL out of its own shader instead has made the function's existence
// depend on a #define - and a define is NOT guaranteed to be present at every compile. OSG
// pre-compiles StateSets at realize time via osgUtil::GLObjectsVisitor, which does not carry the
// accumulated osg::State define stack that State::apply() builds during real rendering, so that
// pass sees an EMPTY define string, keeps the call, finds no definition, and fails to link. The
// later render-time compile (correct define string, call stripped) then succeeds, so the app runs
// and only a "glLinkProgram FAILED ... undefined function osgx_Tonemap" in the log ever shows it.
// Cost a full session in 11-sketchfab.py before being tracked down. An identity function is free
// (every compiler inlines it away) and links under ANY define string.
inline constexpr const char* TONEMAP_HOOK_IDENTITY = R"GLSL(
#version 460 core

vec3 osgx_Tonemap(vec3 color) {
	return color;
}
)GLSL";

void registerPBRShaderLibs();

}
