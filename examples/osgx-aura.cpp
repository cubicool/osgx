// vimrun! ./examples/osgx-aura
// vimrun! ./examples/osgx-aura gbuffer
//
// The simple default keeps Aura independent of any deferred renderer. `gbuffer` renders the same
// selected object through osgx::GBuffer, then lets the final aura composite sample propagated
// source position for depth-aware occlusion. Its energy pattern uses projected model bounds so it
// stays continuous despite source-UV propagation being intentionally approximate.
// In either mode Aura itself remains exactly: mask -> dilate X -> dilate Y.

#include "osgx/Aura.hpp"
#include "osgx/Core.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/IBL.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/Shapes.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/Camera>
#include <osg/GL>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 800;

constexpr const char FORWARD_VERTEX_SHADER[] = R"GLSL(
#version 430 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vNormal;

void main() {
	vNormal = normalize(osg_NormalMatrix * normal);
	gl_Position = osg_ModelViewProjectionMatrix * vec4(position, 1.0);
}
)GLSL";

constexpr const char FORWARD_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform vec3 bodyColor;

in vec3 vNormal;
out vec4 fragColor;

void main() {
	float diffuse = max(dot(normalize(vNormal), normalize(vec3(0.4, 0.6, 0.7))), 0.0);

	fragColor = vec4(bodyColor * (0.22 + 0.78 * diffuse), 1.0);
}
)GLSL";

constexpr const char GBUFFER_VERTEX_SHADER[] = R"GLSL(
#version 430 core

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat4 osg_ModelViewMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vPosition;
out vec3 vNormal;

void main() {
	vec4 eyePosition = osg_ModelViewMatrix * vec4(position, 1.0);

	vPosition = eyePosition.xyz;
	vNormal = normalize(osg_NormalMatrix * normal);
	gl_Position = osg_ModelViewProjectionMatrix * vec4(position, 1.0);
}
)GLSL";

constexpr const char GBUFFER_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform vec3 bodyColor;

in vec3 vPosition;
in vec3 vNormal;

layout(location = 0) out vec4 gColor;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gPosition;

void main() {
	gColor = vec4(bodyColor, 1.0);
	gNormal = vec4(normalize(vNormal), 0.0);
	gPosition = vec4(vPosition, 1.0);
}
)GLSL";

constexpr const char GBUFFER_LIGHTING_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform sampler2D gColor;
uniform sampler2D gNormal;

in vec2 vUV;
out vec4 fragColor;

void main() {
	vec3 color = texture(gColor, vUV).rgb;
	vec3 normal = texture(gNormal, vUV).rgb;

	if(dot(normal, normal) < 0.0001) {
		fragColor = vec4(0.025, 0.035, 0.07, 1.0);

		return;
	}

	float diffuse = max(dot(normalize(normal), normalize(vec3(0.4, 0.6, 0.7))), 0.0);

	fragColor = vec4(color * (0.22 + 0.78 * diffuse), 1.0);
}
)GLSL";

constexpr const char AURA_COMPOSITE_FRAGMENT_SHADER[] = R"GLSL(
#version 430 core

uniform sampler2D auraOriginalMask;
uniform sampler2D auraExpanded;
uniform sampler2D gPosition;
uniform bool auraUseGBuffer;
uniform float auraTime;
uniform vec2 auraObjectScreenCenter;
uniform vec2 auraObjectScreenSize;
uniform vec3 auraEnergyColorA;
uniform vec3 auraEnergyColorB;
uniform float auraEnergyStripeFrequency;
uniform float auraEnergyWaveFrequency;
uniform float auraEnergySpeed;
uniform float auraEnergyBrightness;
uniform float auraOuterFade;
uniform int auraRadius;

in vec2 vUV;
out vec4 fragColor;

