// vimrun! ./utils/osgx-gltf-viewer model.gltf --env papermill.gltf
//
// osgx::gltf-owned viewer for its optional osgx-powered PBR/IBL renderer.
//
// A generated osgx::Environment's getBakeRoot() MUST be added to the scene graph or its BRDF-LUT and
// diffuse-irradiance passes never bake; both are ABSOLUTE_RF, so placement within that graph does
// not matter.

#include "osgx/gltf/Environment.hpp"
#include "osgx/gltf/SimplePlayer.hpp"
#include "osgx/Library.hpp"
#include "osgx/PBRScene.hpp"

#include "osgx/Callbacks.hpp"
#include "osgx/Core.hpp"
#include "osgx/IBL.hpp"
#include "osgx/Skinning.hpp"
#include "osgx/Visitors.hpp"
#include "osgx/Warnings.hpp"

#ifdef OSGX_IMGUI
#include "osgx/ImGui.hpp"
#endif

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

#include <osg/ArgumentParser>
#include <osg/Camera>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgDB/WriteFile>
#include <osg/DisplaySettings>
#include <osg/Group>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>
#include <osg/Vec3>
#include <osg/Vec4>
#include <osg/observer_ptr>
#include <osgGA/GUIActionAdapter>
#include <osgGA/GUIEventAdapter>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include "gltf/tg3_util.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <string>
#include <utility>

namespace {

class FramebufferPNG: public osg::Camera::DrawCallback {
public:
	FramebufferPNG(osg::Camera* camera, std::string filename):
		_camera(camera),
		_filename(std::move(filename)) {}

	void operator()(osg::RenderInfo&) const override {
		if(_started.exchange(true, std::memory_order_acq_rel)) return;

		const auto* viewport = _camera->getViewport();

		if(!viewport || !viewport->valid()) {
			OSG_WARN << "Capture camera has no viewport" << std::endl;
			_done.store(true, std::memory_order_release);

			return;
		}

		auto image = osgx::make_ref<osg::Image>();

		image->readPixels(
			static_cast<int>(viewport->x()),
			static_cast<int>(viewport->y()),
			static_cast<int>(viewport->width()),
			static_cast<int>(viewport->height()),
			GL_RGB,
			GL_UNSIGNED_BYTE
		);
		_success.store(osgDB::writeImageFile(*image, _filename), std::memory_order_relaxed);
		_done.store(true, std::memory_order_release);

		if(_success.load(std::memory_order_relaxed)) {
			OSG_NOTICE << "Wrote framebuffer screenshot: " << _filename << std::endl;
		}

		else OSG_WARN << "Failed to write framebuffer screenshot: " << _filename << std::endl;
	}

	bool done() const {
		return _done.load(std::memory_order_acquire);
	}

