#pragma once

#include "Array.hpp"
#include "Shader.hpp"
#include "Core.hpp"
#include "Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/NodeCallback>
#include <osg/NodeVisitor>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Vec4>

OSGX_ENABLE_WARNINGS

#include <cmath>
#include <string>
#include <vector>

namespace osg {
	class ShaderStorageBufferBinding;
}

namespace osgx {

// ================================================================================================
// Direct lights
//
// Split out of PBR.hpp 2026-09-23: LightSet/LightType/
// OrbitLightRig and the "direct" light math (per-light radiance + the DIRECT_LIGHT/DIRECT_LIGHTING_*
// hook family) were always a separate domain from Material/BRDF - they were bundled into one file
// by history (the C++ side of a light happened to be defined next to PBR.hpp's shader catalog when
// that file was created), not because either needs the other in C++. This header has ZERO C++
// dependency on PBR.hpp: the GLSL entries below reference `osgx_Material` as a type name only,
// resolved by whichever caller's own #pragma line lists MATERIAL_STRUCT (from "osgx::pbr") ahead of
// this header's entries (from "osgx::light") - resolveShaderLibs() links by NAME at shader-build
// time, not by C++ include, so the two catalogs compose without either header including the other.
//
// Vocabulary: our umbrella term for Point/Directional/Spot/Sphere, together, is "direct" - not
// glTF's "punctual". glTF's KHR_lights_punctual is explicitly size-ZERO; our Sphere light has a
// nonzero sourceRadius, so calling it punctual would be wrong, not just informal. "Direct" answers
// the right question instead: is this light's contribution computed explicitly per-light (this
// file), as opposed to baked/prefiltered image-based lighting (osgx::Environment, Environment.hpp)?
// ================================================================================================

// Compile-time bound for LIGHT_UNIFORMS' GLSL buffer array declaration below - kept as a real C++
// constant (not just a literal baked into the GLSL string) so callers can size a LightSet without
// hardcoding a number that has to stay in sync by hand. If this changes, OSGX_MAX_LIGHTS inside
// LIGHT_UNIFORMS must change with it. 6 covers a small handful of torchlights in one room without
// over-provisioning the per-fragment loop.
inline constexpr int MAX_LIGHTS = 6;

// Size, in 4-byte floats, of one packed `osgx_Light` struct in LIGHT_UNIFORMS' std430 buffer below
// (16 floats = 64 bytes) - the C++-side stride LightSet's setters/getters index into `lights`
// with. Must match the GLSL struct exactly; see LIGHT_UNIFORMS' own layout comment.
inline constexpr std::size_t LIGHT_STRUCT_FLOATS = 16;

// Per-light Cook-Torrance specular contribution (direct lighting), already multiplied by NdotL --
// caller multiplies by the light's own radiance (color * intensity/distance^2 or similar) and
// accumulates. Requires D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK (osgx::pbr) already in scope.
//
// Roughness is floored at 0.045 (Filament's MIN_PERCEPTUAL_ROUGHNESS) before evaluating D: for an
// ideal point light, osgx_D_GGX() at the highlight's exact center is 1/(PI*roughness^4), which is
// 0/0 = NaN at roughness 0 and ~3e7 at 0.01. Roughness read back from a low-precision G-buffer can
// quantize to exactly 0. IBL is unaffected: it samples a prefiltered map, not an analytic lobe.
inline constexpr const char* DIRECT_SPECULAR = R"GLSL(
vec3 osgx_DirectSpecular(vec3 N, vec3 V, vec3 L, float NdotV, float roughness, vec3 F0) {
	float NdotL = max(dot(N, L), 0.0);

	roughness = max(roughness, 0.045);

	if(NdotL <= 0.0) return vec3(0.0);

	vec3 H = normalize(L + V);
	float NdotH = max(dot(N, H), 0.0);
	float HdotV = max(dot(H, V), 0.0);

	float D = osgx_D_GGX(NdotH, roughness);
	float G = osgx_G_Smith(NdotV, NdotL, roughness);
	vec3 F = osgx_F_Schlick(HdotV, F0);

	return (D * G * F * NdotL) / max(4.0 * NdotV * NdotL, 0.0001);
}
)GLSL";

