#include "osgx/PBRScene.hpp"
#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/Light.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Skinning.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

namespace osgx {

namespace {

// Complete fragment shader, paired with PBR_VERTEX_SHADER and resolved via resolveShaderLibs()
// inside PBRScene::create() below.
constexpr const char FULL_PBR_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core
#pragma import_defines ( OSGX_PBR_DIAGNOSTICS, OSGX_PBR_ENVIRONMENT )

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, MATERIAL_INPUTS, GET_MATERIAL, GET_SHADING_NORMAL, GET_EMISSIVE, GET_ALPHA, F_MULTISCATTER, SPECULAR_AA, TONEMAP_DECL
#pragma osgx::light DIRECT_LIGHTING_DECL
#ifdef OSGX_PBR_ENVIRONMENT
#pragma osgx::environment ENVIRONMENT_INPUTS, ENVIRONMENT_SAMPLE, ENVIRONMENT_LIGHTING
#endif

in vec3 vNGeom;
in vec3 vPosition;
in vec4 vTangent;
in vec2 vBaseColorUV;
in vec2 vNormalUV;
in vec2 vOrmUV;
in vec2 vEmissiveUV;

uniform mat4 osg_ViewMatrix;
uniform mat4 osg_ViewMatrixInverse;

#ifdef OSGX_PBR_DIAGNOSTICS
// Runtime isolation, ported from OpenSceneGraph.py/pyosg-khronos-viewer.py's Diagnostics
// handler - lets a caller (see osgx-gltf-viewer.cpp) key-toggle which term is actually
// contributing to a surface, useful for isolating why a render looks wrong. debugMode:
// 0=combined, 1=diffuse only, 2=specular only.
uniform int debugMode;
uniform int disableNormalMap;
uniform int disableRoughnessMap;
uniform int disableSpecularAA;
#endif

out vec4 fragColor;

// Used only by the OSGX_PBR_DIAGNOSTICS debug-normal visualizations below; environment lookups
// go through osgx::Environment's own axis (osgx_EnvironmentDirection()).
vec3 osgx_ZUpToGLTF(vec3 d) { return vec3(d.x, d.z, -d.y); }
vec3 osgx_LinearToSRGB(vec3 c) {
	return mix(12.92 * c, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
		step(vec3(0.0031308), c));
}

void main() {
	float alpha = osgx_GetAlpha(vBaseColorUV);

	if(osgx_materialInputs.alphaMode == OSGX_ALPHA_MODE_MASK && alpha < osgx_materialInputs.alphaCutoff) discard;

	vec3 N = osgx_GetShadingNormal(vNGeom, vTangent, vPosition, vNormalUV);

#ifdef OSGX_PBR_DIAGNOSTICS
	if(disableNormalMap != 0) N = normalize(vNGeom);
#endif
	vec3 V = normalize(-vPosition);
	osgx_Material mat = osgx_GetMaterial(vBaseColorUV, vOrmUV);


#ifdef OSGX_PBR_DIAGNOSTICS
	if(disableRoughnessMap != 0) mat.roughness = osgx_materialInputs.roughnessFactor;

	// Match pyosg-khronos-viewer.py's material/coordinate diagnostics. These deliberately return
	// before lighting so a channel can be compared without IBL, Fresnel, or tonemapping involved.
	mat3 invView = transpose(mat3(osg_ViewMatrix));
	vec3 NgeomWorld = normalize(invView * vNGeom);
	vec3 Nworld = normalize(invView * N);

	if(debugMode == 3) { fragColor = vec4(osgx_LinearToSRGB(mat.albedo), alpha); return; }
	if(debugMode == 4) { fragColor = vec4(vec3(mat.roughness), alpha); return; }
	if(debugMode == 5) { fragColor = vec4(vec3(mat.metallic), alpha); return; }
	if(debugMode == 6) {
		vec3 raw = texture(osgx_normalMap, vNormalUV).rgb;
		fragColor = vec4(bool(osgx_materialInputs.hasNormalMap) ? normalize(raw * 2.0 - 1.0) * 0.5 + 0.5 : vec3(1.0), alpha);
		return;
	}
	if(debugMode == 7) {
		fragColor = vec4(
			bool(osgx_materialInputs.hasNormalMap) ? texture(osgx_normalMap, vNormalUV).rgb : vec3(1.0),
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

#ifdef OSGX_PBR_DIAGNOSTICS
	if(disableSpecularAA == 0) mat.roughness = aaRoughness;
#else
	mat.roughness = aaRoughness;
#endif

	// World-space N/V, shared by osgx_EvaluateEnvironment() and osgx_DirectLighting() below - both
	// take world-space input. Named distinctly from the diagnostics block's own invView/Nworld above
	// so both compile together regardless of OSGX_PBR_DIAGNOSTICS.
	mat3 invViewRot = transpose(mat3(osg_ViewMatrix));
	vec3 N_world = invViewRot * N;
	vec3 V_world = invViewRot * V;

	vec3 ambientDiffuse = vec3(0.0);
	vec3 ambientSpecular = vec3(0.0);

#ifdef OSGX_PBR_ENVIRONMENT
	osgx_EnvironmentLight ambient = osgx_EvaluateEnvironment(mat, N_world, V_world);

	ambientDiffuse = ambient.diffuse;
	ambientSpecular = ambient.specular;
#endif

	vec3 surface = ambientDiffuse + ambientSpecular;
	vec3 emissive = osgx_GetEmissive(vEmissiveUV);

#ifdef OSGX_PBR_DIAGNOSTICS
	surface = (debugMode == 1 || debugMode == 12)
		? ambientDiffuse
		: (debugMode == 2 || debugMode == 13) ? ambientSpecular : ambientDiffuse + ambientSpecular
	;
	emissive = (debugMode == 0 || debugMode == 14) ? emissive : vec3(0.0);
#endif

	// Direct/punctual lights: point, directional, and spot, e.g. a torch or a sun - the
	// osgx_DirectLighting() CONTRACT (see the comment above this shader) does the per-light dispatch;
	// this shader only supplies N/V/worldPos/mat. Reuses N_world/V_world computed above.
	vec3 worldPos = (osg_ViewMatrixInverse * vec4(vPosition, 1.0)).xyz;
	vec3 direct = osgx_DirectLighting(N_world, V_world, worldPos, mat);

	vec3 color = surface + direct + emissive;

#ifdef OSGX_PBR_DIAGNOSTICS
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

}

bool PBRScene::valid() const { return node.valid(); }

PBRScene PBRScene::create(osg::Node* node, const PBRSceneOptions& options) {
	PBRScene pis;

	if(!node) return pis;

	const auto* shadowMap = options.shadowMap;

	pis.node = node;

	auto* ss = node->getOrCreateStateSet();
	auto prog = osgx::make_nref<osg::Program>("osgx_PBRScene");

	// Bind osg_Tangent (and the joint attributes) to their locations before linking; otherwise GLSL
	// may assign osg_Tangent to another generic attribute and normal mapping reads a default value.
	osgx::bindMeshAttributes(*prog);

	auto* vertexShader = new osg::Shader(
		osg::Shader::VERTEX,
		resolveShaderLibs(PBR_VERTEX_SHADER)
	);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	auto* fragmentShader = new osg::Shader(
		osg::Shader::FRAGMENT,
		resolveShaderLibs(FULL_PBR_FRAGMENT_SHADER_SRC)
	);

	fragmentShader->setName(prog->getName() + ".fragment");
	prog->addShader(fragmentShader);

	// osgx_DirectLighting() CONTRACT's definition - a second, separately compiled FRAGMENT shader
	// object with no main() of its own; GLSL cross-shader-object linking resolves the
	// FULL_PBR_FRAGMENT_SHADER_SRC's forward-declared osgx_DirectLighting() call against this at
	// Program-link time. See Light.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT comment.
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
	// osgx_ApplySkin()/osgx_Tonemap() CONTRACTS' default definitions - each a separately
	// compiled shader object with no main() of its own. See PBR.hpp's TONEMAP_DECL/
	// TONEMAP_HOOK_DEFAULT comment and Skinning.hpp for the full rationale. `hooks` SUBSTITUTES a
	// slot's default shader object, it is never attached alongside it - see applyHooks()'s own
	// comment (Shader.hpp) for the exactly-one-definition invariant this preserves, the same one
	// osgx::PBRLightingPass::create() relies on.
	osgx::applyHooks(prog, options.hooks, {
		{osgx::Hook::Skinning, new osg::Shader(
			osg::Shader::VERTEX,
			resolveShaderLibs(osgx::SKINNING_HOOK_IDENTITY)
		)},
		{osgx::Hook::Tonemap, new osg::Shader(
			osg::Shader::FRAGMENT,
			resolveShaderLibs(osgx::TONEMAP_HOOK_DEFAULT)
		)}
	});

	// resolveShaderLibs() preserves OSG's import_defines pragma; OSG builds each define variant from
	// render state and places the generated defines after #version.
	if(options.diagnostics) ss->setDefine("OSGX_PBR_DIAGNOSTICS");

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

	if(options.environment) {
		pis.environment = options.environment;

		ss->setAttributeAndModes(options.environment);
		ss->setDefine("OSGX_PBR_ENVIRONMENT");
	}

	if(shadowMap) {
		const auto unit = osgx::Library::instance().bindings().get("osgx::shadowMap");

		ss->setTextureAttributeAndModes(unit, shadowMap->depthTexture, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform("osgx_shadowMap", static_cast<int>(unit)));
		ss->addUniform(shadowMap->shadowMatrix);
		ss->addUniform(shadowMap->bias);
		ss->addUniform(shadowMap->strength);
		ss->addUniform(shadowMap->casterIndex);
	}

	if(options.diagnostics) {
		pis.debugMode = new osg::Uniform("debugMode", 0);
		pis.disableNormalMap = new osg::Uniform("disableNormalMap", 0);
		pis.disableRoughnessMap = new osg::Uniform("disableRoughnessMap", 0);
		pis.disableSpecularAA = new osg::Uniform("disableSpecularAA", 0);

		ss->addUniform(pis.debugMode);
		ss->addUniform(pis.disableNormalMap);
		ss->addUniform(pis.disableRoughnessMap);
		ss->addUniform(pis.disableSpecularAA);
	}

	return pis;
}

}