	bool success() const {
		return _success.load(std::memory_order_relaxed);
	}

private:
	osg::observer_ptr<osg::Camera> _camera;
	std::string _filename;
	mutable std::atomic<bool> _started{false};
	mutable std::atomic<bool> _done{false};
	mutable std::atomic<bool> _success{false};
};

bool applyKhronosCamera(osg::Camera* camera, const std::string& filename) {
	tinygltf3::Model document;
	tinygltf3::ErrorStack errors;
	tg3_parse_options opts;

	tg3_parse_options_init(&opts);

	opts.fs.read_file = &osgx::gltf::detail::tg3_read_file;
	opts.fs.free_file = &osgx::gltf::detail::tg3_free_file;

	if(tinygltf3::parse_file(document, errors, filename.c_str(), &opts) != TG3_OK || errors.has_error()) {
		std::cerr << "Failed to read camera export '" << filename << "'" << std::endl;

		for(std::uint32_t i = 0; i < errors.count(); i++) {
			const tg3_error_entry* entry = errors.entry(i);

			if(entry->severity == TG3_SEVERITY_ERROR) {
				std::cerr << "  " << (entry->message ? entry->message : "") << std::endl;
			}
		}

		return false;
	}

	if(document->cameras_count == 0) {
		std::cerr << "Camera export contains no cameras: " << filename << std::endl;

		return false;
	}

	const tg3_node* node = nullptr;

	for(std::uint32_t i = 0; i < document->nodes_count; i++) {
		if(document->nodes[i].camera >= 0) {
			node = &document->nodes[i];

			break;
		}
	}

	if(!node || !node->has_matrix) {
		std::cerr << "Camera export needs a camera node with a 4x4 matrix: " << filename << std::endl;

		return false;
	}

	const std::uint32_t cameraIndex = static_cast<std::uint32_t>(node->camera);

	if(cameraIndex >= document->cameras_count) {
		std::cerr << "Camera export references an invalid camera: " << filename << std::endl;

		return false;
	}

	const auto& perspective = document->cameras[cameraIndex].perspective;

	if(perspective.yfov <= 0.0 || perspective.aspect_ratio <= 0.0 || perspective.znear <= 0.0 || perspective.zfar <= perspective.znear) {
		std::cerr << "Camera export has an unsupported perspective projection: " << filename << std::endl;

		return false;
	}

	const auto& matrix = node->matrix;
	const auto zUp = [](double x, double y, double z) {
		return osg::Vec3d(x, -z, y);
	};
	const osg::Vec3d eye = zUp(matrix[12], matrix[13], matrix[14]);
	const osg::Vec3d forward = zUp(-matrix[8], -matrix[9], -matrix[10]);
	const osg::Vec3d up = zUp(matrix[4], matrix[5], matrix[6]);

	camera->setViewMatrixAsLookAt(eye, eye + forward, up);
	camera->setProjectionMatrixAsPerspective(
		perspective.yfov * 180.0 / std::numbers::pi,
		perspective.aspect_ratio,
		perspective.znear,
		perspective.zfar
	);

	return true;
}

bool readRawRGBA32F(const std::filesystem::path& filename, int width, int height, osg::ref_ptr<osg::Image>& image) {
	const std::size_t expectedBytes =
		static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4 * sizeof(float);
	std::error_code error;
	const auto actualBytes = std::filesystem::file_size(filename, error);

	if(error || actualBytes != expectedBytes) {
		std::cerr << "Expected " << expectedBytes << " bytes in '" << filename.string()
			<< "', got " << (error ? 0 : actualBytes) << std::endl;

		return false;
	}

	auto result = osgx::make_ref<osg::Image>();

	result->allocateImage(width, height, 1, GL_RGBA, GL_FLOAT);

	std::ifstream input(filename, std::ios::binary);

	input.read(reinterpret_cast<char*>(result->data()), static_cast<std::streamsize>(expectedBytes));

	if(!input) {
		std::cerr << "Failed to read '" << filename.string() << "'" << std::endl;

		return false;
	}

	image = result;

	return true;
}

// These are functions for using the "official" Khronos assets (the same that are used in the
// web-based viewer).
struct OfficialIBL {
	osg::ref_ptr<osg::TextureCubeMap> diffuse;
	osg::ref_ptr<osg::Texture2D> lut;
};

bool loadOfficialIBL(const std::filesystem::path& directory, OfficialIBL& ibl) {
	static constexpr std::array faces = {
		osg::TextureCubeMap::POSITIVE_X,
		osg::TextureCubeMap::NEGATIVE_X,
		osg::TextureCubeMap::POSITIVE_Y,
		osg::TextureCubeMap::NEGATIVE_Y,
		osg::TextureCubeMap::POSITIVE_Z,
		osg::TextureCubeMap::NEGATIVE_Z
	};
	auto diffuse = osgx::make_ref<osg::TextureCubeMap>();

	for(std::size_t face = 0; face < faces.size(); ++face) {
		osg::ref_ptr<osg::Image> image;

		if(!readRawRGBA32F(
			directory / ("khronos-lambertian-m0-f" + std::to_string(face) + ".rgba32f"),
			256,
			256,
			image
		)) return false;

		diffuse->setImage(faces[face], image);
	}

	diffuse->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	diffuse->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	diffuse->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	diffuse->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	diffuse->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	diffuse->setUseHardwareMipMapGeneration(false);

	osg::ref_ptr<osg::Image> lutImage;

	if(!readRawRGBA32F(directory / "khronos-ggx-lut.rgba32f", 1024, 1024, lutImage)) return false;

	auto lut = osgx::make_ref<osg::Texture2D>();

	lut->setImage(lutImage);
	lut->setInternalFormat(GL_RGBA32F);
	lut->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	lut->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	lut->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	lut->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

	ibl.diffuse = diffuse;
	ibl.lut = lut;

	return true;
}

std::filesystem::path findModelFile(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(
		requested.stem().string(),
		{"glTF-Sample-Assets/Models/{}/glTF/{}.gltf"}
	);
}

std::filesystem::path findHDREnvironment(std::string_view filename) {
	return osgx::findDataFile(
		filename,
		{
			"{}",
			"glTF-Sample-Environments/{}"
		},
		".hdr"
	);
}

std::filesystem::path findEnvironmentManifest(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(requested.stem().string(), {"env/{}.gltf"});
}

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);
	auto lib = osgx::initialize(args);

	args.getApplicationUsage()->setCommandLineUsage(
		std::string(args.getApplicationName()) +
		" <model.gltf> (--hdr <path> | --env <manifest.gltf> | --official-ibl <dir>) [--camera <camera.gltf>] [--capture <path.png>] [--samples <count>] [--debug [mode]] [--animation]"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hdr <path>",
		"Source HDR environment. Bakes diffuse irradiance, BRDF LUT, and GGX-prefiltered specular "
		"all live from this one file."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--env <manifest.gltf>",
		"Static/shipping path: an osgx_environment manifest referencing pre-baked specular/diffuse KTX2 "
		"cubemaps and a declared BRDF LUT. No HDR decode or cubemap bake at runtime."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--official-ibl <directory>",
		"Use the preserved Khronos raw diffuse/LUT assets plus khronos-ggx.ktx2 (parity harness only)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--capture <path.png>",
		"Write the first complete framebuffer to a PNG and exit"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--camera <path.gltf>",
		"Apply a Khronos Sample Viewer camera export"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--samples <count>",
		"Request this many default-framebuffer MSAA samples (default: 4)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--animation",
		"Show an ImGui panel selecting/pausing/restarting the model's animations (ImGui builds "
		"only), and deform skinned meshes when the model has any (linear blend skinning for the "
		"whole model, so unskinned meshes in a partly skinned model deform wrongly)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--debug [mode]",
		"combined, diffuse, specular, base-color, roughness, metallic, normal-texture, normal-texture-raw, geometry-normal, shading-normal, geometry-tangent, bitangent, linear-diffuse, linear-specular, or linear-combined"
	);

	std::string hdrPath, envPath, officialIBLPath, cameraPath, capturePath, debugName = "combined";
	int samples = 4;
	const std::map<std::string, int> debugModes = {
		{"combined", 0}, {"diffuse", 1}, {"specular", 2},
		{"base-color", 3}, {"roughness", 4}, {"metallic", 5},
		{"normal-texture", 6}, {"normal-texture-raw", 7}, {"geometry-normal", 8},
		{"shading-normal", 9}, {"geometry-tangent", 10}, {"bitangent", 11},
		{"linear-diffuse", 12}, {"linear-specular", 13}, {"linear-combined", 14}
	};

	const bool haveHdr = args.read("--hdr", hdrPath);
	const bool haveEnv = args.read("--env", envPath);
	const bool haveOfficialIBL = args.read("--official-ibl", officialIBLPath);
	const bool haveCamera = args.read("--camera", cameraPath);
	const bool captureRequested = args.read("--capture", capturePath);
	const bool animation = args.read("--animation");
	args.read("--samples", samples);
	const int debugPos = args.find("--debug");
	const bool diagnostics = debugPos >= 0;

	if(diagnostics) {
		const bool hasMode = debugPos + 1 < args.argc() && debugModes.contains(args[debugPos + 1]);

		if(hasMode) args.read(debugPos, "--debug", debugName);
		else args.read(debugPos, "--debug");
	}

	if(args.argc() < 2 || (!haveOfficialIBL && !haveHdr && !haveEnv) || samples < 0) {
		args.getApplicationUsage()->write(std::cerr);

		return 1;
	}

	osg::DisplaySettings::instance()->setNumMultiSamples(static_cast<unsigned int>(samples));
	osgViewer::Viewer viewer(args);

	// ReaderWriterGLTF registers this same alias in its own constructor, but that
	// constructor only runs *after* the registry has already resolved which plugin
	// library to dlopen for a given extension - too late for a cold ".glb" load.
	// Registering it here first breaks the chicken-and-egg.
	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	const auto modelPath = findModelFile(args[1]);
	osg::ref_ptr<osg::Node> model;

	if(!modelPath.empty()) model = osgDB::readRefNodeFile(modelPath.string());

	if(!model) {
		std::cerr << "Failed to load: " << args[1] << std::endl;

		return 1;
	}

	osg::ref_ptr<osgx::Environment> environment;
	std::filesystem::path hdrEnvironment;

	if(haveHdr) {
		hdrEnvironment = findHDREnvironment(hdrPath);

		if(hdrEnvironment.empty()) {
			std::cerr << "Failed to find HDR environment: " << hdrPath << std::endl;

			return 1;
		}
	}

	if(haveOfficialIBL) {
		OfficialIBL officialIBL;

		if(!loadOfficialIBL(officialIBLPath, officialIBL)) return 1;

		const std::filesystem::path directory(officialIBLPath);

		auto specularMap = osgx::loadPrefilterCubemap((directory / "khronos-ggx.ktx2").string());

		if(specularMap) {
			// The Khronos viewer's exported GGX cube is six levels (256 down to 8) with the GGX
			// roughness range over the first five; the sixth is not part of it.
			environment = osgx::make_ref<osgx::Environment>(
				specularMap.get(), officialIBL.diffuse.get(), officialIBL.lut.get(), 4.0f
			);

			environment->setRotation(osgx::gltf::KHRONOS_ENVIRONMENT_ROTATION);
		}
	}

	else if(haveEnv) {
		const auto manifest = findEnvironmentManifest(envPath);

		if(manifest.empty()) {
			std::cerr << "Failed to find environment manifest: " << envPath << std::endl;

			return 1;
		}

		environment = osgx::gltf::loadEnvironment(manifest.string());
	}

	else if(auto hdrImage = osgDB::readRefImageFile(hdrEnvironment.string())) {
		environment = osgx::make_ref<osgx::Environment>(hdrImage.get());

		environment->setRotation(osgx::gltf::KHRONOS_ENVIRONMENT_ROTATION);
	}

	if(!environment) {
		std::cerr << "Failed to prepare PBR IBL resources" << std::endl;

		return 1;
	}

	// Linear blend skinning replaces the identity hook for the whole model, so it is used only when
	// some geometry carries joint weights: a vertex with no joint arrays reads GL's default
	// attribute values and would be deformed by an arbitrary joint matrix.
	bool skinned = false;

	if(animation) {
		osgx::LambdaVisitor<osg::Geometry> findJointWeights([&skinned](osg::Geometry& geometry) {
			if(geometry.getVertexAttribArray(osgx::JOINT_WEIGHTS_ATTRIBUTE)) skinned = true;
		});

		findJointWeights(model.get());
	}

	osgx::HookList hooks;

	if(skinned) hooks.push_back({
		osgx::Hook::Skinning,
		new osg::Shader(
			osg::Shader::VERTEX,
			osgx::resolveShaderLibs(osgx::SKINNING_HOOK_LINEAR_BLEND)
		)
	});

	auto pbrs = osgx::PBRScene::create(model, {
		.environment = environment.get(),
		.hooks = hooks,
		.diagnostics = diagnostics
	});

	if(!pbrs.valid()) return 1;

	const auto debug = debugModes.find(debugName);

	if(diagnostics && debug == debugModes.end()) {
		std::cerr << "Unknown --debug mode: " << debugName << std::endl;

		return 1;
	}

	if(diagnostics) pbrs.debugMode->set(debug->second);

	auto root = osgx::make_ref<osg::Group>();

	if(environment->getBakeRoot()) root->addChild(environment->getBakeRoot());
	root->addChild(model);

	viewer.setSceneData(root);
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.getCamera()->setClearColor(osg::Vec4f(
		48.0f / 255.0f,
		53.0f / 255.0f,
		66.0f / 255.0f,
		1.0f
	)); // #303542

	// A Khronos camera export pins an exact view matrix on the camera itself, which only
	// sticks frame-to-frame because nothing else drives it. Installing a manipulator in that
	// case would fight it (the manipulator's matrix wins every frame). Without --camera there
	// was previously no manipulator at all, so the interactive viewer sat at OSG's default
	// (near-origin, unframed) view with no mouse navigation - this is that missing wiring.
	if(haveCamera) {
		if(!applyKhronosCamera(viewer.getCamera(), cameraPath)) return 1;
	}

	else viewer.setCameraManipulator(new osgGA::TrackballManipulator());

	// Diagnostics, ported from pyosg-khronos-viewer.py: 1/2/3 pick debugMode; N/R toggle the
	// normal/roughness maps.
	if(diagnostics) std::cout <<
		"Diagnostics: 1=combined 2=diffuse 3=specular N=toggle normal map "
		"R=toggle roughness map A=toggle specular AA" << std::endl
	;

	if(diagnostics) viewer.addEventHandler(new osgx::LambdaKeyHandler(
		{'1', '2', '3', 'n', 'N', 'r', 'R', 'a', 'A'},
		[pbrs](const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&, int key) {
			switch(key) {
				case '1': {
					pbrs.debugMode->set(0);

					std::cout << "[diagnostic] combined" << std::endl;

					break;
				}

				case '2': {
					pbrs.debugMode->set(1);

					std::cout << "[diagnostic] diffuse only" << std::endl;

					break;
				}

				case '3': {
					pbrs.debugMode->set(2);

					std::cout << "[diagnostic] specular only" << std::endl;

					break;
				}

				case 'n': case 'N': {
					int v = 0;

					pbrs.disableNormalMap->get(v);
					pbrs.disableNormalMap->set(1 - v);

					std::cout << "[diagnostic] normal map " << (v ? "on" : "off") << std::endl;

					break;
				}

				case 'r': case 'R': {
					int v = 0;

					pbrs.disableRoughnessMap->get(v);
					pbrs.disableRoughnessMap->set(1 - v);

					std::cout << "[diagnostic] roughness map " << (v ? "on" : "off") << std::endl;

					break;
				}

				case 'a': case 'A': {
					int v = 0;

					pbrs.disableSpecularAA->get(v);
					pbrs.disableSpecularAA->set(1 - v);

					std::cout << "[diagnostic] specular AA " << (v ? "on" : "off") << std::endl;

					break;
				}

				default: return false;
			}

			return true;
		}
	));

	osgx::gltf::SimplePlayer player(model);

	if(animation && !player) std::cout << "--animation: the model has no animations" << std::endl;

