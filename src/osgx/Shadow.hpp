#pragma once

#include "Array.hpp"
#include "CaptureCubeMap.hpp"
#include "Core.hpp"
#include "RTT.hpp"
#include "Shader.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BoundingSphere>
#include <osg/BufferIndexBinding>
#include <osg/Camera>
#include <osg/Group>
#include <osg/Matrixd>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec3>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// ================================================================================================
// Shadow mapping for any osgx::LightSet-lit scene (nothing here is glTF/PBR-specific). Lives
// directly under `osgx::`, not its own namespace - it's not a separate opt-in subsystem (its own
// #include outside the umbrella, its own CMake link target) the way osgx::debug/imgui/platform/
// gltf/ktx2 are; see TODO.md's namespace-boundary decision. The `"osgx::shadow"` catalog name is
// the shader-lib registry's conventional tag, unrelated to the C++ namespace. This is the real
// osgx home for the shadow_cam/shadowFactor() pattern OpenSceneGraph.py/examples/pyosg-lighting/
// 08-shadows.py and 09-ibl.py both independently hand-rolled (and, in 08-shadows.py's case,
// duplicated a second time between the model and floor fragment shaders).
//
// Directional, spot, AND point lights can all cast shadows (ShadowMap::create()/createSpot()/
// createPoint()), and - as of the osgx::ShadowSet/Hook::ShadowFactor redesign below (2026-10-02) -
// any mix of them SIMULTANEOUSLY, up to MAX_SHADOWED_2D directional/spot maps and
// MAX_SHADOWED_CUBE point maps at once. Earlier versions could shadow at most one light, ever
// (directional/spot and point were even mutually exclusive, since each required swapping the
// ENTIRE osgx_DirectLighting() implementation for a near-duplicate copy of the same per-light
// loop). See osgx::ShadowSet's own comment for the current design.
//
// World-space, not eye-space: osgx::PBRScene's direct-lighting call site (PBRScene.cpp's
// FULL_PBR_FRAGMENT_SHADER_SRC) already reconstructs a genuine world-space `worldPos` for
// osgx_DirectLighting() (via osg_ViewMatrixInverse), unlike the old hand-rolled examples, which
// shaded in eye space and had to compose `inverse(camView)` into their shadow matrix every frame
// to compensate. Working in world space here instead means `shadowMatrix` is just `lightProj *
// lightView` - no per-frame main-camera dependency at all - and only needs recomputing
// (updateMatrix()) if the light itself moves, which no pyosg-lighting example has ever done.
// ================================================================================================

// Compile-time caps for osgx::ShadowSet's two small per-kind slot arrays (GLSL can't mix
// sampler2D/samplerCube in one array, so directional/spot and point need separate arrays - see
// SHADOW_UNIFORMS_MULTI below). Deliberately small, matching osgx::MAX_LIGHTS' own "a small
// handful, not a general-purpose budget" philosophy (Light.hpp) - raise them if a real scene needs
// more simultaneously-shadowed lights than this. Real C++ constants (not just GLSL #defines) so
// osgx::ShadowSet can size its own arrays without hardcoding a number that has to stay in sync by
// hand - matching MAX_LIGHTS/OSGX_MAX_LIGHTS's own precedent. If either changes,
// SHADOW_UNIFORMS_MULTI's OSGX_MAX_SHADOWED_2D/OSGX_MAX_SHADOWED_CUBE #defines must change with it.
inline constexpr int MAX_SHADOWED_2D = 2;
inline constexpr int MAX_SHADOWED_CUBE = 2;

