#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

OSGX_ENABLE_WARNINGS

// Also brings in tinygltf_json_c.h (declarations only - tiny_gltf_v3.c already compiles the
// implementation once into osgx_gltf, which osgx_gltf_pbribl links).
#include "Json.hpp"

#include "osgx/gltf/PBRIBL.hpp"
#include "osgx/gltf/Shader.hpp"

#include "osgx/GGXPrefilter.hpp"
#include "osgx/IBL.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/TextureCubeMap>
#include <osgDB/ReadFile>

#ifndef GL_RGB32F
#  define GL_RGB32F 0x8815
#endif

OSGX_ENABLE_WARNINGS

#include <filesystem>
#include <fstream>

// ================================================================================================
// osgx::gltf::pbribl - the glue between the glTF loader material interface
// and osgx::pbr's material-agnostic BRDF math: reads the material-buffer/texture-unit interface the C++ loader
// populates per primitive into an osgx_Material (see osgx::MATERIAL_STRUCT), plus a couple
// of small glTF-specific fragment helpers (shading normal, emissive, alpha coverage) that need
// the same texture interface.
//
// Candidates identified by porting OpenSceneGraph.py/examples/pyosg-lighting/09-ibl.py's material-
// reading fragment code (getMaterial()/getShadingNormal()/getEmissive()/getAlphaCoverage()) down
// to OpenSceneGraph.py/examples/pyosg-voxelize.py's trimmed no-IBL-required PBR fallback shader --
// the same material-reading code was about to get copy-pasted a third time, which is exactly what
// this adapter exists to avoid. Shader.hpp's MATERIAL_INPUTS is the fixed external interface (field
// order/types must match Material.cpp's layout exactly); everything else is GLSL glue.
// ================================================================================================

namespace osgx::gltf::pbribl {

// Reads MATERIAL_INPUTS into an osgx_Material (osgx::MATERIAL_STRUCT). Requires
// MATERIAL_INPUTS and MATERIAL_STRUCT already in scope.
//
// Every texture read here is conditioned on the matching `has*Map` flag rather than sampled
// unconditionally: a factor-only material (no baseColorTexture/metallicRoughnessTexture/
// occlusionTexture - common in the glTF-Sample-Models conformance set, e.g. Fox ships
// roughnessFactor=0.58 with no texture at all) would otherwise read an unbound texture unit as
// black/zero and silently discard the authored factor instead of falling back to it.
const char GET_MATERIAL[] = R"GLSL(
osgx_Material osgx_gltf_GetMaterial(vec2 baseColorUV, vec2 ormUV, vec3 N) {
	osgx_Material mat;

	mat.albedo = bool(osgx_gltf_material.hasBaseColorMap)
		? texture(osgx_gltf_textures.baseColor, baseColorUV).rgb
		: osgx_gltf_material.baseColorFactor.rgb
	;
	mat.ao = bool(osgx_gltf_material.hasOcclusion) ? texture(osgx_gltf_textures.orm, ormUV).r : 1.0;
	mat.roughness = bool(osgx_gltf_material.hasMetallicRoughnessMap)
		? texture(osgx_gltf_textures.orm, ormUV).g * osgx_gltf_material.roughnessFactor
		: osgx_gltf_material.roughnessFactor
	;
	mat.metallic = bool(osgx_gltf_material.hasMetallicRoughnessMap)
		? texture(osgx_gltf_textures.orm, ormUV).b * osgx_gltf_material.metallicFactor
		: osgx_gltf_material.metallicFactor
	;

	mat.F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

	return mat;
}
)GLSL";

// TBN reconstructed per-pixel from screen-space derivatives of position/UV (Christian Schuler's
// "normal mapping without precomputed tangents" - http://www.thetenthplanet.de/archives/1180)
// when the mesh's own TANGENT is degenerate/absent, falling back to the vertex TANGENT otherwise.
// glTF's TANGENT accessor is optional and frequently absent (DamagedHelmet, MetalRoughSpheres,
// Fox in glTF-Sample-Models all ship without one); an unbound osg_Tangent attribute reads OpenGL's
// default (0,0,0,1), and normalizing that zero vector produces NaN, so the degenerate check here
// isn't optional. Requires MATERIAL_INPUTS already in scope.
const char SHADING_NORMAL[] = R"GLSL(
vec3 osgx_gltf_ShadingNormal(vec3 Ngeom, vec4 tangent, vec3 position, vec2 normalUV) {
	vec3 Nb = normalize(Ngeom);

	if(!bool(osgx_gltf_material.hasNormalMap)) return Nb;

	// The Khronos reference normalizes in tangent space before applying TBN. Keeping that
	// normalization here matters when interpolation leaves TBN slightly non-orthonormal.
	vec3 tangentNormal = normalize(texture(osgx_gltf_textures.normal, normalUV).rgb * 2.0 - 1.0);
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

// Requires MATERIAL_INPUTS already in scope.
const char EMISSIVE[] = R"GLSL(
vec3 osgx_gltf_Emissive(vec2 emissiveUV) {
	vec3 emissive = osgx_gltf_hasEmissiveMap != 0
		? texture(osgx_gltf_textures.emissive, emissiveUV).rgb
		: vec3(1.0)
	;

	return emissive * osgx_gltf_emissiveFactor;
}
)GLSL";

// MASK-mode coverage test input (caller still does `if(osgx_gltf_alphaMode == 1.0 && alpha <
// osgx_gltf_alphaCutoff) discard;` itself - kept as a plain value here, not baked into a
// discard, since some callers want the alpha for BLEND instead). Requires MATERIAL_INPUTS
// already in scope.
const char ALPHA_COVERAGE[] = R"GLSL(
float osgx_gltf_AlphaCoverage(vec2 baseColorUV) {
	float alpha = bool(osgx_gltf_material.hasBaseColorMap)
		? texture(osgx_gltf_textures.baseColor, baseColorUV).a
		: 1.0
	;

	return alpha * osgx_gltf_material.baseColorFactor.a;
}
)GLSL";

// The minimum declarations ANY osgx::Hook::DeferredLighting override needs against
// PBRIBLLightingScene::create()'s fullscreen quad - the five G-buffer sampler uniforms
// PBRIBLGBuffer::create() writes (view-space normal/position, NOT world-space - see
// PBRIBLGBuffer's own field comments), the view-matrix uniforms PBRIBLLightingScene::update()
// keeps fresh every frame (needed to rotate view-space normal/position into world space; see that
// function's own comment for why this quad's own osg_ViewMatrix can't be trusted), `vUV`, and the
// pass's single color output. Deliberately does NOT include the IBL environment uniforms
// (envMap/brdfLUT/diffuseEnv/iblAxis/iblDiffuseIntensity/iblSpecularIntensity) or `aoTex` --
// those are specific to callers still wanting the real osgx_EvaluateIBL() path (like
// LIGHTING_FRAGMENT_SHADER_SRC's own built-in default below), not universal G-buffer boilerplate
// every override needs; `#pragma osgx::gltf *` still pulls all of it in for a caller that does.
const char DEFERRED_LIGHTING_INPUTS[] = R"GLSL(
in vec2 vUV;

uniform sampler2D gAlbedo;
uniform sampler2D gNormal;
uniform sampler2D gMaterial;
uniform sampler2D gEmissive;
uniform sampler2D gPosition;

uniform mat4 osgx_mainViewMatrix;
uniform mat4 osgx_mainViewMatrixInverse;

out vec4 fragColor;
)GLSL";

// Structured decode of PBRIBLGBuffer::create()'s fixed 5-channel layout, the same "struct +
// osgx_GetX(uv)" shape osgx_gltf_GetMaterial() uses for MATERIAL_INPUTS - lets an
// osgx::Hook::DeferredLighting override read `gb.albedo`/`gb.normal`/etc. instead of hand-sampling
// five textures and unpacking channels itself. `normal`/`position` stay VIEW-space, exactly as
// PBRIBLGBuffer writes them (see PBRIBLGBuffer::normalTexture/positionTexture's own comments in
// PBRIBL.hpp for why) - rotate into world space via osgx_mainViewMatrixInverse only if the
// override actually needs it. Requires DEFERRED_LIGHTING_INPUTS already in scope.
const char GET_GBUFFER[] = R"GLSL(
struct osgx_GBuffer {
	vec3 albedo;
	float ao;
	vec3 normal;
	float roughness;
	float metallic;
	vec3 emissive;
	float alphaCoverage;
	vec3 position;
};

