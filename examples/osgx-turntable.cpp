// vimrun! ./examples/osgx-turntable --env env/modern_buildings_night_2k.gltf model.gltf
//
// A deliberately small staging ground for the turntable viewer. The procedural infinite grid is
// always present; without a model path, colored cubes provide a quick scene-setup diagnostic.

#include "osgx/Core.hpp"
#include "osgx/Cursor.hpp"
#include "osgx/IBL.hpp"
#include "osgx/Manipulators.hpp"
#include "osgx/Projection.hpp"
#include "osgx/Shader.hpp"
#include "osgx/gltf/PBRIBL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/CullSettings>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osgDB/ReadFile>
#include <osgDB/Registry>
#include <osgGA/GUIEventHandler>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

constexpr const char SKY_VERTEX_SHADER[] = R"GLSL(
#version 430 core

uniform mat4 osg_ProjectionMatrix;
uniform mat4 osg_ViewMatrixInverse;

in vec4 osg_Vertex;

out vec3 worldDirection;

void main() {
	vec4 viewDirection = inverse(osg_ProjectionMatrix) * vec4(osg_Vertex.xy, 1.0, 1.0);

	worldDirection = normalize(mat3(osg_ViewMatrixInverse) * viewDirection.xyz);
	gl_Position = osg_Vertex;
}
)GLSL";

constexpr const char SKY_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform samplerCube environmentMap;
uniform int useEnvironmentMap;
uniform int displayRawSkybox;

in vec3 worldDirection;

out vec4 fragColor;

void main() {
	const vec3 horizonBlue = vec3(0.055, 0.130, 0.275);
	const vec3 horizonSteel = vec3(0.105, 0.185, 0.315);
	const vec3 horizonViolet = vec3(0.180, 0.075, 0.250);
	const vec3 horizonCrimson = vec3(0.200, 0.060, 0.150);
	const vec3 zenith = vec3(0.002, 0.005, 0.016);
	const vec3 belowHorizon = vec3(0.001, 0.002, 0.006);
	float elevation = clamp(worldDirection.z, -1.0, 1.0);
	float azimuth = atan(worldDirection.y, worldDirection.x) / (2.0 * 3.14159265) + 0.5;
	float region = azimuth * 4.0;
	float blend = smoothstep(0.0, 1.0, fract(region));
	float sky = smoothstep(-0.08, 0.16, elevation);
	float zenithBlend = smoothstep(0.02, 0.65, elevation);
	vec3 horizon;

	if(region < 1.0) horizon = mix(horizonBlue, horizonSteel, blend);
	else if(region < 2.0) horizon = mix(horizonSteel, horizonViolet, blend);
	else if(region < 3.0) horizon = mix(horizonViolet, horizonCrimson, blend);
	else horizon = mix(horizonCrimson, horizonBlue, blend);

	vec3 color = mix(belowHorizon, horizon, sky);

	color = mix(color, zenith, zenithBlend);

	// A little atmospheric glow at the horizon keeps the grid from disappearing into a hard void.
	color += vec3(0.025, 0.040, 0.075) * exp(-22.0 * abs(elevation));

	if(useEnvironmentMap != 0) {
		// KTX cubemaps use a Y-up basis while this turntable is Z-up.
		vec3 cubeDirection = vec3(worldDirection.x, worldDirection.z, -worldDirection.y);

		if(displayRawSkybox != 0) {
			// A raw visual cubemap has no roughness-convolved mip chain. Keep it as a restrained
			// upper-hemisphere atmosphere: the procedural below-horizon color remains the dark ground.
			vec3 city = textureLod(environmentMap, normalize(cubeDirection), 0.0).rgb;
			float skyboxFade = 0.45 * smoothstep(-0.06, 0.16, elevation);

			city = city / (city + vec3(1.0));
			color = mix(color, city, skyboxFade);
		}

		else {
			// First art-direction rung after the direct mip-0 diagnostic: visibly urban, but softened
			// enough that it begins reading as a backdrop instead of a literal panorama.
			vec3 city = textureLod(environmentMap, normalize(cubeDirection), 1.5).rgb;

			city = city / (city + vec3(1.0));
			color = mix(color, city, 0.70);
		}
	}

	fragColor = vec4(color, 1.0);
}
)GLSL";