struct ShadowMap {
	// The scene-bound pair every create()/createSpot()/createPoint()/reposition*() overload used
	// to take as two separate positional arguments (`sceneBoundCenter`, `sceneBoundRadius`) -
	// bundled here per the "too many long position-order-dependent positional arguments" complaint
	// in ai/todo-shadow.md, then grown to carry the Codex Overview's own `ShadowCoverage` idea
	// (caster bounds distinct from requested receiver bounds - see `receiverCenter`/
	// `receiverRadius`/`bound()` below). Deliberately NOT folded into Options itself - unlike
	// Options, which is genuinely shared verbatim across repositioning calls, `position`/
	// `direction`/`outerConeAngle` stay their own light-kind-specific arguments outside of either.
	struct Coverage {
		// What must be RENDERED into the depth map - geometry outside this bound may be clipped
		// out of the shadow camera's own cull pass and simply never cast at all.
		osg::Vec3 center;
		float radius = 1.0f;

		// What must be able to SAMPLE the depth map correctly - e.g. a floor/room extending well
		// past the casting model's own bound. 0 (default, `receiverRadius` unset) means "receivers
		// never extend past the caster bound itself," matching every existing caller exactly -
		// same 0-is-auto sentinel convention as Options::extent. Left UNSET rather than defaulted
		// to the caster bound, so bound() below can tell "no receiver given" apart from "receiver
		// happens to equal the caster." See ai/todo-shadow.md's "chopped off" writeup for why a
		// receiver outside the caster-only bound silently reads as unshadowed rather than erroring.
		osg::Vec3 receiverCenter;
		float receiverRadius = 0.0f;

		// The single sphere every create()/reposition*() fitting call actually sizes its frustum/
		// far-plane against - the caster bound, expanded to also enclose the receiver bound when
		// one was given. A directional/spot map's single frustum has to contain BOTH: casting
		// geometry that isn't rendered into the map can't cast at all (see `center`/`radius`
		// above), and a receiver point outside the map's coverage silently reads as unshadowed
		// (see `receiverCenter`/`receiverRadius` above) - there is no second frustum to split the
		// two needs across yet (that's what directional cascades would eventually be for).
		osg::BoundingSphere bound() const {
			osg::BoundingSphere b(center, radius);

			if(receiverRadius > 0.0f) b.expandBy(osg::BoundingSphere(receiverCenter, receiverRadius));

			return b;
		}
	};

	struct Options {
		int size = 1024;

		// Half-width, in world units, of the ORTHOGRAPHIC shadow frustum's box (both the X/Y extent
		// and the margin added to near/far - see `margin` below). A directional light's rays are
		// parallel by definition, so this - not a field-of-view angle - is what actually determines
		// coverage; a perspective frustum here would make the light behave like a nearby spotlight
		// whose rays diverge, which visibly disagrees with a direct-lighting term that (correctly)
		// treats every point in the scene as lit from the same direction. 0 (the default) derives the
		// extent from `coverage.bound().radius() * margin` (the caster bound, or the caster+receiver
		// merged bound when Coverage::receiverRadius is set - see Coverage's own comment), matching
		// every existing caller's coverage exactly. Prefer Coverage::receiverCenter/receiverRadius
		// over setting this directly when the real need is "a floor/room must receive shadows well
		// past the model's own bound" - this raw override remains for anything that still needs to
		// bypass the derivation entirely (ported from OpenSceneGraph.py's 11-sketchfab.py, which
		// computed this by hand as `max(bound_radius * margin, floor_size)` before either existed).
		float extent = 0.0f;

		// Multiplies coverage.bound().radius() both when deriving a default `extent` above and when sizing
		// near/far planes. Ported directly from 09-ibl.py's own investigation: a shadow camera placed
		// at a FIXED distance from the scene puts near/far arbitrarily close together for a small
		// scene and arbitrarily far apart for a large one - Lantern (a ~15-unit-radius glTF model)
		// hit a ~2870:1 near:far ratio this way, collapsing shadow-map depth precision to nothing (the
		// depth comparison (osgx_ShadowFactorForLight(), below) never triggers - looks like "no shadow" but is
		// really "no usable depth precision"). Scaling by the scene's own bound keeps near:far bounded
		// to a healthy ratio regardless of scene scale, with no per-scene tuning.
		float margin = 1.3f;