// Lambertian direct-light diffuse term, kept a companion to DIRECT_SPECULAR above rather than
// folded into it - callers that only need specular can still pull just that one, matching the
// "atomic snippet" contract everywhere else in this file. Shares DIRECT_SPECULAR's own
// F_SCHLICK-based kD split so the two stay energy-consistent when combined (see DIRECT_LIGHT
// below). Requires F_SCHLICK (osgx::pbr) already in scope, and `const float PI` in the consuming
// shader (see PBR.hpp's file-level contract note).
inline constexpr const char* DIRECT_DIFFUSE = R"GLSL(
vec3 osgx_DirectDiffuse(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, vec3 F0) {
	float NdotL = max(dot(N, L), 0.0);

	if(NdotL <= 0.0) return vec3(0.0);

	vec3 H = normalize(L + V);
	float HdotV = max(dot(H, V), 0.0);
	vec3 F = osgx_F_Schlick(HdotV, F0);
	vec3 kD = (1.0 - F) * (1.0 - metallic);

	return kD * albedo / PI * NdotL;
}
)GLSL";

// Point-light radiance: inverse-square falloff, no artificial radius cutoff, plus the resulting
// light direction `L`, both needed by DIRECT_LIGHT below. `posIntensity` is world-space position
// in .xyz and intensity in .w - the exact packing osgx::OrbitLightRig writes via
// LightSet::setPosition() into each osgx_Light's own posIntensity field (LIGHT_UNIFORMS' buffer
// struct below), so a caller wiring a static (e.g. torch-style) light just calls
// LightSet::setPoint() once instead of installing a NodeCallback.
inline constexpr const char* POINT_LIGHT_RADIANCE = R"GLSL(
vec3 osgx_PointLightRadiance(vec4 posIntensity, vec3 color, vec3 worldPos, out vec3 L) {
	vec3 toLight = posIntensity.xyz - worldPos;
	float dist2 = max(dot(toLight, toLight), 1e-4);

	L = toLight * inversesqrt(dist2);

	return color * posIntensity.w / dist2;
}
)GLSL";

// Declarations shared by every consumer of DIRECT_LIGHT/POINT_LIGHT_RADIANCE below - one
// contract, so a caller can populate one osgx::LightSet on an ancestor StateSet and have it
// inherited by every lit subgraph (dice, backdrop, whatever else) instead of wiring the same
// uniforms into each shader by hand. OSGX_MAX_LIGHTS is a compile-time array bound, not a runtime
// one - osgx_lightCount (set at runtime, <= OSGX_MAX_LIGHTS) is what actually gates the loop a
// caller (or DIRECT_LIGHTING_HOOK_DEFAULT's osgx_DirectLighting) writes over osgx_lights.
//
// A single std430 buffer struct array replaces what used to be seven parallel flat uniform arrays
// (lightPosIntensity/lightColor/lightType/lightDir/lightSpotAngles/lightSourceRadius plus
// lightCount) - modeled on gltf::detail::Skin's paletteMatrices buffer (a small, runtime-variable-
// count array of structs), not Material.cpp's fixed-size, one-per-draw
// read-only data - the right shape for a single material, not an array of lights). std430 (not
// std140, which only applies to `uniform` blocks) is what actually buys the tighter packing here
// - no forced 16-byte rounding on scalar/vec2 array elements. Every uniform/block name below
// carries the `osgx_` prefix (2026-08-16 rename) to avoid collision with an unrelated consumer
// shader's own similarly-named uniforms, matching the rest of this catalog (osgx_Material,
// osgx_DirectLight, etc.).
//
// Packed layout of one osgx_Light (std430; 16 floats / 64 bytes - must match
// LIGHT_STRUCT_FLOATS and the float offsets LightSet's setters/getters use in Light.cpp):
//   vec4  posIntensity   offset  0  (xyz = world-space position, w = intensity)
//   vec3  color          offset 16
//   int   type           offset 28  (OSGX_LIGHT_TYPE_* below)
//   vec3  dir            offset 32  (ray travel direction, KHR_lights_punctual convention)
//   float sourceRadius   offset 44  (0 = ideal point/spot; >0 = "sphere" specular widening)
//   vec2  spotAngles     offset 48  (cos(inner), cos(outer))
//   int   enabled        offset 56  (0 = off; non-zero = contributes light)
//   float _pad0          offset 60  (rounds the struct to a multiple of 16)
//
// type/dir/sourceRadius/spotAngles are additive - a caller that only ever calls setPoint() with
// sourceRadius=0 gets exactly today's point-light behavior. A per-light `type` (LightType in C++
// below) picks the radiance function (POINT_LIGHT_RADIANCE/DIRECTIONAL_LIGHT_RADIANCE/
// SPOT_LIGHT_RADIANCE); sourceRadius is deliberately NOT a fourth "sphere" type - it is a
// physical-size knob on a point or spot light, matching how a sphere light actually differs from
// a point light: same inverse-square falloff, only the specular highlight's shape changes (see
// DIRECT_LIGHT_SPHERE below). This is unrelated to POINT_LIGHT_RADIANCE's own "no artificial
// radius cutoff" decision (about attenuation distance); this is about physical light size.
inline constexpr const char* LIGHT_UNIFORMS = R"GLSL(
#define OSGX_MAX_LIGHTS 6
#define OSGX_LIGHT_TYPE_POINT 0
#define OSGX_LIGHT_TYPE_DIRECTIONAL 1
#define OSGX_LIGHT_TYPE_SPOT 2