void main() {
	vec4 expanded = texture(auraExpanded, vUV);
	float auraMask = expanded.r * (1.0 - texture(auraOriginalMask, vUV).r);

	if(auraMask < 0.5) {
		fragColor = vec4(0.0);

		return;
	}

	if(auraUseGBuffer) {
		// sourceDepth comes straight from Aura's own propagated depth channel (expanded.g), not a
		// UV-driven re-lookup into gPosition -- see Aura.hpp for why that distinction matters for
		// occlusion specifically (a separable max filter's legitimate ties are far less visible
		// when the propagated payload is the value itself, bounded by local geometry, rather than
		// a coordinate that can re-sample somewhere unrelated on the surface).
		float sourceDepth = expanded.g;
		vec3 herePosition = texture(gPosition, vUV).xyz;

		// View space looks down -Z, so a larger Z is closer. Do not paint a selected object's
		// aura across unrelated geometry in front of the selected surface.
		if(dot(herePosition, herePosition) > 0.0001 && herePosition.z > sourceDepth + 0.03) {
			fragColor = vec4(0.0);

			return;
		}
	}

	float fadeFraction = auraOuterFade * 0.01;

	if(fadeFraction > 0.0) {
		float distance = expanded.a;
		float fadeStart = float(auraRadius) * (1.0 - fadeFraction);

		auraMask *= 1.0 - smoothstep(fadeStart, float(auraRadius), distance);
	}

	// Do not use propagated sourceUV for a spatially continuous fill: a separable max filter has
	// legitimate source-choice seams. This projected-bounds field is continuous across the whole
	// aura, and follows the selected model's projected translation and scale every frame.
	vec2 modelP = (vUV - auraObjectScreenCenter) / max(auraObjectScreenSize, vec2(0.0001));
	vec2 stripeAxis = normalize(vec2(0.8, 0.6));
	float stripe = 0.5 + 0.5 * sin(
		dot(modelP, stripeAxis) * auraEnergyStripeFrequency - auraTime * auraEnergySpeed * 3.0
	);
	float wave = 0.6 + 0.4 * sin(
		modelP.x * auraEnergyWaveFrequency - modelP.y * auraEnergyWaveFrequency * 0.7
		+ auraTime * auraEnergySpeed * 2.0
	);
	vec3 color = mix(auraEnergyColorA, auraEnergyColorB, stripe);

	color *= (0.55 + 0.8 * wave) * auraEnergyBrightness;
	color = clamp(color, 0.0, 1.0);
	fragColor = vec4(color, auraMask * 0.82);
}
)GLSL";

osg::ref_ptr<osg::Program> makeProgram(const char* name, const char* vertex, const char* fragment) {
	auto program = osgx::make_nref<osg::Program>(name);

	program->addShader(new osg::Shader(osg::Shader::VERTEX, vertex));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragment));

	return program;
}

osg::ref_ptr<osg::Group> makeSelectedModel() {
	auto group = osgx::make_ref<osg::Group>();
	auto transform = osgx::make_ref<osg::MatrixTransform>();
	auto geode = osgx::make_ref<osg::Geode>();
	auto geometry = osgx::make_ref<osgx::Dodecahedron>(osg::Vec3(), 1.7f);

	geometry->setUseVertexBufferObjects(true);
	geode->addDrawable(geometry);
	transform->setMatrix(osg::Matrix::translate(0.0f, 0.0f, 0.0f));
	transform->addChild(geode);
	group->addChild(transform);
	group->getOrCreateStateSet()->setAttributeAndModes(
		makeProgram("osgx_aura_Forward", FORWARD_VERTEX_SHADER, FORWARD_FRAGMENT_SHADER),
		osg::StateAttribute::ON
	);
	group->getOrCreateStateSet()->addUniform(new osg::Uniform("bodyColor", osg::Vec3(0.75f, 0.34f, 0.12f)));

	return group;
}

osg::ref_ptr<osg::Camera> makeFullscreenComposite(
	const char* name,
	const char* fragmentShader,
	int renderOrder
) {
	auto camera = osgx::make_nref<osg::Camera>(name);
	auto quad = osg::createTexturedQuadGeometry(
		osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0)
	);
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(quad);
	camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	camera->setRenderOrder(osg::Camera::POST_RENDER, renderOrder);
	camera->setClearMask(0);
	camera->setProjectionMatrix(osg::Matrix::identity());
	camera->setViewMatrix(osg::Matrix::identity());
	camera->addChild(geode);

	auto* stateSet = camera->getOrCreateStateSet();

	stateSet->setAttributeAndModes(makeProgram(name, osgx::FULLSCREEN_VERT, fragmentShader), osg::StateAttribute::ON);
	stateSet->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);

	return camera;
}