constexpr const char INFINITE_FLOOR_VERTEX_SHADER[] = R"GLSL(
#version 430 core

#pragma osgx::projection UNPROJECT

in vec4 osg_Vertex;

out vec3 nearPoint;
out vec3 farPoint;

void main() {
	nearPoint = osgx_Unproject(osg_Vertex.xy, -1.0);
	farPoint = osgx_Unproject(osg_Vertex.xy, 1.0);
	gl_Position = osg_Vertex;
}
)GLSL";

constexpr const char INFINITE_FLOOR_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform mat4 osg_ProjectionMatrix;
uniform mat4 osg_ViewMatrix;
uniform mat4 osg_ViewMatrixInverse;

in vec3 nearPoint;
in vec3 farPoint;

out vec4 fragColor;

float gridLine(vec2 position, float interval) {
	vec2 coord = position / interval;
	vec2 deriv = max(fwidth(coord), vec2(1e-6));
	vec2 distanceToLine = abs(fract(coord - 0.5) - 0.5) / deriv;

	return 1.0 - min(min(distanceToLine.x, distanceToLine.y), 1.0);
}

void main() {
	vec3 ray = farPoint - nearPoint;

	if(abs(ray.z) < 1e-6) discard;

	float t = -nearPoint.z / ray.z;

	if(t <= 0.0 || t > 1.0) discard;

	vec3 hit = nearPoint + ray * t;
	vec4 clip = osg_ProjectionMatrix * osg_ViewMatrix * vec4(hit, 1.0);
	float ndcDepth = clip.z / clip.w;

	if(ndcDepth < -1.0 || ndcDepth > 1.0) discard;

	gl_FragDepth = ndcDepth * 0.5 + 0.5;

	float minor = gridLine(hit.xy, 1.0);
	float major = gridLine(hit.xy, 10.0);
	vec3 camera = osg_ViewMatrixInverse[3].xyz / osg_ViewMatrixInverse[3].w;
	float distanceFade = 1.0 - smoothstep(35.0, 180.0, length(hit - camera));
	float horizonFade = smoothstep(0.015, 0.12, abs(normalize(ray).z));
	float fade = distanceFade * horizonFade;
	vec3 color = mix(vec3(0.11, 0.17, 0.25), vec3(0.42, 0.62, 0.88), major);
	float alpha = max(minor * 0.32, major * 0.80) * fade;

	fragColor = vec4(color, alpha);
}
)GLSL";

osg::ref_ptr<osg::Geometry> makeFullscreenTriangle() {
	auto vertices = osgx::make_ref<osg::Vec3Array>();

	vertices->push_back(osg::Vec3(-1.0f, -1.0f, 0.0f));
	vertices->push_back(osg::Vec3(3.0f, -1.0f, 0.0f));
	vertices->push_back(osg::Vec3(-1.0f, 3.0f, 0.0f));

	auto triangle = osgx::make_ref<osg::Geometry>();

	// The vertex shader expands this small object-space triangle to the entire clip volume. Its
	// ordinary bounds therefore do not describe what it draws, and must not participate in culling.
	triangle->setCullingActive(false);
	triangle->setVertexArray(vertices);
	triangle->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));

	return triangle;
}

osg::ref_ptr<osg::Node> makeHemisphereSky(
	osg::TextureCubeMap* environmentMap=nullptr,
	bool displayRawSkybox=false
) {
	auto program = osgx::make_ref<osg::Program>();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, SKY_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, SKY_FRAGMENT_SHADER));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->setCullingActive(false);
	geode->addDrawable(makeFullscreenTriangle());

	auto* stateSet = geode->getOrCreateStateSet();

	stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
	stateSet->addUniform(new osg::Uniform("environmentMap", 0));
	stateSet->addUniform(new osg::Uniform("useEnvironmentMap", environmentMap ? 1 : 0));
	stateSet->addUniform(new osg::Uniform("displayRawSkybox", displayRawSkybox ? 1 : 0));

	if(environmentMap) {
		stateSet->setTextureAttributeAndModes(0, environmentMap, osg::StateAttribute::ON);
	}

	stateSet->setMode(GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
	stateSet->setRenderBinDetails(-10, "RenderBin");

	return geode;
}