		float bias = 0.005f;
		float strength = 0.7f; // 0 = shadows have no effect, 1 = fully black
	};

	// A directional (create()) or spot (createSpot()) shadow map: owns the PRE_RENDER depth-only
	// camera (`camera` - add it to the scene graph, e.g. as a sibling of whatever the shadowed
	// model's own parent is, exactly where the old hand-rolled examples added their own
	// `shadow_cam`) plus the uniform VALUES osgx::ShadowSet reads into its own combined arrays
	// (see ShadowSet's own comment below - these are no longer attached directly to a StateSet by
	// ShadowMap itself; ShadowSet owns that job now). Depth-only: no dummy color attachment needed
	// (the old hand-rolled Python examples worked around a since-irrelevant pybind11 binding gap -
	// osg::Camera::setDrawBuffer/setReadBuffer are ordinary C++ calls here).
	osg::ref_ptr<osg::Camera> camera;
	osg::ref_ptr<osg::Texture2D> depthTexture;

	// world space -> light clip space. Set once by ShadowMap::create(); only needs
	// recomputing (updateMatrix()) if the light direction changes after creation.
	osg::ref_ptr<osg::Uniform> shadowMatrix;
	osg::ref_ptr<osg::Uniform> bias;
	osg::ref_ptr<osg::Uniform> strength;

	// World-space distance, applied along the receiver's OWN normal to worldPos BEFORE it is
	// transformed into light space (SHADOW_FACTOR_2D/SHADOW_FACTOR_CUBE below) - unlike `bias`
	// above, this is not a comparison-space value, so the identical formula works for
	// directional/spot's non-linear projected depth AND point's linear cube distance alike.
	// Derived from this map's own texel footprint (coverage / resolution) at create()/
	// createSpot()/createPoint() time - see each one's own comment for its exact formula -
	// rather than a single literal, since a map covering a tabletop and one covering a terrain
	// need very different absolute offsets for the same number of texels of safety margin.
	// `bias` remains available as a raw escape hatch on top of this for content normal offset
	// alone doesn't fully resolve.
	osg::ref_ptr<osg::Uniform> normalOffset;

	// Which osgx_lights[] index (osgx::LightSet) this shadow map is cast by/matched against - read
	// by osgx::ShadowSet::add() at the moment a map is added to a set, and again by sync() after
	// any reposition*() call, to know which slot of ShadowSet's own casterIndex array to write.
	// Defaults to 0 (the common "index 0 is the key light" convention every existing
	// pyosg-lighting example already follows); change it before adding this map to a ShadowSet if
	// it shadows a different light.
	osg::ref_ptr<osg::Uniform> casterIndex;

	osg::Matrixd lightView, lightProj;

	// Point-light-only (see createPoint()) - null/invalid for a directional or spot map, whose
	// `camera`/`depthTexture`/`shadowMatrix` above are the ones populated instead. A point light
	// needs visibility in every direction, not one 2D frustum's worth, so it's built on
	// CaptureCubeMap's six-camera distance-cube capture (CaptureCubeMap.hpp) rather than a
	// single osgx::RTT camera. `casters` is createPoint()'s counterpart to adding children
	// directly to `camera` for the other two kinds - there's no single camera here for a caller to
	// hang casting geometry off of, so all six cameras share this one Group instead.
	CaptureCubeMap cubeCapture;
	osg::ref_ptr<osg::Group> casters;
	osg::ref_ptr<osg::Uniform> lightPosition;

	bool valid() const;