struct EnergyControls {
	osg::ref_ptr<osg::Uniform> colorA = new osg::Uniform(
		"auraEnergyColorA", osg::Vec3(0.04f, 0.4f, 1.0f)
	);
	osg::ref_ptr<osg::Uniform> colorB = new osg::Uniform(
		"auraEnergyColorB", osg::Vec3(0.35f, 1.0f, 0.9f)
	);
	osg::ref_ptr<osg::Uniform> stripeFrequency = new osg::Uniform("auraEnergyStripeFrequency", 18.0f);
	osg::ref_ptr<osg::Uniform> waveFrequency = new osg::Uniform("auraEnergyWaveFrequency", 13.0f);
	osg::ref_ptr<osg::Uniform> speed = new osg::Uniform("auraEnergySpeed", 1.0f);
	osg::ref_ptr<osg::Uniform> brightness = new osg::Uniform("auraEnergyBrightness", 1.0f);
	osg::ref_ptr<osg::Uniform> outerFade = new osg::Uniform("auraOuterFade", 0.0f);
};

osg::ref_ptr<osg::Camera> makeAuraComposite(
	const osgx::Aura& aura,
	const osgx::GBuffer* gbuffer,
	osg::Uniform* objectScreenCenter,
	osg::Uniform* objectScreenSize,
	const EnergyControls& energy,
	osg::Uniform*& timeOut
) {
	auto camera = makeFullscreenComposite("osgx_aura_Composite", AURA_COMPOSITE_FRAGMENT_SHADER, 1);
	auto* stateSet = camera->getOrCreateStateSet();

	stateSet->setTextureAttributeAndModes(0, aura.originalMask, osg::StateAttribute::ON);
	stateSet->setTextureAttributeAndModes(1, aura.expanded, osg::StateAttribute::ON);
	stateSet->addUniform(new osg::Uniform("auraOriginalMask", 0));
	stateSet->addUniform(new osg::Uniform("auraExpanded", 1));
	stateSet->addUniform(new osg::Uniform("auraUseGBuffer", gbuffer != nullptr));
	stateSet->addUniform(objectScreenCenter);
	stateSet->addUniform(objectScreenSize);
	stateSet->addUniform(energy.colorA);
	stateSet->addUniform(energy.colorB);
	stateSet->addUniform(energy.stripeFrequency);
	stateSet->addUniform(energy.waveFrequency);
	stateSet->addUniform(energy.speed);
	stateSet->addUniform(energy.brightness);
	stateSet->addUniform(energy.outerFade);
	stateSet->addUniform(aura.radius);

	if(gbuffer) {
		stateSet->setTextureAttributeAndModes(2, gbuffer->colorTextures[2], osg::StateAttribute::ON);
		stateSet->addUniform(new osg::Uniform("gPosition", 2));
	}

	timeOut = new osg::Uniform("auraTime", 0.0f);
	stateSet->addUniform(timeOut);
	stateSet->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON
	);

	return camera;
}

class UpdateAuraScreenBounds: public osg::Camera::DrawCallback {
public:
	struct Sizing {
		bool automatic = true;
		float relativeRadius = 0.10f;
	};

	UpdateAuraScreenBounds(
		osg::Camera* mainCamera,
		const osg::BoundingBox& bounds,
		osg::Uniform* objectScreenCenter,
		osg::Uniform* objectScreenSize,
		osg::Uniform* auraRadius,
		Sizing* sizing,
		int width,
		int height
	):
		_mainCamera(mainCamera),
		_bounds(bounds),
		_objectScreenCenter(objectScreenCenter),
		_objectScreenSize(objectScreenSize),
		_auraRadius(auraRadius),
		_sizing(sizing),
		_width(width),
		_height(height) {}

