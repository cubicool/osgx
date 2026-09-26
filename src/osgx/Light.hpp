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
	class UniformBufferBinding;
}

namespace osgx {

// ================================================================================================
// Direct lights
//
// LightSet/LightType/OrbitLightRig and the Material-free GLSL: per-light radiance
// (POINT/DIRECTIONAL/SPOT_LIGHT_RADIANCE), LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR's representative
// point, and the osgx_DirectLighting() contract. The BRDF response to a light (DIRECT_SPECULAR,
// DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE) is material math and lives in PBR.hpp's
// "osgx::pbr" catalog. This header has no C++ dependency on PBR.hpp: DIRECT_LIGHTING_DECL/
// DIRECT_LIGHTING_HOOK_DEFAULT reference `osgx_Material` as a type name only, resolved by the
// caller's own `#pragma osgx::pbr MATERIAL_STRUCT` line.
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

// Size, in 4-byte floats, of one packed `osgx_Light` struct in LIGHT_UNIFORMS' std140 uniform
// block below (16 floats = 64 bytes) - the C++-side stride LightSet's setters/getters index into
// `lights` with. Must match the GLSL struct exactly; see LIGHT_UNIFORMS' own layout comment.
inline constexpr std::size_t LIGHT_STRUCT_FLOATS = 16;

// Point-light radiance: inverse-square falloff, no artificial radius cutoff, plus the resulting
// light direction `L`, both needed by DIRECT_LIGHT (PBR.hpp). `posIntensity` is world-space position
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

// Declarations shared by every consumer of the osgx_lights buffer - one
// contract, so a caller can populate one osgx::LightSet on an ancestor StateSet and have it
// inherited by every lit subgraph (dice, backdrop, whatever else) instead of wiring the same
// uniforms into each shader by hand. A consumer loops over all OSGX_MAX_LIGHTS slots and skips
// those whose `enabled` is 0, as DIRECT_LIGHTING_HOOK_DEFAULT's osgx_DirectLighting() does.
//
// The lights live in one std140 uniform block holding a fixed-size (OSGX_MAX_LIGHTS) struct array:
// small, fixed-layout and read-only, which is what uniform blocks are for (see docs/CORE.md's
// "Buffer blocks and samplers"). osgx_Light is built from 16-byte rows, so std140's array-stride
// rounding adds no padding. Every uniform/block name below carries the `osgx_` prefix to avoid collision with an
// unrelated consumer shader's own similarly-named uniforms, matching the rest of this catalog
// (osgx_Material, osgx_DirectLight, etc.).
//
// Packed layout of one osgx_Light (std140; 16 floats / 64 bytes - must match
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
// DIRECT_LIGHT_SPHERE in PBR.hpp). This is unrelated to POINT_LIGHT_RADIANCE's own "no artificial
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

layout(std140, binding = @osgx::light@) uniform osgx_LightBuffer {
	osgx_Light osgx_lights[OSGX_MAX_LIGHTS];
};
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
// term (DIRECT_LIGHT_SPHERE, PBR.hpp) needs; a diffuse-only consumer can ignore both. Requires
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
// DIRECT_LIGHT_SPHERE in PBR.hpp). `toLightCenter` is UNNORMALIZED (light center minus shading point);
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

// osgx_DirectLighting() CONTRACT - the per-light dispatch loop above (LIGHT_UNIFORMS'
// osgx_lights buffer array) factored out behind a single function boundary, instead of every consumer
// hand-copying it into its own main() (PBRScene.cpp's FULL_PBR_FRAGMENT_SHADER_SRC and
// OpenSceneGraph.py's pyosg_dice.py both did exactly that, and the latter has already drifted out
// of sync - see osgx TODO.md). Follows the separate-compiled-shader-object "hook" pattern osgSlug
// already uses to good effect (~/dev/osgSlug/src/Atlas.shaders.cpp's SHADER_NOOP_*_HOOK/HookList,
// Atlas.cpp's createDefaultStateSet()): a consumer's OWN fragment shader only needs
// DIRECT_LIGHTING_DECL spliced in (list MATERIAL_STRUCT earlier via #pragma osgx::pbr - this is a
// bare forward declaration, osgx_Material must already be a known type) plus a call site
// (`color += osgx_DirectLighting(N, V, worldPos, mat);`); it never touches osgx_lights/
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
// #pragma, so it is deliberately NOT in the "osgx::light" catalog. Pulls MATERIAL_STRUCT
// and the pure BRDF snippets from "osgx::pbr" and the rest of its own dependencies from
// "osgx::light" - two separate pragma lines, since resolveShaderLibs() resolves each line against
// whichever registered namespace it names, independent of the others.
inline constexpr const char* DIRECT_LIGHTING_HOOK_DEFAULT = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
#pragma osgx::light POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR
#pragma osgx::pbr DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE

vec3 osgx_DirectLighting(vec3 N, vec3 V, vec3 worldPos, osgx_Material mat) {
	vec3 color = vec3(0.0);

	// Every slot, gated by its `enabled` flag, which lives in the light buffer itself.
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
// LIGHT_UNIFORMS uniform block's buffer and binds it at the "osgx::light" slot. Every slot starts
// disabled; setPoint()/setDirectional()/setSpot() configure and enable one. A caller wiring a fixed rig
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

	// Whether this LightSet still has its buffer and binding. A normal constructed
	// LightSet is valid immediately and can be attached directly with setAttributeAndModes().
	bool valid() const;

	// `const` - these mutate the buffer this LightSet owns, not its object identity. Lets
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

	// Enables or disables an already configured light without changing its data. The typed setup
	// methods enable their slot.
	void setEnabled(std::size_t index, bool enabled) const;

	// Sets ONLY a light's posIntensity field, leaving color/type/dir/spotAngles/sourceRadius
	// untouched - the one primitive OrbitLightRig below needs to animate a light already
	// configured via setPoint/setSpot, without re-specifying everything else every frame.
	void setPosition(std::size_t index, const osg::Vec3& position, float intensity) const;

	// Read accessors - e.g. osgx::LightMarkers/LightGizmos read back a live LightSet's per-light
	// state to place gizmo geometry; individual fields are no longer separately retrievable
	// osg::Uniforms the way the old parallel-array design allowed.
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
	// floats, std140 layout - see LIGHT_UNIFORMS' struct comment), bound through _binding at the
	// "osgx::light" slot. It stays private so it cannot be replaced independently of that binding.
		osg::ref_ptr<osgx::FloatArray> _lights;
		osg::ref_ptr<osg::UniformBufferBinding> _binding;
		mutable std::once_flag _bindingResolved;

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

// The `#pragma osgx::light` catalog is registered by osgx::Library and expanded by
// resolveShaderLibs() (Shader.hpp). It is a separate catalog from "osgx::pbr", matching the header
// split.

}