struct osgx_Light {
	vec4 posIntensity;
	vec3 color;
	int type;
	vec3 dir;
	float sourceRadius;
	vec2 spotAngles;
	int enabled;
	float _pad0;
};

layout(std430, binding = @osgx::light@) readonly buffer osgx_LightBuffer {
	osgx_Light osgx_lights[OSGX_MAX_LIGHTS];
};

uniform int osgx_lightCount;
)GLSL";

// Combines osgx_DirectDiffuse + osgx_DirectSpecular into one per-light contribution against an
// osgx_Material (MATERIAL_STRUCT, "osgx::pbr") - the "modular hook" PBRIBLScene::create()'s own
// comment has been waiting on: a caller loops `osgx_lightCount` times, calling
// osgx_PointLightRadiance for L/radiance then this for the shaded result, and accumulates (or just
// calls osgx_DirectLighting(), see DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT below, which
// already does exactly that). Requires D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK/MATERIAL_STRUCT
// (osgx::pbr) and DIRECT_SPECULAR/DIRECT_DIFFUSE (this file) already in scope.
inline constexpr const char* DIRECT_LIGHT = R"GLSL(
vec3 osgx_DirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance, osgx_Material mat) {
	float NdotV = max(dot(N, V), 0.0);
	vec3 diffuse = osgx_DirectDiffuse(N, V, L, mat.albedo, mat.metallic, mat.F0);
	vec3 specular = osgx_DirectSpecular(N, V, L, NdotV, mat.roughness, mat.F0);

	return (diffuse + specular) * radiance * mat.ao;
}
)GLSL";

// Directional-light radiance: no position, no falloff - a directional light is already parallel
// rays of constant irradiance (the sun, at scene scale). `direction` is the ray travel direction
// (matching the glTF/KHR_lights_punctual convention a caller would eventually import), so the
// direction TO the light is its negation. Ported from
// OpenSceneGraph.py/examples/pyosg-lighting/99-repl.py's `L = normalize(mat3(osg_ViewMatrix) *
// -directionalDir)` (that file's own validated REPL prototype of this exact term).
inline constexpr const char* DIRECTIONAL_LIGHT_RADIANCE = R"GLSL(
vec3 osgx_DirectionalLightRadiance(vec3 direction, vec3 color, float intensity, out vec3 L) {
	L = -normalize(direction);

	return color * intensity;
}
)GLSL";

