#include "ShaderLibs.hpp"

#include "osgx/PBRDeferred.hpp"
#include "osgx/Core.hpp"
#include "osgx/IBL.hpp"
#include "osgx/Library.hpp"
#include "osgx/Light.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Skinning.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

namespace osgx {

namespace {

// The minimum declarations ANY osgx::Hook::DeferredLighting override needs against
// PBRLightingPass::create()'s fullscreen quad - the five G-buffer sampler uniforms
// PBRGBuffer::create() writes (view-space normal/position, NOT world-space - see
// PBRGBuffer's own field comments), the view-matrix uniforms PBRLightingPass::update()
// keeps fresh every frame (needed to rotate view-space normal/position into world space; see that
// function's own comment for why this quad's own osg_ViewMatrix can't be trusted), `vUV`, and the
// pass's single color output. Deliberately does NOT include the IBL environment uniforms
// (osgx::Environment's ENVIRONMENT_INPUTS) or `aoTex` --
// those are specific to callers still wanting the real osgx_EvaluateEnvironment() path (like
// LIGHTING_FRAGMENT_SHADER_SRC's own built-in default below), not universal G-buffer boilerplate
// every override needs; a caller that wants them adds `#pragma osgx::environment ...` itself.
constexpr const char DEFERRED_LIGHTING_INPUTS[] = R"GLSL(
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

// Structured decode of PBRGBuffer::create()'s fixed 5-channel layout, the same "struct +
// osgx_GetX(uv)" shape osgx_GetMaterial() (PBR.hpp) uses for MATERIAL_INPUTS - lets an
// osgx::Hook::DeferredLighting override read `gb.albedo`/`gb.normal`/etc. instead of hand-sampling
// five textures and unpacking channels itself. `normal`/`position` stay VIEW-space, exactly as
// PBRGBuffer writes them (see PBRGBuffer::normalTexture/positionTexture's own comments in
// PBRDeferred.hpp for why) - rotate into world space via osgx_mainViewMatrixInverse only if the
// override actually needs it. Requires DEFERRED_LIGHTING_INPUTS already in scope.
constexpr const char GET_GBUFFER[] = R"GLSL(
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

// Geometry-pass fragment shader (PBRGBuffer::create() below): material only, no lighting, not even
// the emissive add (emissive is stored). Paired with PBR_VERTEX_SHADER (PBR.hpp), whose view-space
// position/normal/tangent and per-map UV varyings are what osgx_GetShadingNormal()/
// osgx_GetMaterial() take; gNormal stores the same view-space convention.
constexpr const char GBUFFER_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core

#pragma osgx::pbr MATERIAL_STRUCT, MATERIAL_INPUTS, GET_MATERIAL, GET_SHADING_NORMAL, GET_EMISSIVE, GET_ALPHA

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
	float alpha = osgx_GetAlpha(vBaseColorUV);

	if(osgx_materialInputs.alphaMode == OSGX_ALPHA_MODE_MASK && alpha < osgx_materialInputs.alphaCutoff) discard;

	vec3 N = osgx_GetShadingNormal(vNGeom, vTangent, vPosition, vNormalUV);
	osgx_Material mat = osgx_GetMaterial(vBaseColorUV, vOrmUV);

	gAlbedo = vec4(mat.albedo, mat.ao);
	gNormal = vec4(normalize(N), 0.0);
	gMaterial = vec4(mat.roughness, mat.metallic, 0.0, 0.0);
	gEmissive = vec4(osgx_GetEmissive(vEmissiveUV), alpha);
	// Real eye-space position, straight from the vertex shader - NOT reconstructed from depth
	// in the lighting pass (see PBRGBuffer::positionTexture's comment in PBRDeferred.hpp for why).
	gPosition = vec4(vPosition, 1.0);
}
)GLSL";

// Lighting-pass fragment shader for the deferred split (PBRLightingPass::create() below) --
// runs the same osgx_EvaluateEnvironment() (Environment.hpp) as a forward PBR shader, on N/V
// rotated to world space from the G-buffer's view-space channels.
// Plus osgx_DirectLighting(), reading G-buffer textures (position included - NOT reconstructed
// from depth; see PBRGBuffer::positionTexture's comment) instead of interpolated per-vertex
// varyings. OSGX_PBR_NO_TONEMAP/OSGX_PBR_AO mirror PBRLightingPassOptions::tonemap/
// aoTexture - see that struct's comment in PBRDeferred.hpp for why each is an independent opt-out/
// opt-in rather than one flag.
constexpr const char LIGHTING_FRAGMENT_SHADER_SRC[] = R"GLSL(
#version 460 core
#pragma import_defines ( OSGX_PBR_DIAGNOSTICS, OSGX_PBR_NO_TONEMAP, OSGX_PBR_AO, OSGX_PBR_ENVIRONMENT )

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT, F_MULTISCATTER, SPECULAR_AA, TONEMAP_DECL
#pragma osgx::light DIRECT_LIGHTING_DECL
#ifdef OSGX_PBR_ENVIRONMENT
#pragma osgx::environment ENVIRONMENT_INPUTS, ENVIRONMENT_SAMPLE, ENVIRONMENT_LIGHTING
#endif
// DEFERRED_LIGHTING_INPUTS/GET_GBUFFER: the same osgx_GBuffer/osgx_GetGBuffer() an
// osgx::Hook::DeferredLighting override uses - this built-in default is deliberately not a
// hand-rolled special case, so the two stay provably equivalent decode paths.
#pragma osgx::gbuffer DEFERRED_LIGHTING_INPUTS, GET_GBUFFER