osg::ref_ptr<osg::Node> makeInfiniteFloor() {
	osgx::registerProjectionShaderLibs();

	auto program = osgx::make_ref<osg::Program>();

	program->addShader(new osg::Shader(
		osg::Shader::VERTEX, osgx::resolveShaderLibs(INFINITE_FLOOR_VERTEX_SHADER)
	));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, INFINITE_FLOOR_FRAGMENT_SHADER));

	auto floor = makeFullscreenTriangle();
	auto* stateSet = floor->getOrCreateStateSet();

	stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
	stateSet->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
		osg::StateAttribute::ON
	);
	stateSet->setAttributeAndModes(
		new osg::Depth(osg::Depth::LESS, 0.0, 1.0, true),
		osg::StateAttribute::ON
	);
	stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
	stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);

	auto geode = osgx::make_ref<osg::Geode>();

	geode->setCullingActive(false);
	geode->addDrawable(floor);

	return geode;
}

osg::ref_ptr<osg::Node> makePlaceholders() {
	auto geode = osgx::make_ref<osg::Geode>();

	auto addBox = [&](const osg::Vec3& center, const osg::Vec3& size, const osg::Vec4& color) {
		auto box = osgx::make_ref<osg::ShapeDrawable>(new osg::Box(center, size.x(), size.y(), size.z()));

		box->setColor(color);
		geode->addDrawable(box);
	};

	addBox(
		osg::Vec3(-2.0f, 0.5f, 1.0f),
		osg::Vec3(2.0f, 2.0f, 2.0f),
		osg::Vec4(0.88f, 0.32f, 0.24f, 1.0f)
	);
	addBox(
		osg::Vec3(1.5f, 1.5f, 1.5f),
		osg::Vec3(1.5f, 1.5f, 3.0f),
		osg::Vec4(0.22f, 0.65f, 0.94f, 1.0f)
	);
	addBox(
		osg::Vec3(3.8f, -1.2f, 0.6f),
		osg::Vec3(1.2f, 1.2f, 1.2f),
		osg::Vec4(0.95f, 0.76f, 0.22f, 1.0f)
	);

	return geode;
}

osg::ref_ptr<osg::Node> normalizeSubject(osg::Node* subject) {
	osg::ComputeBoundsVisitor boundsVisitor;

	subject->accept(boundsVisitor);

	const auto& bounds = boundsVisitor.getBoundingBox();

	if(!bounds.valid()) return subject;

	const double height = static_cast<double>(bounds.zMax()) - static_cast<double>(bounds.zMin());

	if(height <= 0.0) return subject;

	constexpr double SUBJECT_HEIGHT = 3.0;
	const double scale = SUBJECT_HEIGHT / height;
	const osg::Vec3 center = bounds.center();
	auto transform = osgx::make_ref<osg::MatrixTransform>();

	// OSG vectors multiply from the left: scale first, then move the scaled bound's horizontal
	// center to the turntable axis and its bottom to the z=0 stage.
	transform->setMatrix(
		osg::Matrix::scale(scale, scale, scale) *
		osg::Matrix::translate(-center.x() * scale, -center.y() * scale, -bounds.zMin() * scale)
	);
	transform->addChild(subject);

	return transform;
}

class OrbitCaptureBridge: public osgGA::GUIEventHandler {
public:
	OrbitCaptureBridge(osgx::CursorCapture* capture, osgx::OrbitAxisManipulator* manipulator):
		_capture(capture),
		_manipulator(manipulator) {}

	bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override {
		auto* capture = _capture.get();
		auto* manipulator = _manipulator.get();

		if(!capture || !manipulator) return false;

		if(event.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && event.getKey() == 'c') {
			const bool captured = !capture->isCaptured();

			capture->setCaptured(captured);
			manipulator->setLiveOrbitEnabled(!captured);
			std::cout << "CursorCapture: " << (captured ? "ON" : "OFF") << std::endl;

			return true;
		}

		if(event.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && event.getKey() == 'i') {
			const bool inverted = !manipulator->getInvertY();

			manipulator->setInvertY(inverted);
			std::cout << "Vertical controls: " << (inverted ? "INVERTED" : "NORMAL") << std::endl;

			return true;
		}

		if(event.getEventType() == osgGA::GUIEventAdapter::KEYDOWN && event.getKey() == 'x') {
			const bool inverted = !manipulator->getInvertX();

			manipulator->setInvertX(inverted);
			std::cout << "Horizontal controls: " << (inverted ? "INVERTED" : "NORMAL") << std::endl;

			return true;
		}

		if(event.getEventType() != osgGA::GUIEventAdapter::FRAME || !capture->isCaptured()) return false;

		const osg::Vec2 delta = capture->consume();
		const double width = event.getXmax() - event.getXmin();
		const double height = event.getYmax() - event.getYmin();
		constexpr float POINTER_DEAD_ZONE = 1.0f;
		const double dx = std::abs(delta.x()) <= POINTER_DEAD_ZONE ? 0.0 : delta.x();
		double dy = std::abs(delta.y()) <= POINTER_DEAD_ZONE ? 0.0 : delta.y();

		if(event.getMouseYOrientation() == osgGA::GUIEventAdapter::Y_INCREASING_DOWNWARDS) dy = -dy;
		if(width > 0.0 && height > 0.0) {
			manipulator->orbitByDelta(2.0 * dx / width, 2.0 * dy / height);
		}

		return false;
	}

private:
	osg::observer_ptr<osgx::CursorCapture> _capture;
	osg::observer_ptr<osgx::OrbitAxisManipulator> _manipulator;
};

}