// Spot-light radiance: point-light falloff (POINT_LIGHT_RADIANCE) times a cone attenuation term.
// `coneAngles` is (cos(innerConeAngle), cos(outerConeAngle)) - pre-cosined so this stays a single
// dot/smoothstep per fragment instead of an acos. Ported from 99-repl.py's spot block
// (`smoothstep(spotOuterCos, spotInnerCos, cone)`). Requires POINT_LIGHT_RADIANCE already in scope.
inline constexpr const char* SPOT_LIGHT_RADIANCE = R"GLSL(
vec3 osgx_SpotLightRadiance(
	vec4 posIntensity, vec3 color, vec3 direction, vec2 coneAngles, vec3 worldPos, out vec3 L
) {
	vec3 radiance = osgx_PointLightRadiance(posIntensity, color, worldPos, L);
	float cone = dot(-L, normalize(direction));
	float atten = smoothstep(coneAngles.y, coneAngles.x, cone);

	return radiance * atten;
}
)GLSL";

// Material-free evaluation of one osgx_Light at a shading point: the per-type dispatch (directional/
// spot/point) that picks the right *_RADIANCE function, returning everything a shading model needs
// about the incoming light and nothing about how a surface responds to it. This is the seam between
// "light" and "material": osgx_DirectLighting() (DIRECT_LIGHTING_HOOK_DEFAULT below) consumes it for
// PBR, and a Lambert/toon/NPR shader consumes it directly with no osgx_Material in scope at all.
//
// `toLight` is the UNNORMALIZED light center minus `worldPos` (zero for a directional light), and
// `sourceRadius` is zeroed for a directional light - together they're what a sphere-aware specular
// term (DIRECT_LIGHT_SPHERE) needs; a diffuse-only consumer can ignore both. Requires
// LIGHT_UNIFORMS, POINT_LIGHT_RADIANCE, DIRECTIONAL_LIGHT_RADIANCE, and SPOT_LIGHT_RADIANCE already
// in scope. Does not check `light.enabled` - the caller's loop does, so a caller can still evaluate
// a disabled light on purpose (e.g. a gizmo/debug view).
inline constexpr const char* LIGHT_SAMPLE = R"GLSL(
struct osgx_LightSample {
	vec3 L;
	vec3 radiance;
	vec3 toLight;
	float sourceRadius;
};

osgx_LightSample osgx_SampleLight(osgx_Light light, vec3 worldPos) {
	osgx_LightSample s;

	s.toLight = vec3(0.0);
	s.sourceRadius = 0.0;

	if(light.type == OSGX_LIGHT_TYPE_DIRECTIONAL) {
		s.radiance = osgx_DirectionalLightRadiance(light.dir, light.color, light.posIntensity.w, s.L);

		return s;
	}

	if(light.type == OSGX_LIGHT_TYPE_SPOT) {
		s.radiance = osgx_SpotLightRadiance(
			light.posIntensity, light.color, light.dir, light.spotAngles, worldPos, s.L
		);
	}

	else {
		s.radiance = osgx_PointLightRadiance(light.posIntensity, light.color, worldPos, s.L);
	}

	s.toLight = light.posIntensity.xyz - worldPos;
	s.sourceRadius = light.sourceRadius;

	return s;
}
)GLSL";

// Sphere-light "representative point" trick (Karis, "Real Shading in Unreal Engine 4", 2013):
// bends the direction used for the SPECULAR term toward the closest point on the light's physical
// sphere to the ideal mirror-reflection ray, instead of always pointing at its center - this is
// what actually makes a highlight bigger/softer as the light's physical size grows (diffuse has
// no equivalent "highlight shape" to distort, so it keeps using the true light direction; see
// DIRECT_LIGHT_SPHERE below). `toLightCenter` is UNNORMALIZED (light center minus shading point);
// `R` is the normalized reflection vector. Ported verbatim from 99-repl.py's `sphereLightDir()`,
// independently corroborated by the committed
// OpenSceneGraph.py/examples/pyosg-polyhaven.py:116-123's identical formula.
inline constexpr const char* SPHERE_LIGHT_SPECULAR = R"GLSL(
vec3 osgx_SphereLightDir(vec3 toLightCenter, vec3 R, float sourceRadius) {
	vec3 centerToRay = dot(toLightCenter, R) * R - toLightCenter;
	vec3 closestPoint = toLightCenter + centerToRay * clamp(
		sourceRadius / max(length(centerToRay), 0.0001), 0.0, 1.0
	);

	return normalize(closestPoint);
}
)GLSL";

