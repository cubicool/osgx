// vimrun! ./examples/osgx-gbuffer model.gltf --env env/papermill.gltf
//
// Proves osgx::gltf::pbribl's deferred split (PBRIBLGBuffer::create() + PBRIBLLightingScene::create())
// renders identically to PBRIBLScene::create()'s monolithic forward shader - same model, same
// --hdr/--env environment loading osgx-gltf-viewer.cpp/osgx-turntable.cpp already use, plus a
// THIRD camera proving osgx::shadow fits into the split too: shadow-casting only needs depth,
// not material data, so it sits alongside the geometry pass rather than inside it, and plugs
// into the lighting pass via the exact same PBRIBLLightingPassOptions::shadowMap seam
// PBRIBLScene::create()'s own shadowMap parameter uses.
//
// Press 0-6 to inspect the raw G-buffer channels (0=lit composite, 1=albedo, 2=normal,
// 3=material(roughness/metallic), 4=emissive, 5=depth, 6=SSAO) - the same diagnostic shape
// pyosg-mrt.py's own visualizeMode toggle uses, confirming each channel independently instead
// of only ever looking at the final composite.
//
// Also the first live consumer of osgx::SSAO (GBuffer.hpp) - the generic hemisphere-kernel
// screen-space AO pass ported from OpenSceneGraph.py/examples/pyosg-lighting/11-sketchfab.py's
// hand-rolled one. Feeds PBRIBLLightingPassOptions::aoTexture, the same seam a caller's own
// hand-built SSAO (or a baked lightmap, or nothing) would use instead.
//
// Also proves osgx::shadow's 3 correctness fixes (same as osgx-shadow.cpp - see that file's own
// header comment for the full writeup) inside the deferred G-buffer pipeline specifically: the
// shadow camera below is orthographic (not perspective), never sets its own depth-only Program
// (ShadowMap::create() does that internally now), and its "Directional Light" ImGui
// section drags the key light live via ShadowMap::reposition() - watch
// the floor's shadow track the drag with no camera/FBO rebuild, while osgx::LightGizmos (reading
// the same LightSet) shows the plane/arrow moving in sync.

#include "osgx/Callbacks.hpp"
#include "osgx/Core.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/Gizmos.hpp"
#include "osgx/IBL.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Shadow.hpp"
#include "osgx/gltf/PBRIBL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/Camera>
#include <osg/ComputeBoundsVisitor>
#include <osg/DisplaySettings>
#include <osg/GL>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 800;

std::filesystem::path findModelFile(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(
		requested.stem().string(), {"glTF-Sample-Assets/Models/{}/glTF/{}.gltf"}
	);
}

std::filesystem::path findHDREnvironment(std::string_view filename) {
	return osgx::findDataFile(filename, {"{}", "glTF-Sample-Environments/{}"}, ".hdr");
}

std::filesystem::path findEnvironmentManifest(std::string_view filename) {
	if(auto path = osgx::findDataFile(filename); !path.empty()) return path;

	const std::filesystem::path requested(filename);

	return osgx::findDataFile(requested.stem().string(), {"env/{}.gltf"});
}

// Refreshes the lighting pass's view-matrix uniforms from a preDrawCallback on the shadow
// camera - the FIRST PRE_RENDER camera in this scene graph by render order (default order 0,
// same as the geometry pass, but added to `root` before it). Every PRE_RENDER camera finishes
// drawing before the main viewer camera's own preDrawCallback fires (confirmed against OSG
// 3.6.5's RenderStage::draw()), so this is the earliest point in the frame that still sees the
// CURRENT frame's fresh camera matrices - calling PBRIBLLightingScene::update() from application
// code after viewer.frame() returns (the previous, buggy version of this example) hands the
// lighting pass a one-frame-stale matrix instead, which showed up live as a shadow/position
// artifact that visibly worsened while the camera was actively orbiting/zooming.
// `ssaoProjection` is optional (nullptr when SSAO wasn't built) - refreshed here alongside the
// lighting pass's own view-matrix uniforms for the same reason: SSAO's forward re-projection
// needs the CURRENT frame's projection matrix, not a stale one from application code running
// after viewer.frame() returns. See osgx::SSAO::create()'s own doc comment (GBuffer.hpp).
class UpdateLightingPassCallback: public osg::Camera::DrawCallback {
public:
	UpdateLightingPassCallback(
		osgx::gltf::pbribl::PBRIBLLightingScene* scene,
		osg::Camera* mainCamera,
		osg::Uniform* ssaoProjection=nullptr
	):
		_scene(scene), _mainCamera(mainCamera), _ssaoProjection(ssaoProjection) {}