	void operator()(osg::RenderInfo&) const override {
		if(!_mainCamera.valid() || !_bounds.valid()) return;

		const osg::Matrixd modelViewProjection =
			_mainCamera->getViewMatrix() * _mainCamera->getProjectionMatrix();
		osg::Vec2 minimum(1.0f, 1.0f);
		osg::Vec2 maximum(0.0f, 0.0f);

		for(int z = 0; z < 2; z++) {
			for(int y = 0; y < 2; y++) {
				for(int x = 0; x < 2; x++) {
					const osg::Vec3 corner(
						x ? _bounds.xMax() : _bounds.xMin(),
						y ? _bounds.yMax() : _bounds.yMin(),
						z ? _bounds.zMax() : _bounds.zMin()
					);
					const osg::Vec4d clip = osg::Vec4d(
						corner.x(), corner.y(), corner.z(), 1.0
					) * modelViewProjection;

					if(std::abs(clip.w()) < 0.0001f) continue;

					const osg::Vec2 uv = osg::Vec2(
						static_cast<float>(clip.x() / clip.w()),
						static_cast<float>(clip.y() / clip.w())
					) * 0.5f + osg::Vec2(0.5f, 0.5f);

					minimum.x() = std::min(minimum.x(), uv.x());
					minimum.y() = std::min(minimum.y(), uv.y());
					maximum.x() = std::max(maximum.x(), uv.x());
					maximum.y() = std::max(maximum.y(), uv.y());
				}
			}
		}

		_objectScreenCenter->set((minimum + maximum) * 0.5f);
		const osg::Vec2 objectScreenSize(
			std::max(maximum.x() - minimum.x(), 0.0001f),
			std::max(maximum.y() - minimum.y(), 0.0001f)
		);

		_objectScreenSize->set(objectScreenSize);

		if(_sizing && _sizing->automatic && _auraRadius.valid()) {
			const float smallestExtentPixels = std::min(
				objectScreenSize.x() * static_cast<float>(_width),
				objectScreenSize.y() * static_cast<float>(_height)
			);
			const auto radius = static_cast<int>(std::round(smallestExtentPixels * _sizing->relativeRadius));

			_auraRadius->set(std::clamp(radius, 1, 32));
		}
	}

private:
	osg::observer_ptr<osg::Camera> _mainCamera;
	osg::BoundingBox _bounds;
	osg::observer_ptr<osg::Uniform> _objectScreenCenter;
	osg::observer_ptr<osg::Uniform> _objectScreenSize;
	osg::observer_ptr<osg::Uniform> _auraRadius;
	Sizing* _sizing;
	int _width;
	int _height;
};

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);
	const bool useGBuffer = args.argc() > 1 && std::string_view(args[1]) == "gbuffer";

	if(args.argc() > 1 && !useGBuffer) {
		std::cerr << "Usage: " << args.getApplicationName() << " [gbuffer]" << std::endl;

		return 1;
	}

	osgViewer::Viewer viewer(args);

	viewer.setUpViewInWindow(50, 50, WIDTH, HEIGHT);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.getCamera()->setClearColor(osg::Vec4(0.025f, 0.035f, 0.07f, 1.0f));

	auto selected = makeSelectedModel();
	auto aura = osgx::Aura::create(WIDTH, HEIGHT, 14);

	if(!aura.valid()) {
		std::cerr << "Failed to create Aura" << std::endl;

		return 1;
	}

	aura.selectionCamera->addChild(selected);

	auto root = osgx::make_ref<osg::Group>();
	osg::Uniform* auraTime = nullptr;
	EnergyControls energy;
	UpdateAuraScreenBounds::Sizing auraSizing;
	auto objectScreenCenter = osgx::make_ref<osg::Uniform>("auraObjectScreenCenter", osg::Vec2(0.5f, 0.5f));
	auto objectScreenSize = osgx::make_ref<osg::Uniform>("auraObjectScreenSize", osg::Vec2(1.0f, 1.0f));
	osg::ComputeBoundsVisitor boundsVisitor;

	selected->accept(boundsVisitor);
	aura.selectionCamera->setPreDrawCallback(new UpdateAuraScreenBounds(
		viewer.getCamera(), boundsVisitor.getBoundingBox(), objectScreenCenter, objectScreenSize,
		aura.radius, &auraSizing, WIDTH, HEIGHT
	));

	if(useGBuffer) {
		selected->getOrCreateStateSet()->setAttributeAndModes(
			makeProgram("osgx_aura_GBuffer", GBUFFER_VERTEX_SHADER, GBUFFER_FRAGMENT_SHADER),
			osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE
		);

		static constexpr std::array formats = {
			osgx::AttachmentFormat::RGBA8,
			osgx::AttachmentFormat::RGB16F,
			osgx::AttachmentFormat::RGBA32F
		};
		auto gbuffer = osgx::GBuffer::create(selected, WIDTH, HEIGHT, formats);

		if(!gbuffer.valid()) {
			std::cerr << "Failed to create G-buffer" << std::endl;

			return 1;
		}

		auto lighting = makeFullscreenComposite(
			"osgx_aura_GBufferLighting", GBUFFER_LIGHTING_FRAGMENT_SHADER, 0
		);
		auto* lightingStateSet = lighting->getOrCreateStateSet();

		lightingStateSet->setTextureAttributeAndModes(0, gbuffer.colorTextures[0], osg::StateAttribute::ON);
		lightingStateSet->setTextureAttributeAndModes(1, gbuffer.colorTextures[1], osg::StateAttribute::ON);
		lightingStateSet->addUniform(new osg::Uniform("gColor", 0));
		lightingStateSet->addUniform(new osg::Uniform("gNormal", 1));

		root->addChild(gbuffer.camera);
		root->addChild(aura.selectionCamera);
		root->addChild(aura.dilateXCamera);
		root->addChild(aura.dilateYCamera);
		root->addChild(lighting);
		root->addChild(makeAuraComposite(
			aura, &gbuffer, objectScreenCenter, objectScreenSize, energy, auraTime
		));
	}

	else {
		root->addChild(aura.selectionCamera);
		root->addChild(aura.dilateXCamera);
		root->addChild(aura.dilateYCamera);
		root->addChild(selected);
		root->addChild(makeAuraComposite(
			aura, nullptr, objectScreenCenter, objectScreenSize, energy, auraTime
		));
	}

	viewer.setSceneData(root);