	// Builds `camera` - an ORTHOGRAPHIC depth-only camera, the physically-correct frustum shape
	// for a directional (parallel-ray) light - looking from a point `2 * extent` away from
	// `coverage.center` (`extent` per Options::extent), back along `lightDirection`,
	// toward `coverage.center`. `lightDirection` is the ray TRAVEL direction, matching
	// osgx::LightSet::setDirectional()'s own convention - the camera looks the opposite way,
	// toward where the light is coming FROM, same as any physical shadow-casting light would.
	//
	// `camera`'s own StateSet carries a minimal depth-only Program (`ON|OVERRIDE`, vertex-transform
	// only, empty fragment main()) - ANY subgraph added as `camera`'s child renders through this,
	// not through whatever (potentially expensive: normal-mapped/textured/IBL-lit) Program that
	// subgraph's own StateSet carries for the main render. Every existing pyosg-lighting example
	// previously ran its full PBR/IBL fragment shader during the shadow pass too, computing lighting
	// it then threw away except for gl_FragDepth - see osgx/TODO.md's Shadow section ("use simpler
	// shadow shaders... do not re-use shaders and then just discard the complicated color values").
	// This does NOT alpha-test glTF MASK materials (no material/texture awareness at all, by design
	// - see this struct's own comment) - a caller whose casting geometry relies on alpha-cutout
	// shadows needs to override this Program on that geometry's own StateSet with something
	// alpha-aware (no existing pyosg-lighting example needs this yet).
	static ShadowMap create(
		const osg::Vec3& lightDirection,
		const Coverage& coverage,
		const Options& options
	);
	static ShadowMap create(
		const osg::Vec3& lightDirection,
		const Coverage& coverage
	);

	// Recomputes `shadowMatrix` from `lightView`/`lightProj` - call after mutating either
	// directly; a no-op to call redundantly otherwise. Does NOT reposition `camera` itself or touch
	// its view/projection - see reposition() for that.
	void updateMatrix();

	// Repositions this EXISTING ShadowMap for a new light direction/scene bound, in place - no new
	// camera/FBO/depth-texture allocation, just recomputed view/projection matrices (same math
	// create() itself uses) plus updateMatrix(). Cheap enough to call every
	// frame, or on every GUI-slider tick, for an interactively-moving light - create()
	// itself remains the right call for a light that's fixed at scene-build time (07/08/09/10's
	// pyosg-lighting rig, none of which move their light); this is for the genuinely-live case (a
	// light an operator can drag around, e.g. 11-sketchfab.py's orbiting key light), where rebuilding
	// the whole camera/texture on every drag tick would be wasteful and could visibly stutter.
	// `options` should match whatever was originally passed to create() - passing
	// a different `size` here does NOT resize `depthTexture`, only the view/projection matrices are
	// recomputed.
	void reposition(
		const osg::Vec3& lightDirection,
		const Coverage& coverage,
		const Options& options
	);
	void reposition(
		const osg::Vec3& lightDirection,
		const Coverage& coverage
	);

	// A spot light's shadow map: a PERSPECTIVE depth-only camera at `position` looking along
	// `direction` (ray travel direction, as LightSet::setSpot()), its field of view covering
	// `outerConeAngle` (radians, half-angle, as LightSet::setSpot()). Near/far bracket the scene
	// bound (`coverage.radius * options.margin`) as seen from the light; `options.extent` is not
	// used. Everything else - `camera`, `depthTexture`, the uniforms - is the same as create()'s.
	// `bias` is compared in the map's non-linear depth, so a spot map usually wants a smaller bias
	// than a directional one.
	static ShadowMap createSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const Coverage& coverage,
		const Options& options
	);
	static ShadowMap createSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const Coverage& coverage
	);

	// reposition()'s counterpart for a createSpot() map.
	void repositionSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const Coverage& coverage,
		const Options& options
	);
	void repositionSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const Coverage& coverage
	);

	// A point light's shadow: an omnidirectional distance CUBE MAP (CaptureCubeMap, six
	// perspective views written by a distance-only Program) instead of a single 2D depth camera -
	// see this struct's own header comment for why a point light specifically needs this.
	// `cubeSize` is separate from Options::size (which this ignores) since it's six
	// real-time cameras, not one - start small (256, the default) and raise it once the demo's
	// actually running. `options.extent` is unused (no ortho box); `options.margin` sizes the far
	// plane the same way createSpot() does.
	static ShadowMap createPoint(
		const osg::Vec3& position,
		const Coverage& coverage,
		int cubeSize,
		const Options& options
	);
	static ShadowMap createPoint(
		const osg::Vec3& position,
		const Coverage& coverage,
		int cubeSize=256
	);

	// reposition()/repositionSpot()'s counterpart for a createPoint() map - re-aims the six capture
	// cameras at a new position and refreshes their shared far plane/clear value for the new
	// distance to the scene (unlike the 2D map kinds, a point map's far plane has to track the
	// light directly - see this method's own .cpp comment).
	void repositionPoint(
		const osg::Vec3& position,
		const Coverage& coverage,
		const Options& options
	);
	void repositionPoint(
		const osg::Vec3& position,
		const Coverage& coverage
	);
};