	void operator()(osg::RenderInfo&) const override {
		_scene->update(_mainCamera.get());

		if(_ssaoProjection.valid()) {
			_ssaoProjection->set(osg::Matrixf(_mainCamera->getProjectionMatrix()));
		}
	}

private:
	osgx::gltf::pbribl::PBRIBLLightingScene* _scene;
	osg::observer_ptr<osg::Camera> _mainCamera;
	osg::observer_ptr<osg::Uniform> _ssaoProjection;
};

// Debug blit: samples any one texture into a fullscreen quad, with a small per-channel remap
// (raw color passthrough / signed-normal-to-[0,1] / single-channel depth grayscale) - NOT part
// of osgx::gbuffer or osgx::gltf::pbribl itself, this is purely an example-level diagnostic aid,
// same role pyosg-mrt.py's own visualizeMode branches played.
constexpr const char DEBUG_BLIT_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

uniform sampler2D blitTex;
uniform int channelMode; // 0 = color passthrough, 1 = signed normal, 2 = depth

in vec2 vUV;

out vec4 fragColor;

void main() {
	vec4 s = texture(blitTex, vUV);

	if(channelMode == 1) {
		fragColor = vec4(s.rgb * 0.5 + 0.5, 1.0);

		return;
	}

	if(channelMode == 2) {
		fragColor = vec4(vec3(s.r), 1.0);

		return;
	}

	fragColor = vec4(s.rgb, 1.0);
}
)GLSL";

class VisualizeModeHandler: public osgGA::GUIEventHandler {
public:
	VisualizeModeHandler(
		osg::Camera* lightingCamera,
		osg::Camera* debugCamera,
		osg::Uniform* channelModeUniform,
		const osgx::gltf::pbribl::PBRIBLGBuffer& gbuffer,
		osg::Texture2D* aoTexture=nullptr
	):
		_lightingCamera(lightingCamera),
		_debugCamera(debugCamera),
		_channelModeUniform(channelModeUniform),
		_gbuffer(gbuffer),
		_aoTexture(aoTexture) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override {
		if(ea.getEventType() != osgGA::GUIEventAdapter::KEYDOWN) return false;

		switch(ea.getKey()) {
			case '0': select(-1, 0, "lit composite"); return true;
			case '1': select(0, 0, "albedo"); return true;
			case '2': select(1, 1, "view-space normal"); return true;
			case '3': select(2, 0, "material (r=roughness, g=metallic)"); return true;
			case '4': select(3, 0, "emissive"); return true;
			case '5': select(4, 2, "depth"); return true;
			case '6': if(_aoTexture.valid()) { select(5, 2, "SSAO"); } return true;
			default: return false;
		}
	}

private:
	void select(int textureUnit, int channelMode, const char* label) {
		const bool composite = textureUnit < 0;

		_lightingCamera->setNodeMask(composite ? 0xffffffff : 0);
		_debugCamera->setNodeMask(composite ? 0 : 0xffffffff);

		if(!composite) {
			osg::Texture2D* tex = nullptr;

			switch(textureUnit) {
				case 0: tex = _gbuffer.albedoTexture; break;
				case 1: tex = _gbuffer.normalTexture; break;
				case 2: tex = _gbuffer.materialTexture; break;
				case 3: tex = _gbuffer.emissiveTexture; break;
				case 4: tex = _gbuffer.depthTexture; break;
				case 5: tex = _aoTexture.get(); break;
				default: break;
			}

			_debugCamera->getOrCreateStateSet()->setTextureAttributeAndModes(
				0, tex, osg::StateAttribute::ON
			);
			_channelModeUniform->set(channelMode);
		}

		std::cout << "osgx-gbuffer: showing " << label << std::endl;
	}

	osg::observer_ptr<osg::Camera> _lightingCamera;
	osg::observer_ptr<osg::Camera> _debugCamera;
	osg::observer_ptr<osg::Uniform> _channelModeUniform;
	osgx::gltf::pbribl::PBRIBLGBuffer _gbuffer;
	osg::observer_ptr<osg::Texture2D> _aoTexture;
};