osgx_GBuffer osgx_GetGBuffer(vec2 uv) {
	osgx_GBuffer gb;

	vec4 albedoSample = texture(gAlbedo, uv);
	vec4 materialSample = texture(gMaterial, uv);
	vec4 emissiveSample = texture(gEmissive, uv);

	gb.albedo = albedoSample.rgb;
	gb.ao = albedoSample.a;
	gb.normal = texture(gNormal, uv).rgb;
	gb.roughness = materialSample.r;
	gb.metallic = materialSample.g;
	gb.emissive = emissiveSample.rgb;
	gb.alphaCoverage = emissiveSample.a;
	gb.position = texture(gPosition, uv).xyz;

	return gb;
}
)GLSL";

}

namespace osgx::gltf::pbribl {

void registerShaderLibs() {
	static const osgx::ShaderLib libs[] = {
		{"MATERIAL_INPUTS", "osgx_gltf_Material", shader::MATERIAL_INPUTS},
		{"GET_MATERIAL", "osgx_gltf_GetMaterial", GET_MATERIAL},
		{"SHADING_NORMAL", "osgx_gltf_ShadingNormal", SHADING_NORMAL},
		{"EMISSIVE", "osgx_gltf_Emissive", EMISSIVE},
		{"ALPHA_COVERAGE", "osgx_gltf_AlphaCoverage", ALPHA_COVERAGE},
		{"DEFERRED_LIGHTING_INPUTS", "osgx_gltf_DeferredLightingInputs", DEFERRED_LIGHTING_INPUTS},
		{"GET_GBUFFER", "osgx_GetGBuffer", GET_GBUFFER}
	};
	osgx::registerShaderLibs("osgx::gltf", libs);
}

std::string resolveShaderLibs(std::string_view source) {
	osgx::registerPBRShaderLibs();
	osgx::registerIBLShaderLibs();
	osgx::registerShadowShaderLibs();

	registerShaderLibs();

	return osgx::resolveShaderLibs(std::string(source));
}

}

