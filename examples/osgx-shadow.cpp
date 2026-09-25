// vimrun! ./examples/osgx-shadow
//
// A standalone proof that osgx::DIRECT_LIGHTING_HOOK_SHADOWED actually shadows: three
// osgx::Cube shapes of different sizes/colors sitting on a flat floor quad, lit by one directional
// osgx::LightSet light, its shadow cast via osgx::ShadowMap::create().
//
// Deliberately NOT osgx::PBRScene - no glTF asset, no IBL environment, nothing but the
// generic osgx::pbr direct-lighting hook contract plus the new shadow one, mirroring
// osgx-lights.cpp's own "load nothing, just press a key" shape as closely as possible: the
// fragment shader here is IDENTICAL to osgx-lights.cpp's (only DIRECT_LIGHTING_DECL + a call
// site) - the only difference is which hook shader object makeProgram() adds alongside it
// (DIRECT_LIGHTING_HOOK_SHADOWED instead of DIRECT_LIGHTING_HOOK_DEFAULT) and the extra
// shadow-map texture/uniforms wired onto the StateSet. That's the whole point: proving the hook
// swap is really a drop-in, no other shader change needed.
//
// Scene-graph shape (avoids a shadow-texture read/write feedback loop, same pattern the old
// hand-rolled pyosg-lighting/08-shadows.py used): root -> [shadowMap.camera (PRE_RENDER, renders
// ONLY the three cubes into the depth texture) , mainGroup (shadow texture + shadow/light
// uniforms; renders the cubes AND the floor, lit+shadowed)].
//
// Press 's' to toggle the shadow on/off (swaps back to the unshadowed hook shader) - the
// clearest possible A/B: same scene, same light, only the shadow term changes.
//
// Also exercises three fixes made to osgx::shadow itself (see osgx/TODO.md's old Shadow section,
// and OpenSceneGraph.py's 11-sketchfab.py pivot, which is what surfaced all three):
//
// 1. ShadowMap::create() now builds an ORTHOGRAPHIC frustum, not a perspective one - the
//    physically-correct shape for a directional (parallel-ray) light. This file used to get away
//    with perspective because its floor is small/close; nothing here changed to accommodate it.
// 2. ShadowMap::create() now installs its OWN minimal depth-only Program on the shadow
//    camera (ON|OVERRIDE) - this file's own hand-rolled makeDepthOnlyProgram()/casters-StateSet
//    workaround is GONE below; the library now does this for every caller, for free.
// 3. The light direction is live-draggable (ImGui section below, OSGX_IMGUI builds only) via
//    ShadowMap::reposition() - an in-place camera reposition, not a full
//    ShadowMap::create() rebuild, cheap enough to call on every slider tick. The light
//    gizmo (osgx::LightGizmos) reads the same live osgx::LightSet, so it and the shadow
//    track the dragged direction together with no manual sync code.

#include "osgx/osgx.hpp"
#include "osgx/ImGui.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/GL>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Uniform>

#include <osgGA/TrackballManipulator>

#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <iostream>
#include <string_view>

namespace {

// Same attribute layout osgx::Cube uses (Shapes.hpp's VertexLayout default: position=0,
// normal=1) - the floor quad below is built by hand to match, so both it and the cubes work
// with the exact same Program.
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

// Identical to osgx-lights.cpp's FRAGMENT_SHADER - see this file's header comment for why that's
// the whole point. Only needs osgx_DirectLighting()'s CONTRACT declaration + a call site; whether
// that call is shadowed or not is entirely decided by which hook shader object makeProgram()
// below adds alongside this one.
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

// `shadowed` selects DIRECT_LIGHTING_HOOK_SHADOWED vs. plain DIRECT_LIGHTING_HOOK_DEFAULT - the
// 's'-key toggle in main() rebuilds the Program via this same function, swapping only that one
// shader object.
osg::ref_ptr<osg::Program> makeProgram(bool shadowed) {
	auto program = osgx::make_nref<osg::Program>(
		shadowed ? "osgx_shadow_demo_shadowed" : "osgx_shadow_demo_unshadowed"
	);
	auto fragmentSrc = osgx::resolveShaderLibs(std::string(FRAGMENT_SHADER));
	auto hookSrc = osgx::resolveShaderLibs(std::string(
		shadowed ? osgx::DIRECT_LIGHTING_HOOK_SHADOWED : osgx::DIRECT_LIGHTING_HOOK_DEFAULT
	));

	program->addShader(new osg::Shader(osg::Shader::VERTEX, std::string(VERTEX_SHADER)));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragmentSrc));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, hookSrc));
	program->addBindAttribLocation("position", 0);
	program->addBindAttribLocation("normal", 1);

	return program;
}

