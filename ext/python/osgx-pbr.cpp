#include "osgx-python.hpp"
#include "osgx/PBR.hpp"
#include "osgx/PBRDeferred.hpp"
#include "osgx/PBRScene.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Shader>
#include <osg/StateAttribute>
#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_pbr(py::module_& m) {
	m.attr("D_GGX") = osgx::D_GGX;
	m.attr("G_SCHLICK") = osgx::G_SCHLICK;
	m.attr("G_SMITH") = osgx::G_SMITH;
	m.attr("F_SCHLICK") = osgx::F_SCHLICK;
	m.attr("F_SCHLICK_ROUGHNESS") = osgx::F_SCHLICK_ROUGHNESS;
	m.attr("DIRECT_SPECULAR") = osgx::DIRECT_SPECULAR;
	m.attr("DIRECT_DIFFUSE") = osgx::DIRECT_DIFFUSE;
	m.attr("DIRECT_LIGHT") = osgx::DIRECT_LIGHT;
	m.attr("DIRECT_LIGHT_SPHERE") = osgx::DIRECT_LIGHT_SPHERE;
	m.attr("F_MULTISCATTER") = osgx::F_MULTISCATTER;
	m.attr("TONEMAP_PBR_NEUTRAL") = osgx::TONEMAP_PBR_NEUTRAL;
	m.attr("TONEMAP_DECL") = osgx::TONEMAP_DECL;
	m.attr("TONEMAP_HOOK_DEFAULT") = osgx::TONEMAP_HOOK_DEFAULT;
	m.attr("MATERIAL_INPUTS") = osgx::MATERIAL_INPUTS;
	m.attr("GET_MATERIAL") = osgx::GET_MATERIAL;
	m.attr("GET_EMISSIVE") = osgx::GET_EMISSIVE;
	m.attr("GET_ALPHA") = osgx::GET_ALPHA;
	m.attr("GET_SHADING_NORMAL") = osgx::GET_SHADING_NORMAL;
	m.attr("BASE_COLOR_UV_CHANNEL") = osgx::BASE_COLOR_UV_CHANNEL;
	m.attr("NORMAL_UV_CHANNEL") = osgx::NORMAL_UV_CHANNEL;
	m.attr("ORM_UV_CHANNEL") = osgx::ORM_UV_CHANNEL;
	m.attr("EMISSIVE_UV_CHANNEL") = osgx::EMISSIVE_UV_CHANNEL;

	// osgx::MaterialFactors/attachMaterialFactors() are gone - collapsed into osgx::Material, a
	// real osg::StateAttribute (PBR.hpp/PBR.cpp). Bound the same way osgx-callbacks.cpp already
	// binds osgx::NodeCallbacksGroup et al. against a real pyosg-registered OSG base
	// (osg::StateAttribute is bound in pyosg/osg/State.cpp) - cross-module inheritance works here
	// because both modules link the identical vendored pybind11 (same ABI tag), so pybind11's
	// process-wide type registry (populated at import time, not link time) already has
	// osg::StateAttribute by the time this module's own init runs. Requires `import osg` (pyosg)
	// to have already happened in this interpreter - pybind11 has no way to resolve a base class
	// it hasn't seen registered yet.
	auto material = py::class_<osgx::Material, osg::StateAttribute, osg::ref_ptr<osgx::Material>>(
		m,
		"Material",
		"A real osg.StateAttribute carrying PBR material factors (base color/roughness/metallic/"
		"occlusion/emissive/alpha) and texture maps (base color/normal/metallicRoughness/emissive), "
		"applied via a std140 uniform block read by MATERIAL_INPUTS/GET_MATERIAL. Attach "
		"with setAttributeAndModes() like any StateAttribute - two drawables sharing an equal "
		"Material dedup automatically."
	);

	py::enum_<osgx::Material::AlphaMode>(
		material,
		"AlphaMode",
		"How a shader should treat the material's alpha: Opaque ignores it, Mask discards below "
		"alphaCutoff, Blend writes it out. Only the shader-visible value - GL blend state is the "
		"caller's job."
	)
		.value("Opaque", osgx::Material::AlphaMode::Opaque)
		.value("Mask", osgx::Material::AlphaMode::Mask)
		.value("Blend", osgx::Material::AlphaMode::Blend)
	;

	material
		.def(py::init<>(), "Constructs a default-white, fully-rough, fully-metallic Material with no maps set.")
		.def_property(
			"baseColor", &osgx::Material::getBaseColor, &osgx::Material::setBaseColor,
			"RGBA base color factor, multiplied against baseColorMap when one is set."
		)
		.def_property(
			"roughness", &osgx::Material::getRoughness, &osgx::Material::setRoughness,
			"Roughness factor in [0, 1], multiplied against metallicRoughnessMap's G channel when one is set."
		)
		.def_property(
			"metallic", &osgx::Material::getMetallic, &osgx::Material::setMetallic,
			"Metallic factor in [0, 1], multiplied against metallicRoughnessMap's B channel when one is set."
		)
		.def_property(
			"emissiveFactor", &osgx::Material::getEmissiveFactor, &osgx::Material::setEmissiveFactor,
			"RGB emissive factor, multiplied against emissiveMap when one is set; defaults to black."
		)
		.def_property(
			"alphaMode", &osgx::Material::getAlphaMode, &osgx::Material::setAlphaMode,
			"Material.AlphaMode.Opaque/Mask/Blend."
		)
		.def_property(
			"alphaCutoff", &osgx::Material::getAlphaCutoff, &osgx::Material::setAlphaCutoff,
			"Mask-mode discard threshold; defaults to 0.5."
		)
		.def_property(
			"hasOcclusion", &osgx::Material::getHasOcclusion, &osgx::Material::setHasOcclusion,
			"Whether metallicRoughnessMap's R channel carries real per-pixel occlusion - unlike "
			"the other has*Map flags, this has no dedicated texture of its own to derive from."
		)
		.def_property(
			"baseColorMap", &osgx::Material::getBaseColorMap, &osgx::Material::setBaseColorMap,
			"Base color (albedo) texture."
		)
		.def_property(
			"normalMap", &osgx::Material::getNormalMap, &osgx::Material::setNormalMap,
			"Tangent-space normal map texture."
		)
		.def_property(
			"metallicRoughnessMap",
			&osgx::Material::getMetallicRoughnessMap, &osgx::Material::setMetallicRoughnessMap,
			"glTF's combined occlusion/roughness/metallic texture."
		)
		.def_property(
			"emissiveMap", &osgx::Material::getEmissiveMap, &osgx::Material::setEmissiveMap,
			"Emissive color texture."
		)
	;

	m.def(
		"snippets", &osgx::snippets,
		"Returns the five core BRDF snippets (D_GGX, G_SCHLICK, G_SMITH, F_SCHLICK, "
		"F_SCHLICK_ROUGHNESS) concatenated in dependency order - convenience for a caller that "
		"wants the whole toolkit at once; use the individual constants if only part is needed."
	);

	// osgx_Tonemap() CONTRACT's Python-side convenience - same shape as osgx-light.cpp's
	// makeDirectLightingHookShader(). See PBR.hpp's TONEMAP_DECL/TONEMAP_HOOK_DEFAULT comment.
	m.def(
		"makeTonemapHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::TONEMAP_HOOK_DEFAULT))
			);

			shader->setName("tonemapHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds the osgx_Tonemap() CONTRACT's default-definition FRAGMENT shader object "
		"(osgx_TonemapPBRNeutral) - add it to a Program alongside a consumer fragment shader "
		"that only declares TONEMAP_DECL plus a call site."
	);

	// osgx/PBRScene.hpp and osgx/PBRDeferred.hpp - the forward PBR renderer, and the deferred
	// G-buffer with its fullscreen lighting pass. Both take their light sources (environment,
	// shadow map) in an options object; custom deferred lighting shaders read the G-buffer through
	// the "osgx::gbuffer" catalog.
	py::class_<osgx::PBRSceneOptions>(
		m,
		"PBRSceneOptions",
		"PBRScene.create() inputs, each optional: environment (an osgx.Environment), shadowMap (an "
		"osgx.ShadowMap for the key light), hooks, diagnostics."
	)
		.def(
			py::init([](
				osgx::Environment* environment,
				const osgx::ShadowMap* shadowMap,
				py::object hooks,
				bool diagnostics
			) {
				osgx::PBRSceneOptions options;

				options.environment = environment;
				options.shadowMap = shadowMap;
				options.hooks = pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks);
				options.diagnostics = diagnostics;

				return options;
			}),
			"environment"_a=nullptr,
			"shadowMap"_a=nullptr,
			"hooks"_a=py::dict(),
			"diagnostics"_a=false,
			"Constructs the options; every argument is optional."
		)
		.def_readwrite(
			"environment", &osgx::PBRSceneOptions::environment,
			"The osgx.Environment lighting the scene; None leaves the environment term at zero."
		)
		.def_readwrite(
			"shadowMap", &osgx::PBRSceneOptions::shadowMap,
			"An osgx.ShadowMap shadowing the key/directional light; None is unshadowed."
		)
		.def_property(
			"hooks",
			// A property so the setter goes through pyx::unpack_one_or_many<T>() (pybind11x.hpp): a
			// dict of {osgx.Hook: osg.Shader} (preferred), a list of (Hook, Shader) pairs, or a
			// single bare pair.
			[](const osgx::PBRSceneOptions& self) { return self.hooks; },
			[](osgx::PBRSceneOptions& self, py::object hooks) {
				self.hooks = pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks);
			},
			"Substitutes a built-in shader object per slot: osgx.Hook.Skinning (a VERTEX shader "
			"defining osgx_ApplySkin(), e.g. osgx.SKINNING_HOOK_LINEAR_BLEND wrapped in "
			"osgx.resolveShaderLibs()) and osgx.Hook.Tonemap (a FRAGMENT shader defining "
			"osgx_Tonemap()). Each REPLACES its default."
		)
		.def_readwrite(
			"diagnostics", &osgx::PBRSceneOptions::diagnostics,
			"Adds the debugMode/disableNormalMap/disableRoughnessMap/disableSpecularAA uniforms."
		)
	;

	py::class_<osgx::PBRScene>(
		m,
		"PBRScene",
		"The result of PBRScene.create(): the node the forward PBR Program was attached to, plus "
		"the diagnostics uniforms when requested."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRScene.")
		.def_readwrite("node", &osgx::PBRScene::node, "The node the renderer was applied to.")
		.def_readwrite(
			"environment", &osgx::PBRScene::environment,
			"The environment from the options, attached to `node`; None if none."
		)
		.def_readwrite(
			"debugMode", &osgx::PBRScene::debugMode,
			"Debug-visualization mode uniform (diagnostics only)."
		)
		.def_readwrite(
			"disableNormalMap", &osgx::PBRScene::disableNormalMap,
			"When set, shades with the geometric normal (diagnostics only)."
		)
		.def_readwrite(
			"disableRoughnessMap", &osgx::PBRScene::disableRoughnessMap,
			"When set, ignores the roughness texture (diagnostics only)."
		)
		.def_readwrite(
			"disableSpecularAA", &osgx::PBRScene::disableSpecularAA,
			"When set, disables specular anti-aliasing (diagnostics only)."
		)
		.def("valid", &osgx::PBRScene::valid, "True once node is set.")
		.def_static(
			"create",
			&osgx::PBRScene::create,
			"node"_a,
			"options"_a=osgx::PBRSceneOptions{},
			"Attaches the forward PBR Program (OVERRIDE) to `node`, shading osgx.Material geometry by "
			"the options' environment and shadow map and the osgx.LightSet lights inherited from "
			"the scene graph."
		)
	;

	py::class_<osgx::PBRGBuffer>(
		m,
		"PBRGBuffer",
		"Deferred-split geometry-pass output: material only (no lighting, not even emissive "
		"add), ready to feed PBRLightingPass.create()."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRGBuffer.")
		.def_readwrite(
			"gbuffer", &osgx::PBRGBuffer::gbuffer,
			"The underlying osgx.GBuffer this was built from."
		)
		.def_readwrite(
			"albedoTexture", &osgx::PBRGBuffer::albedoTexture,
			"rgb = albedo, a = ambient occlusion."
		)
		.def_readwrite(
			"normalTexture", &osgx::PBRGBuffer::normalTexture,
			"rgb = view-space shading normal (RGB16F)."
		)
		.def_readwrite(
			"materialTexture", &osgx::PBRGBuffer::materialTexture,
			"r = roughness, g = metallic."
		)
		.def_readwrite(
			"emissiveTexture", &osgx::PBRGBuffer::emissiveTexture,
			"rgb = emissive (HDR), a = alpha coverage."
		)
		.def_readwrite(
			"positionTexture", &osgx::PBRGBuffer::positionTexture,
			"rgb = view-space position (RGBA32F)."
		)
		.def_readwrite(
			"depthTexture", &osgx::PBRGBuffer::depthTexture,
			"The geometry pass's depth attachment."
		)
		.def(
			"valid", &osgx::PBRGBuffer::valid,
			"True once every G-buffer texture is set."
		)
		.def_static(
			"create",
			[](osg::Node* node, int width, int height, py::object hooks) {
				return osgx::PBRGBuffer::create(
					node,
					width,
					height,
					pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks)
				);
			},
			"node"_a,
			"width"_a,
			"height"_a,
			"hooks"_a=py::dict(),
			"Deferred-split geometry pass: writes material only (albedo/view-space normal/ORM/"
			"emissive + depth) to a PBRGBuffer, no lighting. Feed the result to "
			"PBRLightingPass.create(). `hooks` may substitute osgx.Hook.Skinning (a VERTEX shader "
			"defining osgx_ApplySkin(), e.g. osgx.SKINNING_HOOK_LINEAR_BLEND wrapped in "
			"osgx.resolveShaderLibs())."
		)
	;

	py::class_<osgx::PBRLightingPassOptions>(
		m,
		"PBRLightingPassOptions",
		"PBRLightingPass.create() inputs, each an independent, optional seam rather than one "
		"monolithic flag blob."
	)
		.def(
			py::init([](
				osgx::Environment* environment,
				bool tonemap,
				py::object hooks,
				const osgx::ShadowMap* shadowMap,
				osg::Texture2D* aoTexture,
				bool diagnostics
			) {
				osgx::PBRLightingPassOptions options;

				options.environment = environment;
				options.tonemap = tonemap;
				options.hooks = pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks);
				options.shadowMap = shadowMap;
				options.aoTexture = aoTexture;
				options.diagnostics = diagnostics;

				return options;
			}),
			"environment"_a=nullptr,
			"tonemap"_a=true,
			"hooks"_a=py::dict(),
			"shadowMap"_a=nullptr,
			"aoTexture"_a=nullptr,
			"diagnostics"_a=false,
			"Constructs the options; every argument is optional."
		)
		.def_readwrite(
			"environment", &osgx::PBRLightingPassOptions::environment,
			"The osgx.Environment lighting the pass; None leaves the built-in shader's environment "
			"term at zero."
		)
		.def_readwrite(
			"tonemap", &osgx::PBRLightingPassOptions::tonemap,
			"True applies the built-in (or hooks[osgx.Hook.Tonemap]-substituted) tonemap curve; "
			"False leaves the result linear HDR, for a caller chaining further passes."
		)
		.def_property(
			"hooks",
			// A property (not def_readwrite) purely so the setter can go through
			// pyx::unpack_one_or_many<T>() (pybind11x.hpp) - same reasoning, and same accepted
			// shapes, as PBRSceneOptions.hooks: a dict of
			// {osgx.Hook: osg.Shader} (preferred), a list of (Hook, Shader) pairs, or a single
			// bare pair. The getter is unchanged - reading back a plain osgx.HookList (a list of
			// pairs) is unambiguous, there's nothing to disambiguate on the way out.
			[](const osgx::PBRLightingPassOptions& self) { return self.hooks; },
			[](osgx::PBRLightingPassOptions& self, py::object hooks) {
				self.hooks = pyx::unpack_one_or_many<osgx::HookList::value_type>(hooks);
			},
			"Substitutes this pass's built-in shader for a slot - a dict of "
			"{osgx.Hook: osg.Shader} (preferred), a list of (osgx.Hook, osg.Shader) pairs, or a "
			"single bare (osgx.Hook, osg.Shader) pair are all accepted. osgx.Hook.DeferredLighting "
			"(the whole fragment main()), osgx.Hook.DirectLighting, and osgx.Hook.Tonemap are "
			"supported. Each REPLACES its default, it does not add alongside it."
		)
		.def_readwrite(
			"shadowMap", &osgx::PBRLightingPassOptions::shadowMap,
			"An osgx.ShadowMap shadowing the key/directional light; None is unshadowed."
		)
		.def_readwrite(
			"aoTexture", &osgx::PBRLightingPassOptions::aoTexture,
			"Optional ambient-occlusion texture, multiplied into the ambient term. This pass does "
			"not bake SSAO itself - feed osgx.SSAO.create()'s result here, or any other "
			"occlusion source."
		)
		.def_readwrite(
			"diagnostics", &osgx::PBRLightingPassOptions::diagnostics,
			"Enables extra debug output on the lighting pass."
		)
	;

	py::class_<osgx::PBRLightingPass>(
		m,
		"PBRLightingPass",
		"The result of PBRLightingPass.create(): a fullscreen-quad deferred lighting pass "
		"reading a PBRGBuffer, plus live uniforms a caller must keep in sync via update()."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRLightingPass.")
		.def_readwrite(
			"node", &osgx::PBRLightingPass::node,
			"The fullscreen-quad lighting-pass node (an ABSOLUTE_RF camera)."
		)
		.def_readwrite(
			"environment",
			&osgx::PBRLightingPass::environment,
			"The environment from the options, attached to the lighting pass; None if none."
		)
		.def_readwrite(
			"mainViewMatrix",
			&osgx::PBRLightingPass::mainViewMatrix,
			"mainCamera's view matrix, refreshed by update() - the quad's own camera is "
			"ABSOLUTE_RF, so OSG's automatic osg_ViewMatrix resolves to identity, not mainCamera's "
			"real matrix."
		)
		.def_readwrite(
			"mainViewMatrixInverse",
			&osgx::PBRLightingPass::mainViewMatrixInverse,
			"mainCamera's inverse view matrix, refreshed by update() alongside mainViewMatrix."
		)
		.def(
			"valid", &osgx::PBRLightingPass::valid,
			"True once node is set."
		)
		.def_static(
			"create",
			&osgx::PBRLightingPass::create,
			"gbuffer"_a,
			"mainCamera"_a,
			"options"_a=osgx::PBRLightingPassOptions{},
			"Deferred-split lighting pass: a fullscreen quad reading `gbuffer` (position included, "
			"not reconstructed from depth), rotating it into world space via `mainCamera`'s real "
			"view matrix, running the same osgx_EvaluateEnvironment()/osgx_DirectLighting() logic "
			"a forward PBR shader does. options.hooks supports osgx.Hook.DeferredLighting (the "
			"entire fullscreen lighting shader), osgx.Hook.DirectLighting (the "
			"osgx_DirectLighting() definition), and osgx.Hook.Tonemap. Each replaces its default "
			"shader object. Call .update() from a preDrawCallback on the "
			"FIRST PRE_RENDER camera in the scene graph every frame to keep it in sync as mainCamera "
			"moves - NOT from mainCamera's own preDrawCallback or from application code after "
			"frame() returns, both of which hand this pass a stale matrix."
		)
		.def(
			"update",
			&osgx::PBRLightingPass::update,
			"mainCamera"_a,
			"Refreshes this scene's manually-maintained view-matrix uniforms from mainCamera's "
			"current matrices. Call from a preDrawCallback on the FIRST PRE_RENDER camera in the "
			"scene graph (by render order) every frame - every PRE_RENDER camera finishes drawing "
			"before mainCamera's own preDrawCallback fires, so calling this from mainCamera's "
			"callback (or from application code after viewer.frame() returns) hands the lighting "
			"pass a one-frame-stale matrix relative to what the geometry pass just rendered with, "
			"which shows up as artifacts that worsen while the camera is moving."
		)
	;
}

}