namespace osgx::gltf::pbribl {

// ================================================================================================
// PBRIBLScene::create() - the one-call convenience helper requested after proving out, live, that
// glTF material glue + osgx::pbr's F_MULTISCATTER + osgx::ibl's LAMBERTIAN_IRRADIANCE
// compose correctly against a real osgDB::readNodeFile()'d glTF model (confirmed against Batman +
// papermill.ktx2/papermill.hdr from OpenSceneGraph.py's pyosg-lighting/data, 2026-07-22). That
// prototype was ~90 lines of Python + GLSL wiring; this collapses it to one C++ (and, via bindings,
// one Python) call. A genuine C++ API first, with a separate binding in ext/python/osgx-gltf.cpp.
//
// Uses osgx::ibl's frame-driven GPU-baked Lambertian cubemap for diffuse IBL.
// Pixel-parity with the Khronos glTF-Sample-Viewer reference is the explicit goal here, and SH9's
// low-frequency basis measurably washes out the high-contrast environments used by this helper.
// SH9 remains available as an independent osgx::ibl utility for callers that prefer its compact,
// low-cost representation; it is deliberately not part of this reference-quality convenience path.
//
// IBL plus an optional handful of direct/punctual lights, via the osgx_DirectLighting() CONTRACT
// (DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT in PBR.hpp) - one call
// (`osgx_DirectLighting(N, V, worldPos, mat)`) against osgx::LightSet's buffer-backed light array
// (up to osgx::MAX_LIGHTS), instead of this shader hand-copying the per-light dispatch loop
// itself (a prior revision did exactly that, and drifted out of sync with OpenSceneGraph.py's
// pyosg_dice.py copy - see TODO.md; both now share the one hook definition,
// DIRECT_LIGHTING_HOOK_DEFAULT, added as a second FRAGMENT osg::Shader object in
// PBRIBLScene::create() below). A LightSet's osgx_lightCount defaults to 0 (LightSet::create()
// zero-initializes it), so a caller that only wants IBL sees no change; one that wants direct
// lights just populates osgx::LightSet, here or on an ancestor StateSet shared with whatever
// else should light identically (e.g. dice sharing a scene's --scene backdrop, see
// OpenSceneGraph.py/examples/pyosg_dice.py's FRAGMENT_SHADER_IBL).
// ================================================================================================

namespace detail {

// Full vertex/fragment shader pair, NOT registered as pragma-includable ShaderLib snippets (these
// are complete shaders with their own main(), not function-body fragments meant to be concatenated
// into a caller's shader) - resolved via resolveShaderLibs() at setup time inside PBRIBLScene::create()
// below, same mechanism a caller assembling their own shader by hand would use.
constexpr const char FULL_PBR_VERTEX_SHADER[] = R"GLSL(
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
// Material.cpp assigns each glTF texture slot's requested TEXCOORD_n array to
// the corresponding texture unit. Keep those four coordinates distinct here:
// glTF permits the slots to select different UV sets, and an emissive-only
// material has no reason to populate unit 0 at all.
out vec2 vBaseColorUV;
out vec2 vNormalUV;
out vec2 vOrmUV;
out vec2 vEmissiveUV;

// osgx_gltf_ApplySkin() CONTRACT - forward-declared here, DEFINED in a separate, separately
// compiled VERTEX shader object (shader::SKINNING_HOOK_IDENTITY by default, or a caller-supplied
// `{{osgx::Hook::Skinning, ...}}` hooks entry, e.g. shader::SKINNING_HOOK_LINEAR_BLEND) attached
// alongside this one via applyHooks() (Shader.hpp) - same "forward-declare + call site here,
// definition elsewhere" hook pattern PBR.hpp's DIRECT_LIGHTING_DECL uses.
struct osgx_gltf_SkinnedVertex {
	vec4 position;
	vec3 normal;
	vec3 tangent;
};

osgx_gltf_SkinnedVertex osgx_gltf_ApplySkin(vec4 position, vec3 normal, vec3 tangent);

void main() {
	osgx_gltf_SkinnedVertex skinned = osgx_gltf_ApplySkin(osg_Vertex, osg_Normal, osg_Tangent.xyz);
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

constexpr const char FULL_PBR_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core
#pragma import_defines ( OSGX_PBRIBL_DIAGNOSTICS )

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, F_MULTISCATTER, SPECULAR_AA, TONEMAP_DECL, DIRECT_LIGHTING_DECL
#pragma osgx::ibl IBL_LIGHTING_INPUTS, EVALUATE_IBL
#pragma osgx::gltf MATERIAL_INPUTS, GET_MATERIAL, SHADING_NORMAL, EMISSIVE, ALPHA_COVERAGE

in vec3 vNGeom;
in vec3 vPosition;
in vec4 vTangent;
in vec2 vBaseColorUV;
in vec2 vNormalUV;
in vec2 vOrmUV;
in vec2 vEmissiveUV;

uniform mat4 osg_ViewMatrix;
uniform mat4 osg_ViewMatrixInverse;

#ifdef OSGX_PBRIBL_DIAGNOSTICS
// Runtime isolation, ported from OpenSceneGraph.py/pyosg-khronos-viewer.py's Diagnostics
// handler - lets a caller (see osggltf-viewer.cpp) key-toggle which term is actually
// contributing to a surface, useful for isolating why a render looks wrong. debugMode:
// 0=combined, 1=diffuse only, 2=specular only.
uniform int debugMode;
uniform int disableNormalMap;
uniform int disableRoughnessMap;
uniform int disableSpecularAA;
#endif

out vec4 fragColor;

vec3 osgx_ZUpToGLTF(vec3 d) { return vec3(d.x, d.z, -d.y); }
// `iblAxis` is uploaded PRE-FOLDED with osgx_ZUpToGLTF's Z-up -> glTF/Y-up permutation (see
// osgx::gltf::pbribl::foldZUpToGLTFAxis(), PBRIBL.hpp), consumed inside osgx_EvaluateIBL()
// (osgx::ibl EVALUATE_IBL, pulled in by the #pragma above) via its own osgx_OrientIBL() --
// osgx_ZUpToGLTF() itself stays standalone here for the OSGX_PBRIBL_DIAGNOSTICS debug-normal
// visualizations further down, which have no iblAxis involved at all.
vec3 osgx_LinearToSRGB(vec3 c) {
	return mix(12.92 * c, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
		step(vec3(0.0031308), c));
}

void main() {
	float alpha = osgx_gltf_AlphaCoverage(vBaseColorUV);

	if(osgx_gltf_alphaMode == 1.0 && alpha < osgx_gltf_alphaCutoff) discard;

	vec3 N = osgx_gltf_ShadingNormal(vNGeom, vTangent, vPosition, vNormalUV);

#ifdef OSGX_PBRIBL_DIAGNOSTICS
	if(disableNormalMap != 0) N = normalize(vNGeom);
#endif
	vec3 V = normalize(-vPosition);
	osgx_Material mat = osgx_gltf_GetMaterial(vBaseColorUV, vOrmUV, N);


#ifdef OSGX_PBRIBL_DIAGNOSTICS
	if(disableRoughnessMap != 0) mat.roughness = osgx_gltf_material.roughnessFactor;

	// Match pyosg-khronos-viewer.py's material/coordinate diagnostics. These deliberately return
	// before lighting so a channel can be compared without IBL, Fresnel, or tonemapping involved.
	mat3 invView = transpose(mat3(osg_ViewMatrix));
	vec3 NgeomWorld = normalize(invView * vNGeom);
	vec3 Nworld = normalize(invView * N);

	if(debugMode == 3) { fragColor = vec4(osgx_LinearToSRGB(mat.albedo), alpha); return; }
	if(debugMode == 4) { fragColor = vec4(vec3(mat.roughness), alpha); return; }
	if(debugMode == 5) { fragColor = vec4(vec3(mat.metallic), alpha); return; }
	if(debugMode == 6) {
		vec3 raw = texture(osgx_gltf_textures.normal, vNormalUV).rgb;
		fragColor = vec4(bool(osgx_gltf_material.hasNormalMap) ? normalize(raw * 2.0 - 1.0) * 0.5 + 0.5 : vec3(1.0), alpha);
		return;
	}
	if(debugMode == 7) {
		fragColor = vec4(
			bool(osgx_gltf_material.hasNormalMap) ? texture(osgx_gltf_textures.normal, vNormalUV).rgb : vec3(1.0),
			alpha
		);
		return;
	}
	if(debugMode == 8) { fragColor = vec4(osgx_ZUpToGLTF(NgeomWorld) * 0.5 + 0.5, alpha); return; }
	if(debugMode == 9) { fragColor = vec4(osgx_ZUpToGLTF(Nworld) * 0.5 + 0.5, alpha); return; }
	vec3 tangentWorld = normalize(invView * vTangent.xyz);
	vec3 bitangentWorld = normalize(cross(NgeomWorld, tangentWorld)) * vTangent.w;
	if(debugMode == 10) { fragColor = vec4(osgx_ZUpToGLTF(tangentWorld) * 0.5 + 0.5, alpha); return; }
	if(debugMode == 11) { fragColor = vec4(osgx_ZUpToGLTF(bitangentWorld) * 0.5 + 0.5, alpha); return; }
#endif

	// Widens roughness under high-curvature/low-roughness shading normals so the mirror-like
	// reflection vector doesn't alias between neighboring fragments at low MSAA sample counts
	// (a beveled metal trim is the motivating case - see osgx_SpecularAA). Debug channels above
	// intentionally read mat.roughness before this so they show the authored/textured value, not
	// the screen-space-widened one used for actual shading.
	float aaRoughness = osgx_SpecularAA(N, mat.roughness);

#ifdef OSGX_PBRIBL_DIAGNOSTICS
	if(disableSpecularAA == 0) mat.roughness = aaRoughness;
#else
	mat.roughness = aaRoughness;
#endif

	// World-space N/V, shared by osgx_EvaluateIBL() (osgx::ibl EVALUATE_IBL - requires
	// world-space input, see its own comment) and osgx_DirectLighting() below - both need the
	// same rotation, so it is computed once here instead of twice (evaluateIBL() used to do this
	// rotation internally, a second time, before this move). Named distinctly from the
	// diagnostics block's own invView/Nworld above so both compile together regardless of
	// OSGX_PBRIBL_DIAGNOSTICS.
	mat3 invViewRot = transpose(mat3(osg_ViewMatrix));
	vec3 N_world = invViewRot * N;
	vec3 V_world = invViewRot * V;

	osgx_Lighting ambient = osgx_EvaluateIBL(mat, N_world, V_world);
	vec3 surface = ambient.diffuse + ambient.specular;
	vec3 emissive = osgx_gltf_Emissive(vEmissiveUV);

#ifdef OSGX_PBRIBL_DIAGNOSTICS
	surface = (debugMode == 1 || debugMode == 12)
		? ambient.diffuse
		: (debugMode == 2 || debugMode == 13) ? ambient.specular : ambient.diffuse + ambient.specular
	;
	emissive = (debugMode == 0 || debugMode == 14) ? emissive : vec3(0.0);
#endif

	// Direct/punctual lights: point, directional, and spot, e.g. a torch or a sun - the
	// osgx_DirectLighting() CONTRACT (see the comment above this shader) does the per-light dispatch;
	// this shader only supplies N/V/worldPos/mat. Reuses N_world/V_world computed above.
	vec3 worldPos = (osg_ViewMatrixInverse * vec4(vPosition, 1.0)).xyz;
	vec3 direct = osgx_DirectLighting(N_world, V_world, worldPos, mat);

	vec3 color = surface + direct + emissive;

#ifdef OSGX_PBRIBL_DIAGNOSTICS
	// These three modes intentionally bypass PBR Neutral and gamma. They expose the linear
	// values immediately before output, so a comparison is not hidden by tone compression.
	if(debugMode >= 12) {
		fragColor = vec4(color, alpha);

		return;
	}
#endif

	color = osgx_Tonemap(color);
	color = pow(color, vec3(1.0 / 2.2));

	fragColor = vec4(color, alpha);
}
)GLSL";

// Geometry-pass fragment shader for the deferred split (PBRIBLGBuffer::create() below) --
// material only, no lighting at all, not even emissive combine (emissive is stored, not added
// yet). Reuses FULL_PBR_VERTEX_SHADER above unchanged: its view-space position/normal/tangent and
// per-slot UV varyings are already exactly what osgx_gltf_ShadingNormal()/osgx_gltf_GetMaterial()
// need, and are already VIEW
// space, which is exactly the convention gNormal below stores (matching the main camera whose
// real matrices PBRIBLLightingScene::create()'s fullscreen quad reconstructs world-space values
// from).
constexpr const char GBUFFER_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core

#pragma osgx::pbr MATERIAL_STRUCT
#pragma osgx::gltf MATERIAL_INPUTS, GET_MATERIAL, SHADING_NORMAL, EMISSIVE, ALPHA_COVERAGE

in vec3 vNGeom;
in vec3 vPosition;
in vec4 vTangent;
in vec2 vBaseColorUV;
in vec2 vNormalUV;
in vec2 vOrmUV;
in vec2 vEmissiveUV;

layout(location = 0) out vec4 gAlbedo;   // rgb = albedo, a = ambient occlusion
layout(location = 1) out vec4 gNormal;   // rgb = view-space shading normal
layout(location = 2) out vec4 gMaterial; // r = roughness, g = metallic
layout(location = 3) out vec4 gEmissive; // rgb = emissive (HDR), a = alpha coverage
layout(location = 4) out vec4 gPosition; // rgb = view-space position

void main() {
	float alpha = osgx_gltf_AlphaCoverage(vBaseColorUV);

	if(osgx_gltf_alphaMode == 1.0 && alpha < osgx_gltf_alphaCutoff) discard;

	vec3 N = osgx_gltf_ShadingNormal(vNGeom, vTangent, vPosition, vNormalUV);
	osgx_Material mat = osgx_gltf_GetMaterial(vBaseColorUV, vOrmUV, N);

	gAlbedo = vec4(mat.albedo, mat.ao);
	gNormal = vec4(normalize(N), 0.0);
	gMaterial = vec4(mat.roughness, mat.metallic, 0.0, 0.0);
	gEmissive = vec4(osgx_gltf_Emissive(vEmissiveUV), alpha);
	// Real eye-space position, straight from the vertex shader - NOT reconstructed from depth
	// in the lighting pass (see PBRIBLGBuffer::positionTexture's comment in PBRIBL.hpp for why).
	gPosition = vec4(vPosition, 1.0);
}
)GLSL";

// Lighting-pass fragment shader for the deferred split (PBRIBLLightingScene::create() below) --
// runs the SAME osgx_EvaluateIBL() (osgx::ibl EVALUATE_IBL) as FULL_PBR_FRAGMENT_SHADER_SRC above,
// a real shared function since 2026-08-22 (formerly two independent copies of an unprefixed
// `evaluateIBL()` - see TODO.md's "Generic vs. glTF-specific layering" section for why that was
// worth fixing). The two call sites still look
// slightly different: this shader's own N/V are already world-space by the time they reach
// osgx_EvaluateIBL() (rotated below from the G-buffer's view-space channels), while
// FULL_PBR_FRAGMENT_SHADER_SRC rotates its own varying-derived N/V first, since
// osgx_EvaluateIBL() requires world-space input unconditionally and does no rotation itself.
// Plus osgx_DirectLighting(), reading G-buffer textures (position included - NOT reconstructed
// from depth; see PBRIBLGBuffer::positionTexture's comment) instead of interpolated per-vertex
// varyings. OSGX_PBRIBL_NO_TONEMAP/OSGX_PBRIBL_AO mirror PBRIBLLightingPassOptions::tonemap/
// aoTexture - see that struct's comment in PBRIBL.hpp for why each is an independent opt-out/
// opt-in rather than one flag.
constexpr const char LIGHTING_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core
#pragma import_defines ( OSGX_PBRIBL_DIAGNOSTICS, OSGX_PBRIBL_NO_TONEMAP, OSGX_PBRIBL_AO )

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, F_MULTISCATTER, SPECULAR_AA, TONEMAP_DECL, DIRECT_LIGHTING_DECL
#pragma osgx::ibl IBL_LIGHTING_INPUTS, EVALUATE_IBL
// DEFERRED_LIGHTING_INPUTS/GET_GBUFFER: the same osgx_GBuffer/osgx_GetGBuffer() an
// osgx::Hook::DeferredLighting override uses - this built-in default is deliberately not a
// hand-rolled special case, so the two stay provably equivalent decode paths.
#pragma osgx::gltf DEFERRED_LIGHTING_INPUTS, GET_GBUFFER

// Manually maintained every frame by PBRIBLLightingScene::update() - see that function's comment
// (and PBRIBLLightingScene::create()'s) for why this quad's own osg_ViewMatrix/osg_ViewMatrixInverse
// can't be trusted the way FULL_PBR_FRAGMENT_SHADER_SRC's forward-pass equivalents can. No
// projection-matrix uniform here - position comes straight from gPosition, not a depth
// reconstruction, so only the VIEW matrix (genuinely consistent across nested cameras) is needed.

#ifdef OSGX_PBRIBL_AO
uniform sampler2D aoTex;
#endif

void main() {
	osgx_GBuffer gb = osgx_GetGBuffer(vUV);

	// A cleared-but-never-written pixel has a zero-length normal - real geometry always writes
	// a normalized one. Cheaper and more robust than a separate coverage mask texture.
	if(dot(gb.normal, gb.normal) < 0.0001) discard;

	osgx_Material mat;

	mat.albedo = gb.albedo;
	mat.ao = gb.ao;
	mat.roughness = gb.roughness;
	mat.metallic = gb.metallic;
	mat.F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

#ifdef OSGX_PBRIBL_AO
	mat.ao *= texture(aoTex, vUV).r;
#endif

	// Specular AA (see PBR.hpp's SPECULAR_AA) works from screen-space derivatives, which are
	// available on a G-buffer texture sample exactly the same way they are on an interpolated
	// varying - sampling a neighboring fragment's own written normal here is the standard
	// deferred-renderer form of this technique, not an approximation of the forward-pass one.
	vec3 N_view_n = normalize(gb.normal);

	mat.roughness = osgx_SpecularAA(N_view_n, mat.roughness);

	vec3 V_view = normalize(-gb.position);

	mat3 invView = transpose(mat3(osgx_mainViewMatrix));
	vec3 N = invView * N_view_n;
	vec3 V = invView * V_view;
	vec3 worldPos = (osgx_mainViewMatrixInverse * vec4(gb.position, 1.0)).xyz;

	osgx_Lighting ambient = osgx_EvaluateIBL(mat, N, V);
	vec3 surface = ambient.diffuse + ambient.specular;
	vec3 direct = osgx_DirectLighting(N, V, worldPos, mat);

	vec3 color = surface + direct + gb.emissive;

#ifndef OSGX_PBRIBL_NO_TONEMAP
	color = osgx_Tonemap(color);
	color = pow(color, vec3(1.0 / 2.2));
#endif

	fragColor = vec4(color, gb.alphaCoverage);
}
)GLSL";

}