// Sphere-aware counterpart to DIRECT_LIGHT above, for a point or spot light with a non-zero
// physical `sourceRadius`. Diffuse is unchanged (osgx_DirectDiffuse against the true `L`);
// specular is re-evaluated against the representative-point direction (osgx_SphereLightDir) with
// roughness widened by the light's angular size (`alphaPrime`, ported from 99-repl.py's
// `evalSpherePoint()`) so a large/close source reads as a genuinely bigger, softer highlight
// rather than the same small one just brighter. Reuses DIRECT_DIFFUSE/DIRECT_SPECULAR unchanged --
// no new BRDF math, just a second osgx_DirectSpecular call site with a different L/roughness.
// Requires DIRECT_DIFFUSE, DIRECT_SPECULAR, and SPHERE_LIGHT_SPECULAR already in scope.
inline constexpr const char* DIRECT_LIGHT_SPHERE = R"GLSL(
// A plain clamp(x, 0.0, 1.0) here has a hard derivative jump exactly at x==1.0 - invisible for a
// single shaded point, but on a surface close enough to a large-sourceRadius light for x to exceed
// 1.0 on PART of the surface and not another, that jump shows up as a real, visible ring (confirmed
// live 2026-08-16: a cube face close to a sourceRadius=2 light showed a sharp arc separating a
// alphaPrime==1.0-saturated region from an unsaturated one). x is never negative here (alpha and
// sourceRadius/(2*dist) are both >= 0), so only the upper ceiling needs softening. Identity below
// (1.0 - softness), so every already-verified non-saturating case (small/no sourceRadius, or a
// light far enough that alphaPrime never approaches 1.0) is completely unaffected - only the
// approach to the ceiling itself eases smoothly instead of cutting off sharply.
float osgx_SoftCeiling(float x, float softness) {
	float edge = 1.0 - softness;

	if(x <= edge) return x;

	return mix(x, 1.0, smoothstep(edge, 1.0 + softness, x));
}

vec3 osgx_DirectLightSphere(
	vec3 N, vec3 V, vec3 L, vec3 toLightCenter, vec3 radiance, osgx_Material mat, float sourceRadius
) {
	float NdotV = max(dot(N, V), 0.0);
	vec3 diffuse = osgx_DirectDiffuse(N, V, L, mat.albedo, mat.metallic, mat.F0);

	vec3 R = reflect(-V, N);
	vec3 Lspec = osgx_SphereLightDir(toLightCenter, R, sourceRadius);
	float dist = length(toLightCenter);
	float alpha = mat.roughness * mat.roughness;
	float alphaPrime = osgx_SoftCeiling(alpha + sourceRadius / (2.0 * max(dist, 0.0001)), 0.2);
	float roughnessPrime = sqrt(alphaPrime);
	vec3 specular = osgx_DirectSpecular(N, V, Lspec, NdotV, roughnessPrime, mat.F0);

	return (diffuse + specular) * radiance * mat.ao;
}
)GLSL";

