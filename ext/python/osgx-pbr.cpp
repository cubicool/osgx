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
	m.attr("DIRECT_SPECULAR") = osgx::DIRECT_SPECULAR;
	m.attr("DIRECT_DIFFUSE") = osgx::DIRECT_DIFFUSE;
	m.attr("POINT_LIGHT_RADIANCE") = osgx::POINT_LIGHT_RADIANCE;
	m.attr("LIGHT_UNIFORMS") = osgx::LIGHT_UNIFORMS;
	m.attr("DIRECT_LIGHT") = osgx::DIRECT_LIGHT;
	m.attr("DIRECTIONAL_LIGHT_RADIANCE") = osgx::DIRECTIONAL_LIGHT_RADIANCE;
	m.attr("SPOT_LIGHT_RADIANCE") = osgx::SPOT_LIGHT_RADIANCE;
	m.attr("SPHERE_LIGHT_SPECULAR") = osgx::SPHERE_LIGHT_SPECULAR;
	m.attr("DIRECT_LIGHT_SPHERE") = osgx::DIRECT_LIGHT_SPHERE;
	m.attr("MAX_LIGHTS") = osgx::MAX_LIGHTS;
	m.attr("LIGHT_STRUCT_FLOATS") = osgx::LIGHT_STRUCT_FLOATS;
	m.attr("DIRECT_LIGHTING_DECL") = osgx::DIRECT_LIGHTING_DECL;
	m.attr("DIRECT_LIGHTING_HOOK_DEFAULT") = osgx::DIRECT_LIGHTING_HOOK_DEFAULT;
	m.attr("F_MULTISCATTER") = osgx::F_MULTISCATTER;
	m.attr("IBL_SPECULAR") = osgx::IBL_SPECULAR;
	m.attr("AMBIENT_LIGHTING_DECL") = osgx::AMBIENT_LIGHTING_DECL;
	m.attr("AMBIENT_LIGHTING_HOOK_DEFAULT") = osgx::AMBIENT_LIGHTING_HOOK_DEFAULT;
	m.attr("TONEMAP_PBR_NEUTRAL") = osgx::TONEMAP_PBR_NEUTRAL;
	m.attr("TONEMAP_DECL") = osgx::TONEMAP_DECL;
	m.attr("TONEMAP_HOOK_DEFAULT") = osgx::TONEMAP_HOOK_DEFAULT;
	m.attr("MATERIAL_BINDING") = osgx::MATERIAL_BINDING;

	// osgx::MaterialFactors/attachMaterialFactors() are gone -- collapsed into osgx::Material, a
	// real osg::StateAttribute (PBR.hpp/PBR.cpp). Bound the same way osgx-callbacks.cpp already
	// binds osgx::NodeCallbacksGroup et al. against a real pyosg-registered OSG base
	// (osg::StateAttribute is bound in pyosg/osg/State.cpp) -- cross-module inheritance works here
	// because both modules link the identical vendored pybind11 (same ABI tag), so pybind11's
	// process-wide type registry (populated at import time, not link time) already has
	// osg::StateAttribute by the time this module's own init runs. Requires `import osg` (pyosg)
	// to have already happened in this interpreter -- pybind11 has no way to resolve a base class
	// it hasn't seen registered yet.
	py::class_<osgx::Material, osg::StateAttribute, osg::ref_ptr<osgx::Material>>(
		m,
		"Material",
		"A real osg.StateAttribute carrying PBR material factors (base color/roughness/metallic/"
		"occlusion) and texture maps (base color/normal/metallicRoughness/emissive), applied via "
		"a std430 shader storage buffer. Attach with setAttributeAndModes() like any "
		"StateAttribute -- two drawables sharing an equal Material dedup automatically."
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
			"Whether metallicRoughnessMap's R channel carries real per-pixel occlusion -- unlike "
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
		"F_SCHLICK_ROUGHNESS) concatenated in dependency order -- convenience for a caller that "
		"wants the whole toolkit at once; use the individual constants if only part is needed."
	);

	// Assembles the osgx_DirectLighting() CONTRACT's default definition as a standalone FRAGMENT
	// osg::Shader, ready to add()/append() onto an osg::Program alongside a consumer's own
	// fragment shader (which only needs DIRECT_LIGHTING_DECL spliced in via #pragma osgx::pbr, plus a
	// call site) -- so a Python caller doesn't have to hand-assemble
	// osg.Shader(osg.Shader.FRAGMENT, osgx.resolveShaderLibs(osgx.pbr.DIRECT_LIGHTING_HOOK_DEFAULT))
	// itself. See PBR.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT comment for the full
	// rationale.
	m.def(
		"makeDirectLightingHookShader",
		[]() {
			auto* shader = new osg::Shader(
				osg::Shader::FRAGMENT,
				osgx::resolveShaderLibs(std::string(osgx::DIRECT_LIGHTING_HOOK_DEFAULT))
			);

			// No Program exists yet at this call site (unlike applyHooks(), which knows
			// program->getName() and can prefix with it) -- a bare role name is the best this
			// convenience function can do on its own; a caller with more context is always free
			// to overwrite shader.name afterward. See osgx::applyHooks()'s own naming (Shader.cpp)
			// for the "<programName>.<role>" convention this deliberately matches the tail of.
			shader->setName("directLightingHook");

			return osg::ref_ptr<osg::Shader>(shader);
		},
		"Builds the osgx_DirectLighting() CONTRACT's default-definition FRAGMENT shader object -- "
		"add it to a Program alongside a consumer fragment shader that only declares "
		"DIRECT_LIGHTING_DECL plus a call site."
	);

	// osgx_AmbientLighting() CONTRACT's Python-side convenience -- same shape as
	// makeDirectLightingHookShader() above. See PBR.hpp's AMBIENT_LIGHTING_DECL/
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
		"(specular-only IBL, via osgx_IBLSpecular) -- add it to a Program alongside a consumer "
		"fragment shader that only declares AMBIENT_LIGHTING_DECL plus a call site."
	);

	// osgx_Tonemap() CONTRACT's Python-side convenience -- same shape as
	// makeDirectLightingHookShader() above. See PBR.hpp's TONEMAP_DECL/TONEMAP_HOOK_DEFAULT comment.
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
		"(osgx_TonemapPBRNeutral) -- add it to a Program alongside a consumer fragment shader "
		"that only declares TONEMAP_DECL plus a call site."
	);

	py::class_<osgx::OrbitLightRig::Orbit>(
		m,
		"Orbit",
		"One light's orbit parameters for OrbitLightRig: radius/height around `center`, angular "
		"speed and phase offset, and a per-orbit intensity scale."
	)
		.def(
			py::init([](float radius, float height, float speed, float phase, float intensity) {
				return osgx::OrbitLightRig::Orbit{radius, height, speed, phase, intensity};
			}),
			"radius"_a=0.5f,
			"height"_a=0.5f,
			"speed"_a=0.5f,
			"phase"_a=0.0f,
			"intensity"_a=1.0f,
			"Constructs one orbit's parameters."
		)
		.def_readwrite("radius", &osgx::OrbitLightRig::Orbit::radius, "Orbit radius around `center`, in world units.")
		.def_readwrite("height", &osgx::OrbitLightRig::Orbit::height, "Height above `center`, in world units.")
		.def_readwrite("speed", &osgx::OrbitLightRig::Orbit::speed, "Angular speed, in radians per second.")
		.def_readwrite("phase", &osgx::OrbitLightRig::Orbit::phase, "Starting angular phase offset, in radians.")
		.def_readwrite("intensity", &osgx::OrbitLightRig::Orbit::intensity, "Per-orbit intensity scale.")
	;

	py::enum_<osgx::LightType>(
		m,
		"LightType",
		"Selects the radiance function LightSet's per-light `type` field picks in the fragment "
		"shader loop. No Sphere member: a sphere light is a Point or Spot light with a nonzero "
		"sourceRadius, not a fourth type."
	)
		.value("Point", osgx::LightType::Point)
		.value("Directional", osgx::LightType::Directional)
		.value("Spot", osgx::LightType::Spot)
	;

	py::class_<osgx::LightSet, osg::StateAttribute, osg::ref_ptr<osgx::LightSet>>(
		m,
		"LightSet",
		"A real osg.StateAttribute owning up to MAX_LIGHTS punctual lights in one std430 shader "
		"storage buffer plus their count uniform. Attach with setAttributeAndModes() on an "
		"ancestor StateSet and every lit subgraph beneath it inherits the same lights. "
		"OrbitLightRig can animate a subset of an attached LightSet's lights on top of this."
	)
		.def(py::init<>(), "Constructs a LightSet with 0 active lights; valid to attach immediately.")
		.def("valid", &osgx::LightSet::valid, "True if this LightSet still has its buffer binding and count uniform.")
		.def(
			"setPoint",
			&osgx::LightSet::setPoint,
			"index"_a,
			"position"_a,
			"color"_a,
			"intensity"_a,
			"sourceRadius"_a=0.0f,
			"Configures light `index` as a point light and enables it. `sourceRadius` > 0 widens "
			"its specular highlight to read as a physical sphere light; falloff/diffuse are unchanged."
		)
		.def(
			"setDirectional", &osgx::LightSet::setDirectional, "index"_a, "direction"_a, "color"_a, "intensity"_a,
			"Configures light `index` as a directional light and enables it. `direction` is the "
			"ray travel direction (KHR_lights_punctual convention), e.g. (0, 0, -1) for straight "
			"down in a Z-up world."
		)
		.def(
			"setSpot",
			&osgx::LightSet::setSpot,
			"index"_a,
			"position"_a,
			"direction"_a,
			"color"_a,
			"intensity"_a,
			"innerConeAngle"_a,
			"outerConeAngle"_a,
			"sourceRadius"_a=0.0f,
			"Configures light `index` as a spot light and enables it. Cone angles are in radians "
			"(KHR_lights_punctual convention); `sourceRadius` > 0 widens its specular highlight, "
			"same as setPoint()."
		)
		.def_property(
			"count",
			&osgx::LightSet::getCount,
			[](const osgx::LightSet& lights, int count) {
				if(count < 0) throw std::out_of_range("LightSet count out of range");

				lights.setCount(static_cast<std::size_t>(count));
			},
			"How many of MAX_LIGHTS light slots the shader loop actually iterates."
		)
		.def(
			"setEnabled", &osgx::LightSet::setEnabled, "index"_a, "enabled"_a,
			"Enables or disables an already-configured light without changing its data."
		)
		.def(
			"setPosition", &osgx::LightSet::setPosition, "index"_a, "position"_a, "intensity"_a,
			"Sets only light `index`'s position/intensity, leaving color/type/direction/"
			"spotAngles/sourceRadius untouched -- the primitive OrbitLightRig uses to animate an "
			"already-configured light every frame."
		)
		.def("getPosIntensity", &osgx::LightSet::getPosIntensity, "index"_a, "Returns light `index`'s (position.xyz, intensity.w).")
		.def("getColor", &osgx::LightSet::getColor, "index"_a, "Returns light `index`'s color.")
		.def(
			"getType",
			static_cast<osgx::LightType (osgx::LightSet::*)(std::size_t) const>(
				&osgx::LightSet::getType
			),
			"index"_a,
			"Returns light `index`'s LightType."
		)
		.def("getEnabled", &osgx::LightSet::getEnabled, "index"_a, "Returns whether light `index` is currently enabled.")
		.def("getDirection", &osgx::LightSet::getDirection, "index"_a, "Returns light `index`'s ray travel direction.")
		.def("getSpotAngles", &osgx::LightSet::getSpotAngles, "index"_a, "Returns light `index`'s (cos(inner), cos(outer)) cone angles.")
		.def("getSourceRadius", &osgx::LightSet::getSourceRadius, "index"_a, "Returns light `index`'s physical source radius (0 = ideal point/spot).")
	;

	py::class_<
		osgx::OrbitLightRig,
		osg::NodeCallback,
		osg::ref_ptr<osgx::OrbitLightRig>
	>(
		m,
		"OrbitLightRig",
		"Animates a handful of point lights orbiting a center point, writing world-space "
		"position+intensity into an attached LightSet's posIntensity field every update "
		"traversal. `lights` must already have at least len(orbits) lights configured via "
		"setPoint()/setSpot() for their color/type/etc. -- this callback only ever touches "
		"position/intensity."
	)
		.def(py::init<>(), "Constructs a rig with the default three-orbit badge-lighting setup; set `lights` before use.")
		.def_readwrite("lights", &osgx::OrbitLightRig::lights, "The LightSet this rig animates.")
		.def_readwrite("center", &osgx::OrbitLightRig::center, "World-space center every orbit revolves around.")
		.def_readwrite("intensity", &osgx::OrbitLightRig::intensity, "Global intensity scale applied on top of each Orbit's own intensity.")
		.def_readwrite("orbits", &osgx::OrbitLightRig::orbits, "The list of Orbit parameters, one per animated light.")
	;
}

}
