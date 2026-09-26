#pragma once

#include "Array.hpp"
#include "Shader.hpp"
#include "Core.hpp"
#include "Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Vec4>

OSGX_ENABLE_WARNINGS

#include <array>
#include <string>

namespace osg {
	class UniformBufferBinding;
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
// catalog names are the shader-lib registry's conventional tags, unrelated to the C++ namespace.
//
// Ported from the STATIC path of OpenSceneGraph.py/examples/pyosg-lighting/09-ibl.py: a
// pre-baked .ktx2 prefiltered cubemap loaded once, plus a one-shot BRDF LUT bake. Deliberately
// does NOT include 10-dynamicprobes.py's live GPU re-bake - out of scope here.
//
// Direct lights (LightSet, LightType, the Material-free radiance/sampling snippets and the
// osgx_DirectLighting() contract) are in Light.hpp. The BRDF response to one light
// (DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE) is here: it is material
// math. DIRECT_LIGHT_SPHERE also calls osgx_SphereLightDir() (Light.hpp's SPHERE_LIGHT_SPECULAR),
// so a shader pulls the light snippets on an earlier pragma line:
//
//   #pragma osgx::pbr MATERIAL_STRUCT, D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK
//   #pragma osgx::light LIGHT_UNIFORMS, ..., LIGHT_SAMPLE, SPHERE_LIGHT_SPECULAR
//   #pragma osgx::pbr DIRECT_SPECULAR, DIRECT_DIFFUSE, DIRECT_LIGHT, DIRECT_LIGHT_SPHERE
//
// This file has no C++ dependency on Light.hpp.
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
// inside the "osgx::pbr" catalog's `static constexpr ShaderLib` array (PBR.cpp), which needs a
// genuine constant expression - `inline constexpr` in a header is the one form satisfying both,
// same reasoning as osgx::ibl's shader-string constants.

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
// ENVIRONMENT_LIGHTING (Environment.hpp), and a hemisphere/SH ambient term need to shade a fragment.
inline constexpr const char* MATERIAL_STRUCT = R"GLSL(
struct osgx_Material {
	vec3 albedo;
	float ao;
	float roughness;
	float metallic;
	vec3 F0;
};
)GLSL";

// Per-light Cook-Torrance specular contribution (direct lighting), already multiplied by NdotL --
// caller multiplies by the light's own radiance (color * intensity/distance^2 or similar) and
// accumulates. Requires D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK already in scope.
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
// below). Requires F_SCHLICK already in scope, and `const float PI` in the consuming
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

// Combines osgx_DirectDiffuse + osgx_DirectSpecular into one per-light contribution against an
// osgx_Material (MATERIAL_STRUCT). A caller loops over the enabled osgx_lights, gets L/radiance
// from osgx_SampleLight() (Light.hpp's LIGHT_SAMPLE), and accumulates this - or calls
// osgx_DirectLighting() (Light.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT), which does
// exactly that. Requires D_GGX/G_SCHLICK/G_SMITH/F_SCHLICK/MATERIAL_STRUCT and
// DIRECT_SPECULAR/DIRECT_DIFFUSE already in scope.
inline constexpr const char* DIRECT_LIGHT = R"GLSL(
vec3 osgx_DirectLight(vec3 N, vec3 V, vec3 L, vec3 radiance, osgx_Material mat) {
	float NdotV = max(dot(N, V), 0.0);
	vec3 diffuse = osgx_DirectDiffuse(N, V, L, mat.albedo, mat.metallic, mat.F0);
	vec3 specular = osgx_DirectSpecular(N, V, L, NdotV, mat.roughness, mat.F0);

	return (diffuse + specular) * radiance * mat.ao;
}
)GLSL";

// Sphere-aware counterpart to DIRECT_LIGHT above, for a point or spot light with a non-zero
// physical `sourceRadius`. Diffuse is unchanged (osgx_DirectDiffuse against the true `L`);
// specular is re-evaluated against the representative-point direction (osgx_SphereLightDir) with
// roughness widened by the light's angular size (`alphaPrime`, ported from 99-repl.py's
// `evalSpherePoint()`) so a large/close source reads as a genuinely bigger, softer highlight
// rather than the same small one just brighter. Reuses DIRECT_DIFFUSE/DIRECT_SPECULAR unchanged --
// no new BRDF math, just a second osgx_DirectSpecular call site with a different L/roughness.
// Requires DIRECT_DIFFUSE, DIRECT_SPECULAR, and SPHERE_LIGHT_SPECULAR (Light.hpp, "osgx::light")
// already in scope.
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