bool PBRIBLEnvironment::valid() const {
	return envMap.valid() && brdfLUT.valid() && diffuseEnv.valid();
}

bool PBRIBLScene::valid() const { return node.valid(); }

// One-call "get PBR/IBL going from scratch" against an already-loaded glTF node: applies the full
// PBR/IBL shader above (glTF material glue plus generic osgx::pbr/osgx::ibl, zero
// hand-copied GLSL), loads the prefiltered cubemap + bakes a Lambertian diffuse irradiance cubemap
// from the given paths, builds frame-driven GPU bakes for the Lambertian diffuse cubemap and BRDF
// LUT, and wires every uniform/texture unit the shader needs. `node` is modified in place
// (StateSet gets the Program + uniforms, OVERRIDE'd same as apply_gltf_fallback_pbr() in
// pyosg-voxelize.py); the caller owns adding the prepared environment root and returned scene
// node to the graph. `diagnostics=false` is the production path;
// setting it true compiles the debug channels and creates their uniforms. The three diagnostic
// uniform pointers are null in production.
//
// Fully dynamic overload: bakes the GGX-prefiltered specular cubemap live instead of loading one
// from a KTX2 file, calling the exact same osgx::GGXPrefilterScene::create() workflow
// osggltf-iblbake-gpu already wraps to write that KTX2 to disk in the first place - no readback,
// no CPU round-trip, no temporary file. GGXPrefilterScene::prefilterTexture is the live render-
// target cubemap the bake's PRE_RENDER passes write into; it is immediately valid to bind (same
// contract as the existing Lambertian diffuseEnv below), it just isn't correct until
// specularBakeRoot's passes have actually run a few frames of whatever viewer the caller adds
// environment.root to. GGXPrefilterReadback (glFinish-gated CPU readback) is deliberately unused
// here - that machinery exists only for osggltf-iblbake-gpu's serialize-to-KTX2 use case.
//
// prefilterSize=256 matches what this session's Khronos-parity comparisons actually validated
// (GGXPrefilterOptions's own default of 128 has not been checked against the reference); revisit
// once the mip-count/roughness-convention options from TODO.md's "Generic osgx and IBL later work"
// land; and see that same TODO entry for what is deliberately NOT yet handled here (a not-ready-
// yet texture read is undefined driver contents, not a defined placeholder value).
PBRIBLEnvironment PBRIBLEnvironment::prepare(const std::string& hdrPath, int lutSize) {
	PBRIBLEnvironment environment;

	auto hdrImage = osgDB::readRefImageFile(hdrPath);

	if(!hdrImage) return environment;

	auto lut = osgx::SharedBRDFLUT::create(lutSize);

	environment.brdfLUT = lut.texture;
	environment.lutCamera = lut.camera; // null once another environment already baked this size

	auto diffuseBake = osgx::LambertianBakeScene::create(hdrImage);

	environment.diffuseBakeRoot = diffuseBake.root;
	environment.diffuseEnv = diffuseBake.diffuseTexture;

	osgx::GGXPrefilterOptions specularOptions;

	specularOptions.prefilterSize = 256;

	auto specularBake = osgx::GGXPrefilterScene::create(hdrImage, specularOptions);

	environment.specularBakeRoot = specularBake.root;
	environment.envMap = specularBake.prefilterTexture;

	environment.root = osgx::make_ref<osg::Group>();

	if(environment.lutCamera) environment.root->addChild(environment.lutCamera);

	environment.root->addChild(environment.diffuseBakeRoot);
	environment.root->addChild(environment.specularBakeRoot);

	return environment;
}

