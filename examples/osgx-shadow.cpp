// vimrun! ./examples/osgx-shadow
//
// A standalone proof that osgx::DIRECT_LIGHTING_HOOK_SHADOWED(_POINT) actually shadows: one of
// several small osgx::Cube-based test scenes sitting on a flat floor quad, lit by one
// osgx::LightSet light whose shadow is cast via osgx::ShadowMap: `--type directional` (default,
// ShadowMap::create(), orthographic), `--type spot` (ShadowMap::createSpot(), perspective from the
// light's position, covering its cone), or `--type point` (ShadowMap::createPoint(), an
// omnidirectional distance cube map - six real-time cameras instead of one, since a point light
// needs visibility in every direction; starts at a 256 cube size, see ShadowMap::createPoint()'s
// own comment for why).
//
// `--fan` (spot only for now): a single spinning triangular blade sitting in the light's own path,
// an animated-shadow demo built entirely on existing machinery - see FanSpinCallback below.
//
// `--flicker` (any --type): a torch/campfire-style intensity flicker plus (spot/point only) a
// small position wobble that also re-aims the shadow camera(s), so the FLOOR-RECEIVED shadow
// visibly moves too - see FlickerWobbleCallback below for why this stays example-local for now
// rather than an osgx::Light.hpp primitive.
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
// ONLY the selected scene objects into the depth texture) , mainGroup (shadow texture + shadow/
// light uniforms; renders the scene objects AND the floor, lit+shadowed)].
//
// Press 's' to toggle the shadow on/off (swaps back to the unshadowed hook shader) - the
// clearest possible A/B: same scene, same light, only the shadow term changes.
//
// `--scene cubes` (default), `lean-to`, `table`, `stairs`, and `bridge` select compact,
// deliberately code-only arrangements. Each has overlapping or raised parts, so it exercises
// both floor-received shadows and occlusion between scene objects without needing an asset.
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

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
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