// Floor: a plain procedural quad, deliberately NOT sharing the model's G-buffer Program
// (PBRIBLGBuffer::create()'s shader reads a structured osgx_gltf_Material buffer only the
// glTF loader ever populates - a quad built by hand has none of that data, so reading it
// unbound would be garbage, not a harmless default). Writes flat albedo/normal/roughness-
// metallic/zero-emissive/position straight into all 5 G-buffer attachments with its own trivial
// shader pair instead. Added as a child of the GEOMETRY pass camera, never the shadow camera --
// it's a shadow receiver, not a caster. gPosition specifically matters: an attachment a
// fragment shader never writes stays at the geometry pass's clear value (0,0,0,0), and
// forgetting it here once produced a real, hard-to-diagnose bug - every floor pixel's
// reconstructed worldPos silently collapsed to a single constant point (the camera's own eye
// position, from osgx_mainViewMatrixInverse's translation column), landing nowhere near the
// shadow frustum and making osgx_ShadowFactor() read "unshadowed" everywhere regardless of
// shadowStrength.
constexpr const char FLOOR_VERTEX_SHADER[] = R"GLSL(
#version 460 core

in vec4 osg_Vertex;
in vec3 osg_Normal;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat4 osg_ModelViewMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vPosition;
out vec3 vNormal;

void main() {
	vec4 eyePos = osg_ModelViewMatrix * osg_Vertex;

	vPosition = eyePos.xyz;
	vNormal = normalize(osg_NormalMatrix * osg_Normal);
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

constexpr const char FLOOR_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core

in vec3 vPosition;
in vec3 vNormal;

uniform vec3 floorAlbedo;

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gMaterial;
layout(location = 3) out vec4 gEmissive;
layout(location = 4) out vec4 gPosition;

void main() {
	gAlbedo = vec4(floorAlbedo, 1.0);
	gNormal = vec4(normalize(vNormal), 0.0);
	gMaterial = vec4(0.85, 0.0, 0.0, 0.0); // roughness = 0.85, metallic = 0
	gEmissive = vec4(0.0, 0.0, 0.0, 1.0);
	gPosition = vec4(vPosition, 1.0);
}
)GLSL";

// `center`/`halfSize`/`z` are meant to come from the MODEL's own bound (ComputeBoundsVisitor in
// main() below), not hardcoded per-model constants the way the old hand-rolled pyosg-lighting/
// 08-shadows.py's --floor-z/--floor-size CLI defaults were (tuned for BoomBox specifically,
// wrong for an arbitrary CLI-loaded model).
osg::ref_ptr<osg::Geode> makeFloor(const osg::Vec3& center, float halfSize, float z) {
	auto positions = osgx::make_ref<osg::Vec3Array>();

	positions->push_back(osg::Vec3(center.x() - halfSize, center.y() - halfSize, z));
	positions->push_back(osg::Vec3(center.x() + halfSize, center.y() - halfSize, z));
	positions->push_back(osg::Vec3(center.x() + halfSize, center.y() + halfSize, z));
	positions->push_back(osg::Vec3(center.x() - halfSize, center.y() + halfSize, z));

	auto normals = osgx::make_ref<osg::Vec3Array>();

	normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

	auto geometry = osgx::make_ref<osg::Geometry>();

	geometry->setVertexArray(positions.get());
	geometry->setNormalArray(normals.get(), osg::Array::BIND_OVERALL);
	// GL_TRIANGLE_FAN, not GL_QUADS - removed in a core-profile context (this project's shaders
	// are all "#version 460 core"); a 4-vertex fan is the same two triangles for a convex quad.
	geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));

	auto prog = osgx::make_nref<osg::Program>("osgx_gbuffer_Floor");

	prog->addShader(new osg::Shader(osg::Shader::VERTEX, FLOOR_VERTEX_SHADER));
	prog->addShader(new osg::Shader(osg::Shader::FRAGMENT, FLOOR_FRAGMENT_SHADER));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(geometry.get());

	auto* ss = geode->getOrCreateStateSet();

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
	// Neutral warm stone - same role as 08-shadows.py's own floor albedo default.
	ss->addUniform(new osg::Uniform("floorAlbedo", osg::Vec3(0.75f, 0.72f, 0.68f)));

	return geode;
}