// Manually maintained every frame by PBRLightingPass::update() - see that function's comment
// (and PBRLightingPass::create()'s) for why this quad's own osg_ViewMatrix/osg_ViewMatrixInverse
// can't be trusted the way a forward pass's can. No
// projection-matrix uniform here - position comes straight from gPosition, not a depth
// reconstruction, so only the VIEW matrix (genuinely consistent across nested cameras) is needed.

#ifdef OSGX_PBR_AO
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

#ifdef OSGX_PBR_AO
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

	vec3 surface = vec3(0.0);

#ifdef OSGX_PBR_ENVIRONMENT
	osgx_EnvironmentLight ambient = osgx_EvaluateEnvironment(mat, N, V);

	surface = ambient.diffuse + ambient.specular;
#endif
	vec3 direct = osgx_DirectLighting(N, V, worldPos, mat);

	vec3 color = surface + direct + gb.emissive;

#ifndef OSGX_PBR_NO_TONEMAP
	color = osgx_Tonemap(color);
	color = pow(color, vec3(1.0 / 2.2));
#endif

	fragColor = vec4(color, gb.alphaCoverage);
}
)GLSL";

}

void registerGBufferShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"DEFERRED_LIGHTING_INPUTS", "osgx_DeferredLightingInputs", DEFERRED_LIGHTING_INPUTS},
		{"GET_GBUFFER", "osgx_GetGBuffer", GET_GBUFFER}
	};

	registerShaderLibs("osgx::gbuffer", libs);
}

bool PBRGBuffer::valid() const {
	return gbuffer.valid()
		&& albedoTexture.valid()
		&& normalTexture.valid()
		&& materialTexture.valid()
		&& emissiveTexture.valid()
		&& positionTexture.valid()
		&& depthTexture.valid()
	;
}