// osgx_ShadowFactorForLight() CONTRACT - the Hook::ShadowFactor slot (Shader.hpp). Registered in
// the "osgx::shadow" catalog (unlike DIRECT_LIGHTING_DECL, which is only ever concatenated
// directly by Program-building C++ code) specifically so a genuinely custom osgx_DirectLighting()
// (a Hook::DirectLighting override) can pull in just this declaration via
// `#pragma osgx::shadow SHADOW_FACTOR_DECL` and call it directly - the whole point of factoring
// this out of osgx_DirectLighting() instead of leaving it bundled inside one monolithic hook.
inline constexpr const char* SHADOW_FACTOR_DECL = R"GLSL(
float osgx_ShadowFactorForLight(int lightIndex, vec3 worldPos, vec3 N);
)GLSL";

// Hook::ShadowFactor's DEFAULT - every light unshadowed, no shadow-map uniforms or samplers
// declared at all. A scene with no osgx::ShadowSet pays nothing for this slot beyond the
// function-call overhead (optimized away entirely by most drivers for a single `return 1.0`).
// Self-contained (own #version line) so it compiles as a standalone osg::Shader object, same
// convention as DIRECT_LIGHTING_HOOK_DEFAULT (Light.hpp) - not spliced by #pragma, so deliberately
// NOT in the "osgx::shadow" catalog.
inline constexpr const char* SHADOW_FACTOR_HOOK_NONE = R"GLSL(
#version 460 core

float osgx_ShadowFactorForLight(int lightIndex, vec3 worldPos, vec3 N) {
	return 1.0;
}
)GLSL";

// Hook::ShadowFactor's REAL override - osgx::ShadowSet's own shader (see ShadowSet's comment
// below for the C++ side). Two small per-KIND slot arrays, not one combined array: GLSL cannot mix
// sampler2D and samplerCube in the same array, so directional/spot (2D) and point (cube) need
// separate arrays, each sized by this file's own MAX_SHADOWED_2D/MAX_SHADOWED_CUBE C++ constants
// (kept in sync by hand with the #defines below - same precedent as MAX_LIGHTS/OSGX_MAX_LIGHTS in
// Light.hpp). The per-slot matrix/bias/normalOffset/strength/casterIndex fields live in one
// std140 uniform block (binding = @osgx::shadow@, same slot-reuse convention as
// LIGHT_UNIFORMS' "osgx::light" in Light.hpp) - GLSL forbids opaque types (samplers) inside a
// uniform block, so only the two sampler arrays stay plain uniforms; everything else moved out of
// per-field arrays here specifically to match LightSet's own UBO precedent instead of a dozen
// separate osg::Uniform arrays (see ai discussion, 2026-10-02). `casterIndex` is which
// osgx_lights[] index that slot shadows, or -1 for an unused slot (osgx::ShadowSet leaves every
// slot beyond however many maps were actually add()ed at -1).
//
// Packed layout (std140; must match Shadow.cpp's own float offsets exactly):
//   osgx_ShadowData2D (20 floats / 80 bytes):
//     mat4  matrix        offset  0
//     float bias          offset 64
//     float normalOffset  offset 68
//     float strength      offset 72
//     int   casterIndex   offset 76
//   osgx_ShadowDataCube (8 floats / 32 bytes - std140 packs `bias` into the last 4 bytes of
//   `lightPos`'s own 16-byte slot, the same vec3-then-scalar trick LIGHT_UNIFORMS' osgx_Light
//   struct relies on; byte 28 is unused tail padding up to the struct's own 16-byte alignment):
//     vec3  lightPos      offset  0
//     float bias          offset 12
//     float normalOffset  offset 16
//     float strength      offset 20
//     int   casterIndex   offset 24
inline constexpr const char* SHADOW_UNIFORMS_MULTI = R"GLSL(
#define OSGX_MAX_SHADOWED_2D 2
#define OSGX_MAX_SHADOWED_CUBE 2