#ifdef OSGX_IMGUI
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

	// The final aura composite is the last camera in both modes, so pin the widget there. This
	// keeps its draw calls above the fullscreen effect instead of letting the effect cover the UI.
	auto* gui = new osgx::imgui::Widget(
		viewer, dynamic_cast<osg::Camera*>(root->getChild(root->getNumChildren() - 1))
	);

	gui->addSection("Aura / Energy", [&aura, &auraSizing, &energy](osg::RenderInfo&) {
		int radius = 0;
		float stripeFrequency = 0.0f, waveFrequency = 0.0f, speed = 0.0f, brightness = 0.0f;
		float outerFade = 0.0f;
		osg::Vec3 colorA, colorB;

		aura.radius->get(radius);
		energy.colorA->get(colorA);
		energy.colorB->get(colorB);
		energy.stripeFrequency->get(stripeFrequency);
		energy.waveFrequency->get(waveFrequency);
		energy.speed->get(speed);
		energy.brightness->get(brightness);
		energy.outerFade->get(outerFade);

		ImGui::Checkbox("Auto radius", &auraSizing.automatic);

		if(auraSizing.automatic) {
			float relativePercent = auraSizing.relativeRadius * 100.0f;

			if(ImGui::SliderFloat("Relative radius", &relativePercent, 1.0f, 35.0f, "%.1f%%")) {
				auraSizing.relativeRadius = relativePercent * 0.01f;
			}

			ImGui::Text("Computed radius: %d px", radius);
		}

		else if(ImGui::SliderInt("Aura radius (pixels)", &radius, 1, 32)) aura.radius->set(radius);

		if(ImGui::ColorEdit3("Color A", colorA.ptr())) energy.colorA->set(colorA);
		if(ImGui::ColorEdit3("Color B", colorB.ptr())) energy.colorB->set(colorB);
		if(ImGui::SliderFloat("Stripe frequency", &stripeFrequency, 0.0f, 48.0f)) {
			energy.stripeFrequency->set(stripeFrequency);
		}

		if(ImGui::SliderFloat("Wave frequency", &waveFrequency, 0.0f, 48.0f)) {
			energy.waveFrequency->set(waveFrequency);
		}

		if(ImGui::SliderFloat("Animation speed", &speed, -2.0f, 2.0f)) energy.speed->set(speed);
		if(ImGui::SliderFloat("Brightness", &brightness, 0.0f, 3.0f)) energy.brightness->set(brightness);
		if(ImGui::SliderFloat("Outer fade", &outerFade, 0.0f, 100.0f, "%.0f%%")) {
			energy.outerFade->set(outerFade);
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	while(!viewer.done()) {
		auraTime->set(static_cast<float>(osg::Timer::instance()->time_s()));
		viewer.frame();
	}

	return 0;
}