// Texture-coordinate array index (osg_MultiTexCoordN) each of osgx::Material's four maps reads its
// UVs from: the glTF loader writes each map's UV set there, and PBR_VERTEX_SHADER reads it from
// there. A vertex-layout convention, independent of the texture unit each map binds at (the
// "osgx::material.*" slots, osgx::Bindings).
inline constexpr unsigned int BASE_COLOR_UV_CHANNEL = 0;
inline constexpr unsigned int NORMAL_UV_CHANNEL = 1;
inline constexpr unsigned int ORM_UV_CHANNEL = 2;
inline constexpr unsigned int EMISSIVE_UV_CHANNEL = 3;

// A complete vertex shader for osgx::Material geometry: skins the vertex through osgx_ApplySkin()
// (Skinning.hpp; link a Hook::Skinning shader object, SKINNING_HOOK_IDENTITY for unskinned
// geometry), and passes view-space position, normal and tangent (vPosition, vNGeom, vTangent) plus
// each map's UVs from its *_UV_CHANNEL array (vBaseColorUV, vNormalUV, vOrmUV, vEmissiveUV) - the
// inputs osgx_GetMaterial()/osgx_GetShadingNormal()/osgx_GetEmissive()/osgx_GetAlpha() take. Bind
// osg_Tangent with bindMeshAttributes() and pass the source through resolveShaderLibs().
inline constexpr const char* PBR_VERTEX_SHADER = R"GLSL(
#version 460 core

in vec4 osg_Vertex;
in vec3 osg_Normal;
in vec4 osg_Tangent;
in vec2 osg_MultiTexCoord0;
in vec2 osg_MultiTexCoord1;
in vec2 osg_MultiTexCoord2;
in vec2 osg_MultiTexCoord3;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat4 osg_ModelViewMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vNGeom;
out vec3 vPosition;
out vec4 vTangent;
out vec2 vBaseColorUV;
out vec2 vNormalUV;
out vec2 vOrmUV;
out vec2 vEmissiveUV;

#pragma osgx::skinning SKINNING_DECL

void main() {
	osgx_SkinnedVertex skinned = osgx_ApplySkin(osg_Vertex, osg_Normal, osg_Tangent.xyz);
	vec4 eyePos = osg_ModelViewMatrix * skinned.position;
	vPosition = eyePos.xyz;
	vBaseColorUV = osg_MultiTexCoord0;
	vNormalUV = osg_MultiTexCoord1;
	vOrmUV = osg_MultiTexCoord2;
	vEmissiveUV = osg_MultiTexCoord3;
	vNGeom = normalize(osg_NormalMatrix * skinned.normal);
	vTangent = vec4(osg_NormalMatrix * skinned.tangent, osg_Tangent.w);

	gl_Position = osg_ModelViewProjectionMatrix * skinned.position;
}
)GLSL";

// GLSL read side of osgx::Material (below): the factor buffer it builds plus its four texture maps.
// Every material value lives in ONE std140 uniform block - no loose per-material uniforms - and the four
// samplers carry their own `layout(binding = N)` texture unit, so nothing on the C++ side has to set
// sampler uniforms either (GLSL forbids layout(binding) on struct members, which is why these are
// four plain samplers rather than a struct). The buffer and the four samplers bind at the
// "osgx::material" and "osgx::material.*" slots (osgx::Bindings). The glTF loader produces
// osgx::Material too, so this is the one material interface for both hand-built and glTF-loaded
// geometry.
//
// Packed layout (std140; 16 floats / 64 bytes - must match Material::_writeFactors() in PBR.cpp):
//   vec4  baseColorFactor           floats  0-3
//   vec3  emissiveFactor                    4-6
//   float roughnessFactor                   7
//   float metallicFactor                    8
//   float alphaMode                         9   (OSGX_ALPHA_MODE_* below)
//   float alphaCutoff                      10
//   float hasBaseColorMap                  11
//   float hasMetallicRoughnessMap          12
//   float hasOcclusion                     13
//   float hasNormalMap                     14
//   float hasEmissiveMap                   15
inline constexpr const char* MATERIAL_INPUTS = R"GLSL(
#define OSGX_ALPHA_MODE_OPAQUE 0.0
#define OSGX_ALPHA_MODE_MASK 1.0
#define OSGX_ALPHA_MODE_BLEND 2.0

