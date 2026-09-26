// vimrun! ./examples/osgx-library
//
// Customizing osgx's binding slots (osgx/Library.hpp) for an application that has shaders of its
// own. The application here plugs one fragment shader into osgx::PBRScene (a Hook::Tonemap) that
// reads two buffers bound once at the scene root:
//
// - AppTint, a "legacy" SSBO hard-coded to binding 0.
// - AppGlobals, a UBO at the application-declared slot "app::globals", written in GLSL as the
//   @app::globals@ token like osgx's own blocks.
//
// AppLibrary reserves SSBO 0 so osgx never assigns it, pins the "osgx::material" UBO to 7, and
// declares "app::globals". The slot table is printed once when the Library is created (nothing
// assigned yet) and again once the scene is built (only the slots it used are assigned).
//
// With --no-reserve, osgx::joints - the first SSBO a skinned model uses - is assigned index 0. The
// palette binding on each skinned mesh then replaces the application's tint binding under that
// mesh, and the tonemap hook reads joint matrices as its tint.
//
// Usage: osgx-library [model] [--env <manifest>] [--no-reserve]
// `model` defaults to CesiumMan; a bare name is looked up as a glTF-Sample-Assets model and a bare
// environment name under env/, both through osgx::findDataFile() (OSG_FILE_PATH).
// Keys: +/- change AppGlobals' exposure.

#include "osgx/Callbacks.hpp"
#include "osgx/Array.hpp"
#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/Light.hpp"
#include "osgx/PBR.hpp"
#include "osgx/PBRScene.hpp"
#include "osgx/Shader.hpp"
#include "osgx/Skinning.hpp"
#include "osgx/gltf/Environment.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/Group>
#include <osg/Shader>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <filesystem>
#include <iostream>
#include <string>

namespace {

constexpr char APP_TONEMAP_HOOK[] = R"GLSL(
#version 460 core

#pragma osgx::pbr TONEMAP_PBR_NEUTRAL

layout(std430, binding = 0) readonly buffer AppTint {
	vec4 tint;
};

layout(std140, binding = @app::globals@) uniform AppGlobals {
	float exposure;
};

vec3 osgx_Tonemap(vec3 color) {
	return osgx_TonemapPBRNeutral(color * exposure) * tint.rgb;
}
)GLSL";

osgx::LibraryOptions appOptions(bool reserve) {
	osgx::LibraryOptions options{.bindings = {{"osgx::material", 7}}};

	if(reserve) options.reserveBelow(osgx::Bindings::Type::SSBO, 1);

	return options;
}

class AppLibrary: public osgx::Library {
public:
	AppLibrary(osg::ArgumentParser* arguments, bool reserve):
	osgx::Library(arguments, appOptions(reserve)) {
		bindings().declare(osgx::Bindings::Type::UBO, "app::globals");
	}
};

const char* typeName(osgx::Bindings::Type type) {
	switch(type) {
		case osgx::Bindings::Type::UBO: return "UBO";
		case osgx::Bindings::Type::SSBO: return "SSBO";
		case osgx::Bindings::Type::TextureUnit: return "TextureUnit";
	}

	return "?";
}

std::filesystem::path findModelFile(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(
		requested.stem().string(), {"glTF-Sample-Assets/Models/{}/glTF/{}.gltf"}
	);
}

std::filesystem::path findEnvironmentManifest(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(requested.stem().string(), {"env/{}.gltf"});
}

void printSlots(osgx::Library& lib, const char* title) {
	std::cout << title << std::endl;

	for(const auto& slot : lib.bindings().slots()) {
		std::cout << "  " << typeName(slot.type) << " " << slot.name << " ";

		if(slot.index) std::cout << *slot.index << std::endl;

		else std::cout << "-" << std::endl;
	}
}

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	const bool reserve = !args.read("--no-reserve");
	std::string envPath;

	args.read("--env", envPath);

	AppLibrary lib(&args, reserve);

	printSlots(lib, "Declared slots:");

	const std::string modelName = args.argc() > 1 ? args[1] : "CesiumMan";
	const auto modelPath = findModelFile(modelName);

	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	osg::ref_ptr<osg::Node> model = modelPath.empty() ?
		nullptr :
		osgDB::readRefNodeFile(modelPath.string())
	;

	if(!model) {
		std::cerr << "Failed to load " << modelName << std::endl;

		return 1;
	}

	osg::ref_ptr<osgx::Environment> environment;

	if(!envPath.empty()) {
		const auto manifest = findEnvironmentManifest(envPath);

		if(!manifest.empty()) environment = osgx::gltf::loadEnvironment(manifest.string());

		if(!environment) {
			std::cerr << "Failed to load environment " << envPath << std::endl;

			return 1;
		}
	}

	const bool skinned = osgx::hasJointWeights(model.get());

	osgx::HookList hooks = {{
		osgx::Hook::Tonemap,
		new osg::Shader(osg::Shader::FRAGMENT, osgx::resolveShaderLibs(APP_TONEMAP_HOOK))
	}};

	if(skinned) hooks.push_back({
		osgx::Hook::Skinning,
		new osg::Shader(osg::Shader::VERTEX, osgx::resolveShaderLibs(osgx::SKINNING_HOOK_LINEAR_BLEND))
	});

	if(!osgx::PBRScene::create(model, {.environment = environment.get(), .hooks = hooks}).valid()) {
		return 1;
	}

	auto root = osgx::make_ref<osg::Group>();
	auto* ss = root->getOrCreateStateSet();

	root->addChild(model);

	if(environment && environment->getBakeRoot()) root->addChild(environment->getBakeRoot());

	// AppTint: one vec4 at the hard-coded SSBO binding 0.
	auto tint = osgx::Vec4Array::create({osg::Vec4(1.0f, 0.8f, 0.6f, 1.0f)});

	tint->setBufferObject(new osg::ShaderStorageBufferObject());

	ss->setAttributeAndModes(new osg::ShaderStorageBufferBinding(
		0,
		tint.get(),
		0,
		static_cast<GLsizeiptr>(tint->getTotalDataSize())
	));

	// AppGlobals: exposure, then std140's padding to 16 bytes.
	auto globals = osgx::FloatArray::create({1.0f, 0.0f, 0.0f, 0.0f});

	globals->setBufferObject(new osg::UniformBufferObject());

	ss->setAttributeAndModes(new osg::UniformBufferBinding(
		lib.bindings().get("app::globals"),
		globals.get(),
		0,
		static_cast<GLsizeiptr>(globals->getTotalDataSize())
	));

	auto lights = osgx::make_ref<osgx::LightSet>();

	lights->setDirectional(0, osg::Vec3(-0.4f, 0.6f, -0.7f), osg::Vec3(1.0f, 1.0f, 1.0f), 3.0f);

	ss->setAttributeAndModes(lights);

	printSlots(lib, "Assigned after building the scene:");

	osgViewer::Viewer viewer(args);

	viewer.addEventHandler(new osgx::LambdaKeyHandler({'+', '='}, [globals](auto&, auto&) {
		(*globals)[0] *= 1.25f;
		globals->dirty();

		std::cout << "exposure " << (*globals)[0] << std::endl;

		return true;
	}));

	viewer.addEventHandler(new osgx::LambdaKeyHandler('-', [globals](auto&, auto&) {
		(*globals)[0] /= 1.25f;
		globals->dirty();

		std::cout << "exposure " << (*globals)[0] << std::endl;

		return true;
	}));

	viewer.setSceneData(root);

	return viewer.run();
}
