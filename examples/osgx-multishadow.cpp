// vimrun! ./examples/osgx-multishadow
//
// The proof osgx-shadow.cpp's own header comment deferred: osgx::ShadowSet actually compositing
// TWO different lights' shadows in one scene, not just swapping between them. Fixed scene (no
// --type/--scene flags, unlike osgx-shadow.cpp - this file exists specifically to prove the
// two-light case, not to be a general shadow-type sandbox):
//
//   - A directional "sun" (osgx_lights[0]) casts the TABLE's shadow onto the floor via a 2D
//     ShadowMap (ShadowMap::create(), orthographic).
//   - A point "lantern" (osgx_lights[1]) sits UNDER the tabletop, among a few small crates, and
//     casts THEIR shadows onto the floor via a cube ShadowMap (ShadowMap::createPoint()).
//
// The area under the table is where this actually proves something: the tabletop blocks the sun
// entirely (so the directional map alone would leave that whole area flat black, lit by ambient
// only), but the lantern's own cube map has no idea the sun exists or that a tabletop occludes
// it - it only knows whether ITS light reaches each point, independently. The result: crates under
// the table cast sharp lantern-shadows on the floor, while the table itself casts a separate
// sun-shadow outside/around its own footprint - both read from the SAME osgx::ShadowSet, the SAME
// Hook::ShadowFactor shader, in one draw. See osgx::ShadowSet's own comment (Shadow.hpp) for why
// this was structurally impossible before the 2026-10-02 redesign (directional/spot and point used
// to require swapping the ENTIRE osgx_DirectLighting() hook, so at most one light could ever be
// shadowed at a time).
//
// Shaders/Program-building are otherwise identical to osgx-lights.cpp/osgx-shadow.cpp - see
// either's header comment for why that's the whole point of the hook design.

#include "osgx/osgx.hpp"
#include "osgx/Headless.hpp"
#include "osgx/ImGui.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/GL>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Uniform>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <iostream>
#include <string>
#include <string_view>

namespace {

// Identical to osgx-shadow.cpp's own VERTEX_SHADER/FRAGMENT_SHADER - see that file's header
// comment for why: osgx_DirectLighting()/osgx_ShadowFactorForLight() are the whole contract,
// nothing scene-specific belongs in the shader itself.
constexpr std::string_view VERTEX_SHADER = R"GLSL(
#version 460 core

in vec3 position;
in vec3 normal;

uniform mat4 osg_ModelViewProjectionMatrix;
uniform mat4 osg_ModelViewMatrix;
uniform mat3 osg_NormalMatrix;

out vec3 vNormal;
out vec3 vPosition;

void main() {
	vNormal = osg_NormalMatrix * normal;
	vPosition = (osg_ModelViewMatrix * vec4(position, 1.0)).xyz;
	gl_Position = osg_ModelViewProjectionMatrix * vec4(position, 1.0);
}
)GLSL";

constexpr std::string_view FRAGMENT_SHADER = R"GLSL(
#version 460 core

const float PI = 3.14159265359;

#pragma osgx::pbr MATERIAL_STRUCT
#pragma osgx::light DIRECT_LIGHTING_DECL

in vec3 vNormal;
in vec3 vPosition;

uniform mat4 osg_ViewMatrix;
uniform mat4 osg_ViewMatrixInverse;

uniform vec3 albedo;
uniform float roughness;
uniform float metallic;
uniform vec3 ambientColor;
uniform float ambientIntensity;

out vec4 fragColor;

void main() {
	osgx_Material mat;

	mat.albedo = albedo;
	mat.ao = 1.0;
	mat.roughness = roughness;
	mat.metallic = metallic;
	mat.F0 = mix(vec3(0.04), albedo, metallic);

	mat3 invViewRot = transpose(mat3(osg_ViewMatrix));
	vec3 N = invViewRot * normalize(vNormal);
	vec3 V = invViewRot * normalize(-vPosition);
	vec3 worldPos = (osg_ViewMatrixInverse * vec4(vPosition, 1.0)).xyz;

	vec3 color = ambientColor * ambientIntensity * mat.albedo * mat.ao;

	color += osgx_DirectLighting(N, V, worldPos, mat);

	color = pow(clamp(color, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));

	fragColor = vec4(color, 1.0);
}
)GLSL";