layout(std140, binding = @osgx::material@) uniform osgx_MaterialInputs {
	vec4 baseColorFactor;
	vec3 emissiveFactor;
	float roughnessFactor;
	float metallicFactor;
	float alphaMode;
	float alphaCutoff;
	float hasBaseColorMap;
	float hasMetallicRoughnessMap;
	float hasOcclusion;
	float hasNormalMap;
	float hasEmissiveMap;
} osgx_materialInputs;

layout(binding = @osgx::material.baseColor@) uniform sampler2D osgx_baseColorMap;
layout(binding = @osgx::material.normal@) uniform sampler2D osgx_normalMap;
layout(binding = @osgx::material.orm@) uniform sampler2D osgx_ormMap;
layout(binding = @osgx::material.emissive@) uniform sampler2D osgx_emissiveMap;
)GLSL";

// Reads MATERIAL_INPUTS into an osgx_Material (MATERIAL_STRUCT). Requires MATERIAL_INPUTS and
// MATERIAL_STRUCT already in scope.
//
// Every texture read here is conditioned on the matching `has*Map` flag rather than sampled
// unconditionally: a factor-only material (no baseColorTexture/metallicRoughnessTexture/
// occlusionTexture - common in the glTF-Sample-Models conformance set, e.g. Fox ships
// roughnessFactor=0.58 with no texture at all) would otherwise read an unbound texture unit as
// black/zero and silently discard the authored factor instead of falling back to it.
inline constexpr const char* GET_MATERIAL = R"GLSL(
osgx_Material osgx_GetMaterial(vec2 baseColorUV, vec2 ormUV) {
	osgx_Material mat;

	// Base color is texture * factor (glTF), matching GET_ALPHA below; the loader does not
	// pre-multiply the factor into the texture.
	mat.albedo = osgx_materialInputs.baseColorFactor.rgb;

	if(bool(osgx_materialInputs.hasBaseColorMap))
		mat.albedo *= texture(osgx_baseColorMap, baseColorUV).rgb;
	mat.ao = bool(osgx_materialInputs.hasOcclusion) ? texture(osgx_ormMap, ormUV).r : 1.0;
	mat.roughness = bool(osgx_materialInputs.hasMetallicRoughnessMap)
		? texture(osgx_ormMap, ormUV).g * osgx_materialInputs.roughnessFactor
		: osgx_materialInputs.roughnessFactor
	;
	mat.metallic = bool(osgx_materialInputs.hasMetallicRoughnessMap)
		? texture(osgx_ormMap, ormUV).b * osgx_materialInputs.metallicFactor
		: osgx_materialInputs.metallicFactor
	;

	mat.F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

	return mat;
}
)GLSL";

// Emissive radiance from MATERIAL_INPUTS: the emissive map (when present) times emissiveFactor. With
// no map this is just emissiveFactor, which osgx::Material defaults to black. Requires
// MATERIAL_INPUTS already in scope.
inline constexpr const char* GET_EMISSIVE = R"GLSL(
vec3 osgx_GetEmissive(vec2 emissiveUV) {
	vec3 emissive = bool(osgx_materialInputs.hasEmissiveMap)
		? texture(osgx_emissiveMap, emissiveUV).rgb
		: vec3(1.0)
	;

	return emissive * osgx_materialInputs.emissiveFactor;
}
)GLSL";