// See this method's own header comment (PBRIBL.hpp) - identical to prepare() except it never
// constructs a GGXPrefilterScene at all, so hdrPath's specular content is never GPU-baked.
PBRIBLEnvironment PBRIBLEnvironment::prepareDiffuseOnly(const std::string& hdrPath, int lutSize) {
	PBRIBLEnvironment environment;

	auto hdrImage = osgDB::readRefImageFile(hdrPath);

	if(!hdrImage) return environment;

	auto lut = osgx::SharedBRDFLUT::create(lutSize);

	environment.brdfLUT = lut.texture;
	environment.lutCamera = lut.camera;

	auto diffuseBake = osgx::LambertianBakeScene::create(hdrImage);

	environment.diffuseBakeRoot = diffuseBake.root;
	environment.diffuseEnv = diffuseBake.diffuseTexture;

	// Placeholder only - see this method's own header comment for why envMap still needs to be a
	// valid, bindable texture despite never being baked here. 1x1 is enough: nothing samples it
	// before the caller's own live bake replaces it on texture unit 5.
	auto* placeholder = new osg::TextureCubeMap();

	placeholder->setTextureSize(1, 1);
	placeholder->setInternalFormat(GL_RGB32F);
	placeholder->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	placeholder->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	placeholder->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	placeholder->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	placeholder->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

	environment.envMap = placeholder;

	environment.root = osgx::make_ref<osg::Group>();

	if(environment.lutCamera) environment.root->addChild(environment.lutCamera);

	environment.root->addChild(environment.diffuseBakeRoot);

	return environment;
}

namespace {

// decodeString/decodeInt/JsonDocument/readWholeFile live in Json.hpp (shared with the other extension
// decoders in this directory).
// (Fully qualified: this namespace has its own `detail`, which would otherwise shadow osgx::gltf's.)
using ::osgx::gltf::detail::decodeInt;
using ::osgx::gltf::detail::decodeString;
using ::osgx::gltf::detail::JsonDocument;
using ::osgx::gltf::detail::readWholeFile;

IBLEnvironmentManifest decodeIBLEnvironment(const tg3json_value* entry) {
	IBLEnvironmentManifest manifest;

	if(!entry || entry->type != TG3JSON_OBJECT) return manifest;

	const tg3json_value* specular = tg3json_object_get(entry, "specular");

	manifest.specular.uri = decodeString(specular, "uri");
	manifest.specular.prefilterSize = decodeInt(specular, "prefilterSize", 0);
	manifest.specular.lowestMipLevel = decodeInt(specular, "lowestMipLevel", 0);
	manifest.diffuse.uri = decodeString(tg3json_object_get(entry, "diffuse"), "uri");

	const tg3json_value* brdfLUT = tg3json_object_get(entry, "brdfLUT");

	manifest.brdfLUT.uri = decodeString(brdfLUT, "uri");
	manifest.brdfLUT.builtin = decodeString(brdfLUT, "builtin");
	manifest.brdfLUT.size = decodeInt(brdfLUT, "size", 1024);

	return manifest;
}

}

std::vector<IBLEnvironmentManifest> decodeIBLEnvironments(const tg3json_value* extensionValue) {
	std::vector<IBLEnvironmentManifest> result;

	if(!extensionValue || extensionValue->type != TG3JSON_OBJECT) return result;

	const tg3json_value* environments = tg3json_object_get(extensionValue, "environments");

	if(!environments || environments->type != TG3JSON_ARRAY) return result;

	for(std::size_t i = 0; i < tg3json_array_size(environments); i++) result.push_back(
		decodeIBLEnvironment(tg3json_array_get(environments, i))
	);

	return result;
}

PBRIBLEnvironment PBRIBLEnvironment::load(const IBLEnvironmentManifest& manifest, const std::string& baseDir) {
	PBRIBLEnvironment environment;

	if(!manifest.specular.valid() || !manifest.diffuse.valid() || !manifest.brdfLUT.valid()) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: manifest is missing a required resource" << std::endl;

		return environment;
	}

	const std::filesystem::path base(baseDir);

	// loadPrefilterCubemap() is content-agnostic despite its name - it just loads a KTX2 as a
	// TextureCubeMap, which is equally correct for the Lambertian diffuse cube as for GGX specular.
	environment.envMap = osgx::loadPrefilterCubemap((base / manifest.specular.uri).string());

	if(!environment.envMap) return environment;

	environment.diffuseEnv = osgx::loadPrefilterCubemap((base / manifest.diffuse.uri).string());

	if(!environment.diffuseEnv) return environment;

	if(!manifest.brdfLUT.builtin.empty()) {
		if(manifest.brdfLUT.builtin != "osgx:split-sum-ggx-v1" || manifest.brdfLUT.size < 1) {
			OSG_WARN
				<< "osgx::gltf::pbribl::PBRIBLEnvironment::load: unsupported built-in BRDF LUT "
				<< manifest.brdfLUT.builtin << std::endl
			;

			return environment;
		}

		auto lut = osgx::SharedBRDFLUT::create(manifest.brdfLUT.size);

		environment.brdfLUT = lut.texture;
		environment.lutCamera = lut.camera;

		if(environment.lutCamera) {
			environment.root = osgx::make_ref<osg::Group>();
			environment.root->addChild(environment.lutCamera);
		}

		return environment;
	}

	auto lutImage = osgDB::readRefImageFile((base / manifest.brdfLUT.uri).string());

	if(!lutImage) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: failed to load " << manifest.brdfLUT.uri << std::endl;

		return environment;
	}

	environment.brdfLUT = osgx::make_ref<osg::Texture2D>();
	environment.brdfLUT->setImage(lutImage);
	environment.brdfLUT->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	environment.brdfLUT->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	environment.brdfLUT->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	environment.brdfLUT->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

	return environment;
}

PBRIBLEnvironment PBRIBLEnvironment::load(const std::string& manifestPath) {
	std::string text;

	if(!readWholeFile(manifestPath, text)) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: failed to read " << manifestPath << std::endl;

		return {};
	}

	JsonDocument document;

	if(!document.parse(text)) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: failed to parse " << manifestPath << std::endl;

		return {};
	}

	const tg3json_value* extensions = tg3json_object_get(&document.root(), "extensions");
	const tg3json_value* pbriblExtension = extensions
		? tg3json_object_get(extensions, "osgx_pbribl")
		: nullptr
	;

	if(!pbriblExtension) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: " << manifestPath << " has no osgx_pbribl extension" << std::endl;

		return {};
	}

	auto environments = decodeIBLEnvironments(pbriblExtension);

	if(environments.empty()) {
		OSG_WARN << "osgx::gltf::pbribl::PBRIBLEnvironment::load: " << manifestPath << " declares no environments" << std::endl;

		return {};
	}

	const std::string baseDir = std::filesystem::path(manifestPath).parent_path().string();
	const auto& manifest = environments.front();
	auto environment = PBRIBLEnvironment::load(manifest, baseDir);
	const std::string brdfLUT = !manifest.brdfLUT.uri.empty() ? manifest.brdfLUT.uri : manifest.brdfLUT.builtin;

	if(environment.valid()) {
		OSG_NOTICE
			<< "osgx::gltf::pbribl: loaded pre-baked environment manifest \"" << manifestPath
			<< "\" (specular=\"" << manifest.specular.uri
			<< "\", diffuse=\"" << manifest.diffuse.uri
			<< "\", brdfLUT=\"" << brdfLUT << "\")"
			<< std::endl
		;
	}

	return environment;
}