osg::ref_ptr<osg::Program> makeProgram(const osgx::ShadowSet& shadowSet) {
	auto program = osgx::make_nref<osg::Program>("osgx_multishadow_demo");
	auto fragmentSrc = osgx::resolveShaderLibs(std::string(FRAGMENT_SHADER));
	auto directLightingSrc = osgx::resolveShaderLibs(std::string(osgx::DIRECT_LIGHTING_HOOK_DEFAULT));

	program->addShader(new osg::Shader(osg::Shader::VERTEX, std::string(VERTEX_SHADER)));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragmentSrc));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, directLightingSrc));
	program->addShader(shadowSet.shader);
	program->addBindAttribLocation("position", 0);
	program->addBindAttribLocation("normal", 1);

	return program;
}

// Same floor-quad builder as osgx-shadow.cpp - see its own comment.
osg::ref_ptr<osg::Geode> makeFloor(float halfSize, float z) {
	auto positions = osgx::make_ref<osg::Vec3Array>();

	positions->push_back(osg::Vec3(-halfSize, -halfSize, z));
	positions->push_back(osg::Vec3(halfSize, -halfSize, z));
	positions->push_back(osg::Vec3(halfSize, halfSize, z));
	positions->push_back(osg::Vec3(-halfSize, halfSize, z));

	auto normals = osgx::make_ref<osg::Vec3Array>();

	normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

	auto geometry = osgx::make_ref<osg::Geometry>();

	geometry->setVertexArray(positions.get());
	geometry->setNormalArray(normals.get(), osg::Array::BIND_OVERALL);
	geometry->setVertexAttribArray(0, positions.get(), osg::Array::BIND_PER_VERTEX);
	geometry->setVertexAttribArray(1, normals.get(), osg::Array::BIND_OVERALL);
	geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(geometry.get());

	return geode;
}

struct BoxSpec {
	osg::Vec3 center;
	osg::Vec3 size;
	osg::Vec3 color;
};

osg::ref_ptr<osg::MatrixTransform> makeBox(const BoxSpec& spec) {
	auto geode = osgx::make_ref<osg::Geode>();
	auto transform = osgx::make_ref<osg::MatrixTransform>();

	geode->addDrawable(new osgx::Cube(osg::Vec3(), spec.size));
	transform->addChild(geode.get());
	transform->setMatrix(osg::Matrix::translate(spec.center));

	return transform;
}