// Surface alpha from MATERIAL_INPUTS: the base color map's alpha (when present) times
// baseColorFactor.a. Returned as a plain value, not baked into a discard, since BLEND callers want
// the alpha itself; a MASK caller does its own
// `if(osgx_materialInputs.alphaMode == OSGX_ALPHA_MODE_MASK && alpha < osgx_materialInputs.alphaCutoff) discard;`.
// Requires MATERIAL_INPUTS already in scope.
inline constexpr const char* GET_ALPHA = R"GLSL(
float osgx_GetAlpha(vec2 baseColorUV) {
	float alpha = bool(osgx_materialInputs.hasBaseColorMap)
		? texture(osgx_baseColorMap, baseColorUV).a
		: 1.0
	;

	return alpha * osgx_materialInputs.baseColorFactor.a;
}
)GLSL";

// Shading normal from MATERIAL_INPUTS' normal map: TBN reconstructed per-pixel from screen-space
// derivatives of position/UV (Christian Schuler's "normal mapping without precomputed tangents" -
// http://www.thetenthplanet.de/archives/1180) when the mesh's own tangent is degenerate/absent,
// falling back to the vertex tangent otherwise. glTF's TANGENT accessor is optional and frequently
// absent (DamagedHelmet, MetalRoughSpheres, Fox in glTF-Sample-Models all ship without one); an
// unbound osg_Tangent attribute reads OpenGL's default (0,0,0,1), and normalizing that zero vector
// produces NaN, so the degenerate check here isn't optional. Returns normalize(Ngeom) unchanged
// when the material has no normal map. FRAGMENT-ONLY (dFdx/dFdy). Requires MATERIAL_INPUTS
// already in scope.
inline constexpr const char* GET_SHADING_NORMAL = R"GLSL(
vec3 osgx_GetShadingNormal(vec3 Ngeom, vec4 tangent, vec3 position, vec2 normalUV) {
	vec3 Nb = normalize(Ngeom);

	if(!bool(osgx_materialInputs.hasNormalMap)) return Nb;

	// The Khronos reference normalizes in tangent space before applying TBN. Keeping that
	// normalization here matters when interpolation leaves TBN slightly non-orthonormal.
	vec3 tangentNormal = normalize(texture(osgx_normalMap, normalUV).rgb * 2.0 - 1.0);
	vec3 T, B;

	// TODO: How much is this conditional hurting us? It might be worth looking into having two
	// separate functions instead...
	if(dot(tangent.xyz, tangent.xyz) > 1e-10) {
		T = normalize(tangent.xyz);
		B = normalize(cross(Nb, T)) * tangent.w;
	}

	else {
		vec3 q1 = dFdx(position);
		vec3 q2 = dFdy(position);
		vec2 st1 = dFdx(normalUV);
		vec2 st2 = dFdy(normalUV);

		// Derive both tangent axes from position and UV derivatives.  The
		// determinant carries the UV handedness: constructing B from a fixed
		// +/-cross(N, T) works on only one side of a mirrored UV layout.
		float determinant = st1.s * st2.t - st1.t * st2.s;

		if(abs(determinant) <= 1e-10) return Nb;

		float inverseDeterminant = 1.0 / determinant;
		T = (q1 * st2.t - q2 * st1.t) * inverseDeterminant;
		B = (q2 * st1.s - q1 * st2.s) * inverseDeterminant;

		// Projection keeps the reconstructed basis orthogonal to an interpolated
		// vertex normal, and the final Gram-Schmidt step preserves B's derived
		// handedness rather than imposing one.
		T -= Nb * dot(Nb, T);
		B -= Nb * dot(Nb, B);

		if(dot(T, T) <= 1e-10 || dot(B, B) <= 1e-10) return Nb;

		T = normalize(T);
		B -= T * dot(T, B);

		if(dot(B, B) <= 1e-10) return Nb;

		// NormalTangentTest verifies the glTF normal-map convention for this
		// runtime-derived basis. Its bitangent is opposite the derivative-space
		// orientation above; invert only this no-authored-tangent fallback.
		B = -normalize(B);
	}

	mat3 TBN = mat3(T, B, Nb);

	return normalize(TBN * tangentNormal);
}
)GLSL";

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
// (above) already treat them as one interface.
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

		// Matches MATERIAL_INPUTS' OSGX_ALPHA_MODE_* defines (and glTF's alphaMode). Only the
		// shader-visible value lives here - render state a mode implies (BLEND's GL_BLEND/
		// BlendFunc/depth-write setup) is the caller's job.
		enum class AlphaMode: int {
			Opaque = 0,
			Mask = 1,
			Blend = 2
		};

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

		// Multiplies the emissive map (or stands alone without one); defaults to black.
		void setEmissiveFactor(const osg::Vec3& emissiveFactor);
		const osg::Vec3& getEmissiveFactor() const { return _emissiveFactor; }

		void setAlphaMode(AlphaMode alphaMode);
		AlphaMode getAlphaMode() const { return _alphaMode; }

		// Only meaningful for AlphaMode::Mask; defaults to 0.5 (glTF's default).
		void setAlphaCutoff(float alphaCutoff);
		float getAlphaCutoff() const { return _alphaCutoff; }

		// See the class comment - occlusion has no dedicated unit of its own, so unlike the four
		// map setters below, this doesn't derive from a ref_ptr.
		void setHasOcclusion(bool hasOcclusion);
		bool getHasOcclusion() const { return _hasOcclusion; }

		void setBaseColorMap(osg::Texture2D* texture);
		osg::Texture2D* getBaseColorMap() const { return _baseColorMap.get(); }

		void setNormalMap(osg::Texture2D* texture);
		osg::Texture2D* getNormalMap() const { return _normalMap.get(); }

		// glTF's combined occlusion/roughness/metallic texture.
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
		osg::Vec3 _emissiveFactor{0.0f, 0.0f, 0.0f};
		AlphaMode _alphaMode = AlphaMode::Opaque;
		float _alphaCutoff = 0.5f;
		bool _hasOcclusion = false;

		osg::ref_ptr<osg::Texture2D> _baseColorMap;
		osg::ref_ptr<osg::Texture2D> _normalMap;
		osg::ref_ptr<osg::Texture2D> _metallicRoughnessMap;
		osg::ref_ptr<osg::Texture2D> _emissiveMap;

		osg::ref_ptr<osgx::FloatArray> _buffer;
		osg::ref_ptr<osg::UniformBufferBinding> _binding;
		mutable std::once_flag _bindingResolved;

		// The four "osgx::material.*" texture units (base color, normal, ORM, emissive).
		mutable std::array<unsigned int, 4> _units{};
		mutable std::once_flag _unitsResolved;
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
// The classic single-scatter split-sum approximation (Karis 2013) loses energy at higher roughness because it only accounts for
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