struct osgx_ShadowData2D {
	mat4 matrix;
	float bias;
	float normalOffset;
	float strength;
	int casterIndex;
};

struct osgx_ShadowDataCube {
	vec3 lightPos;
	float bias;
	float normalOffset;
	float strength;
	int casterIndex;
};

layout(std140, binding = @osgx::shadow@) uniform osgx_ShadowBuffer {
	osgx_ShadowData2D osgx_shadowData2D[OSGX_MAX_SHADOWED_2D];
	osgx_ShadowDataCube osgx_shadowDataCube[OSGX_MAX_SHADOWED_CUBE];
};

uniform sampler2D osgx_shadowMaps2D[OSGX_MAX_SHADOWED_2D];
uniform samplerCube osgx_shadowMapsCube[OSGX_MAX_SHADOWED_CUBE];
)GLSL";

// PCF 3x3 shadow test, WORLD-space, for 2D (directional/spot) slot `slot` - the same math the old
// single-map SHADOW_FACTOR used to do unconditionally, now parameterized by array index. `slot` is
// a dynamically UNIFORM expression (derived only from a loop counter and uniform data, identical
// across every fragment in the draw call), which core GLSL 4.00+ permits for sampler-array
// indexing with no extension. `N` offsets worldPos along the receiver's own normal by
// osgx_shadowData2D[slot].normalOffset BEFORE the light-space transform - see
// ShadowMap::normalOffset's own comment for why this is preferred over a larger bias alone.
// Requires SHADOW_UNIFORMS_MULTI already in scope.
inline constexpr const char* SHADOW_FACTOR_2D = R"GLSL(
float osgx_ShadowFactor2D(int slot, vec3 worldPos, vec3 N) {
	osgx_ShadowData2D d = osgx_shadowData2D[slot];
	vec4 sc = d.matrix * vec4(worldPos + N * d.normalOffset, 1.0);

	sc /= sc.w;

	vec3 uv = sc.xyz * 0.5 + 0.5;

	if(any(lessThan(uv, vec3(0.0))) || any(greaterThan(uv, vec3(1.0)))) return 1.0;

	vec2 sz = 1.0 / vec2(textureSize(osgx_shadowMaps2D[slot], 0));
	float shadow = 0.0;

	for(int x = -1; x <= 1; x++) {
		for(int y = -1; y <= 1; y++) {
			shadow += (
				uv.z - d.bias > texture(osgx_shadowMaps2D[slot], uv.xy + vec2(x, y) * sz).r
			) ? 1.0 : 0.0;
		}
	}

	return mix(1.0, 1.0 - d.strength, shadow / 9.0);
}
)GLSL";