osg::ref_ptr<osg::Camera> makeDebugBlitCamera(osg::Uniform*& channelModeOut) {
	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0)
	);
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(quad);

	auto prog = osgx::make_nref<osg::Program>("osgx_gbuffer_DebugBlit");

	prog->addShader(new osg::Shader(osg::Shader::VERTEX, osgx::FULLSCREEN_VERT));
	prog->addShader(new osg::Shader(osg::Shader::FRAGMENT, DEBUG_BLIT_FRAGMENT_SHADER));

	auto cam = osgx::make_nref<osg::Camera>("osgx_gbuffer_DebugBlit");

	cam->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	cam->setRenderOrder(osg::Camera::POST_RENDER, 1);
	cam->setClearMask(0);
	cam->setProjectionMatrix(osg::Matrix::identity());
	cam->setViewMatrix(osg::Matrix::identity());
	cam->addChild(geode);
	cam->setNodeMask(0);

	auto* ss = cam->getOrCreateStateSet();

	ss->setAttributeAndModes(prog, osg::StateAttribute::ON);
	ss->addUniform(new osg::Uniform("blitTex", 0));

	auto* channelMode = new osg::Uniform("channelMode", 0);

	ss->addUniform(channelMode);
	channelModeOut = channelMode;

	return cam;
}

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	args.getApplicationUsage()->setCommandLineUsage(
		std::string(args.getApplicationName()) +
		" <model.gltf> (--hdr <path> | --env <manifest.gltf>) [--samples <count>]"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--hdr <path>",
		"Source HDR environment - bakes diffuse irradiance, BRDF LUT, and GGX-prefiltered "
		"specular all live."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--env <manifest.gltf>",
		"Pre-baked osgx_pbribl environment manifest - no HDR decode/bake at runtime."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--samples <count>", "Request this many default-framebuffer MSAA samples (default: 4)"
	);

	std::string hdrPath, envPath;
	int samples = 4;

	const bool haveHdr = args.read("--hdr", hdrPath);
	const bool haveEnv = args.read("--env", envPath);

	args.read("--samples", samples);

	if(args.argc() < 2 || (!haveHdr && !haveEnv) || samples < 0) {
		args.getApplicationUsage()->write(std::cerr);

		return 1;
	}

	osg::DisplaySettings::instance()->setNumMultiSamples(static_cast<unsigned int>(samples));
	osgViewer::Viewer viewer(args);

#ifdef OSGX_IMGUI
	// Dear ImGui's single global context isn't safe to touch from more than one OSG draw thread --
	// see osgx::imgui::Widget's own class comment; harmless to set unconditionally even when
	// OSGX_IMGUI is off.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