PBRIBLScene PBRIBLScene::create(
	osg::Node* node,
	const PBRIBLEnvironment& environment,
	float iblDiffuseIntensity,
	float iblSpecularIntensity,
	bool diagnostics,
	const osgx::ShadowMap* shadowMap,
	const osgx::HookList& hooks
) {
	// osgx::gltf::pbribl::resolveShaderLibs() below idempotently registers the generic osgx catalogs and
	// this component's glTF catalog before expansion. This keeps the one-call helper independent of
	// module-import order and avoids handing raw, uncompilable pragma text to the driver.
	PBRIBLScene pis;

	if(!node || !environment.valid()) return pis;

	pis.node = node;

	auto* ss = node->getOrCreateStateSet();
	auto prog = osgx::make_nref<osg::Program>("osgx_gltf_PBRIBLScene");

	// The glTF loader stores glTF's optional TANGENT accessor in generic vertex attribute 7.
	// Bind it before linking, exactly as pyosg-khronos-viewer.py does; otherwise GLSL may
	// assign osg_Tangent to another generic attribute and normal mapping reads a default value.
	shader::configureProgram(*prog);

	auto* vertexShader = new osg::Shader(osg::Shader::VERTEX, detail::FULL_PBR_VERTEX_SHADER);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	auto* fragmentShader = new osg::Shader(
		osg::Shader::FRAGMENT,
		resolveShaderLibs(detail::FULL_PBR_FRAGMENT_SHADER_SRC)
	);

	fragmentShader->setName(prog->getName() + ".fragment");
	prog->addShader(fragmentShader);

	// osgx_DirectLighting() CONTRACT's definition - a second, separately compiled FRAGMENT shader
	// object with no main() of its own; GLSL cross-shader-object linking resolves the
	// FULL_PBR_FRAGMENT_SHADER_SRC's forward-declared osgx_DirectLighting() call against this at
	// Program-link time. See PBR.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT comment.
	// `shadowMap` swaps in the shadowed variant (Shadow.hpp's DIRECT_LIGHTING_HOOK_SHADOWED) --
	// same contract/call signature, so nothing else in this shader changes either way. Not routed
	// through applyHooks() (unlike Skinning/Tonemap just below) - `shadowMap` picks between two
	// built-ins directly, there's no caller-override HookList slot for it here - so it needs its
	// own explicit setName() rather than picking one up from applyHooks() automatically.
	auto* directLightingShader = new osg::Shader(
		osg::Shader::FRAGMENT,
		resolveShaderLibs(
			shadowMap ? osgx::DIRECT_LIGHTING_HOOK_SHADOWED : osgx::DIRECT_LIGHTING_HOOK_DEFAULT
		)
	);

	directLightingShader->setName(prog->getName() + ".directLightingHook");
	prog->addShader(directLightingShader);
	// osgx_gltf_ApplySkin()/osgx_Tonemap() CONTRACTS' default definitions - each a separately
	// compiled shader object with no main() of its own. See PBR.hpp's TONEMAP_DECL/
	// TONEMAP_HOOK_DEFAULT comment and Shader.hpp's SKINNING_HOOK_IDENTITY/SKINNING_HOOK_LINEAR_BLEND
	// comment for the full rationale. osgx_EvaluateIBL()'s own diffuse/specular blend above is NOT
	// routed through osgx_AmbientLighting() - that hook's default is specular-only (no SH-9
	// diffuse yet), while this scene already has a real baked Lambertian diffuseEnv cubemap; there
	// is no AmbientLighting hook slot here as a result. `hooks` SUBSTITUTES a slot's default shader
	// object, it is never attached alongside it - see applyHooks()'s own comment (Shader.hpp) for
	// the exactly-one-definition invariant this preserves, same one
	// PBRIBLLightingScene::create() relies on at its own tonemap attach site.
	osgx::applyHooks(prog, hooks, {
		{osgx::Hook::Skinning, new osg::Shader(
			osg::Shader::VERTEX,
			resolveShaderLibs(shader::SKINNING_HOOK_IDENTITY)
		)},
		{osgx::Hook::Tonemap, new osg::Shader(
			osg::Shader::FRAGMENT,
			resolveShaderLibs(osgx::TONEMAP_HOOK_DEFAULT)
		)}
	});

	// resolveShaderLibs() expands the osgx snippet pragmas but preserves OSG's
	// import_defines pragma. Let OSG assemble/cache the diagnostic shader variant
	// from render state so it can place the generated define safely after #version.
	if(diagnostics) ss->setDefine("OSGX_PBRIBL_DIAGNOSTICS");

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
	ss->setTextureAttributeAndModes(5, environment.envMap, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(6, environment.brdfLUT, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(7, environment.diffuseEnv, osg::StateAttribute::ON);
	ss->addUniform(new osg::Uniform("envMap", 5));
	ss->addUniform(new osg::Uniform("brdfLUT", 6));
	ss->addUniform(new osg::Uniform("diffuseEnv", 7));
	pis.iblDiffuseIntensity = new osg::Uniform("iblDiffuseIntensity", iblDiffuseIntensity);
	pis.iblSpecularIntensity = new osg::Uniform("iblSpecularIntensity", iblSpecularIntensity);
	ss->addUniform(pis.iblDiffuseIntensity);
	ss->addUniform(pis.iblSpecularIntensity);

	// Shadow: unit 4 - matches the fixed unit the old hand-rolled pyosg-lighting examples always
	// used for their own shadowMap sampler (0-3 are glTF material, 5/6/7 are IBL above), kept here
	// purely for continuity, not a hard requirement of anything else in this shader.
	if(shadowMap) {
		ss->setTextureAttributeAndModes(4, shadowMap->depthTexture, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("osgx_shadowMap", 4));
		ss->addUniform(shadowMap->shadowMatrix);
		ss->addUniform(shadowMap->bias);
		ss->addUniform(shadowMap->strength);
		ss->addUniform(shadowMap->casterIndex);
	}

	// foldZUpToGLTFAxis() (PBRIBL.hpp) - pre-folds osgx_ZUpToGLTF's fixed permutation into each
	// row here, once, so the shader's osgx_OrientIBL() can be called directly on a raw Z-up
	// N_world/R at its real lighting call sites instead of composing two transforms per fragment.
	auto* iblAxis = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "iblAxis", 3);

	for(size_t i = 0; i < environment.iblAxis.size(); i++) {
		iblAxis->setElement(
			static_cast<unsigned int>(i), foldZUpToGLTFAxis(environment.iblAxis[i])
		);
	}

	ss->addUniform(iblAxis);

	// The glTF Material helper binds the actual baseColor/normal/orm/emissive Texture2Ds to units
	// 0-3 per geometry, but deliberately stays shader-agnostic
	// and never sets the sampler *uniforms* that tell osgx_gltf_textures which unit is which --
	// that's the shader glue's job (see MATERIAL_INPUTS's "unit N" comments above). Without this,
	// every sampler in the GLTFTextures struct silently defaults to unit 0 per the GLSL spec, so
	// normal/orm/emissive all end up reading the baseColor texture instead - corrupted shading
	// normals, scrambled roughness/metallic, and the whole baseColor image re-added as fake
	// "emissive" light. Same fix pyosg-khronos-viewer.py applies via its own uniforms.update(...).
	shader::configureStateSet(*ss);

	if(diagnostics) {
		pis.debugMode = new osg::Uniform("debugMode", 0);
		pis.disableNormalMap = new osg::Uniform("disableNormalMap", 0);
		pis.disableRoughnessMap = new osg::Uniform("disableRoughnessMap", 0);
		pis.disableSpecularAA = new osg::Uniform("disableSpecularAA", 0);

		ss->addUniform(pis.debugMode);
		ss->addUniform(pis.disableNormalMap);
		ss->addUniform(pis.disableRoughnessMap);
		ss->addUniform(pis.disableSpecularAA);
	}

	ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);

	return pis;
}