// osgx_DirectLighting() CONTRACT - the per-light dispatch loop above (LIGHT_UNIFORMS' osgx_lightCount/
// osgx_lights buffer array) factored out behind a single function boundary, instead of every consumer
// hand-copying it into its own main() (PBRIBL.cpp's FULL_PBR_FRAGMENT_SHADER_SRC and
// OpenSceneGraph.py's pyosg_dice.py both did exactly that, and the latter has already drifted out
// of sync - see osgx TODO.md). Follows the separate-compiled-shader-object "hook" pattern osgSlug
// already uses to good effect (~/dev/osgSlug/src/Atlas.shaders.cpp's SHADER_NOOP_*_HOOK/HookList,
// Atlas.cpp's createDefaultStateSet()): a consumer's OWN fragment shader only needs
// DIRECT_LIGHTING_DECL spliced in (list MATERIAL_STRUCT earlier via #pragma osgx::pbr - this is a
// bare forward declaration, osgx_Material must already be a known type) plus a call site
// (`color += osgx_DirectLighting(N, V, worldPos, mat);`); it never touches osgx_lightCount/osgx_lights/
// DIRECT_LIGHT/DIRECT_LIGHT_SPHERE/etc. directly, and so can never drift out of sync with them the
// way pyosg_dice.py's hand-copied loop did. The DEFINITION lives in DIRECT_LIGHTING_HOOK_DEFAULT
// below, a fully self-contained, SEPARATELY compiled osg::Shader object added alongside the
// consumer's own - GLSL's ordinary cross-shader-object linking resolves the call at Program-link
// time, exactly like osgSlug's SHADER_VERT calling osgSlug_Vertex(data) defined in a separate hook
// shader object. A caller that genuinely needs different direct-light shading (not just different
// material response, which osgx_Material/MATERIAL_STRUCT already covers) supplies its own shader
// object defining osgx_DirectLighting() instead of adding DIRECT_LIGHTING_HOOK_DEFAULT - same override
// mechanism as osgSlug's HookList, minus the C++-side bookkeeping (a HookList-style helper plus the
// Python binding are a deliberate follow-up, not done in this pass - see TODO.md).
inline constexpr const char* DIRECT_LIGHTING_DECL = R"GLSL(
vec3 osgx_DirectLighting(vec3 N, vec3 V, vec3 worldPos, osgx_Material mat);
)GLSL";

// Self-contained - carries its own #version/PI/#pragma lines so it compiles as a standalone
// osg::Shader object regardless of what the consumer's own fragment shader happens to have in
// scope. Add via:
//   program->addShader(new osg::Shader(
//     osg::Shader::FRAGMENT, osgx::resolveShaderLibs(osgx::DIRECT_LIGHTING_HOOK_DEFAULT)
//   ));
// as an EXTRA shader object on the same Program that already has the consumer's own fragment
// shader (which only needs DIRECT_LIGHTING_DECL + a call site, see above) - not spliced by name via
// #pragma, so it is deliberately NOT in registerLightShaderLibs()'s catalog. Pulls MATERIAL_STRUCT
// and the pure BRDF snippets from "osgx::pbr" and the rest of its own dependencies from
// "osgx::light" - two separate pragma lines, since resolveShaderLibs() resolves each line against
// whichever registered namespace it names, independent of the others.
inline constexpr const char* DIRECT_LIGHTING_HOOK_DEFAULT = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
#pragma osgx::light DIRECT_SPECULAR, DIRECT_DIFFUSE, POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECT_LIGHT, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR, DIRECT_LIGHT_SPHERE

vec3 osgx_DirectLighting(vec3 N, vec3 V, vec3 worldPos, osgx_Material mat) {
	vec3 color = vec3(0.0);

	// Loop OSGX_MAX_LIGHTS (a compile-time constant), gated solely by each light's own `enabled`
	// flag - NOT osgx_lightCount (an SSBO-adjacent uniform LightSet::apply() used to push via
	// osg::State::applyShaderCompositionUniform()/direct getLastAppliedProgramObject() push,
	// see LightSet::apply()'s own history comment). Both were confirmed unreliable whenever a
	// DIFFERENT Program elsewhere in the same frame (a sibling subgraph, even) uses
	// StateAttribute::OVERRIDE - e.g. osgx::gltf::pbribl::PBRIBLScene::create()'s own Program
	// attachment - silently zeroing direct lighting for every OTHER Program sharing this
	// LightSet. `enabled` travels on the SAME SSBO binding the light data itself does
	// (state.applyAttribute(), never State's separate/unreliable uniform-push machinery), so it
	// has none of that fragility.
	for(int i = 0; i < OSGX_MAX_LIGHTS; i++) {
		osgx_Light light = osgx_lights[i];

		if(light.enabled == 0) continue;

		// LIGHT_SAMPLE zeroes sourceRadius for directional lights, so no separate type check here.
		osgx_LightSample s = osgx_SampleLight(light, worldPos);

		if(s.sourceRadius > 0.0) {
			color += osgx_DirectLightSphere(N, V, s.L, s.toLight, s.radiance, mat, s.sourceRadius);
		}

		else {
			color += osgx_DirectLight(N, V, s.L, s.radiance, mat);
		}
	}

	return color;
}
)GLSL";