#ifdef OSGX_IMGUI
	if(animation && player) {
		// Dear ImGui's single global context is not safe across several OSG draw threads.
		viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

		auto* gui = new osgx::imgui::Widget(viewer);

		gui->addSection("Animation", [&player](osg::RenderInfo&) {
			for(std::size_t i = 0; i < player.getNumAnimations(); i++) {
				std::string name = player.getAnimationName(i);

				if(name.empty()) name = "animation " + std::to_string(i);

				ImGui::PushID(static_cast<int>(i));

				if(ImGui::Selectable(name.c_str(), player.getCurrentAnimationIndex() == i)) {
					player.playAnimation(i);
				}

				ImGui::PopID();
			}

			if(ImGui::Button(player.getPlaying() ? "Pause" : "Play")) player.togglePlaying();

			ImGui::SameLine();

			if(ImGui::Button("Restart")) player.restart();
		});
	}
#else
	if(animation && player) {
		std::cout << "--animation: built without ImGui, no animation panel" << std::endl;
	}
#endif

	osg::ref_ptr<FramebufferPNG> capture;

	if(captureRequested) {
		capture = new FramebufferPNG(viewer.getCamera(), capturePath);
		viewer.getCamera()->setFinalDrawCallback(capture);
	}

	while(!viewer.done()) {
		viewer.frame();

		if(capture && capture->done()) break;
	}

	return capture && !capture->success() ? 1 : 0;
}