PBRGBuffer PBRGBuffer::create(osg::Node* node, int width, int height, const HookList& hooks) {
	PBRGBuffer result;

	if(!node) return result;

	auto prog = osgx::make_nref<osg::Program>("osgx_PBRGeometryPass");

	osgx::bindMeshAttributes(*prog);

	auto* vertexShader = new osg::Shader(
		osg::Shader::VERTEX,
		resolveShaderLibs(PBR_VERTEX_SHADER)
	);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	// PBR_VERTEX_SHADER calls osgx_ApplySkin().
	osgx::applyHooks(prog, hooks, {
		{osgx::Hook::Skinning, new osg::Shader(
			osg::Shader::VERTEX,
			resolveShaderLibs(osgx::SKINNING_HOOK_IDENTITY)
		)}
	});

	auto* fragmentShader = new osg::Shader(
		osg::Shader::FRAGMENT,
		resolveShaderLibs(GBUFFER_FRAGMENT_SHADER_SRC)
	);

	fragmentShader->setName(prog->getName() + ".fragment");
	prog->addShader(fragmentShader);

	auto* ss = node->getOrCreateStateSet();

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

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

bool PBRLightingPass::valid() const { return node.valid(); }

// The fullscreen quad's own camera is necessarily ABSOLUTE_RF/identity-view/identity-projection
// (that's what makes an NDC quad cover the screen) - OSG's automatic osg_ViewMatrix therefore
// resolves to identity on it, not `mainCamera`'s real matrices. This pass therefore carries its own
// osgx_mainViewMatrix/osgx_mainViewMatrixInverse uniforms, set here and kept current by update().
// There is no projection-matrix uniform: each nested PRE_RENDER camera clamps its own private copy
// of the projection during cull and never writes it back to the Camera, so a projection read off
// `mainCamera` does not reliably match the one the geometry pass used. `gPosition` (PBRGBuffer)
// carries real view-space position instead; the VIEW matrix is shared correctly across
// RELATIVE_RF-nested cameras.
PBRLightingPass PBRLightingPass::create(
	const PBRGBuffer& gbuffer,
	osg::Camera* mainCamera,
	const PBRLightingPassOptions& options
) {
	PBRLightingPass result;

	if(!gbuffer.valid() || !mainCamera) return result;

	auto prog = osgx::make_nref<osg::Program>("osgx_PBRLightingPass");

	auto* vertexShader = new osg::Shader(osg::Shader::VERTEX, osgx::FULLSCREEN_VERT);

	vertexShader->setName(prog->getName() + ".vertex");
	prog->addShader(vertexShader);

	// EXACTLY ONE definition each of osgx_DirectLighting(), osgx_Tonemap(), and main() (the
	// DeferredLighting slot), always - never zero, never two. applyHooks() (Shader.hpp) enforces
	// this: it always attaches one shader per slot below, the caller's options.hooks override if
	// present, otherwise the built-in.
	//
	// DeferredLighting's built-in default IS this pass's own main() (
	// LIGHTING_FRAGMENT_SHADER_SRC) - an override REPLACES the whole lighting orchestration, not
	// one leaf function, so a caller supplying one is free to ignore osgx_DirectLighting()/
	// osgx_Tonemap() (still harmlessly attached below, unused-but-defined is not a GLSL error) and
	// read the G-buffer via osgx_GetGBuffer() (#pragma osgx::gbuffer GET_GBUFFER) instead. See
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
	// Tonemap: never zero because OSGX_PBR_NO_TONEMAP strips the CALL at render time, but that
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
			resolveShaderLibs(LIGHTING_FRAGMENT_SHADER_SRC)
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

	auto cam = osgx::make_nref<osg::Camera>("osgx_PBRLightingPass");

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
	auto& bindings = osgx::Library::instance().bindings();

	// Binds `texture` at the named texture-unit slot and points `sampler` at it.
	const auto bindInput = [&](const char* slot, osg::Texture* texture, const char* sampler) {
		const auto unit = bindings.get(slot);

		ss->setTextureAttributeAndModes(unit, texture, osg::StateAttribute::ON);
		ss->addUniform(new osg::Uniform(sampler, static_cast<int>(unit)));
	};

	bindInput("osgx::gbuffer.albedo", gbuffer.albedoTexture, "gAlbedo");
	bindInput("osgx::gbuffer.normal", gbuffer.normalTexture, "gNormal");
	bindInput("osgx::gbuffer.material", gbuffer.materialTexture, "gMaterial");
	bindInput("osgx::gbuffer.emissive", gbuffer.emissiveTexture, "gEmissive");
	bindInput("osgx::gbuffer.position", gbuffer.positionTexture, "gPosition");

	if(options.environment) {
		result.environment = options.environment;

		ss->setAttributeAndModes(options.environment);
		ss->setDefine("OSGX_PBR_ENVIRONMENT");
	}

	result.mainViewMatrix = new osg::Uniform("osgx_mainViewMatrix", osg::Matrixf::identity());
	result.mainViewMatrixInverse = new osg::Uniform(
		"osgx_mainViewMatrixInverse", osg::Matrixf::identity()
	);
	ss->addUniform(result.mainViewMatrix);
	ss->addUniform(result.mainViewMatrixInverse);

	// A caller-supplied hook takes precedence over `tonemap`: supplying a curve means wanting it to
	// run, so the call (and the gamma encode this define also gates) stays in.
	if(!options.tonemap && !osgx::hasHook(options.hooks, osgx::Hook::Tonemap)) {
		ss->setDefine("OSGX_PBR_NO_TONEMAP");
	}
	if(options.diagnostics) ss->setDefine("OSGX_PBR_DIAGNOSTICS");

	if(options.aoTexture) {
		bindInput("osgx::gbuffer.ao", options.aoTexture, "aoTex");
		ss->setDefine("OSGX_PBR_AO");
	}

	if(options.shadowMap) {
		bindInput("osgx::shadowMap", options.shadowMap->depthTexture, "osgx_shadowMap");
		ss->addUniform(options.shadowMap->shadowMatrix);
		ss->addUniform(options.shadowMap->bias);
		ss->addUniform(options.shadowMap->strength);
		ss->addUniform(options.shadowMap->casterIndex);
	}

	result.node = cam;

	result.update(mainCamera);

	return result;
}

void PBRLightingPass::update(const osg::Camera* mainCamera) {
	if(!valid() || !mainCamera) return;

	if(mainViewMatrix) {
		mainViewMatrix->set(osg::Matrixf(mainCamera->getViewMatrix()));
	}

	if(mainViewMatrixInverse) {
		mainViewMatrixInverse->set(osg::Matrixf(mainCamera->getInverseViewMatrix()));
	}
}


}