#endif

	// Force the real window to exactly WIDTH x HEIGHT - the G-buffer textures are that fixed
	// size, and if the actual window ends up a different size/aspect (the default, since no
	// --window was passed), the geometry pass renders under one aspect ratio while
	// viewer.getCamera()'s own projection matrix reflects a different one. Same fix
	// 11-sketchfab.py applies via its own OSG_WINDOW env var, just via the C++ API here.
	viewer.setUpViewInWindow(50, 50, WIDTH, HEIGHT);

	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	const auto modelPath = findModelFile(args[1]);
	osg::ref_ptr<osg::Node> model;

	if(!modelPath.empty()) model = osgDB::readRefNodeFile(modelPath.string());

	if(!model) {
		std::cerr << "Failed to load: " << args[1] << std::endl;

		return 1;
	}

	osg::ref_ptr<osgx::Environment> environment;

	if(haveEnv) {
		const auto manifest = findEnvironmentManifest(envPath);

		if(manifest.empty()) {
			std::cerr << "Failed to find environment manifest: " << envPath << std::endl;

			return 1;
		}

		environment = osgx::gltf::pbribl::loadEnvironment(manifest.string());
	}

	else {
		const auto hdrEnvironment = findHDREnvironment(hdrPath);

		if(hdrEnvironment.empty()) {
			std::cerr << "Failed to find HDR environment: " << hdrPath << std::endl;

			return 1;
		}

		if(auto hdrImage = osgDB::readRefImageFile(hdrEnvironment.string())) {
			environment = osgx::make_ref<osgx::Environment>(hdrImage.get());

			environment->setRotation(osgx::gltf::pbribl::KHRONOS_ENVIRONMENT_ROTATION);
		}
	}

	if(!environment) {
		std::cerr << "Failed to prepare PBR IBL resources" << std::endl;

		return 1;
	}

	auto gbuffer = osgx::gltf::pbribl::PBRIBLGBuffer::create(model, WIDTH, HEIGHT);

	if(!gbuffer.valid()) {
		std::cerr << "Failed to build the G-buffer geometry pass" << std::endl;

		return 1;
	}

	// Shadow map: a THIRD, independent camera - shadow-casting only needs depth, not material
	// data, so it has nothing to do with the G-buffer itself (the geometry pass stays completely
	// unaware shadows exist). `model` is multi-parented under it exactly as under gbuffer's own
	// camera; unlike 08-shadows.py's original hand-rolled version, this does NOT re-run the
	// model's own (expensive: normal-mapped/textured) G-buffer-writing fragment shader during the
	// depth-only pass - ShadowMap::create() now installs its own minimal depth-only
	// Program directly on shadowMap.camera (ON|OVERRIDE), which wins over whatever Program `model`
	// carries for its real render. See osgx/TODO.md's old Shadow section.
	osg::ComputeBoundsVisitor boundsVisitor;

	model->accept(boundsVisitor);

	const auto& bounds = boundsVisitor.getBoundingBox();
	const osg::Vec3 boundCenter = bounds.valid() ? bounds.center() : osg::Vec3();
	const float boundRadius = bounds.valid() ? bounds.radius() : 1.0f;
	// Moderate ~45 degree angle - offset enough to cast a clearly visible shadow without the
	// extreme grazing angles that stress-test the shadow frustum's tight, model-sized coverage.
	// Not const: the ImGui "Directional Light" section below drags this live (see
	// ShadowMap::reposition() further down).
	osg::Vec3 lightDir = osg::Vec3(0.6f, 0.4f, -0.6f);
	osg::Vec3 lightColor = osg::Vec3(1.0f, 0.95f, 0.85f);
	float lightIntensity = 3.0f;

	osgx::ShadowMapOptions shadowOptions;

	auto shadowMap = osgx::ShadowMap::create(
		lightDir, boundCenter, boundRadius, shadowOptions
	);

	shadowMap.camera->addChild(model);

	// Floor - sized/placed off the same bound: resting plane at the model's own true bottom
	// (zMin, not the light rig's arbitrary center), centered under the model's actual XY
	// position (not the world origin - a model loaded off-center still needs a floor under IT),
	// half-size generous enough to catch the shadow's tilt-driven overhang regardless of model
	// shape (lightDir's ~26.6 degree tilt off vertical puts that overhang at roughly half the
	// model's height above the floor; radius*3 comfortably covers that without exact trig).
	const float floorZ = bounds.valid() ? bounds.zMin() : 0.0f;
	const float floorHalfSize = boundRadius * 3.0f;
	auto floor = makeFloor(boundCenter, floorHalfSize, floorZ);

	// Receiver, not caster - added to the geometry pass, never the shadow camera.
	gbuffer.gbuffer.camera->addChild(floor);

	// SSAO: reads gbuffer's normal/position directly (both already exist - the geometry pass
	// wrote them, no new attachment needed), so it's built here, after the geometry pass and
	// before the lighting pass whose aoTexture seam consumes its output below. `ssaoProjection`
	// is a separate uniform from the lighting pass's own view-matrix ones - SSAO needs a forward
	// PROJECTION matrix, refreshed the same way and for the same reason (see
	// UpdateLightingPassCallback's own comment). Radius scaled off the model's own bound rather
	// than a fixed constant - same "derive from the model's own bounds" precedent
	// osgx-gbuffer-comic.cpp's hatchFrequency uses; a fixed radius tuned for one model looks wrong
	// at a very different scale.
	auto ssaoProjection = osgx::make_ref<osg::Uniform>(
		"projectionMatrix", osg::Matrixf::identity()
	);
	const float ssaoRadius = std::max(0.05f, boundRadius * 0.15f);

	auto ssao = osgx::SSAO::create(
		gbuffer.normalTexture, gbuffer.positionTexture, ssaoProjection.get(), WIDTH, HEIGHT, ssaoRadius
	);

	if(!ssao.valid()) {
		std::cerr << "Failed to build the SSAO pass" << std::endl;

		return 1;
	}

	osgx::gltf::pbribl::PBRIBLLightingPassOptions lightingOptions;

	lightingOptions.shadowMap = &shadowMap;
	lightingOptions.aoTexture = ssao.aoTexture.get();

	// Back to 1.0/1.0 - 11-sketchfab.py's --ibl-diffuse-intensity/--ibl-specular-intensity
	// default of 0.1 turned out to be a red herring here (user: "0.1 is a bug, I always
	// override it by hand"), and reverting to 0.1 made no visible difference anyway, ruling out
	// the IBL/direct-light balance theory entirely - the shadow is invisible for some other
	// reason.
	auto lighting = osgx::gltf::pbribl::PBRIBLLightingScene::create(
		gbuffer, environment.get(), viewer.getCamera(), lightingOptions
	);

	if(!lighting.valid()) {
		std::cerr << "Failed to build the lighting pass" << std::endl;

		return 1;
	}

	// One directional key light, shadowed (ShadowMap::casterIndex defaults to 0) - lives on the
	// lighting pass camera's own StateSet, since that's where osgx_DirectLighting() actually
	// runs; the geometry pass has no lighting math to feed it to.
	auto* lightingSS = dynamic_cast<osg::Camera*>(lighting.node.get())->getOrCreateStateSet();
	auto lights = osgx::make_ref<osgx::LightSet>();

	lightingSS->setAttributeAndModes(lights);

	// Intensity ~3.0 (not 2.0) - also matching 11-sketchfab.py's own tuned key-light magnitude.
	lights->setCount(1);
	lights->setDirectional(0, lightDir, lightColor, lightIntensity);

	// minMarkerRadius/spotConeLength stay at their unit-scene-scale library defaults --
	// LightMarkers never draws anything for a directional light anyway (see Gizmos.hpp); only the
	// LightGizmos overlay (plane + direction arrow) matters here, and it sizes itself off
	// `model`'s own real bound at construction time.
	auto gizmos = osgx::make_ref<osgx::LightGizmos>(*lights, model.get());
	// Order 2 - after BOTH lighting.node (POST_RENDER, order 0) and debugCamera (POST_RENDER,
	// order 1), and unlike either of those, never nodeMask-toggled by VisualizeModeHandler - the
	// gizmo overlay draws last every frame regardless of which view mode is active, which is what
	// makes it the right thing to pin osgx::imgui::Widget's drawCamera to below.
	gizmos->getOverlay()->setRenderOrder(osg::Camera::POST_RENDER, 2);

	auto root = osgx::make_ref<osg::Group>();

	if(environment->getBakeRoot()) root->addChild(environment->getBakeRoot());

	// Installed on shadowMap.camera specifically - see UpdateLightingPassCallback's own comment
	// for why it has to be the first PRE_RENDER camera in the scene graph, not a post-frame()
	// call in the loop below (the previous, buggy version of this example).
	shadowMap.camera->setPreDrawCallback(
		new UpdateLightingPassCallback(&lighting, viewer.getCamera(), ssaoProjection.get())
	);

	// Add order matters: all four of these are PRE_RENDER at the same default order number (0),
	// so OSG breaks the tie by scene-graph add order, not anything declared on the cameras
	// themselves. ssao.rawCamera/blurCamera MUST come after gbuffer.gbuffer.camera (they read its
	// normal/position output) and before lighting.node (which reads ssao.aoTexture back via
	// PBRIBLLightingPassOptions::aoTexture).
	root->addChild(shadowMap.camera);
	root->addChild(gbuffer.gbuffer.camera);
	root->addChild(ssao.rawCamera);
	root->addChild(ssao.blurCamera);
	root->addChild(lighting.node);
	root->addChild(gizmos);

	osg::Uniform* debugChannelMode = nullptr;
	auto debugCamera = makeDebugBlitCamera(debugChannelMode);

	root->addChild(debugCamera);

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.addEventHandler(new VisualizeModeHandler(
		dynamic_cast<osg::Camera*>(lighting.node.get()),
		debugCamera,
		debugChannelMode,
		gbuffer,
		ssao.aoTexture.get()
	));
	viewer.getCamera()->setClearColor(osg::Vec4f(48.0f / 255.0f, 53.0f / 255.0f, 66.0f / 255.0f, 1.0f));

	// Ground-truth dump: 'w' writes the shadow camera's own depth texture (what it actually
	// captured of `model`, from the light's POV) straight to disk - postDrawCallback so it
	// reads back immediately after that camera's own draw, no frame-delay guessing needed.
	auto shadowDump = osgx::make_ref<osgx::WriteTextureCallback>(shadowMap.depthTexture.get());

	shadowMap.camera->setPostDrawCallback(shadowDump);
	viewer.addEventHandler(new osgx::LambdaKeyHandler(
		'w',
		[shadowDump](const osgGA::GUIEventAdapter&, osgGA::GUIActionAdapter&) {
			shadowDump->write("osgx-gbuffer-shadow-depth.png");

			std::cout << "osgx-gbuffer: writing osgx-gbuffer-shadow-depth.png" << std::endl;

			return true;
		}
	));

	std::cout
		<< "osgx-gbuffer: deferred PBRIBLGBuffer::create() + PBRIBLLightingScene::create()" << std::endl
		<< " 0=lit composite (default) 1=albedo 2=normal 3=material 4=emissive 5=depth 6=SSAO" << std::endl
		<< " w=dump the shadow camera's own depth texture to osgx-gbuffer-shadow-depth.png" << std::endl
	;