// Cube-map distance-compare shadow test for point slot `slot` - the same math the old single-map
// SHADOW_FACTOR_POINT used to do, parameterized by array index (see SHADOW_FACTOR_2D's own comment
// on why dynamic array indexing here is safe). No projection/uv math needed the way the 2D kinds
// need: the lookup direction IS worldPos - d.lightPos, and the value stored at that direction IS
// the real linear distance from the light to whatever ShadowMap::createPoint()'s distance-only
// Program saw there. Single-tap, no PCF yet (the 2D kind's 3x3 offset pattern doesn't translate
// directly to a cube's non-uniform texel spacing at the seams) - a starting point, not a final
// form. Requires SHADOW_UNIFORMS_MULTI already in scope.
inline constexpr const char* SHADOW_FACTOR_CUBE = R"GLSL(
float osgx_ShadowFactorCube(int slot, vec3 worldPos, vec3 N) {
	osgx_ShadowDataCube d = osgx_shadowDataCube[slot];
	vec3 offsetPos = worldPos + N * d.normalOffset;
	vec3 toFragment = offsetPos - d.lightPos;
	float dist = length(toFragment);
	float stored = texture(osgx_shadowMapsCube[slot], toFragment).r;

	return (dist - d.bias > stored) ? (1.0 - d.strength) : 1.0;
}
)GLSL";

// Hook::ShadowFactor's real override shader TEXT - osgx::ShadowSet::shader (built via
// resolveShaderLibs() in Shadow.cpp) is exactly this. Scans both small slot arrays for a
// casterIndex match and evaluates whichever kind it finds; a light with no matching slot in
// either array falls through to 1.0 (unshadowed), identical to Hook::ShadowFactor's own default
// (SHADOW_FACTOR_HOOK_NONE above) for that one light. Self-contained (own #version line); not
// itself in the "osgx::shadow" catalog (a caller swaps in osgx::ShadowSet::shader directly via
// Hook::ShadowFactor, never by name via #pragma), same reasoning as
// DIRECT_LIGHTING_HOOK_DEFAULT/SHADOW_FACTOR_HOOK_NONE.
inline constexpr const char* SHADOW_FACTOR_HOOK_MULTI = R"GLSL(
#version 460 core

#pragma osgx::shadow SHADOW_UNIFORMS_MULTI, SHADOW_FACTOR_2D, SHADOW_FACTOR_CUBE

float osgx_ShadowFactorForLight(int lightIndex, vec3 worldPos, vec3 N) {
	for(int slot = 0; slot < OSGX_MAX_SHADOWED_2D; slot++) {
		if(osgx_shadowData2D[slot].casterIndex == lightIndex) return osgx_ShadowFactor2D(slot, worldPos, N);
	}

	for(int slot = 0; slot < OSGX_MAX_SHADOWED_CUBE; slot++) {
		if(osgx_shadowDataCube[slot].casterIndex == lightIndex) return osgx_ShadowFactorCube(slot, worldPos, N);
	}

	return 1.0;
}
)GLSL";

// Aggregates however many osgx::ShadowMaps a scene has (any mix of directional/spot and point, up
// to MAX_SHADOWED_2D/MAX_SHADOWED_CUBE each) into the combined uniform arrays
// SHADOW_FACTOR_HOOK_MULTI reads, and owns that hook's shader object - the Hook::ShadowFactor
// override a caller passes to applyHooks()/PBRScene::Options::hooks/PBRLightingPass::Options::hooks
// in place of the slot's SHADOW_FACTOR_HOOK_NONE default. Replaces the old design's one
// `const ShadowMap*` field entirely: that design could shadow at most ONE light, ever (directional/
// spot and point were even mutually exclusive, since each needed the ENTIRE osgx_DirectLighting()
// swapped for a near-duplicate copy of the same per-light loop - see this file's own header
// comment). A ShadowSet with maps for two different osgx_lights[] indices shadows both at once.
struct ShadowSet {
	// The two sampler arrays - plain uniforms, since GLSL forbids opaque types inside a uniform
	// block (see SHADOW_UNIFORMS_MULTI's own comment on why `shadowData` below can't absorb these
	// too). apply() assigns each active slot's texture unit into these.
	osg::ref_ptr<osg::Uniform> shadowMaps2D;
	osg::ref_ptr<osg::Uniform> shadowMapsCube;

