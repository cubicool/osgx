#include "osgx-python.hpp"
#include "osgx/Environment.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Group>
#include <osg/Image>
#include <osg/Quat>
#include <osg/StateAttribute>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_environment(py::module_& m) {
	osgx::registerEnvironmentShaderLibs();

	m.attr("ENVIRONMENT_INPUTS") = osgx::ENVIRONMENT_INPUTS;
	m.attr("ENVIRONMENT_SAMPLE") = osgx::ENVIRONMENT_SAMPLE;
	m.attr("ENVIRONMENT_LIGHTING") = osgx::ENVIRONMENT_LIGHTING;
	m.attr("ENVIRONMENT_BINDING") = osgx::ENVIRONMENT_BINDING;
	m.attr("ENVIRONMENT_SPECULAR_TEXTURE_UNIT") = osgx::ENVIRONMENT_SPECULAR_TEXTURE_UNIT;
	m.attr("ENVIRONMENT_BRDF_LUT_TEXTURE_UNIT") = osgx::ENVIRONMENT_BRDF_LUT_TEXTURE_UNIT;
	m.attr("ENVIRONMENT_DIFFUSE_TEXTURE_UNIT") = osgx::ENVIRONMENT_DIFFUSE_TEXTURE_UNIT;

	py::class_<osgx::EnvironmentBakeOptions>(
		m,
		"EnvironmentBakeOptions",
		"Options for Environment's bake-from-HDR constructor: the specular (GGX prefilter) and "
		"diffuse (Lambertian) bakes, plus the shared BRDF LUT size."
	)
		.def(py::init<>(), "Constructs default options (specular.prefilterSize=256, lutSize=1024).")
		.def_readwrite("specular", &osgx::EnvironmentBakeOptions::specular, "GGXPrefilterOptions for the specular bake.")
		.def_readwrite("diffuse", &osgx::EnvironmentBakeOptions::diffuse, "LambertianBakeOptions for the diffuse bake.")
		.def_readwrite("lutSize", &osgx::EnvironmentBakeOptions::lutSize, "BRDF LUT resolution (shared per size, process-wide).")
		.def_readwrite(
			"bakeSpecular", &osgx::EnvironmentBakeOptions::bakeSpecular,
			"False skips the GGX specular bake; the specular map is a 1x1 placeholder until "
			"Environment.specularMap is assigned (e.g. a live-rebaked probe)."
		)
	;

	py::class_<osgx::Environment, osg::StateAttribute, osg::ref_ptr<osgx::Environment>>(
		m,
		"Environment",
		"Distant image-based lighting as one osg.StateAttribute: a prefiltered specular cubemap, a "
		"diffuse irradiance cubemap, and the split-sum BRDF LUT, plus orientation, roughness-to-mip "
		"mapping, and intensities in one std430 buffer. Attaching it binds all of that and enables "
		"seamless cubemap filtering. Read in GLSL via `#pragma osgx::environment ENVIRONMENT_INPUTS, "
		"ENVIRONMENT_SAMPLE[, ENVIRONMENT_LIGHTING]`."
	)
		.def(py::init<>(), "Constructs an empty Environment (no textures).")
		.def(
			py::init<osg::Image*, const osgx::EnvironmentBakeOptions&>(),
			"image"_a,
			"options"_a = osgx::EnvironmentBakeOptions(),
			"Bakes specular and diffuse cubemaps from an equirectangular HDR osg.Image on the GPU. "
			"Add `bakeRoot` to a rendered scene graph; textures are bindable immediately but only "
			"correct once the bake passes have rendered."
		)
		.def(
			py::init<osg::TextureCubeMap*, osg::TextureCubeMap*, float, int>(),
			"specularMap"_a,
			"diffuseMap"_a,
			"maxSpecularMip"_a = -1.0f,
			"lutSize"_a = 1024,
			"Wraps existing cubemaps. `maxSpecularMip` is the mip level of `specularMap` holding "
			"roughness 1.0 (negative = its last level; Khronos-style KTX2 prefilters need levels - 2)."
		)
		.def(
			py::init<osg::TextureCubeMap*, osg::TextureCubeMap*, osg::Texture2D*, float>(),
			"specularMap"_a,
			"diffuseMap"_a,
			"brdfLUT"_a,
			"maxSpecularMip"_a = -1.0f,
			"Wraps existing cubemaps with a caller-supplied BRDF LUT instead of the shared one."
		)
		.def_property(
			"rotation", &osgx::Environment::getRotation, &osgx::Environment::setRotation,
			"World-space rotation of the environment (osg.Quat). Identity is the equirect's own "
			"orientation; osg.Quat(-pi/2, osg.Vec3(0, 0, 1)) matches the Khronos glTF-Sample-Viewer."
		)
		.def_property(
			"maxSpecularMip", &osgx::Environment::getMaxSpecularMip, &osgx::Environment::setMaxSpecularMip,
			"Mip level of specularMap holding roughness 1.0."
		)
		.def_property(
			"diffuseIntensity", &osgx::Environment::getDiffuseIntensity, &osgx::Environment::setDiffuseIntensity,
			"Scale applied to osgx_EnvironmentIrradiance()."
		)
		.def_property(
			"specularIntensity", &osgx::Environment::getSpecularIntensity, &osgx::Environment::setSpecularIntensity,
			"Scale applied to osgx_EnvironmentSpecular()."
		)
		.def(
			"setSpecularMap", &osgx::Environment::setSpecularMap,
			"specularMap"_a, "maxSpecularMip"_a = -1.0f,
			"Replaces the specular map (e.g. a re-baked live probe) and resets maxSpecularMip "
			"(negative = the new map's last level)."
		)
		.def_property(
			"specularMap",
			&osgx::Environment::getSpecularMap,
			[](osgx::Environment& self, osg::TextureCubeMap* map) { self.setSpecularMap(map); },
			"The prefiltered specular cubemap. Assigning resets maxSpecularMip to its last level."
		)
		.def_property(
			"diffuseMap", &osgx::Environment::getDiffuseMap, &osgx::Environment::setDiffuseMap,
			"The diffuse irradiance cubemap."
		)
		.def_property_readonly("brdfLUT", &osgx::Environment::getBRDFLUT, "The shared split-sum BRDF LUT.")
		.def_property_readonly(
			"bakeRoot", &osgx::Environment::getBakeRoot,
			"PRE_RENDER passes still populating a texture; add to a rendered scene graph. None when "
			"nothing needs baking."
		)
	;
}

}
