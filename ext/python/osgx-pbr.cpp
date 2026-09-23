#include "osgx-python.hpp"
#include "osgx/PBR.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Shader>
#include <osg/StateAttribute>
#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_pbr(py::module_& m) {
	osgx::registerPBRShaderLibs();

	m.attr("D_GGX") = osgx::D_GGX;
	m.attr("G_SCHLICK") = osgx::G_SCHLICK;
	m.attr("G_SMITH") = osgx::G_SMITH;
	m.attr("F_SCHLICK") = osgx::F_SCHLICK;
	m.attr("F_SCHLICK_ROUGHNESS") = osgx::F_SCHLICK_ROUGHNESS;
	m.attr("F_MULTISCATTER") = osgx::F_MULTISCATTER;
	m.attr("IBL_SPECULAR") = osgx::IBL_SPECULAR;
	m.attr("AMBIENT_LIGHTING_DECL") = osgx::AMBIENT_LIGHTING_DECL;
	m.attr("AMBIENT_LIGHTING_HOOK_DEFAULT") = osgx::AMBIENT_LIGHTING_HOOK_DEFAULT;
	m.attr("TONEMAP_PBR_NEUTRAL") = osgx::TONEMAP_PBR_NEUTRAL;
	m.attr("TONEMAP_DECL") = osgx::TONEMAP_DECL;
	m.attr("TONEMAP_HOOK_DEFAULT") = osgx::TONEMAP_HOOK_DEFAULT;
	m.attr("MATERIAL_BINDING") = osgx::MATERIAL_BINDING;

	// osgx::MaterialFactors/attachMaterialFactors() are gone - collapsed into osgx::Material, a
	// real osg::StateAttribute (PBR.hpp/PBR.cpp). Bound the same way osgx-callbacks.cpp already
	// binds osgx::NodeCallbacksGroup et al. against a real pyosg-registered OSG base
	// (osg::StateAttribute is bound in pyosg/osg/State.cpp) - cross-module inheritance works here
	// because both modules link the identical vendored pybind11 (same ABI tag), so pybind11's
	// process-wide type registry (populated at import time, not link time) already has
	// osg::StateAttribute by the time this module's own init runs. Requires `import osg` (pyosg)
	// to have already happened in this interpreter - pybind11 has no way to resolve a base class
	// it hasn't seen registered yet.
	py::class_<osgx::Material, osg::StateAttribute, osg::ref_ptr<osgx::Material>>(
		m,
		"Material",
		"A real osg.StateAttribute carrying PBR material factors (base color/roughness/metallic/"
		"occlusion) and texture maps (base color/normal/metallicRoughness/emissive), applied via "
		"a std430 shader storage buffer. Attach with setAttributeAndModes() like any "
		"StateAttribute - two drawables sharing an equal Material dedup automatically."
	)
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

	// osgx_AmbientLighting() CONTRACT's Python-side convenience - same shape as osgx-light.cpp's
	// makeDirectLightingHookShader(). See PBR.hpp's AMBIENT_LIGHTING_DECL/
	// AMBIENT_LIGHTING_HOOK_DEFAULT comment: specular-only default (no SH-9 diffuse yet).
	m.def(
		"makeAmbientLightingHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::AMBIENT_LIGHTING_HOOK_DEFAULT))
			);

			shader->setName("ambientLightingHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds the osgx_AmbientLighting() CONTRACT's default-definition FRAGMENT shader object "
		"(specular-only IBL, via osgx_IBLSpecular) - add it to a Program alongside a consumer "
		"fragment shader that only declares AMBIENT_LIGHTING_DECL plus a call site."
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

}

}