// Adds one BoxSpec as both a shadow CASTER (into `casters`) and a lit/shadowed RECEIVER (into
// `receivers`) - every scene object in this file needs to be both (it casts its own shadow and
// receives everyone else's), unlike osgx-shadow.cpp's floor, which only receives.
void addBox(const BoxSpec& spec, osg::Group& casters, osg::Group& receivers) {
	auto caster = makeBox(spec);
	auto receiver = makeBox(spec);

	receiver->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", spec.color));

	casters.addChild(caster.get());
	receivers.addChild(receiver.get());
}

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	auto headless = osgx::headless::readArguments(args);
	auto lib = osgx::initialize(args);

	// --- Table: tabletop + 4 legs, casts/receives the directional "sun" shadow. -------------------
	const BoxSpec TABLETOP{
		osg::Vec3(0.0f, 0.0f, 1.30f), osg::Vec3(2.60f, 1.60f, 0.16f), osg::Vec3(0.60f, 0.38f, 0.18f)
	};
	const BoxSpec LEGS[] = {
		{osg::Vec3(-1.05f, -0.55f, 0.61f), osg::Vec3(0.16f, 0.16f, 1.22f), osg::Vec3(0.24f, 0.30f, 0.38f)},
		{osg::Vec3(-1.05f, 0.55f, 0.61f), osg::Vec3(0.16f, 0.16f, 1.22f), osg::Vec3(0.24f, 0.30f, 0.38f)},
		{osg::Vec3(1.05f, -0.55f, 0.61f), osg::Vec3(0.16f, 0.16f, 1.22f), osg::Vec3(0.24f, 0.30f, 0.38f)},
		{osg::Vec3(1.05f, 0.55f, 0.61f), osg::Vec3(0.16f, 0.16f, 1.22f), osg::Vec3(0.24f, 0.30f, 0.38f)},
	};

	// --- Crates: sit UNDER the tabletop (well below its z=1.22 underside, clear of the legs'
	// |x|<0.97/|y|<0.47 footprint), casts/receives the point "lantern" shadow. ---------------------
	const BoxSpec CRATES[] = {
		{osg::Vec3(-0.35f, -0.20f, 0.17f), osg::Vec3(0.34f, 0.34f, 0.34f), osg::Vec3(0.85f, 0.55f, 0.18f)},
		{osg::Vec3(0.30f, 0.15f, 0.15f), osg::Vec3(0.30f, 0.30f, 0.30f), osg::Vec3(0.75f, 0.60f, 0.22f)},
		{osg::Vec3(0.0f, -0.40f, 0.14f), osg::Vec3(0.28f, 0.28f, 0.28f), osg::Vec3(0.80f, 0.52f, 0.16f)},
	};

	const float floorHalfSize = 4.0f;

	osg::Vec3 sunDirection(0.45f, 0.30f, -1.0f);
	osg::Vec3 sunColor(1.0f, 0.96f, 0.88f);
	float sunIntensity = 3.2f;

	// Just above the crates, below the tabletop's underside (z=1.22) - its cube map sees the
	// tabletop occluding straight up, and the crates occluding each other sideways, all in one
	// omnidirectional capture.
	osg::Vec3 lanternPosition(0.0f, 0.0f, 0.55f);
	osg::Vec3 lanternColor(1.0f, 0.75f, 0.45f);
	float lanternIntensity = 7.0f;

	auto root = osgx::make_ref<osg::Group>();
	auto sunCasters = osgx::make_ref<osg::Group>();
	auto lanternCasters = osgx::make_ref<osg::Group>();
	auto receivers = osgx::make_ref<osg::Group>();
	auto floor = makeFloor(floorHalfSize, 0.0f);

	addBox(TABLETOP, *sunCasters, *receivers);

	for(const auto& leg : LEGS) addBox(leg, *sunCasters, *receivers);
	for(const auto& crate : CRATES) addBox(crate, *lanternCasters, *receivers);

	receivers->addChild(floor.get());

	auto* mainSS = receivers->getOrCreateStateSet();

	mainSS->addUniform(new osg::Uniform("roughness", 0.6f));
	mainSS->addUniform(new osg::Uniform("metallic", 0.0f));
	mainSS->addUniform(new osg::Uniform("ambientColor", osg::Vec3(1.0f, 1.0f, 1.0f)));
	mainSS->addUniform(new osg::Uniform("ambientIntensity", 0.06f));
	floor->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", osg::Vec3(0.70f, 0.66f, 0.58f)));

	auto lights = osgx::make_ref<osgx::LightSet>();

	lights->setDirectional(0, sunDirection, sunColor, sunIntensity);
	lights->setPoint(1, lanternPosition, lanternColor, lanternIntensity);
	mainSS->setAttributeAndModes(lights);

	// Sun's coverage: the table is the caster; the whole floor is the receiver (sqrt(2) covers the
	// floor square's own corners, same convention as osgx-shadow.cpp) - without this, the floor's
	// outer ring would silently read as unshadowed (ai/todo-shadow.md's "chopped off" writeup).
	const osgx::ShadowMap::Coverage sunCoverage{
		osg::Vec3(0.0f, 0.0f, 0.75f), 2.2f, osg::Vec3(), floorHalfSize * 1.42f
	};

	// Lantern's coverage: the crates are the caster; the receiver only needs the floor patch UNDER
	// the table (the lantern's own inverse-square falloff makes it irrelevant everywhere else) -
	// sized generously (1.8) so its cube map's far plane comfortably reaches the floor on every side.
	const osgx::ShadowMap::Coverage lanternCoverage{
		osg::Vec3(0.0f, -0.15f, 0.2f), 0.5f, osg::Vec3(), 1.8f
	};

	auto sunShadow = osgx::ShadowMap::create(sunDirection, sunCoverage);
	auto lanternShadow = osgx::ShadowMap::createPoint(lanternPosition, lanternCoverage, 256);

	sunShadow.casterIndex->set(0);
	lanternShadow.casterIndex->set(1);

	sunShadow.camera->addChild(sunCasters.get());
	lanternShadow.casters->addChild(lanternCasters.get());

	// The whole point of this file: ONE ShadowSet holding BOTH a 2D (sun) and a cube (lantern)
	// map, each matched to its own osgx_lights[] index via casterIndex above - see ShadowSet's own
	// comment (Shadow.hpp) for why this was impossible before the 2026-10-02 redesign.
	auto shadowSet = osgx::ShadowSet::create();

	shadowSet.add(sunShadow);
	shadowSet.add(lanternShadow);
	shadowSet.apply(mainSS);

	// TEMPORARY diagnostic (remove once the "has no binding" warning is understood) - dumps every
	// resolved UBO/SSBO/TextureUnit index so a collision (two names sharing one index) is visible
	// directly, instead of inferred from driver warnings.
	for(const auto& slot : lib.bindings().slots()) {
		std::cout << "binding: " << slot.name << " = " << (slot.index ? std::to_string(*slot.index) : "?")
			<< std::endl;
	}

	mainSS->setAttributeAndModes(makeProgram(shadowSet).get(), osg::StateAttribute::ON);

	auto gizmos = osgx::make_ref<osgx::LightGizmos>(*lights, receivers.get());

	root->addChild(sunShadow.camera.get());
	root->addChild(lanternShadow.cubeCapture.root.get());
	root->addChild(new osgx::FrustumGizmo(sunShadow.camera.get(), sunColor));
	root->addChild(new osgx::CaptureCubeGizmo(lanternShadow.cubeCapture.cameras[0].get(), lanternColor));
	root->addChild(receivers.get());
	root->addChild(gizmos.get());

	std::cout << "osgx-multishadow: sun (directional, light 0) shadows the table; "
		"lantern (point, light 1) shadows the crates underneath it" << std::endl;

	auto viewer = osgViewer::Viewer(args);