#ifdef OSGX_IMGUI
	// Proves ShadowMap::reposition() inside the deferred pipeline
	// specifically: dragging the light live reshapes the floor's shadow (and moves
	// osgx::LightGizmos' overlay, reading the same LightSet) without ever rebuilding shadowMap's
	// camera/FBO/depth texture - same mechanism osgx-shadow.cpp already proved standalone.
	//
	// gizmos->getOverlay() pinned explicitly as the draw camera - left at the default
	// (drawCamera=nullptr), the panel drew via the master camera's own PostDrawCallback, which
	// fires BEFORE lighting.node/debugCamera (nested POST_RENDER cameras, not View slaves) ever
	// run - their later draw painted straight over the ImGui panel every frame. Bumped the
	// gizmo overlay's own render order above (POST_RENDER, 2) specifically so it's the one camera
	// guaranteed to draw last regardless of which view mode is active - see Widget's own
	// constructor comment for this exact deferred-rendering scenario.
	auto* gui = new osgx::imgui::Widget(viewer, gizmos->getOverlay());

	gui->addSection("Directional Light", [
		lights, &shadowMap, &lightDir, &lightColor, &lightIntensity, boundCenter, boundRadius, shadowOptions
	](osg::RenderInfo&) {
		bool changed = false;

		changed |= ImGui::SliderFloat3("Direction", lightDir.ptr(), -1.0f, 1.0f);
		changed |= ImGui::ColorEdit3("Color", lightColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 10.0f);

		if(changed) {
			// A dragged slider can pass through (0,0,0) - lookAt() (inside
			// ShadowMap::reposition()) is degenerate for a zero-length direction, so
			// hold the last valid direction instead of feeding it one.
			if(lightDir.length2() > 1e-8f) {
				lights->setDirectional(0, lightDir, lightColor, lightIntensity);

				shadowMap.reposition(lightDir, boundCenter, boundRadius, shadowOptions);
			}

			else {
				lights->setDirectional(0, osg::Vec3(0.0f, 0.0f, -1.0f), lightColor, lightIntensity);
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));

	// Live radius/bias tuning - reads the CURRENT uniform value each frame rather than tracking
	// a separate local float, since osgx::SSAO::create() already returns these as real
	// osg::Uniform*s meant to be set at any time (no pass rebuild), not one-shot constructor
	// arguments. Press '6' to actually see the effect while dragging these.
	gui->addSection("SSAO", [ssao](osg::RenderInfo&) {
		float radius = 0.0f, bias = 0.0f;

		ssao.radius->get(radius);
		ssao.bias->get(bias);

		bool changed = false;

		changed |= ImGui::SliderFloat("Radius", &radius, 0.01f, 2.0f);
		changed |= ImGui::SliderFloat("Bias", &bias, 0.0f, 0.1f);

		if(changed) {
			ssao.radius->set(radius);
			ssao.bias->set(bias);
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();

	// while(!viewer.done()) viewer.frame();

	// return 0;
}