// `shadowed` selects DIRECT_LIGHTING_HOOK_SHADOWED(_POINT) vs. plain DIRECT_LIGHTING_HOOK_DEFAULT -
// the 's'-key toggle in main() rebuilds the Program via this same function, swapping only that one
// shader object. `pointShadow` picks the cube-map-sampling sibling hook for --type point; ignored
// when !shadowed.
osg::ref_ptr<osg::Program> makeProgram(bool shadowed, bool pointShadow) {
	auto program = osgx::make_nref<osg::Program>(
		shadowed ? "osgx_shadow_demo_shadowed" : "osgx_shadow_demo_unshadowed"
	);
	auto fragmentSrc = osgx::resolveShaderLibs(std::string(FRAGMENT_SHADER));
	auto hookSrc = osgx::resolveShaderLibs(std::string(
		!shadowed ? osgx::DIRECT_LIGHTING_HOOK_DEFAULT :
		pointShadow ? osgx::DIRECT_LIGHTING_HOOK_SHADOWED_POINT :
		osgx::DIRECT_LIGHTING_HOOK_SHADOWED
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

struct SceneObjectSpec {
	osg::Vec3 center;
	osg::Vec3 size;
	osg::Vec3 color;
	float pitchDegrees;
};

struct SceneSpec {
	const SceneObjectSpec* objects;
	std::size_t objectCount;
	osg::Vec3 boundCenter;
	float boundRadius;
	float floorHalfSize;
};

// Returns a transformed box with its local center at the origin. Keeping every test scene to
// boxes makes differences in a shadow result attributable to the shadow implementation, rather
// than a mesh importer or an asset's normals/tangents.
osg::ref_ptr<osg::MatrixTransform> makeSceneObject(const SceneObjectSpec& spec) {
	auto geode = osgx::make_ref<osg::Geode>();
	auto transform = osgx::make_ref<osg::MatrixTransform>();

	geode->addDrawable(new osgx::Cube(osg::Vec3(), spec.size));
	transform->addChild(geode.get());
	transform->setMatrix(
		osg::Matrix::rotate(osg::DegreesToRadians(spec.pitchDegrees), osg::Vec3(0.0f, 1.0f, 0.0f)) *
		osg::Matrix::translate(spec.center)
	);

	return transform;
}

const SceneSpec* findScene(std::string_view name) {
	// Baseline: isolated box silhouettes with three different footprints/heights.
	static const SceneObjectSpec CUBES[] = {
		{osg::Vec3(-1.3f, 0.0f, 0.5f), osg::Vec3(1.0f, 1.0f, 1.0f), osg::Vec3(0.90f, 0.25f, 0.20f), 0.0f},
		{osg::Vec3(0.3f, 0.4f, 0.75f), osg::Vec3(0.9f, 0.9f, 1.5f), osg::Vec3(0.20f, 0.55f, 0.90f), 0.0f},
		{osg::Vec3(0.5f, -0.7f, 0.35f), osg::Vec3(0.7f, 0.7f, 0.7f), osg::Vec3(0.95f, 0.75f, 0.10f), 0.0f},
	};
	static const SceneObjectSpec LEAN_TO[] = {
		{osg::Vec3(0.75f, 0.0f, 1.0f), osg::Vec3(0.16f, 2.4f, 2.0f), osg::Vec3(0.50f, 0.28f, 0.18f), 0.0f},
		{osg::Vec3(-0.20f, 0.0f, 1.58f), osg::Vec3(2.20f, 2.65f, 0.16f), osg::Vec3(0.65f, 0.48f, 0.22f), 26.0f},
		{osg::Vec3(-1.00f, -1.00f, 0.62f), osg::Vec3(0.16f, 0.16f, 1.24f), osg::Vec3(0.25f, 0.42f, 0.65f), 0.0f},
		{osg::Vec3(-1.00f, 1.00f, 0.62f), osg::Vec3(0.16f, 0.16f, 1.24f), osg::Vec3(0.25f, 0.42f, 0.65f), 0.0f},
	};
	static const SceneObjectSpec TABLE[] = {
		{osg::Vec3(0.0f, 0.0f, 1.45f), osg::Vec3(2.70f, 1.55f, 0.18f), osg::Vec3(0.62f, 0.38f, 0.18f), 0.0f},
		{osg::Vec3(-1.05f, -0.55f, 0.72f), osg::Vec3(0.18f, 0.18f, 1.44f), osg::Vec3(0.24f, 0.30f, 0.38f), 0.0f},
		{osg::Vec3(-1.05f, 0.55f, 0.72f), osg::Vec3(0.18f, 0.18f, 1.44f), osg::Vec3(0.24f, 0.30f, 0.38f), 0.0f},
		{osg::Vec3(1.05f, -0.55f, 0.72f), osg::Vec3(0.18f, 0.18f, 1.44f), osg::Vec3(0.24f, 0.30f, 0.38f), 0.0f},
		{osg::Vec3(1.05f, 0.55f, 0.72f), osg::Vec3(0.18f, 0.18f, 1.44f), osg::Vec3(0.24f, 0.30f, 0.38f), 0.0f},
	};
	static const SceneObjectSpec STAIRS[] = {
		{osg::Vec3(-1.10f, 0.0f, 0.18f), osg::Vec3(0.55f, 2.0f, 0.36f), osg::Vec3(0.34f, 0.48f, 0.70f), 0.0f},
		{osg::Vec3(-0.55f, 0.0f, 0.36f), osg::Vec3(0.55f, 2.0f, 0.72f), osg::Vec3(0.38f, 0.54f, 0.78f), 0.0f},
		{osg::Vec3(0.00f, 0.0f, 0.54f), osg::Vec3(0.55f, 2.0f, 1.08f), osg::Vec3(0.42f, 0.60f, 0.84f), 0.0f},
		{osg::Vec3(0.55f, 0.0f, 0.72f), osg::Vec3(0.55f, 2.0f, 1.44f), osg::Vec3(0.46f, 0.66f, 0.90f), 0.0f},
		{osg::Vec3(1.10f, 0.0f, 0.90f), osg::Vec3(0.55f, 2.0f, 1.80f), osg::Vec3(0.50f, 0.72f, 0.96f), 0.0f},
	};
	static const SceneObjectSpec BRIDGE[] = {
		{osg::Vec3(0.0f, 0.0f, 1.45f), osg::Vec3(3.40f, 1.20f, 0.20f), osg::Vec3(0.62f, 0.48f, 0.22f), 0.0f},
		{osg::Vec3(-1.25f, -0.38f, 0.70f), osg::Vec3(0.30f, 0.30f, 1.40f), osg::Vec3(0.26f, 0.40f, 0.62f), 0.0f},
		{osg::Vec3(-1.25f, 0.38f, 0.70f), osg::Vec3(0.30f, 0.30f, 1.40f), osg::Vec3(0.26f, 0.40f, 0.62f), 0.0f},
		{osg::Vec3(1.25f, -0.38f, 0.70f), osg::Vec3(0.30f, 0.30f, 1.40f), osg::Vec3(0.26f, 0.40f, 0.62f), 0.0f},
		{osg::Vec3(1.25f, 0.38f, 0.70f), osg::Vec3(0.30f, 0.30f, 1.40f), osg::Vec3(0.26f, 0.40f, 0.62f), 0.0f},
	};
	static const SceneSpec CUBES_SCENE = {CUBES, std::size(CUBES), osg::Vec3(0.0f, 0.0f, 0.5f), 2.2f, 6.0f};
	static const SceneSpec LEAN_TO_SCENE = {LEAN_TO, std::size(LEAN_TO), osg::Vec3(0.0f, 0.0f, 0.9f), 2.7f, 6.0f};
	static const SceneSpec TABLE_SCENE = {TABLE, std::size(TABLE), osg::Vec3(0.0f, 0.0f, 0.8f), 2.5f, 6.0f};
	static const SceneSpec STAIRS_SCENE = {STAIRS, std::size(STAIRS), osg::Vec3(0.0f, 0.0f, 0.8f), 2.7f, 6.0f};
	static const SceneSpec BRIDGE_SCENE = {BRIDGE, std::size(BRIDGE), osg::Vec3(0.0f, 0.0f, 0.8f), 2.7f, 6.0f};

	if(name == "cubes") return &CUBES_SCENE;
	if(name == "lean-to") return &LEAN_TO_SCENE;
	if(name == "table") return &TABLE_SCENE;
	if(name == "stairs") return &STAIRS_SCENE;
	if(name == "bridge") return &BRIDGE_SCENE;

	return nullptr;
}

// A single flat triangular "fan blade" for the --fan demo below - local space, flat in the XY
// plane, apex pointing +Y, pivot at the local origin so FanSpinCallback can spin it in place.
// Same attribute layout as makeFloor/osgx::Cube (position=0, normal=1) - renders through the
// identical shared Program, no separate shader needed.
osg::ref_ptr<osg::Geode> makeFanBlade() {
	auto positions = osgx::make_ref<osg::Vec3Array>();

	positions->push_back(osg::Vec3(0.0f, 0.0f, 0.0f));
	positions->push_back(osg::Vec3(0.5f, 1.6f, 0.0f));
	positions->push_back(osg::Vec3(-0.5f, 1.2f, 0.0f));

	auto normals = osgx::make_ref<osg::Vec3Array>();

	normals->push_back(osg::Vec3(0.0f, 0.0f, 1.0f));

	auto geometry = osgx::make_ref<osg::Geometry>();

	geometry->setVertexArray(positions.get());
	geometry->setNormalArray(normals.get(), osg::Array::BIND_OVERALL);
	geometry->setVertexAttribArray(0, positions.get(), osg::Array::BIND_PER_VERTEX);
	geometry->setVertexAttribArray(1, normals.get(), osg::Array::BIND_OVERALL);
	geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(geometry.get());

	return geode;
}

// Keeps the --fan blade sitting in the spot light's own path and spinning - a cheap "gobo"
// (rotating occluder between light and scene) that proves the shadow system reacts to a moving
// caster in real time, no PointLight cube-map work needed. Reads *position/*direction live
// (pointers into main()'s spotPosition/spotDirection) so it tracks the ImGui sliders too.
struct FanSpinCallback: osg::NodeCallback {
	const osg::Vec3* position;
	const osg::Vec3* direction;
	float anchorDistance;

	FanSpinCallback(const osg::Vec3* position_, const osg::Vec3* direction_, float anchorDistance_):
	position(position_),
	direction(direction_),
	anchorDistance(anchorDistance_) {
	}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		auto* transform = static_cast<osg::MatrixTransform*>(node);
		const float t = nv->getFrameStamp() ? float(nv->getFrameStamp()->getSimulationTime()) : 0.0f;
		osg::Vec3 dir = *direction;

		dir.normalize();

		const osg::Vec3 anchor = *position + dir * anchorDistance;
		// Spin around the blade's OWN normal first (local space), then align that spun normal to
		// point back toward the light (-dir) - the opposite order (aligning first) would spin
		// around the WORLD-space dir axis instead, which only coincides with the blade's own
		// normal once it's already aligned, i.e. one frame late.
		const osg::Quat spin(t * 2.0, osg::Vec3(0.0f, 0.0f, 1.0f));
		osg::Quat face;

		face.makeRotate(osg::Vec3(0.0f, 0.0f, 1.0f), -dir);

		transform->setMatrix(osg::Matrix::rotate(spin * face) * osg::Matrix::translate(anchor));

		traverse(node, nv);
	}
};

// --flicker (this file's own experiment, deliberately NOT osgx::FlickerLightRig - see this file's
// header comment): torch/campfire intensity flicker PLUS a small position wobble that also
// re-aims the ShadowMap's own camera(s), so the FLOOR-RECEIVED shadow visibly moves too, not just
// the lit faces' shading. osgx::FlickerLightRig can't do this on its own - it only ever touches
// the LightSet (for shading), and has no reason to know osgx::ShadowMap exists; that composition
// belongs here, at the application level, same as every other reposition() call in this file.
// `setLight`/`reposition` are supplied per light type in main() (setPoint/setSpot vs.
// repositionPoint/repositionSpot); directional supplies `setLight` only (no position to wobble
// against, so `reposition` stays unset and `wobbleAmount` has no visible effect for it).
struct FlickerWobbleCallback: osg::NodeCallback {
	osg::Vec3 basePosition;
	float baseIntensity = 1.0f;
	float intensityAmplitude = 0.4f;
	float intensitySpeed = 3.0f;
	float wobbleAmount = 0.0f; // world units - 0 reproduces plain intensity-only flicker
	float wobbleSpeed = 4.0f;

	std::function<void(const osg::Vec3&, float)> setLight; // (position, intensity)
	std::function<void(const osg::Vec3&)> reposition; // shadowMap.reposition{Spot,Point}(position, ...)

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		const float t = nv->getFrameStamp() ? float(nv->getFrameStamp()->getSimulationTime()) : 0.0f;

		// Same layered-sine shape as osgx::FlickerLightRig (see its own comment for why three
		// incommensurate terms) - duplicated here rather than reused since this callback also owns
		// the (osgx-unaware) shadow-repositioning half of the effect.
		const float iPhase = t * intensitySpeed;
		const float wave =
			0.5f * std::sin(iPhase * 5.6f) +
			0.3f * std::sin(iPhase * 11.3f + 1.7f) +
			0.2f * std::sin(iPhase * 19.1f + 4.2f);
		const float intensity = std::max(0.0f, baseIntensity * (1.0f + intensityAmplitude * wave));

		osg::Vec3 position = basePosition;
		bool wobbled = false;

		if(wobbleAmount > 0.0f) {
			const float wPhase = t * wobbleSpeed;

			// Different frequencies/phases per axis (and from the intensity wave above) so the
			// wobble doesn't visibly correlate with the brightness pulse or move along one line.
			position += osg::Vec3(
				std::sin(wPhase * 3.1f),
				std::sin(wPhase * 2.7f + 2.1f),
				std::sin(wPhase * 3.9f + 4.4f)
			) * wobbleAmount;

			wobbled = true;
		}

		if(setLight) setLight(position, intensity);

		// Only re-aim the shadow camera(s) when there's an actual wobble to apply - at
		// wobbleAmount=0, `position == basePosition` every frame, so this would just recompute
		// identical matrices for no visible effect.
		if(wobbled && reposition) reposition(position);

		traverse(node, nv);
	}
};

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	auto lib = osgx::initialize(args);

	std::string type = "directional";
	std::string sceneName = "cubes";

	args.read("--type", type);
	args.read("--scene", sceneName);

	if(type != "directional" && type != "spot" && type != "point") {
		std::cerr << "osgx-shadow: --type must be 'directional', 'spot', or 'point'" << std::endl;

		return 1;
	}

	const SceneSpec* scene = findScene(sceneName);

	if(!scene) {
		std::cerr << "osgx-shadow: --scene must be 'cubes', 'lean-to', 'table', 'stairs', or 'bridge'"
			<< std::endl;

		return 1;
	}

	const bool spot = type == "spot";
	const bool point = type == "point";
	const bool fan = args.read("--fan");

	if(fan && !spot) {
		std::cerr << "osgx-shadow: --fan currently requires --type spot" << std::endl;

		return 1;
	}

	const bool flicker = args.read("--flicker");

	// Directional light travel direction (down and across) - steep enough that elevated parts cast
	// a clearly visible shadow onto the floor without completely burying neighboring detail. Not
	// const: the ImGui "Directional Light" section below drags this live (see
	// ShadowMap::reposition() further down).
	osg::Vec3 lightDir = osg::Vec3(0.5f, 0.35f, -1.0f);
	osg::Vec3 lightColor = osg::Vec3(1.0f, 0.96f, 0.88f);
	float lightIntensity = spot ? 40.0f : (point ? 20.0f : 3.0f);

	// Spot light (--type spot): above and in front of the scene, aimed near its center.
	osg::Vec3 spotPosition(2.2f, -2.0f, 3.4f);
	osg::Vec3 spotDirection = osg::Vec3(0.0f, 0.0f, 0.3f) - spotPosition;
	float spotInnerDegrees = 22.0f;
	float spotOuterDegrees = 32.0f;

	// Point light (--type point): no direction, no cone - just a position, close enough among the
	// scene that its inverse-square falloff and 90 degree-per-face cube coverage both stay visible.
	osg::Vec3 pointPosition(1.2f, -1.5f, 1.8f);

	// Caster bound is the scene's own objects; receiver bound is the (much larger) floor square
	// beneath them, centered at the origin per makeFloor() below - sqrt(2) covers the square's own
	// corners. Every scene's floorHalfSize (6.0) comfortably exceeds boundRadius*margin (~2.9-4.6
	// by default), so without this the floor's outer ring would silently read as unshadowed - see
	// ai/todo-shadow.md's "chopped off" writeup, case (a).
	const osgx::ShadowMap::Coverage sceneCoverage{
		scene->boundCenter, scene->boundRadius, osg::Vec3(), scene->floorHalfSize * 1.42f
	};

	auto root = osgx::make_ref<osg::Group>();
	auto casters = osgx::make_ref<osg::Group>();
	auto mainGroup = osgx::make_ref<osg::Group>();
	auto floor = makeFloor(scene->floorHalfSize, 0.0f);

	for(std::size_t i = 0; i < scene->objectCount; i++) {
		const auto& spec = scene->objects[i];
		auto caster = makeSceneObject(spec);
		auto receiver = makeSceneObject(spec);

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
	// Floor never sets its own "albedo" (unlike the scene objects above) - a flat, slightly warm
	// gray-stone default so it reads clearly against the shadow it receives.
	floor->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", osg::Vec3(0.72f, 0.68f, 0.60f)));

	auto lights = osgx::make_ref<osgx::LightSet>();

	mainSS->setAttributeAndModes(lights);

	osgx::ShadowMap::Options shadowOptions;
	float shadowBias = shadowOptions.bias;

	// Spot maps store perspective (non-linear) depth: a directional map's bias pushes shadows
	// visibly off their casters there.
	if(spot) shadowOptions.bias = 0.0005f;

	const auto setSpotLight = [&]() {
		lights->setSpot(
			0,
			spotPosition,
			spotDirection,
			lightColor,
			lightIntensity,
			osg::DegreesToRadians(spotInnerDegrees),
			osg::DegreesToRadians(spotOuterDegrees)
		);
	};

	if(spot) setSpotLight();

	else if(point) lights->setPoint(0, pointPosition, lightColor, lightIntensity);

	else lights->setDirectional(0, lightDir, lightColor, lightIntensity);

	osgx::ShadowMap shadowMap;

	// Point maps start small (256) - six real-time cameras, not one; see ShadowMap::createPoint()'s
	// own comment. Tweak up once the demo's actually running.
	if(spot) {
		shadowMap = osgx::ShadowMap::createSpot(
			spotPosition,
			spotDirection,
			osg::DegreesToRadians(spotOuterDegrees),
			sceneCoverage,
			shadowOptions
		);
	}

	else if(point) {
		shadowMap = osgx::ShadowMap::createPoint(
			pointPosition, sceneCoverage, 256, shadowOptions
		);
	}

	else shadowMap = osgx::ShadowMap::create(lightDir, sceneCoverage, shadowOptions);

	// Seeds the directional panel's slider from the real auto-derived value create()/createSpot()
	// just computed, rather than starting it at 0 and surprising the first drag.
	float shadowNormalOffset = 0.0f;

	if(shadowMap.normalOffset) shadowMap.normalOffset->get(shadowNormalOffset);

	// No depth-only Program set here anymore - ShadowMap::create()/createSpot() now install one
	// directly on shadowMap.camera's own StateSet (ON|OVERRIDE), which applies automatically to
	// any subgraph added as its child (createPoint()'s six cameras get their own distance-only
	// Program the same way, via CaptureCubeMap::Options::overrideProgram). `casters` used to need its
	// own explicit workaround; it doesn't anymore, and neither does any other osgx::shadow caller.
	// Point maps have no single camera to hang casters off of - shadowMap.casters is createPoint()'s
	// counterpart, shared by all six capture cameras (see ShadowMap's own header comment).
	if(point) shadowMap.casters->addChild(casters.get());

	else shadowMap.camera->addChild(casters.get());

	// --flicker: torch/campfire intensity flicker (all types) plus, for spot/point, a position
	// wobble that also re-aims shadowMap's own camera(s) every frame - see FlickerWobbleCallback's
	// own comment for why this composition lives here instead of a generic osgx::Light.hpp
	// primitive. `mainGroup` (not `root`) as the attach point, matching where the LightSet itself
	// lives (mainSS); free of any other update callback regardless of --type/--fan (FanSpinCallback
	// lives on its own MatrixTransform).
	osg::ref_ptr<FlickerWobbleCallback> flickerRig;

	if(flicker) {
		flickerRig = osgx::make_ref<FlickerWobbleCallback>();
		flickerRig->baseIntensity = lightIntensity;

		if(spot) {
			flickerRig->basePosition = spotPosition;

			flickerRig->setLight = [
				lights, &lightColor, &spotDirection, &spotInnerDegrees, &spotOuterDegrees
			] (const osg::Vec3& pos, float intensity) {
				lights->setSpot(
					0,
					pos,
					spotDirection,
					lightColor,
					intensity,
					osg::DegreesToRadians(spotInnerDegrees),
					osg::DegreesToRadians(spotOuterDegrees)
				);
			};

			flickerRig->reposition = [
				&shadowMap,
				&spotDirection,
				&spotOuterDegrees,
				sceneCoverage,
				shadowOptions
			] (const osg::Vec3& pos) {
				shadowMap.repositionSpot(
					pos,
					spotDirection,
					osg::DegreesToRadians(spotOuterDegrees),
					sceneCoverage,
					shadowOptions
				);
			};
		}

		else if(point) {
			flickerRig->basePosition = pointPosition;

			flickerRig->setLight = [lights, &lightColor] (const osg::Vec3& pos, float intensity) {
				lights->setPoint(0, pos, lightColor, intensity);
			};

			flickerRig->reposition = [&shadowMap, sceneCoverage, shadowOptions] (
				const osg::Vec3& pos
			) {
				shadowMap.repositionPoint(pos, sceneCoverage, shadowOptions);
			};
		}

		else {
			// No position to wobble against - reposition stays unset, wobbleAmount has no effect.
			flickerRig->setLight = [lights, &lightDir, &lightColor] (const osg::Vec3&, float intensity) {
				lights->setDirectional(0, lightDir, lightColor, intensity);
			};
		}

		mainGroup->setUpdateCallback(flickerRig.get());
	}

	// --fan: a single spinning triangular blade sitting in the light's own path, partially
	// blocking it - a cheap animated-shadow demo (see this file's own FanSpinCallback comment).
	// Shares ONE MatrixTransform between the caster pass (shadowMap.camera) and the visible scene
	// (mainGroup) - OSG nodes support multiple parents, so this stays a single spinning instance
	// instead of two independently-updated copies.
	if(fan) {
		auto fanTransform = osgx::make_ref<osg::MatrixTransform>();
		auto blade = makeFanBlade();

		blade->getOrCreateStateSet()->addUniform(new osg::Uniform("albedo", osg::Vec3(0.55f, 0.55f, 0.60f)));
		fanTransform->addChild(blade.get());
		fanTransform->setUpdateCallback(new FanSpinCallback(&spotPosition, &spotDirection, 1.6f));

		shadowMap.camera->addChild(fanTransform.get());
		mainGroup->addChild(fanTransform.get());
	}

	// Shadow texture unit 0 - this demo has no other textures. `shadowMap`'s own bias/strength/
	// casterIndex uniforms are added as-is (their defaults already match ShadowMap::Options above).
	// A point map has no shadowMatrix (SHADOW_FACTOR_POINT rebuilds direction/distance straight
	// from osgx_shadowLightPos instead - see Shadow.hpp) but does need that light-position uniform.
	if(point) {
		mainSS->setTextureAttributeAndModes(
			0, shadowMap.cubeCapture.texture.get(), osg::StateAttribute::ON
		);
		mainSS->addUniform(new osg::Uniform("osgx_shadowCubeMap", 0));
		mainSS->addUniform(shadowMap.lightPosition.get());
	}

	else {
		mainSS->setTextureAttributeAndModes(0, shadowMap.depthTexture.get(), osg::StateAttribute::ON);
		mainSS->addUniform(new osg::Uniform("osgx_shadowMap", 0));
		mainSS->addUniform(shadowMap.shadowMatrix.get());
	}

	mainSS->addUniform(shadowMap.bias.get());
	mainSS->addUniform(shadowMap.normalOffset.get());
	mainSS->addUniform(shadowMap.strength.get());
	mainSS->addUniform(shadowMap.casterIndex.get());

	bool shadowed = true;

	mainSS->setAttributeAndModes(makeProgram(shadowed, point).get(), osg::StateAttribute::ON);

	// minMarkerRadius/spotConeLength stay at their unit-scene-scale library defaults - this
	// scene's own objects/floor are already close to unit scale, unlike osgx-lights.cpp's object.
	// `mainGroup` (not `root`) so the gizmo sizes itself off the actual shaded scene, not the
	// shadow camera/gizmo overlay's own unrelated bounds.
	auto gizmos = osgx::make_ref<osgx::LightGizmos>(*lights, mainGroup.get());

	// Point maps have no single camera (see the `casters` attach point above) - their six capture
	// cameras live under shadowMap.cubeCapture.root instead.
	if(point) root->addChild(shadowMap.cubeCapture.root.get());

	else root->addChild(shadowMap.camera.get());

	// Shadow-frustum visualization - composed here at the application level, same as LightGizmos
	// itself, NOT built into either ShadowMap or LightGizmos (see FrustumGizmo's own header
	// comment). Directional/spot get the real wireframe frustum (orthographic box or perspective
	// pyramid, same reconstruction code either way); point gets a wireframe cube at the shadow
	// capture's own far-plane range instead - six pyramids would be visual noise, and a point
	// light's illumination has no hard edge to draw in the first place, only its CAPTURE range does.
	if(point) {
		root->addChild(
			new osgx::CaptureCubeGizmo(shadowMap.cubeCapture.cameras[0].get(), lightColor)
		);
	}

	else root->addChild(new osgx::FrustumGizmo(shadowMap.camera.get(), lightColor));

	root->addChild(mainGroup.get());
	root->addChild(gizmos.get());

	std::cout << "osgx-shadow: " << sceneName << " scene, " << type
		<< " light, shadow ON (press 's' to toggle)" << std::endl;

	auto viewer = osgViewer::Viewer(args);

#ifdef OSGX_IMGUI
	// Dear ImGui's single global context isn't safe to touch from more than one OSG draw thread --
	// see osgx::imgui::Widget's own class comment; harmless to set unconditionally even when
	// OSGX_IMGUI is off.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
#endif

	viewer.addEventHandler(new osgx::LambdaKeyHandler('s', [mainSS, &shadowed, point](auto&, auto&) {
		shadowed = !shadowed;

		mainSS->setAttributeAndModes(makeProgram(shadowed, point).get(), osg::StateAttribute::ON);

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

	if(spot) gui->addSection("Spot Light", [
		&shadowMap,
		&spotPosition,
		&spotDirection,
		&spotInnerDegrees,
		&spotOuterDegrees,
		&lightColor,
		&lightIntensity,
		setSpotLight,
		flickerRig,
		sceneCoverage,
		shadowOptions
	] (osg::RenderInfo&) {
		bool changed = false;

		changed |= ImGui::SliderFloat3("Position", spotPosition.ptr(), -5.0f, 5.0f);
		changed |= ImGui::SliderFloat3("Direction", spotDirection.ptr(), -1.0f, 1.0f);
		changed |= ImGui::SliderFloat("Inner Cone (deg)", &spotInnerDegrees, 1.0f, 80.0f);
		changed |= ImGui::SliderFloat("Outer Cone (deg)", &spotOuterDegrees, 1.0f, 80.0f);
		changed |= ImGui::ColorEdit3("Color", lightColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 100.0f);

		// Live every frame, not gated by `changed` - flickerRig reads wobbleAmount directly, no
		// separate commit step needed (see FlickerWobbleCallback's own comment).
		if(flickerRig) ImGui::SliderFloat("Flicker Wobble", &flickerRig->wobbleAmount, 0.0f, 1.0f);

		spotInnerDegrees = std::min(spotInnerDegrees, spotOuterDegrees);

		// lookAt() (inside repositionSpot()) is degenerate for a zero-length direction.
		if(changed && spotDirection.length2() > 1e-8f) {
			setSpotLight();

			shadowMap.repositionSpot(
				spotPosition,
				spotDirection,
				osg::DegreesToRadians(spotOuterDegrees),
				sceneCoverage,
				shadowOptions
			);

			if(flickerRig) {
				flickerRig->baseIntensity = lightIntensity;
				flickerRig->basePosition = spotPosition;
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));

	else if(point) gui->addSection("Point Light", [
		lights,
		&shadowMap,
		&pointPosition,
		&lightColor,
		&lightIntensity,
		flickerRig,
		sceneCoverage,
		shadowOptions
	] (osg::RenderInfo&) {
		bool changed = false;

		changed |= ImGui::SliderFloat3("Position", pointPosition.ptr(), -5.0f, 5.0f);
		changed |= ImGui::ColorEdit3("Color", lightColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 100.0f);

		if(flickerRig) ImGui::SliderFloat("Flicker Wobble", &flickerRig->wobbleAmount, 0.0f, 1.0f);

		if(changed) {
			lights->setPoint(0, pointPosition, lightColor, lightIntensity);

			shadowMap.repositionPoint(pointPosition, sceneCoverage, shadowOptions);

			if(flickerRig) {
				flickerRig->baseIntensity = lightIntensity;
				flickerRig->basePosition = pointPosition;
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));

	else gui->addSection("Directional Light", [
		lights,
		&shadowMap,
		&lightDir,
		&lightColor,
		&lightIntensity,
		flickerRig,
		&shadowBias,
		&shadowNormalOffset,
		sceneCoverage,
		shadowOptions
	] (osg::RenderInfo&) {
		bool changed = false;

		changed |= ImGui::SliderFloat3("Direction", lightDir.ptr(), -1.0f, 1.0f);
		changed |= ImGui::ColorEdit3("Color", lightColor.ptr());
		changed |= ImGui::SliderFloat("Intensity", &lightIntensity, 0.0f, 10.0f);

		if(ImGui::SliderFloat("Shadow Bias", &shadowBias, 0.0f, 0.05f, "%.5f")) {
			shadowMap.bias->set(shadowBias);
		}

		if(ImGui::SliderFloat(
			"Normal Offset", &shadowNormalOffset, 0.0f, std::max(0.01f, sceneCoverage.radius * 0.1f), "%.5f"
		)) {
			shadowMap.normalOffset->set(shadowNormalOffset);
		}

		if(changed) {
			if(flickerRig) flickerRig->baseIntensity = lightIntensity;

			// A dragged slider can pass through (0,0,0) - lookAt() (inside
			// ShadowMap::reposition()) is degenerate for a zero-length direction, so
			// hold the last valid direction instead of feeding it one.
			if(lightDir.length2() > 1e-8f) {
				lights->setDirectional(0, lightDir, lightColor, lightIntensity);

				shadowMap.reposition(lightDir, sceneCoverage, shadowOptions);

				// reposition() recomputes normalOffset's own derived default (coverage changed),
				// unlike bias - resync the slider's tracked value so it doesn't go stale.
				if(shadowMap.normalOffset) shadowMap.normalOffset->get(shadowNormalOffset);
			}

			else {
				lights->setDirectional(0, osg::Vec3(0.0f, 0.0f, -1.0f), lightColor, lightIntensity);
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();
}