#ifdef OSGX_IMGUI
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
#endif

	viewer.setSceneData(root.get());

	auto* manip = new osgGA::TrackballManipulator();

	viewer.setCameraManipulator(manip);
	viewer.getCamera()->setClearColor(osg::Vec4(0.04f, 0.05f, 0.08f, 1.0f));
	viewer.addEventHandler(new osgViewer::StatsHandler());

	manip->setHomePosition(
		osg::Vec3(4.5f, -5.5f, 2.6f),
		osg::Vec3(0.0f, 0.0f, 0.5f),
		osg::Vec3(0.0f, 0.0f, 1.0f)
	);
	manip->home(0.0);

#ifdef OSGX_IMGUI
	// Proves both maps stay live-correct independently: dragging either light repositions only ITS
	// OWN ShadowMap, then re-syncs the one shared ShadowSet - the other light's shadow is
	// untouched.
	auto* gui = new osgx::imgui::Widget(viewer, gizmos->getOverlay());

	gui->addSection("Sun (directional)", [
		lights, &sunShadow, &shadowSet, &sunDirection, &sunColor, &sunIntensity, sunCoverage
	] (osg::RenderInfo&) {
		// Without PushID/PopID, this section's "Color"/"Intensity" widgets hash to the SAME Dear
		// ImGui id as the Lantern section's own "Color"/"Intensity" below - confirmed live: dragging
		// one fought over the other's shared drag/popup state. Same fix as osgx-lights.cpp's own
		// per-section PushID wrapping.
		ImGui::PushID("Sun");

		bool changed = false;

		changed |= ImGui::SliderFloat3("Direction", sunDirection.ptr(), -1.0f, 1.0f);
		changed |= ImGui::ColorEdit3("Color", sunColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &sunIntensity, 0.0f, 10.0f);

		// lookAt() (inside reposition()) is degenerate for a zero-length direction.
		if(changed && sunDirection.length2() > 1e-8f) {
			lights->setDirectional(0, sunDirection, sunColor, sunIntensity);

			sunShadow.reposition(sunDirection, sunCoverage);
			shadowSet.sync();
		}

		ImGui::PopID();
	}, osgx::imgui::SectionOptions::create(false, true));

	gui->addSection("Lantern (point)", [
		lights, &lanternShadow, &shadowSet, &lanternPosition, &lanternColor, &lanternIntensity, lanternCoverage
	] (osg::RenderInfo&) {
		ImGui::PushID("Lantern");

		bool changed = false;

		changed |= ImGui::SliderFloat3("Position", lanternPosition.ptr(), -1.5f, 1.5f);
		changed |= ImGui::ColorEdit3("Color", lanternColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &lanternIntensity, 0.0f, 20.0f);

		if(changed) {
			lights->setPoint(1, lanternPosition, lanternColor, lanternIntensity);

			lanternShadow.repositionPoint(lanternPosition, lanternCoverage);
			shadowSet.sync();
		}

		ImGui::PopID();
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return osgx::headless::run(viewer, headless);
}
