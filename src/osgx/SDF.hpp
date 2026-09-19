#pragma once

// osgx::SDF - a small catalog of pure, closed-form 2D signed-distance functions (negative =
// inside, matching every other SDF convention in this codebase), published only as a
// `#pragma osgx::sdf` GLSL shader-library entry - there is nothing to call from C++ here, unlike
// Projection.hpp's CPU-side twins, since these primitives have no meaningful CPU-side
// counterpart (they're pure per-fragment shape tests).
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

// GLSL `#pragma osgx::sdf SHAPES` catalog registration - see registerShaderLibs()/
// resolveShaderLibs() in Shader.hpp. Publishes the nine functions listed above.
void registerSDFShaderLibs();

}