bool PBRIBLGBuffer::valid() const {
	return gbuffer.valid()
		&& albedoTexture.valid()
		&& normalTexture.valid()
		&& materialTexture.valid()
		&& emissiveTexture.valid()
		&& positionTexture.valid()
		&& depthTexture.valid()
	;
}

PBRIBLGBuffer PBRIBLGBuffer::create(osg::Node* node, int width, int height) {
	PBRIBLGBuffer result;

	if(!node) return result;

	auto prog = osgx::make_nref<osg::Program>("osgx_gltf_PBRIBLGeometryPass");

	shader::configureProgram(*prog);

	auto* vertexShader = new osg::Shader(osg::Shader::VERTEX, detail::FULL_PBR_VERTEX_SHADER);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	// osgx_gltf_ApplySkin() CONTRACT's default (identity) definition - FULL_PBR_VERTEX_SHADER
	// always calls this hook now (see PBRIBLScene::create()'s own comment for the full mechanism).
	// This geometry pass doesn't expose a `hooks` parameter of its own yet (see TODO.md), so it
	// always gets the passthrough - a skinned model through the deferred split still animates
	// its joint transforms/gizmos correctly, it just won't deform the G-buffer mesh yet. Not
	// routed through applyHooks() (no `hooks` parameter here to substitute it), hence the
	// explicit setName() rather than picking one up automatically the way PBRIBLScene::create()'s
	// own Skinning/Tonemap hooks do.
	auto* skinningShader = new osg::Shader(
		osg::Shader::VERTEX,
		resolveShaderLibs(shader::SKINNING_HOOK_IDENTITY)
	);

	skinningShader->setName(prog->getName() + ".skinningHook");
	prog->addShader(skinningShader);

	auto* fragmentShader = new osg::Shader(
		osg::Shader::FRAGMENT,
		resolveShaderLibs(detail::GBUFFER_FRAGMENT_SHADER_SRC)
	);

	fragmentShader->setName(prog->getName() + ".fragment");
	prog->addShader(fragmentShader);

	auto* ss = node->getOrCreateStateSet();

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

	// Same texture-unit-labeling fix PBRIBLScene::create() needs - the loader binds the actual
	// Texture2Ds per geometry but never sets the sampler uniforms naming which unit is which.
	shader::configureStateSet(*ss);

	static constexpr osgx::AttachmentFormat formats[] = {
		osgx::AttachmentFormat::RGBA8,   // gAlbedo
		osgx::AttachmentFormat::RGB16F,  // gNormal (signed, view-space)
		osgx::AttachmentFormat::RGBA8,   // gMaterial
		osgx::AttachmentFormat::RGBA16F, // gEmissive (HDR)
		osgx::AttachmentFormat::RGBA32F  // gPosition (view-space, real precision needed)
	};

	result.gbuffer = osgx::GBuffer::create(node, width, height, formats);

	if(!result.gbuffer.valid()) return result;

	result.albedoTexture = result.gbuffer.colorTextures[0];
	result.normalTexture = result.gbuffer.colorTextures[1];
	result.materialTexture = result.gbuffer.colorTextures[2];
	result.emissiveTexture = result.gbuffer.colorTextures[3];
	result.positionTexture = result.gbuffer.colorTextures[4];
	result.depthTexture = result.gbuffer.depthTexture;

	return result;
}

bool PBRIBLLightingScene::valid() const { return node.valid(); }

