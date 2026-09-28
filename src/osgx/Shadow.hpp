#pragma once

#include "CaptureCubeMap.hpp"
#include "Core.hpp"
#include "RTT.hpp"
#include "Shader.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Group>
#include <osg/Matrixd>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec3>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

namespace osgx {

// ================================================================================================
// Directional shadow mapping, shared by any osgx::LightSet-lit scene (nothing here is glTF/PBR-
// specific). Lives directly under `osgx::`, not its own namespace - it's not a separate opt-in
// subsystem (its own #include outside the umbrella, its own CMake link target) the way
// osgx::debug/imgui/platform/gltf/ktx2 are; see TODO.md's namespace-boundary decision. The
// `"osgx::shadow"` catalog name is the shader-lib registry's conventional tag, unrelated to the
// C++ namespace. Only ONE light
// - the key/directional light - is ever shadowed here; point/spot-light shadows need a cubemap
// and meaningfully different frustum math, and are a separate, later feature. This is the real
// osgx home for the shadow_cam/shadowFactor() pattern OpenSceneGraph.py/examples/pyosg-lighting/
// 08-shadows.py and 09-ibl.py both independently hand-rolled (and, in 08-shadows.py's case,
// duplicated a second time between the model and floor fragment shaders).
//
// World-space, not eye-space: osgx::PBRScene's direct-lighting call site (PBRScene.cpp's
// FULL_PBR_FRAGMENT_SHADER_SRC) already reconstructs a genuine world-space `worldPos` for
// osgx_DirectLighting() (via osg_ViewMatrixInverse), unlike the old hand-rolled examples, which
// shaded in eye space and had to compose `inverse(camView)` into their shadow matrix every frame
// to compensate. Working in world space here instead means `shadowMatrix` is just `lightProj *
// lightView` - no per-frame main-camera dependency at all - and only needs recomputing
// (updateMatrix()) if the light itself moves, which no pyosg-lighting example has ever done.
// ================================================================================================

struct ShadowMap {
	struct Options {
		int size = 1024;

		// Half-width, in world units, of the ORTHOGRAPHIC shadow frustum's box (both the X/Y extent
		// and the margin added to near/far - see `margin` below). A directional light's rays are
		// parallel by definition, so this - not a field-of-view angle - is what actually determines
		// coverage; a perspective frustum here would make the light behave like a nearby spotlight
		// whose rays diverge, which visibly disagrees with a direct-lighting term that (correctly)
		// treats every point in the scene as lit from the same direction. 0 (the default) derives the
		// extent from `sceneBoundRadius * margin`, matching every existing caller's coverage exactly;
		// set explicitly to cover more than the casting geometry itself - e.g. a floor/room that
		// needs to receive shadows well past the model's own bound (ported from
		// OpenSceneGraph.py's 11-sketchfab.py, which computed this by hand as
		// `max(bound_radius * margin, floor_size)` before this became a real option).
		float extent = 0.0f;

		// Multiplies sceneBoundRadius both when deriving a default `extent` above and when sizing
		// near/far planes. Ported directly from 09-ibl.py's own investigation: a shadow camera placed
		// at a FIXED distance from the scene puts near/far arbitrarily close together for a small
		// scene and arbitrarily far apart for a large one - Lantern (a ~15-unit-radius glTF model)
		// hit a ~2870:1 near:far ratio this way, collapsing shadow-map depth precision to nothing (the
		// depth comparison in osgx_ShadowFactor() never triggers - looks like "no shadow" but is
		// really "no usable depth precision"). Scaling by the scene's own bound keeps near:far bounded
		// to a healthy ratio regardless of scene scale, with no per-scene tuning.
		float margin = 1.3f;

		float bias = 0.005f;
		float strength = 0.7f; // 0 = shadows have no effect, 1 = fully black
	};

	// A directional (create()) or spot (createSpot()) shadow map: owns the PRE_RENDER depth-only
	// camera (`camera` - add it to the scene graph, e.g. as a sibling of whatever the shadowed
	// model's own parent is, exactly where the old hand-rolled examples added their own
	// `shadow_cam`) plus the uniforms DIRECT_LIGHTING_HOOK_SHADOWED reads every frame. Depth-only:
	// no dummy color attachment needed (the old hand-rolled Python examples worked around a
	// since-irrelevant pybind11 binding gap - osg::Camera::setDrawBuffer/setReadBuffer are ordinary
	// C++ calls here).
	osg::ref_ptr<osg::Camera> camera;
	osg::ref_ptr<osg::Texture2D> depthTexture;

	// world space -> light clip space. Set once by ShadowMap::create(); only needs
	// recomputing (updateMatrix()) if the light direction changes after creation.
	osg::ref_ptr<osg::Uniform> shadowMatrix;
	osg::ref_ptr<osg::Uniform> bias;
	osg::ref_ptr<osg::Uniform> strength;