// A flat floor quad, XY plane at the given Z, built with the same vertex-attribute layout
// osgx::Cube uses (position=0, normal=1) so it renders through the identical Program the cubes
// use - no separate floor shader needed, unlike the old hand-rolled pyosg-lighting examples.
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
	// GL_TRIANGLE_FAN, not GL_QUADS - GL_QUADS is removed in a core-profile context (this project's
	// shaders are all "#version 460 core"); a 4-vertex fan is exactly the same two triangles for a
	// convex quad and works in either profile.
	geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN, 0, 4));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(geometry.get());

	return geode;
}

osg::ref_ptr<osg::Geode> makeCube(const osg::Vec3& center, const osg::Vec3& size) {
	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(new osgx::Cube(center, size));

	return geode;
}

}

int main() {
	auto lib = osgx::initialize();

	// Three cubes of different footprints/heights, loosely matching osgx-grid's own floor demo
	// screenshot - close enough to prove multi-caster shadows land in believable places, not a
	// pixel-exact match. Sizes are (width, depth, height); center.z is size.z()/2 so each cube's
	// base sits exactly on the floor (z=0).
	struct CubeSpec {
		osg::Vec3 center;
		osg::Vec3 size;
		osg::Vec3 color;
	};

	const CubeSpec cubes[] = {
		{osg::Vec3(-1.3f, 0.0f, 0.5f), osg::Vec3(1.0f, 1.0f, 1.0f), osg::Vec3(0.90f, 0.25f, 0.20f)}, // red
		{osg::Vec3(0.3f, 0.4f, 0.75f), osg::Vec3(0.9f, 0.9f, 1.5f), osg::Vec3(0.20f, 0.55f, 0.90f)}, // blue
		{osg::Vec3(0.5f, -0.7f, 0.35f), osg::Vec3(0.7f, 0.7f, 0.7f), osg::Vec3(0.95f, 0.75f, 0.10f)}, // yellow
	};

	// Directional light travel direction (down and across) - steep enough that all three cubes
	// cast a clearly visible shadow onto the floor without one cube's shadow completely burying
	// another's. Not const: the ImGui "Directional Light" section below drags this live (see
	// ShadowMap::reposition() further down).
	osg::Vec3 lightDir = osg::Vec3(0.5f, 0.35f, -1.0f);
	osg::Vec3 lightColor = osg::Vec3(1.0f, 0.96f, 0.88f);
	float lightIntensity = 3.0f;
	const osg::Vec3 sceneBoundCenter(0.0f, 0.0f, 0.5f);
	constexpr float sceneBoundRadius = 2.2f;

	auto root = osgx::make_ref<osg::Group>();
	auto casters = osgx::make_ref<osg::Group>();
	auto mainGroup = osgx::make_ref<osg::Group>();
	auto floor = makeFloor(6.0f, 0.0f);

	for(const auto& spec: cubes) {
		auto caster = makeCube(spec.center, spec.size);
		auto receiver = makeCube(spec.center, spec.size);

		receiver->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", spec.color));

		casters->addChild(caster.get());
		mainGroup->addChild(receiver.get());
	}

	mainGroup->addChild(floor.get());

	auto* mainSS = mainGroup->getOrCreateStateSet();

	mainSS->addUniform(new osg::Uniform("roughness", 0.6f));
	mainSS->addUniform(new osg::Uniform("metallic", 0.0f));
	mainSS->addUniform(new osg::Uniform("ambientColor", osg::Vec3(1.0f, 1.0f, 1.0f)));
	mainSS->addUniform(new osg::Uniform("ambientIntensity", 0.08f));
	// Floor never sets its own "albedo" (unlike the cubes above) - a flat, slightly warm
	// gray-stone default so it reads clearly against the shadow it receives.
	floor->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", osg::Vec3(0.72f, 0.68f, 0.60f)));

	auto lights = osgx::make_ref<osgx::LightSet>();

	mainSS->setAttributeAndModes(lights);

	lights->setCount(1);
	lights->setDirectional(0, lightDir, lightColor, lightIntensity);

	osgx::ShadowMapOptions shadowOptions;

	auto shadowMap = osgx::ShadowMap::create(
		lightDir, sceneBoundCenter, sceneBoundRadius, shadowOptions
	);

	// No depth-only Program set here anymore - ShadowMap::create() now installs one
	// directly on shadowMap.camera's own StateSet (ON|OVERRIDE), which applies automatically to
	// any subgraph added as its child. `casters` used to need its own explicit workaround; it
	// doesn't anymore, and neither does any other osgx::shadow caller.
	shadowMap.camera->addChild(casters.get());

	// Shadow texture unit 0 - this demo has no other textures. `shadowMap`'s own bias/strength/
	// casterIndex uniforms are added as-is (their defaults already match ShadowMapOptions above).
	mainSS->setTextureAttributeAndModes(0, shadowMap.depthTexture.get(), osg::StateAttribute::ON);
	mainSS->addUniform(new osg::Uniform("osgx_shadowMap", 0));
	mainSS->addUniform(shadowMap.shadowMatrix.get());
	mainSS->addUniform(shadowMap.bias.get());
	mainSS->addUniform(shadowMap.strength.get());
	mainSS->addUniform(shadowMap.casterIndex.get());

	bool shadowed = true;

	mainSS->setAttributeAndModes(makeProgram(shadowed).get(), osg::StateAttribute::ON);

	// minMarkerRadius/spotConeLength stay at their unit-scene-scale library defaults - this
	// scene's own cubes/floor are already close to unit scale, unlike osgx-lights.cpp's object.
	// `mainGroup` (not `root`) so the gizmo sizes itself off the actual shaded scene, not the
	// shadow camera/gizmo overlay's own unrelated bounds.
	auto gizmos = osgx::make_ref<osgx::LightGizmos>(*lights, mainGroup.get());

	root->addChild(shadowMap.camera.get());
	root->addChild(mainGroup.get());
	root->addChild(gizmos.get());

	std::cout << "osgx-shadow: shadow ON (press 's' to toggle)" << std::endl;

	auto viewer = osgViewer::Viewer();

#ifdef OSGX_IMGUI
	// Dear ImGui's single global context isn't safe to touch from more than one OSG draw thread --
	// see osgx::imgui::Widget's own class comment; harmless to set unconditionally even when
	// OSGX_IMGUI is off.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
#endif

	viewer.addEventHandler(new osgx::LambdaKeyHandler('s', [mainSS, &shadowed](auto&, auto&) {
		shadowed = !shadowed;

		mainSS->setAttributeAndModes(makeProgram(shadowed).get(), osg::StateAttribute::ON);

		std::cout << "osgx-shadow: shadow " << (shadowed ? "ON" : "OFF") << std::endl;

		return true;
	}));

	viewer.setSceneData(root.get());

	auto* manip = new osgGA::TrackballManipulator();

	viewer.setCameraManipulator(manip);
	manip->setHomePosition(osg::Vec3(4.5, -5.5, 3.5), osg::Vec3(0.0, 0.0, 0.5), osg::Vec3(0.0, 0.0, 1.0));
	manip->home(0.0);
	viewer.getCamera()->setClearColor(osg::Vec4(0.04f, 0.05f, 0.08f, 1.0f));
	viewer.addEventHandler(new osgViewer::StatsHandler());

#ifdef OSGX_IMGUI
	// Proves ShadowMap::reposition(): dragging the light live reshapes the
	// shadow (and moves osgx::LightGizmos' overlay, reading the same LightSet) without ever
	// rebuilding shadowMap's camera/FBO/depth texture.
	//
	// gizmos->getOverlay() pinned explicitly as the draw camera - osgx::LightGizmos' overlay is a
	// POST_RENDER camera nested under `root` (not a View slave), which draws AFTER the master
	// camera's own PostDrawCallback (where Widget's default drawCamera=nullptr guess fires); left
	// at the default, the panel rendered, then was immediately painted over by the gizmo overlay's
	// own later draw. See Widget's own constructor comment for this exact scenario.
	auto* gui = new osgx::imgui::Widget(viewer, gizmos->getOverlay());

	gui->addSection("Directional Light", [
		lights,
		&shadowMap,
		&lightDir,
		&lightColor,
		&lightIntensity,
		sceneBoundCenter,
		shadowOptions
	] (osg::RenderInfo&) {
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

				shadowMap.reposition(lightDir, sceneBoundCenter, sceneBoundRadius, shadowOptions);
			}

			else {
				lights->setDirectional(0, osg::Vec3(0.0f, 0.0f, -1.0f), lightColor, lightIntensity);
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();
}