// Selects the radiance function LIGHT_UNIFORMS' `osgx_lights[i].type` picks in the fragment
// shader loop (OSGX_LIGHT_TYPE_* above) - kept as a real C++ enum, not just the raw int a caller
// would otherwise have to remember, same reasoning as MAX_LIGHTS. Deliberately no `Sphere` member:
// a sphere light is a Point or Spot light with a non-zero LightSet::setPoint/setSpot
// `sourceRadius`, not a fourth branch (see LIGHT_UNIFORMS' own comment for why).
enum class LightType: int {
	Point = 0,
	Directional = 1,
	Spot = 2
};

// The static-position counterpart to OrbitLightRig below: one StateAttribute that owns the
// LIGHT_UNIFORMS SSBO AND its osgx_lightCount uniform, instead of asking every caller to keep a
// buffer binding and a separate StateSet uniform in sync. apply() binds the SSBO and sends the
// count through osg::State::applyShaderCompositionUniform(), OSG's own StateAttribute-to-uniform
// bridge (used by osg::ShaderAttribute, osg::TexEnv, and osg::TexGen). A caller wiring a fixed rig
// (wall torches, sconces, a sun, a flashlight) uses this directly; OrbitLightRig can still animate
// a light's position on top of the SAME LightSet for the subset of lights that should orbit - the
// two are complementary, not alternatives.
//
// CAPABILITY/member 1 deliberately matches its pre-split value (osgx::Material, PBR.hpp, uses
// member 0) - see Material's class comment for why the full (Type, member) pair is the State cache
// key, and why this number must not drift.
struct LightSet: public osg::StateAttribute {
	static constexpr Type LIGHT_SET_TYPE = CAPABILITY;
	static constexpr unsigned int LIGHT_SET_MEMBER = 1;

	LightSet();
	LightSet(const LightSet& lights, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);

	OSGX_META_StateAttribute(osgx, LightSet, LIGHT_SET_TYPE)

	unsigned int getMember() const override { return LIGHT_SET_MEMBER; }
	int compare(const osg::StateAttribute& sa) const override;
	void apply(osg::State& state) const override;

	// Whether this LightSet still has its buffer binding and count uniform. A normal constructed
	// LightSet is valid immediately and can be attached directly with setAttributeAndModes().
	bool valid() const;

	// `const` - these mutate the buffer/uniform this LightSet owns, not its object identity. Lets
	// an osg::ref_ptr<LightSet> captured by value into a `const`-qualified lambda (e.g. an ordinary,
	// non-`mutable` event-handler callback) still call these directly.
	// Every index must be less than MAX_LIGHTS; otherwise the setter/getter throws std::out_of_range.
	//
	// `sourceRadius` > 0 switches this light's specular term to the representative-point path
	// (DIRECT_LIGHT_SPHERE) - this is what makes it read as a "sphere" light instead of an ideal
	// point light; everything else about it (falloff, diffuse) is unchanged.
	void setPoint(
		std::size_t index,
		const osg::Vec3& position,
		const osg::Vec3& color,
		float intensity,
		float sourceRadius=0.0f
	) const;

	// `direction` is the ray travel direction (matching KHR_lights_punctual, for eventual loader
	// compatibility) - e.g. (0, 0, -1) for a light shining straight down in a Z-up world.
	void setDirectional(
		std::size_t index,
		const osg::Vec3& direction,
		const osg::Vec3& color,
		float intensity
	) const;

	// `innerConeAngle`/`outerConeAngle` are in radians, matching KHR_lights_punctual; converted to
	// the shader's pre-cosined spotAngles here so the fragment shader never calls acos/cos.
	void setSpot(
		std::size_t index,
		const osg::Vec3& position,
		const osg::Vec3& direction,
		const osg::Vec3& color,
		float intensity,
		float innerConeAngle,
		float outerConeAngle,
		float sourceRadius=0.0f
	) const;