int main(int argc, char** argv) {
	std::string modelPath;
	std::string environmentPath;
	std::string skyboxPath;

	for(int i = 1; i < argc; i++) {
		const std::string arg(argv[i]);

		if(arg == "--env" && i + 1 < argc) environmentPath = argv[++i];
		else if(arg == "--skybox" && i + 1 < argc) skyboxPath = argv[++i];
		else if(arg[0] != '-' && modelPath.empty()) modelPath = arg;
		else {
			std::cerr << "Usage: " << argv[0] << " [--env manifest.gltf] [--skybox raw.ktx2] [model.gltf]" << std::endl;

			return 1;
		}
	}

	auto root = osgx::make_ref<osg::Group>();
	osgx::gltf::pbribl::PBRIBLEnvironment environment;
	osg::ref_ptr<osg::TextureCubeMap> skybox;

	if(!environmentPath.empty()) {
		environment = osgx::gltf::pbribl::PBRIBLEnvironment::load(environmentPath);

		if(!environment.valid()) {
			std::cerr << "Unable to load PBR/IBL environment manifest: " << environmentPath << std::endl;

			return 1;
		}

		// A built-in BRDF LUT still needs its one-time render pass. Pre-baked manifests otherwise
		// have no work here, but adding this root is harmless and keeps the environment complete.
		if(environment.root) root->addChild(environment.root);
	}

	if(!skyboxPath.empty()) {
		skybox = osgx::loadPrefilterCubemap(skyboxPath);

		if(!skybox) {
			std::cerr << "Unable to load raw skybox cubemap: " << skyboxPath << std::endl;

			return 1;
		}
	}

	osg::ref_ptr<osg::Node> subject;

	if(!modelPath.empty()) {
		if(environmentPath.empty()) {
			std::cerr << "A glTF model requires --env manifest.gltf for PBR/IBL lighting" << std::endl;

			return 1;
		}

		// ReaderWriterGLTF registers this alias on construction, but plugin lookup needs it before a
		// cold .glb load has constructed the reader.
		osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");
		subject = osgDB::readRefNodeFile(modelPath);

		if(!subject) {
			std::cerr << "Unable to load glTF model: " << modelPath << std::endl;

			return 1;
		}

		if(!osgx::gltf::pbribl::PBRIBLScene::create(subject, environment).valid()) return 1;
	}

	else subject = makePlaceholders();

	subject = normalizeSubject(subject);

	root->addChild(makeHemisphereSky(
		skybox.valid() ? skybox : environment.envMap,
		skybox.valid()
	));
	root->addChild(subject);
	root->addChild(makeInfiniteFloor());

	auto manipulator = osgx::make_ref<osgx::OrbitAxisManipulator>();

	manipulator->setUpAxis(osg::Vec3d(0.0, 0.0, 1.0));
	manipulator->setHomeDirection(osg::Vec3d(0.0, -1.0, 0.0));
	manipulator->setYawSensitivity(osg::PI);
	manipulator->setHeightSensitivity(0.20);
	manipulator->setInvertX(false);
	manipulator->setInvertY(false);
	manipulator->setWheelZoomFactor(1.08);
	// Keep a generous overview distance, but stop close enough for material/detail inspection
	// without entering the subject. These are model-relative coverage fractions, not world units.
	manipulator->setCoverageLimits(0.35, 6.0);
	osgViewer::Viewer viewer;

	viewer.setSceneData(root);

	// The default OSG near plane is farther away than close model inspection permits. It cannot sit
	// behind the eye, but a small positive distance prevents it from slicing the face at the closest
	// allowed dolly position while preserving the camera's existing FOV, aspect ratio, and far plane.
	double fovy, aspect, zNear, zFar;

	if(viewer.getCamera()->getProjectionMatrix().getPerspective(fovy, aspect, zNear, zFar)) {
		viewer.getCamera()->setProjectionMatrixAsPerspective(fovy, aspect, std::min(zNear, 0.02), zFar);
	}

	viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
	viewer.setCameraManipulator(manipulator);
	manipulator->setNode(subject);
	viewer.home();

	// Keep the eye just above the stage and no higher than the subject itself. A bounding sphere's
	// Z extent is inflated by a wide model (such as Batman's outstretched arms), so use its actual
	// bounding box instead of OrbitAxisManipulator's sphere-derived automatic limits.
	osg::ComputeBoundsVisitor boundsVisitor;

	subject->accept(boundsVisitor);

	const auto& bounds = boundsVisitor.getBoundingBox();

	if(bounds.valid()) {
		constexpr double MINIMUM_CAMERA_HEIGHT = 0.25;
		const double minHeight = std::max(MINIMUM_CAMERA_HEIGHT, static_cast<double>(bounds.zMin()));
		const double maxHeight = std::max(minHeight, static_cast<double>(bounds.zMax()));

		manipulator->setHeightLimits(minHeight, maxHeight);
	}

	auto capture = osgx::make_ref<osgx::CursorCapture>(viewer);

	viewer.addEventHandler(capture);
	viewer.addEventHandler(new OrbitCaptureBridge(capture, manipulator));

	std::cout
		<< "OrbitAxisManipulator" << std::endl
		<< " Mouse move/drag orbit (X) + height (Y), always active" << std::endl
		<< " Scroll dolly zoom" << std::endl
		<< " Space/Home reset view" << std::endl
		<< " 'c' toggle CursorCapture (unbounded orbit/height)" << std::endl
		<< " 'x' toggle horizontal control direction" << std::endl
		<< " 'i' toggle vertical control direction" << std::endl
		<< " [model.gltf] loads a PBR glTF model (requires --env)" << std::endl
		<< " --env manifest.gltf supplies its prefiltered IBL lighting and background" << std::endl
		<< " --skybox raw.ktx2 displays a raw visual cubemap background" << std::endl
	;

	return viewer.run();
}