// Geometric specular anti-aliasing (Tokuyoshi & Kaplanyan 2019 / Filament's normal filtering):
// widens roughness where the shading normal changes rapidly across a pixel's screen-space
// footprint, so a low-roughness, high-curvature surface (a beveled metal trim is the case that
// motivated this) converges under low sample counts instead of showing per-pixel reflection
// sparkle/aliasing as its mirror-like reflection vector jitters between neighboring fragments.
// Works in alpha space (roughness^2, the actual GGX parameter) since the added variance term is
// only meaningful there, then converts back to the perceptual roughness this codebase otherwise
// passes around (osgx_EnvironmentSpecular()'s LOD selection and BRDF LUT lookups both expect
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

// osgx_Tonemap() CONTRACT - same "hook" pattern as osgx_DirectLighting() (Light.hpp): a consumer's fragment shader only needs TONEMAP_DECL spliced in plus a call site
// (`color = osgx_Tonemap(color);`) on its final linear color, before gamma. The DEFINITION lives
// in TONEMAP_HOOK_DEFAULT below (osgx_TonemapPBRNeutral, unchanged) - a consumer wanting a
// different tone curve (ACES, a flat clamp, a look-specific LUT) supplies its own shader object
// defining osgx_Tonemap() instead of adding TONEMAP_HOOK_DEFAULT.
inline constexpr const char* TONEMAP_DECL = R"GLSL(
vec3 osgx_Tonemap(vec3 color);
)GLSL";

// Self-contained, same shape as DIRECT_LIGHTING_HOOK_DEFAULT (Light.hpp) - not spliced by name via #pragma osgx::pbr, so deliberately NOT in the "osgx::pbr" catalog.
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

}