	// The matrix/bias/normalOffset/strength/casterIndex fields for every slot (both kinds), packed
	// into one std140 buffer bound at the "osgx::shadow" UBO slot - same raw-float-buffer technique
	// as LightSet's own "osgx::light" buffer (Light.hpp/Light.cpp), not a dozen separate
	// osg::Uniform arrays. `shadowData`'s exact float layout is Shadow.cpp's own private detail;
	// see SHADOW_UNIFORMS_MULTI's comment for the byte layout it must match. add() populates the
	// next free slot of the right kind; sync() re-reads every already-assigned slot's CURRENT
	// values (call after reposition()/repositionSpot()/repositionPoint() on any member map - same
	// place those are already called from an `if(changed)` ImGui block in every existing example,
	// not a per-frame unconditional call).
	osg::ref_ptr<osgx::FloatArray> shadowData;
	osg::ref_ptr<osg::UniformBufferBinding> shadowDataBinding;

	// SHADOW_FACTOR_HOOK_MULTI, ready to pass as the Hook::ShadowFactor override.
	osg::ref_ptr<osg::Shader> shader;

	// True once construct()ed - a default-constructed ShadowSet has none of the above and must not
	// be add()ed to or apply()'d.
	bool valid() const;

	// Builds an empty set (all casterIndex slots -1, "unused") ready for add(). `shader` is built
	// via osgx::cachedShader() (its text never varies across instances, so every ShadowSet shares
	// one compiled osg::Shader - see Shadow.cpp's own comment).
	static ShadowSet create();

	// Registers `map` in the next free slot of whichever array matches its kind (2D for a
	// create()/createSpot() map, cube for createPoint() - detected from `map.camera.valid()`,
	// matching ShadowMap::valid()'s own discriminant), writing its CURRENT uniform values
	// (shadowMatrix/bias/normalOffset/strength, and `map.casterIndex`'s value as that slot's
	// casterIndex) immediately. Throws std::out_of_range if that kind's array is already full
	// (MAX_SHADOWED_2D/MAX_SHADOWED_CUBE) or std::invalid_argument if `map` is itself invalid.
	// `map` must outlive every sync() call this ShadowSet makes afterward - it is not copied, only
	// its current uniform VALUES are, each time.
	void add(const ShadowMap& map);

	// Re-reads every already-add()ed slot's CURRENT uniform values from its own ShadowMap and
	// re-uploads them - call after reposition()/repositionSpot()/repositionPoint() on any member
	// map so a live-dragged light's shadow stays in sync (see `reposition()`'s own comment on
	// ShadowMap: cheap enough to call on every GUI-slider tick).
	void sync();

	// Binds each active 2D/cube texture to its own texture unit via
	// osgx::Library::instance().bindings() (one named slot per array element -
	// "osgx::shadowMap2D#0".."#1", "osgx::shadowMapCube#0".."#1" - so this never collides with any
	// other texture the rest of the pipeline reserves), points every slot beyond what was actually
	// add()ed at Bindings::unused()'s shared placeholder unit (never bound to a real texture -
	// nothing samples a dead slot - just a real number the allocator guarantees nothing else will
	// ever also be assigned), adds the two sampler uniforms, and resolves + attaches
	// `shadowDataBinding` at the "osgx::shadow" UBO slot. Call once after every add() this
	// ShadowSet will ever receive; sync() alone is enough after that for live updates.
	void apply(osg::StateSet* stateSet) const;

private:
	int _next2D = 0;
	int _nextCube = 0;
	// Which ShadowMap populated each slot, kept only so sync() has something to re-read from -
	// not exposed, and not copied (a ShadowSet is a live binding to specific ShadowMap objects,
	// not a value type).
	const ShadowMap* _maps2D[MAX_SHADOWED_2D] = {};
	const ShadowMap* _mapsCube[MAX_SHADOWED_CUBE] = {};
};

}