	// `count` must be in [0, MAX_LIGHTS]; otherwise throws std::out_of_range.
	void setCount(std::size_t count) const;

	// Enables or disables an already configured light without changing its data. Typed setup
	// methods enable their slot, so the established setCount()+setPoint()/setDirectional()/setSpot()
	// workflow continues to activate lights as before.
	void setEnabled(std::size_t index, bool enabled) const;

	// Sets ONLY a light's posIntensity field, leaving color/type/dir/spotAngles/sourceRadius
	// untouched - the one primitive OrbitLightRig below needs to animate a light already
	// configured via setPoint/setSpot, without re-specifying everything else every frame.
	void setPosition(std::size_t index, const osg::Vec3& position, float intensity) const;

	// Read accessors - e.g. osgx::LightMarkers/LightGizmos read back a live LightSet's per-light
	// state to place gizmo geometry; individual fields are no longer separately retrievable
	// osg::Uniforms the way the old parallel-array design allowed.
	int getCount() const;
	osg::Vec4 getPosIntensity(std::size_t index) const;
	osg::Vec3 getColor(std::size_t index) const;
	LightType getType(std::size_t index) const;
	bool getEnabled(std::size_t index) const;
	osg::Vec3 getDirection(std::size_t index) const;
	osg::Vec2 getSpotAngles(std::size_t index) const;
	float getSourceRadius(std::size_t index) const;

	protected:
	virtual ~LightSet();

	private:
	// Backing store for every light's packed osgx_Light struct (MAX_LIGHTS * LIGHT_STRUCT_FLOATS
	// floats, std430 layout - see LIGHT_UNIFORMS' struct comment), bound through _binding at the
	// "osgx::light" slot. It stays private so it cannot be replaced independently of that binding.
		osg::ref_ptr<osgx::FloatArray> _lights;
		osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
		mutable std::once_flag _bindingResolved;
		osg::ref_ptr<osg::Uniform> _lightCount;

		// Validates the LightSet and `index`, then returns the float offset of that light's struct
		// within _lights (the base every osgx_Light field offset is added to).
		std::size_t lightOffset(std::size_t index) const;
		float* lightFloats(std::size_t index, std::size_t offset) const;
};

// Animates a handful of point lights orbiting a center point, writing world-space position+
// intensity into an existing LightSet's posIntensity field (via LightSet::setPosition) every
// update traversal - the motion is what confirms N/V/specular are wired correctly rather than
// just a static flat-shaded color. Install as the update callback on whichever node the lit shape
// hangs from; `lights` must already be an attached LightSet with at least
// orbits.size() lights configured via setPoint/setSpot (for their color/type/etc. - this callback
// only ever touches position/intensity).
//
// Reusable across any PBR-lit scene - configure `center`/`orbits`/`intensity` per use
// instead of copying this callback into each consumer.
struct OrbitLightRig: public osg::NodeCallback {
	struct Orbit {
		float radius, height, speed, phase, intensity;
	};

	osg::ref_ptr<LightSet> lights;
	osg::Vec3 center{0.0f, 0.0f, 0.0f};
	float intensity = 1.0f; // global scale, e.g. a --light-intensity CLI flag

	// Default rig: (orbit radius, height above center, angular speed, phase, per-orbit intensity).
	// Matches the original osgslug-pbr-ibl.cpp badge rig; override for a different look.
	std::vector<Orbit> orbits = {
		{0.55f, 0.70f, 0.50f, 0.0f, 1.00f},
		{0.70f, 0.90f, -0.33f, 2.1f, 0.75f},
		{0.45f, 0.50f, 0.80f, 4.2f, 0.50f},
	};

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;
};

// GLSL `#pragma osgx::light` catalog registration - see registerShaderLibs()/resolveShaderLibs()
// in Shader.hpp. A separate namespace from "osgx::pbr" (2026-09-23 decision - real churn at every
// existing call site, accepted deliberately: taxonomy should match the header split exactly, not
// be papered over for compatibility).
void registerLightShaderLibs();

}