	// Which osgx_lights[] index (osgx::LightSet) this shadow map is cast by/matched against --
	// DIRECT_LIGHTING_HOOK_SHADOWED multiplies exactly that light's contribution by
	// osgx_ShadowFactor(); every other light is unaffected. Defaults to 0 (the common "index 0 is
	// the key light" convention every existing pyosg-lighting example already follows).
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
	// `sceneBoundCenter` (`extent` per Options::extent), back along `lightDirection`,
	// toward `sceneBoundCenter`. `lightDirection` is the ray TRAVEL direction, matching
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
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		const Options& options
	);
	static ShadowMap create(
		const osg::Vec3& lightDirection,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius
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
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		const Options& options
	);
	void reposition(
		const osg::Vec3& lightDirection,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius
	);

	// A spot light's shadow map: a PERSPECTIVE depth-only camera at `position` looking along
	// `direction` (ray travel direction, as LightSet::setSpot()), its field of view covering
	// `outerConeAngle` (radians, half-angle, as LightSet::setSpot()). Near/far bracket the scene
	// bound (`sceneBoundRadius * options.margin`) as seen from the light; `options.extent` is not
	// used. Everything else - `camera`, `depthTexture`, the uniforms, DIRECT_LIGHTING_HOOK_SHADOWED
	// - is the same as create()'s. `bias` is compared in the map's non-linear depth, so a spot
	// map usually wants a smaller bias than a directional one.
	static ShadowMap createSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		const Options& options
	);
	static ShadowMap createSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius
	);

	// reposition()'s counterpart for a createSpot() map.
	void repositionSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		const Options& options
	);
	void repositionSpot(
		const osg::Vec3& position,
		const osg::Vec3& direction,
		float outerConeAngle,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius
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
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		int cubeSize,
		const Options& options
	);
	static ShadowMap createPoint(
		const osg::Vec3& position,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		int cubeSize=256
	);

	// reposition()/repositionSpot()'s counterpart for a createPoint() map - re-aims the six capture
	// cameras at a new position and refreshes their shared far plane/clear value for the new
	// distance to the scene (unlike the 2D map kinds, a point map's far plane has to track the
	// light directly - see this method's own .cpp comment).
	void repositionPoint(
		const osg::Vec3& position,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius,
		const Options& options
	);
	void repositionPoint(
		const osg::Vec3& position,
		const osg::Vec3& sceneBoundCenter,
		float sceneBoundRadius
	);
};

// GLSL uniform declarations osgx_ShadowFactor() (SHADOW_FACTOR below) and
// DIRECT_LIGHTING_HOOK_SHADOWED both assume are already in scope. `osgx_shadowMap` is
// intentionally left for the caller to bind to whatever texture unit it likes (no fixed
// convention imposed here) - see ShadowMap::depthTexture.
inline constexpr const char* SHADOW_UNIFORMS = R"GLSL(
uniform sampler2D osgx_shadowMap;
uniform mat4 osgx_shadowMatrix;
uniform float osgx_shadowBias;
uniform float osgx_shadowStrength;
uniform int osgx_shadowCasterIndex;
)GLSL";

// PCF 3x3 shadow test, WORLD-space - ported from 08-shadows.py/09-ibl.py's shadowFactor(),
// generalized from the eye-space `vPosition` those files fed it to a world-space `worldPos`
// instead (see the file-level comment above for why). Requires SHADOW_UNIFORMS already in scope.
inline constexpr const char* SHADOW_FACTOR = R"GLSL(
float osgx_ShadowFactor(vec3 worldPos) {
	vec4 sc = osgx_shadowMatrix * vec4(worldPos, 1.0);

	sc /= sc.w;

	vec3 uv = sc.xyz * 0.5 + 0.5;

	if(any(lessThan(uv, vec3(0.0))) || any(greaterThan(uv, vec3(1.0)))) return 1.0;

	vec2 sz = 1.0 / vec2(textureSize(osgx_shadowMap, 0));
	float shadow = 0.0;

	for(int x = -1; x <= 1; x++) {
		for(int y = -1; y <= 1; y++) {
			shadow += (
				uv.z - osgx_shadowBias > texture(osgx_shadowMap, uv.xy + vec2(x, y) * sz).r
			) ? 1.0 : 0.0;
		}
	}

	return mix(1.0, 1.0 - osgx_shadowStrength, shadow / 9.0);
}
)GLSL";

// Shadowed counterpart to osgx::DIRECT_LIGHTING_HOOK_DEFAULT (Light.hpp) - identical per-light
// loop (both share LIGHT_SAMPLE's osgx_SampleLight() dispatch), except the light at index
// `osgx_shadowCasterIndex` (default 0, the "index 0 is the key light" convention every existing
// pyosg-lighting example already follows) has its contribution multiplied by
// osgx_ShadowFactor(worldPos), computed once per fragment (not once per
// light - it only depends on position, not which light is being evaluated). A caller with a
// ShadowMap adds THIS shader object instead of DIRECT_LIGHTING_HOOK_DEFAULT - same hook-swap
// mechanism (see Light.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT contract comment),
// no other shader changes needed: both define osgx_DirectLighting() with the identical (N, V,
// worldPos, mat) signature DIRECT_LIGHTING_DECL forward-declares. Self-contained (own #version/PI/
// #pragma lines), so not spliced by name via #pragma osgx::shadow - deliberately NOT in
// the "osgx::shadow" catalog, same reasoning as DIRECT_LIGHTING_HOOK_DEFAULT itself.
inline constexpr const char* DIRECT_LIGHTING_HOOK_SHADOWED = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
#pragma osgx::light POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR
#pragma osgx::pbr DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE
#pragma osgx::shadow SHADOW_UNIFORMS, SHADOW_FACTOR