// The fullscreen quad's own camera is necessarily ABSOLUTE_RF/identity-view/identity-projection
// (that's what makes an NDC quad cover the screen) - OSG's automatic osg_ViewMatrix therefore
// resolves to identity on it, not `mainCamera`'s real matrices. Rather than fight that (as
// PBRIBLScene::create()'s forward-pass shader can, by simply living on real scene geometry under
// the real camera), this pass carries its own osgx_mainViewMatrix/osgx_mainViewMatrixInverse
// uniforms, populated once here and kept current by PBRIBLLightingScene::update() every frame --
// the same fix 11-sketchfab.py's own Increment 1 needed for exactly this reason. There is
// deliberately NO projection-matrix uniform: an earlier version reconstructed view-space
// position from gDepth + an inverse projection matrix here, which turned out to be unreliable
// - each nested PRE_RENDER camera (this lighting pass, the geometry pass, mainCamera itself)
// clamps its own PRIVATE per-camera projection copy during its own cull pass and never writes
// that clamped result back onto the Camera object, so a projection matrix read off `mainCamera`
// after the fact does not reliably match what the geometry pass actually used to write depth --
// confirmed as the real cause of a live position/shadow artifact that visibly worsened while
// orbiting the camera. `gPosition` (PBRIBLGBuffer) sidesteps the problem entirely by writing
// real position straight from the vertex shader instead; only the VIEW matrix is reconstructed
// here now, and that one genuinely is shared correctly across RELATIVE_RF-nested cameras.
PBRIBLLightingScene PBRIBLLightingScene::create(
	const PBRIBLGBuffer& gbuffer,
	const PBRIBLEnvironment& environment,
	osg::Camera* mainCamera,
	float iblDiffuseIntensity,
	float iblSpecularIntensity,
	const PBRIBLLightingPassOptions& options
) {
	PBRIBLLightingScene result;

	if(!gbuffer.valid() || !mainCamera) return result;

	auto prog = osgx::make_nref<osg::Program>("osgx_gltf_PBRIBLLightingPass");

	auto* vertexShader = new osg::Shader(osg::Shader::VERTEX, osgx::FULLSCREEN_VERT);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	// EXACTLY ONE definition each of osgx_DirectLighting(), osgx_Tonemap(), and main() (the
	// DeferredLighting slot), always - never zero, never two. applyHooks() (Shader.hpp) enforces
	// this: it always attaches one shader per slot below, the caller's options.hooks override if
	// present, otherwise the built-in.
	//
	// DeferredLighting's built-in default IS this pass's own main() (detail::
	// LIGHTING_FRAGMENT_SHADER_SRC) - an override REPLACES the whole lighting orchestration, not
	// one leaf function, so a caller supplying one is free to ignore osgx_DirectLighting()/
	// osgx_Tonemap() (still harmlessly attached below, unused-but-defined is not a GLSL error) and
	// read the G-buffer via osgx_GetGBuffer() (#pragma osgx::gltf GET_GBUFFER) instead. See
	// Shader.hpp's own Hook::DeferredLighting comment for why this is pass-specific rather than a
	// generically-named slot.
	//
	// DirectLighting: was attached unconditionally here, OUTSIDE applyHooks() entirely, until this
	// collided with a real DeferredLighting override that also wanted the same low-level BRDF
	// primitives (D_GGX/G_Schlick/G_Smith/F_Schlick/DirectSpecular/DirectDiffuse/DirectLight) for
	// its own use - DIRECT_LIGHTING_HOOK_DEFAULT/_SHADOWED pull those in via their own `#pragma
	// osgx::pbr` to implement osgx_DirectLighting() itself, so two shader objects ended up defining
	// the same GLSL functions (a link error). Now a real slot - see Hook::DirectLighting's own
	// comment (Shader.hpp) for the full incident.
	//
	// Tonemap: never zero because OSGX_PBRIBL_NO_TONEMAP strips the CALL at render time, but that
	// define is absent during OSG's realize-time GLObjectsVisitor pre-compile, which would then
	// link a call with no definition. See TONEMAP_HOOK_IDENTITY's comment in PBR.hpp for the full
	// mechanism - the rule is that a #define may gate a call, but must never be the only thing
	// making a function exist. Never two because GLSL allows one body per function, so a caller
	// cannot "override" by adding a second shader defining osgx_Tonemap() - that is a
	// duplicate-definition link error. Which is exactly why options.hooks exists: customization
	// SUBSTITUTES a slot's shader rather than competing with it.
	osgx::applyHooks(prog, options.hooks, {
		{osgx::Hook::DeferredLighting, new osg::Shader(
			osg::Shader::FRAGMENT,
			resolveShaderLibs(detail::LIGHTING_FRAGMENT_SHADER_SRC)
		)},
		{osgx::Hook::DirectLighting, new osg::Shader(
			osg::Shader::FRAGMENT,
			resolveShaderLibs(
				options.shadowMap
					? osgx::DIRECT_LIGHTING_HOOK_SHADOWED
					: osgx::DIRECT_LIGHTING_HOOK_DEFAULT
			)
		)},
		{osgx::Hook::Tonemap, new osg::Shader(
			osg::Shader::FRAGMENT,
			resolveShaderLibs(
				options.tonemap ? osgx::TONEMAP_HOOK_DEFAULT : osgx::TONEMAP_HOOK_IDENTITY
			)
		)}
	});

	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0)
	);
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(quad);

	auto cam = osgx::make_nref<osg::Camera>("osgx_gltf_PBRIBLLightingPass");

	cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	cam->setProjectionMatrix(osg::Matrix::identity());
	cam->setViewMatrix(osg::Matrix::identity());
	cam->addChild(geode);

	// POST_RENDER, drawing to whatever framebuffer this camera ends up under (the backbuffer, if
	// added directly to the viewer's scene graph) - this pass is always the pipeline's terminal
	// step, matching options.tonemap's own default (true). No FBO, so this is deliberately a plain
	// osg::Camera rather than an osgx::RTT (whose contract assumes an FBO attachment).
	cam->setRenderOrder(osg::Camera::POST_RENDER);
	cam->setClearMask(0);

	auto* ss = cam->getOrCreateStateSet();

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
	// This quad covers every pixel unconditionally and resolves the whole frame's shading - there
	// is nothing for it to be depth-tested AGAINST, so it has to own its depth state rather than
	// inherit whatever the surrounding framebuffer happens to be carrying.
	//
	// Leaving this to ambient state worked only by accident, and only when this camera drew to the
	// backbuffer: the main camera clears depth to 1.0 every frame, so the quad passed GL_LESS. The
	// moment a caller hand-retargets this camera to an FBO (attach() + PRE_RENDER, to chain
	// bloom/exposure after it - not a built-in option here), OSG attaches an IMPLICIT depth
	// renderbuffer to that FBO
	// (DisplaySettings::DEFAULT_IMPLICIT_BUFFER_ATTACHMENT includes IMPLICIT_DEPTH_BUFFER_ATTACHMENT;
	// see RenderStage.cpp's own implicit-attachment block) which nothing ever clears - undefined
	// depth, every fragment of this quad discarded, and the attachment left holding nothing but the
	// clear color. That failure is completely silent: the FBO is valid, the viewport is right, the
	// draw call is issued, and the output is a single flat color, which reads as a broken shader
	// rather than a depth-test rejection. Diagnosed exactly that way (osgx-gbuffer --rtt) after it
	// cost a full session in pyosg-lighting/11-sketchfab.py.
	//
	// GL_DEPTH_TEST OFF also disables depth writes, so no osg::Depth attribute is needed alongside.
	ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
	ss->setTextureAttributeAndModes(0, gbuffer.albedoTexture, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(1, gbuffer.normalTexture, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(2, gbuffer.materialTexture, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(3, gbuffer.emissiveTexture, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(4, gbuffer.positionTexture, osg::StateAttribute::ON);
	ss->addUniform(new osg::Uniform("gAlbedo", 0));
	ss->addUniform(new osg::Uniform("gNormal", 1));
	ss->addUniform(new osg::Uniform("gMaterial", 2));
	ss->addUniform(new osg::Uniform("gEmissive", 3));
	ss->addUniform(new osg::Uniform("gPosition", 4));

	// `environment` is now OPTIONAL - a caller whose osgx::Hook::DeferredLighting override doesn't
	// need osgx_EvaluateIBL() (see that Hook's own comment, Shader.hpp - a custom override CAN
	// still pull it in via `#pragma osgx::ibl EVALUATE_IBL`, it just isn't required to) has no use
	// for a baked/loaded IBL environment at all, and forcing one anyway meant a full HDR bake or
	// KTX2 load purely to populate textures nothing ever samples. Skip binding envMap/brdfLUT/
	// diffuseEnv/iblAxis entirely when environment.valid() is false, rather than binding null
	// texture refs.
	if(environment.valid()) {
		ss->setTextureAttributeAndModes(5, environment.envMap, osg::StateAttribute::ON);
		ss->setTextureAttributeAndModes(6, environment.brdfLUT, osg::StateAttribute::ON);
		ss->setTextureAttributeAndModes(7, environment.diffuseEnv, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("envMap", 5));
		ss->addUniform(new osg::Uniform("brdfLUT", 6));
		ss->addUniform(new osg::Uniform("diffuseEnv", 7));

		auto* iblAxis = new osg::Uniform(osg::Uniform::FLOAT_VEC3, "iblAxis", 3);

		for(std::size_t i = 0; i < environment.iblAxis.size(); i++) {
			iblAxis->setElement(
				static_cast<unsigned int>(i), foldZUpToGLTFAxis(environment.iblAxis[i])
			);
		}

		ss->addUniform(iblAxis);
	}

	result.iblDiffuseIntensity = new osg::Uniform("iblDiffuseIntensity", iblDiffuseIntensity);
	result.iblSpecularIntensity = new osg::Uniform("iblSpecularIntensity", iblSpecularIntensity);
	ss->addUniform(result.iblDiffuseIntensity);
	ss->addUniform(result.iblSpecularIntensity);

	result.mainViewMatrix = new osg::Uniform("osgx_mainViewMatrix", osg::Matrixf::identity());
	result.mainViewMatrixInverse = new osg::Uniform(
		"osgx_mainViewMatrixInverse", osg::Matrixf::identity()
	);
	ss->addUniform(result.mainViewMatrix);
	ss->addUniform(result.mainViewMatrixInverse);

	// A caller-supplied hook takes precedence over `tonemap`: supplying a curve means wanting it to
	// run, so the call (and the gamma encode this define also gates) stays in.
	if(!options.tonemap && !osgx::hasHook(options.hooks, osgx::Hook::Tonemap)) {
		ss->setDefine("OSGX_PBRIBL_NO_TONEMAP");
	}
	if(options.diagnostics) ss->setDefine("OSGX_PBRIBL_DIAGNOSTICS");

	if(options.aoTexture) {
		ss->setTextureAttributeAndModes(8, options.aoTexture, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("aoTex", 8));
		ss->setDefine("OSGX_PBRIBL_AO");
	}

	if(options.shadowMap) {
		ss->setTextureAttributeAndModes(9, options.shadowMap->depthTexture, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("osgx_shadowMap", 9));
		ss->addUniform(options.shadowMap->shadowMatrix);
		ss->addUniform(options.shadowMap->bias);
		ss->addUniform(options.shadowMap->strength);
		ss->addUniform(options.shadowMap->casterIndex);
	}

	ss->setMode(GL_TEXTURE_CUBE_MAP_SEAMLESS, osg::StateAttribute::ON);

	result.node = cam;

	result.update(mainCamera);

	return result;
}

void PBRIBLLightingScene::update(const osg::Camera* mainCamera) {
	if(!valid() || !mainCamera) return;

	if(mainViewMatrix) {
		mainViewMatrix->set(osg::Matrixf(mainCamera->getViewMatrix()));
	}

	if(mainViewMatrixInverse) {
		mainViewMatrixInverse->set(osg::Matrixf(mainCamera->getInverseViewMatrix()));
	}
}

}
