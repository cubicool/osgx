#pragma once

#include "Array.hpp"
#include "Shader.hpp"
#include "Core.hpp"

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

// Per-light Cook-Torrance specular contribution (direct lighting), already multiplied by NdotL --
// caller multiplies by the light's own radiance (color * intensity/distance^2 or similar) and
// accumulates. Requires D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK already in scope (include snippets()
// or those four individually before this one).
inline constexpr const char* DIRECT_SPECULAR = R"GLSL(
vec3 osgx_DirectSpecular(vec3 N, vec3 V, vec3 L, float NdotV, float roughness, vec3 F0) {
	float NdotL = max(dot(N, L), 0.0);

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
// below). Requires F_SCHLICK already in scope, and `const float PI` in the consuming shader (see
// the file-level contract note above).
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

// Binding point for LIGHT_UNIFORMS' osgx_LightBuffer below. It deliberately differs from
// osgx::gltf::shader::JOINT_MATRICES_BINDING (2), so a skinned glTF asset lit via
// osgx::LightSet can bind both at once.
inline constexpr unsigned int LIGHT_BINDING = 3;

// Punctual point-light radiance (glTF punctual-light convention: inverse-square falloff, no
// artificial radius cutoff) plus the resulting light direction `L`, both needed by DIRECT_LIGHT
// below. `posIntensity` is world-space position in .xyz and intensity in .w - the exact packing
// osgx::OrbitLightRig writes via LightSet::setPosition() into each osgx_Light's own
// posIntensity field (LIGHT_UNIFORMS' buffer struct below), so a caller wiring a static (e.g.
// torch-style) light just calls LightSet::setPoint() once instead of installing a NodeCallback.
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
// LIGHT_STRUCT_FLOATS and the float offsets LightSet's setters/getters use in PBR.cpp):
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
// DIRECT_LIGHT_SPHERE below). This is unrelated to the "no artificial radius cutoff" decision
// noted on POINT_LIGHT_RADIANCE above - that was about attenuation distance (deliberately not
// reintroduced here); this is about physical light size.
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

// binding = 3 here must match LIGHT_BINDING in C++ - same hardcode-and-cross-reference
// pattern osgx::gltf::shader::MATERIAL_INPUTS uses for its own `binding = 0`.
layout(std430, binding = 3) readonly buffer osgx_LightBuffer {
	osgx_Light osgx_lights[OSGX_MAX_LIGHTS];
};

uniform int osgx_lightCount;
)GLSL";

// Combines osgx_DirectDiffuse + osgx_DirectSpecular into one per-light contribution against an
// osgx_Material (MATERIAL_STRUCT) - the "modular hook" PBRIBLScene::create()'s own comment has been
// waiting on: a caller loops `osgx_lightCount` times, calling osgx_PointLightRadiance for
// L/radiance then this for the shaded result, and accumulates (or just calls osgx_DirectLighting(),
// see DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT below, which already does exactly that). Requires
// D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK/DIRECT_SPECULAR/DIRECT_DIFFUSE/MATERIAL_STRUCT already in scope.
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
// DIRECT_LIGHTING_DECL spliced in (list MATERIAL_STRUCT earlier in the SAME pragma line - this is a
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

// Self-contained - carries its own #version/PI/#pragma line so it compiles as a standalone
// osg::Shader object regardless of what the consumer's own fragment shader happens to have in
// scope. Add via:
//   program->addShader(new osg::Shader(
//     osg::Shader::FRAGMENT, osgx::resolveShaderLibs(osgx::DIRECT_LIGHTING_HOOK_DEFAULT)
//   ));
// as an EXTRA shader object on the same Program that already has the consumer's own fragment
// shader (which only needs DIRECT_LIGHTING_DECL + a call site, see above) - not spliced by name via
// #pragma osgx::pbr, so it is deliberately NOT in registerPBRShaderLibs()'s catalog.
inline constexpr const char* DIRECT_LIGHTING_HOOK_DEFAULT = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK, DIRECT_SPECULAR, DIRECT_DIFFUSE, POINT_LIGHT_RADIANCE, LIGHT_UNIFORMS, DIRECT_LIGHT, DIRECTIONAL_LIGHT_RADIANCE, SPOT_LIGHT_RADIANCE, SPHERE_LIGHT_SPECULAR, DIRECT_LIGHT_SPHERE

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

		vec3 L;
		vec3 radiance;

		if(light.type == OSGX_LIGHT_TYPE_DIRECTIONAL) {
			radiance = osgx_DirectionalLightRadiance(light.dir, light.color, light.posIntensity.w, L);
		}

		else if(light.type == OSGX_LIGHT_TYPE_SPOT) {
			radiance = osgx_SpotLightRadiance(
				light.posIntensity, light.color, light.dir, light.spotAngles, worldPos, L
			);
		}

		else {
			radiance = osgx_PointLightRadiance(light.posIntensity, light.color, worldPos, L);
		}

		if(light.sourceRadius > 0.0 && light.type != OSGX_LIGHT_TYPE_DIRECTIONAL) {
			color += osgx_DirectLightSphere(
				N, V, L, light.posIntensity.xyz - worldPos, radiance, mat, light.sourceRadius
			);
		}

		else {
			color += osgx_DirectLight(N, V, L, radiance, mat);
		}
	}

	return color;
}
)GLSL";

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
// CAPABILITY/member 1 deliberately differs from Material's CAPABILITY/member 0 - see Material's
// class comment for why the full (Type, member) pair is the State cache key.
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
	// floats, std430 layout - see LIGHT_UNIFORMS' struct comment), bound through _binding at
	// LIGHT_BINDING. It stays private so it cannot be replaced independently of that binding.
		osg::ref_ptr<osgx::FloatArray> _lights;
		osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
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

void registerPBRShaderLibs();

}