vec3 osgx_DirectLighting(vec3 N, vec3 V, vec3 worldPos, osgx_Material mat) {
	vec3 color = vec3(0.0);
	float shadow = osgx_ShadowFactor(worldPos);

	// Every slot, gated by its `enabled` flag (as DIRECT_LIGHTING_HOOK_DEFAULT in Light.hpp).
	for(int i = 0; i < OSGX_MAX_LIGHTS; i++) {
		osgx_Light light = osgx_lights[i];

		if(light.enabled == 0) continue;

		osgx_LightSample s = osgx_SampleLight(light, worldPos);
		vec3 contribution;

		if(s.sourceRadius > 0.0) {
			contribution = osgx_DirectLightSphere(N, V, s.L, s.toLight, s.radiance, mat, s.sourceRadius);
		}

		else {
			contribution = osgx_DirectLight(N, V, s.L, s.radiance, mat);
		}

		color += contribution * ((i == osgx_shadowCasterIndex) ? shadow : 1.0);
	}

	return color;
}
)GLSL";

// GLSL uniform declarations osgx_ShadowFactorPoint() (SHADOW_FACTOR_POINT below) and
// DIRECT_LIGHTING_HOOK_SHADOWED_POINT both assume are already in scope - the point-light
// counterpart to SHADOW_UNIFORMS above. `osgx_shadowLightPos` is world-space, needed here (unlike
// the 2D map kinds) since there's no single shadowMatrix to fold the light's position into - the
// cube lookup direction and the compared distance both have to be rebuilt from it per fragment.
inline constexpr const char* SHADOW_UNIFORMS_POINT = R"GLSL(
uniform samplerCube osgx_shadowCubeMap;
uniform vec3 osgx_shadowLightPos;
uniform float osgx_shadowBias;
uniform float osgx_shadowStrength;
uniform int osgx_shadowCasterIndex;
)GLSL";

// Cube-map distance-compare shadow test - the point-light counterpart to SHADOW_FACTOR. No
// projection/uv math needed the way the 2D map kinds need: the lookup direction IS
// worldPos - osgx_shadowLightPos, and the value stored at that direction IS the real linear
// distance from the light to whatever ShadowMap::createPoint()'s distance-only Program saw there
// (see its own comment in Shadow.cpp for why no separate light-position uniform was needed to
// WRITE that cube in the first place). Single-tap, no PCF yet (SHADOW_FACTOR's 3x3 doesn't
// translate directly to a cube's non-uniform texel spacing) - a starting point, not a final form.
// Requires SHADOW_UNIFORMS_POINT already in scope.
inline constexpr const char* SHADOW_FACTOR_POINT = R"GLSL(
float osgx_ShadowFactorPoint(vec3 worldPos) {
	vec3 toFragment = worldPos - osgx_shadowLightPos;
	float dist = length(toFragment);
	float stored = texture(osgx_shadowCubeMap, toFragment).r;

	return (dist - osgx_shadowBias > stored) ? (1.0 - osgx_shadowStrength) : 1.0;
}
)GLSL";

// DIRECT_LIGHTING_HOOK_SHADOWED's point-light counterpart - identical apart from which shadow
// catalog entries it splices and which shadow-factor function it calls. A sibling hook rather than
// a branch inside DIRECT_LIGHTING_HOOK_SHADOWED itself, matching the existing "one shadowed light,
// one hook" contract (see this file's own header comment on the still-open casterIndex/
// one-shadowed-light-per-hook limitation) - a caller shadowing a point light adds THIS hook object
// instead, same hook-swap mechanism as the other two.
inline constexpr const char* DIRECT_LIGHTING_HOOK_SHADOWED_POINT = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
#pragma osgx::light POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR
#pragma osgx::pbr DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE
#pragma osgx::shadow SHADOW_UNIFORMS_POINT, SHADOW_FACTOR_POINT

vec3 osgx_DirectLighting(vec3 N, vec3 V, vec3 worldPos, osgx_Material mat) {
	vec3 color = vec3(0.0);
	float shadow = osgx_ShadowFactorPoint(worldPos);

	for(int i = 0; i < OSGX_MAX_LIGHTS; i++) {
		osgx_Light light = osgx_lights[i];

		if(light.enabled == 0) continue;

		osgx_LightSample s = osgx_SampleLight(light, worldPos);
		vec3 contribution;

		if(s.sourceRadius > 0.0) {
			contribution = osgx_DirectLightSphere(N, V, s.L, s.toLight, s.radiance, mat, s.sourceRadius);
		}

		else {
			contribution = osgx_DirectLight(N, V, s.L, s.radiance, mat);
		}

		color += contribution * ((i == osgx_shadowCasterIndex) ? shadow : 1.0);
	}

	return color;
}
)GLSL";

}
