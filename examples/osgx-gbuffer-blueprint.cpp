// vimrun! ./examples/osgx-gbuffer-blueprint model.gltf
// vimrun! ./examples/osgx-gbuffer-blueprint --shape icosahedron
//
// A "blueprint / hologram" technical-visualization style, built the same way
// osgx-gbuffer-comic.cpp is: an osgx::Hook::DeferredLighting override REPLACING
// PBRIBLLightingScene::create()'s entire lighting-pass main() - see that file's own header
// comment for why the environment/LightSet/ShadowMap stay unused (this style mostly ignores the
// loaded material entirely, even more than comic does - see GBUFFER.md's "Blueprint / Holographic
// Renderer" experiment, which is the design brief this file implements).
//
// The shader reads the G-buffer via osgx_GetGBuffer() and renders: a Fresnel/rim glow (the
// "luminous edge" look); silhouette + crease outlines reused near-verbatim from
// osgx-gbuffer-comic.cpp's own detectEdge(); periodic depth-range isolines (screen-derivative
// antialiased "distance to nearest integer boundary" grid lines, the same shape hatchStripe()'s
// smoothstep antialiasing used in comic, just over view-space depth instead of a world-space
// stripe coordinate); and an animated scan plane sweeping along a world-space axis, wrapping every
// `scanRange` world units. Everything composites additively onto a near-black base in a single
// flat cyan/emissive palette - no albedo, no specular, no lighting bands.
//
// Deliberately skips osgx::SSAO (unlike osgx-gbuffer-comic.cpp) - this style doesn't shade
// contact/crevice occlusion at all, so there's nothing for it to feed.
//
// No environment/IBL, no LightSet, no ShadowMap - same reasoning as osgx-gbuffer-comic.cpp's own
// file header: PBRIBLLightingScene::create()'s `environment` parameter is optional specifically
// for a Hook::DeferredLighting override like this one.
//
// NOT YET IMPLEMENTED - "soft" contour/edge glow (design intent only, 2026-08-21). The moodboard's
// own Blueprint/Holographic reference has its contour/edge lines visibly BLURRED into a soft halo;
// this file's rangeRing()/detectEdge() are deliberately crisp (a couple-pixel smoothstep
// antialias only), which reads noticeably sharper/thinner than the reference. A true version of
// that blur can't happen inside this single fullscreen-quad fragment shader - it needs a real
// spatial blur, which means sampling a PRE-blurred texture, not more per-pixel math. Likely
// approach: render the edge/contour/scan mask(s) to a small offscreen target (same shape as
// osgx::SSAO's rawCamera->blurCamera two-pass split, GBuffer.hpp), box/gaussian-blur it in a
// second RTT pass, then add the blurred result back as a soft glow ON TOP OF the existing sharp
// lines (not replacing them) in this shader. A genuinely separate future pass, not a tweak to the
// current one.
//
// `--shape <name>` (added 2026-08-21) feeds a bare osgx::Polyhedron through this exact same
// pipeline instead of loading a glTF asset - see attachFlatMaterial()/buildShapeNode() below.
// First concrete step on TODO.md's "Generic vs. glTF-specific layering" goal: proves the deferred
// G-buffer + Hook::DeferredLighting pipeline doesn't actually require glTF-sourced geometry, only
// the shader-side material CONTRACT (which was already glTF-independent, just never had a non-glTF
// C++ producer before now). Deliberately the plainest possible non-glTF consumer - no decals, no
// per-face materials, no dice-specific anything.

#include "osgx/Core.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/gltf/PBRIBL.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Shapes.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/BlendFunc>
#include <osg/ComputeBoundsVisitor>
#include <osg/DisplaySettings>
#include <osg/Geode>
#include <osg/Program>
#include <osg/Shader>
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

// Gives a hand-authored, non-glTF geometry (a bare osgx::Polyhedron below) the osgx::Material that
// PBRIBLGBuffer::create()'s geometry pass reads via MATERIAL_INPUTS/GET_MATERIAL (PBR.hpp). It is
// the same StateAttribute the glTF loader builds for a factor-only material. No textures are bound:
// every texture read in GET_MATERIAL/GET_ALPHA/GET_EMISSIVE is gated on a has*Map flag that is
// false here, and alpha mode/cutoff and emissive factor keep their Opaque/0.5/black defaults.
void attachFlatMaterial(
	osg::Geometry* geometry, const osg::Vec4& baseColor, float roughness, float metallic
) {
	auto* material = new osgx::Material();

	material->setBaseColor(baseColor);
	material->setRoughness(roughness);
	material->setMetallic(metallic);

	geometry->getOrCreateStateSet()->setAttributeAndModes(material);
}

// `nullptr` on an unrecognized name - caller prints usage and exits, same contract
// osgDB::readRefNodeFile() has for a bad path.
osg::ref_ptr<osg::Node> buildShapeNode(const std::string& name) {
	osg::ref_ptr<osgx::Polyhedron> shape;

	if(name == "cube") shape = new osgx::Cube();
	else if(name == "tetrahedron") shape = new osgx::Tetrahedron();
	else if(name == "octahedron") shape = new osgx::Octahedron();
	else if(name == "icosahedron") shape = new osgx::Icosahedron();
	else if(name == "dodecahedron") shape = new osgx::Dodecahedron();
	else if(name == "d10") shape = new osgx::PentagonalTrapezohedron();
	else return nullptr;

	// Deliberately plain - this style barely looks at albedo/roughness/metallic anyway (see the
	// file header: blueprint "mostly ignores normal surface materials"), so there's nothing to
	// tune here; the point is proving the material CONTRACT round-trips, not picking a look.
	attachFlatMaterial(shape.get(), osg::Vec4(0.6f, 0.6f, 0.65f, 1.0f), 0.4f, 0.0f);

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(shape);

	return geode;
}

// Refreshes the lighting pass's view-matrix uniforms every frame (same requirement as
// osgx-gbuffer-comic.cpp's own UpdateLightingPassCallback) and pushes the current simulation time
// into `time`, which the shader's scanBand() uses to animate the scan plane - no other per-frame
// state is needed for this style (no SSAO projection, unlike comic's variant of this class).
class UpdateLightingPassCallback: public osg::Camera::DrawCallback {
public:
	UpdateLightingPassCallback(
		osgx::gltf::pbribl::PBRIBLLightingScene* scene,
		osg::Camera* mainCamera,
		osg::Uniform* timeUniform
	):
		_scene(scene), _mainCamera(mainCamera), _timeUniform(timeUniform) {}

	void operator()(osg::RenderInfo& renderInfo) const override {
		_scene->update(_mainCamera.get());

		if(_timeUniform.valid()) {
			const auto* frameStamp = renderInfo.getState()->getFrameStamp();

			if(frameStamp) _timeUniform->set(static_cast<float>(frameStamp->getSimulationTime()));
		}
	}

private:
	osgx::gltf::pbribl::PBRIBLLightingScene* _scene;
	osg::observer_ptr<osg::Camera> _mainCamera;
	osg::observer_ptr<osg::Uniform> _timeUniform;
};

// The osgx::Hook::DeferredLighting override itself. Requires nothing but the G-buffer contract --
// no LightSet, no ShadowMap, no IBL environment - see the file-level comment above.
constexpr const char CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core
#pragma osgx::gltf DEFERRED_LIGHTING_INPUTS, GET_GBUFFER

uniform vec3 baseColor;
uniform vec3 edgeColor;
uniform float fresnelPower;
uniform float contourFrequency;
uniform float contourWidth;
uniform vec3 contourCenter;
// Selects which coordinate the range rings key off: false (model-space, default) anchors them to
// distance-from-contourCenter so they read as shells fixed to the object; true (view-space) uses
// raw camera-relative depth instead, which visibly slides across the surface as the camera moves --
// see rangeRing()'s own comment for the full swimming-vs-fixed tradeoff. Both are legitimate looks,
// not a bug/fix pair - exposed as a live toggle rather than picking one.
uniform bool contourViewSpace;
uniform float edgeThickness;
uniform float translucency;
uniform float time;
uniform float scanSpeed;
uniform float scanWidth;
uniform float scanGlow;
uniform float scanRange;
uniform vec3 scanAxis;

const float EDGE_NORMAL_THRESHOLD = 0.35;
const float EDGE_DEPTH_THRESHOLD = 0.05;

// Silhouette + crease outline test - same shape as osgx-gbuffer-comic.cpp's own detectEdge(),
// just parameterized by a live `edgeThickness` uniform instead of a compile-time constant (this
// style has no hatch/band pass competing for ImGui real estate, so exposing it costs nothing).
float detectEdge(vec2 uv, vec3 N0, float depth0) {
	vec2 texel = edgeThickness / vec2(textureSize(gNormal, 0));
	vec2 offsets[4] = vec2[](
		vec2(texel.x, 0.0), vec2(-texel.x, 0.0), vec2(0.0, texel.y), vec2(0.0, -texel.y)
	);
	float edge = 0.0;

	for(int i = 0; i < 4; i++) {
		vec3 N = texture(gNormal, uv + offsets[i]).rgb;

		if(dot(N, N) < 0.0001) { edge = 1.0; continue; } // neighbor is background -> silhouette

		float depth = texture(gPosition, uv + offsets[i]).z;

		edge = max(edge, step(EDGE_NORMAL_THRESHOLD, 1.0 - dot(N0, normalize(N))));
		edge = max(edge, step(EDGE_DEPTH_THRESHOLD * abs(depth0), abs(depth0 - depth)));
	}

	return edge;
}

// Periodic "range ring" isolines - distance-to-nearest-integer-boundary, antialiased by the
// screen-space derivative of `coord` so line width stays roughly constant in pixels regardless of
// how fast `coord` changes across the surface (a fixed width alone goes razor-thin on near-
// parallel/glancing geometry and thick on head-on faces).
//
// `coord` is caller-chosen (see contourViewSpace, main() below): world-space distance from the
// model's own center gives rings that read as concentric shells fixed to the object, immune to
// camera movement; raw view-space depth (`-gb.position.z`) instead visibly slides the same rings
// across the surface as the camera orbits/dollies, since the same physical point reports different
// view depth from different viewpoints - the identical swimming-vs-fixed tradeoff
// osgx-gbuffer-comic.cpp's own hatchCoord ran into with a screen-space coordinate before switching
// to world position, except here BOTH looks are kept as a live toggle rather than one replacing the
// other - confirmed live as two genuinely different, both-useful effects (a "sonar shell" read vs.
// a "depth-of-field rangefinder" read), not a bug/fix pair.
float rangeRing(float coord) {
	float f = fract(coord);
	float d = min(f, 1.0 - f);
	float aa = fwidth(coord) + 0.001;

	return 1.0 - smoothstep(0.0, contourWidth + aa, d);
}

// Distance from `worldPos` to a moving plane at `dot(worldPos, scanAxis) == offset`, wrapping
// every `scanRange` world units so the plane loops continuously back through the model instead of
// sweeping off one edge and going dark forever.
float scanBand(vec3 worldPos) {
	float coord = dot(worldPos, scanAxis);
	float offset = mod(time * scanSpeed, scanRange);
	float half_ = scanRange * 0.5;
	float dist = abs(mod(coord - offset + half_, scanRange) - half_);

	return 1.0 - smoothstep(0.0, scanWidth, dist);
}

void main() {
	osgx_GBuffer gb = osgx_GetGBuffer(vUV);

	// Same "cleared-but-never-written pixel" background test the built-in default uses.
	if(dot(gb.normal, gb.normal) < 0.0001) discard;

	vec3 N_view_n = normalize(gb.normal);
	mat3 invView = transpose(mat3(osgx_mainViewMatrix));
	vec3 N = invView * N_view_n;
	vec3 V = normalize(invView * normalize(-gb.position));
	vec3 worldPos = (osgx_mainViewMatrixInverse * vec4(gb.position, 1.0)).xyz;

	float fresnel = pow(1.0 - max(dot(N, V), 0.0), fresnelPower);
	float edge = detectEdge(vUV, N_view_n, gb.position.z);
	// See contourViewSpace's own comment above and rangeRing()'s - both coordinate choices are
	// kept live-toggleable rather than one replacing the other.
	float ringCoord = contourViewSpace ? -gb.position.z : length(worldPos - contourCenter);
	// Suppress the ring pattern right on top of a silhouette/crease line - otherwise the two
	// signals visually compete exactly where the outline is already carrying the read.
	float contour = rangeRing(ringCoord * contourFrequency) * (1.0 - edge);
	float scan = scanBand(worldPos);

	vec3 color = baseColor * (fresnel * 0.6 + 0.05);

	color += edgeColor * edge;
	color += edgeColor * contour * 0.5;
	color += edgeColor * scan * scanGlow;

	// Silhouette/creases/scan band read as fully opaque; interior surface fades toward
	// `translucency` so the hologram reads as see-through rather than a solid model recolored
	// cyan - the "optional transparency" GBUFFER.md's Blueprint/Holographic brief calls for.
	float alpha = clamp(max(max(fresnel, edge), max(contour * 0.5, scan)), translucency, 1.0);

	fragColor = vec4(color, alpha * gb.alphaCoverage);
}
)GLSL";

}

int main(int argc, char** argv) {
	osg::ArgumentParser args(&argc, argv);

	args.getApplicationUsage()->setCommandLineUsage(
		std::string(args.getApplicationName()) +
		" <model.gltf> | --shape <name> [--samples <count>] [blueprint options]"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--samples <count>", "Request this many default-framebuffer MSAA samples (default: 4)"
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--shape <name>",
		"Use a bare osgx::Polyhedron instead of loading <model.gltf> - one of: cube, tetrahedron, "
		"octahedron, icosahedron, dodecahedron, d10. Proves this deferred pipeline against a "
		"hand-authored, non-glTF-sourced geometry (see attachFlatMaterial()'s own comment)."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--fresnel-power <exponent>", "Rim-glow falloff exponent (default: 2.5)."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--contour-density <multiplier>",
		"Scales the depth-ring frequency (default: 1.0). The auto-derived base (from the loaded "
		"model's own bounding radius) should look reasonable on any model size without this."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--contour-width <fraction>", "Depth-ring line width, 0..0.5 (default: 0.08)."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--scan-speed <units/sec>",
		"Scan-plane sweep speed as a fraction of the model's bounding diameter per second "
		"(default: 0.3)."
	);
	args.getApplicationUsage()->addCommandLineOption(
		"--translucency <fraction>", "Minimum interior opacity, 0..1 (default: 0.25)."
	);
	int samples = 4;
	float fresnelPower = 2.5f;
	float contourDensity = 1.0f;
	float contourWidth = 0.08f;
	float scanSpeed = 0.3f;
	float translucency = 0.25f;
	std::string shapeName;

	args.read("--samples", samples);
	args.read("--fresnel-power", fresnelPower);
	args.read("--contour-density", contourDensity);
	args.read("--contour-width", contourWidth);
	args.read("--scan-speed", scanSpeed);
	args.read("--translucency", translucency);
	args.read("--shape", shapeName);

	if((args.argc() < 2 && shapeName.empty()) || samples < 0) {
		args.getApplicationUsage()->write(std::cerr);

		return 1;
	}

	osg::DisplaySettings::instance()->setNumMultiSamples(static_cast<unsigned int>(samples));
	osgViewer::Viewer viewer(args);

	// Dear ImGui's single global context isn't safe to touch from more than one OSG draw thread --
	// see osgx::imgui::Widget's own class comment; harmless to set unconditionally even when
	// OSGX_IMGUI is off.
	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
	viewer.setUpViewInWindow(50, 50, WIDTH, HEIGHT);

	osgDB::Registry::instance()->addFileExtensionAlias("glb", "gltf");

	osg::ref_ptr<osg::Node> model;

	if(!shapeName.empty()) {
		model = buildShapeNode(shapeName);

		if(!model) {
			std::cerr << "Unknown --shape: " << shapeName << std::endl;

			return 1;
		}
	}

	else {
		const auto modelPath = findModelFile(args[1]);

		if(!modelPath.empty()) model = osgDB::readRefNodeFile(modelPath.string());

		if(!model) {
			std::cerr << "Failed to load: " << args[1] << std::endl;

			return 1;
		}
	}

	// Same "derive from the model's own bounds" precedent osgx-gbuffer-comic.cpp's own
	// hatchFrequencyFor() and osgx::SSAO::create()'s radius/bias arguments use - a fixed
	// depth-ring frequency or scan range tuned for one model looks wrong at a wildly different
	// scale (DamagedHelmet vs. Fox, same as comic's own note).
	osg::ComputeBoundsVisitor boundsVisitor;

	model->accept(boundsVisitor);

	const auto& bounds = boundsVisitor.getBoundingBox();
	const float boundRadius = bounds.valid() ? bounds.radius() : 1.0f;
	const float safeRadius = boundRadius > 0.001f ? boundRadius : 0.001f;
	// World-space, assumed == model space here - the loaded model is parented directly under the
	// G-buffer geometry pass with no further transform, same assumption scanAxis's dot(worldPos,
	// scanAxis) above already relies on. Fixed once at load time; the model itself never moves
	// (only the camera does, via TrackballManipulator), so this never needs a per-frame update.
	const osg::Vec3 contourCenter = bounds.valid() ? bounds.center() : osg::Vec3(0.0f, 0.0f, 0.0f);
	// Roughly 5 concentric shells from the model's own center out to its bounding radius, at
	// density=1.0.
	auto contourFrequencyFor = [safeRadius](float density) {
		return density * 5.0f / safeRadius;
	};
	const float contourFrequency = contourFrequencyFor(contourDensity);
	const float scanRange = safeRadius * 2.2f;
	// Scan speed is expressed as bounding-diameters/sec so it reads at the same visual rate on
	// any model scale, same reasoning contourFrequencyFor() above uses.
	auto scanSpeedFor = [scanRange](float speed) { return speed * scanRange; };
	const osg::Vec3 scanAxis(0.0f, 1.0f, 0.0f);
	const osg::Vec3 baseColor(0.15f, 0.65f, 1.0f);
	const osg::Vec3 edgeColor(0.35f, 0.95f, 1.0f);

	// No HDR/manifest environment loaded here at all - this style never samples IBL, and
	// PBRIBLLightingScene::create()'s `environment` parameter is optional specifically for
	// callers like this one (see that function's own comment).
	osgx::gltf::pbribl::PBRIBLEnvironment environment;
	auto gbuffer = osgx::gltf::pbribl::PBRIBLGBuffer::create(model, WIDTH, HEIGHT);

	if(!gbuffer.valid()) {
		std::cerr << "Failed to build the G-buffer geometry pass" << std::endl;

		return 1;
	}

	osgx::gltf::pbribl::PBRIBLLightingPassOptions lightingOptions;

	lightingOptions.hooks = {{
		osgx::Hook::DeferredLighting,
		new osg::Shader(
			osg::Shader::FRAGMENT,
			// resolveShaderLibs() is what expands `#pragma osgx::gltf DEFERRED_LIGHTING_INPUTS,
			// GET_GBUFFER` above into real GLSL - see osgx-gbuffer-comic.cpp's own comment at
			// this exact call site for why raw un-expanded source can't be passed directly.
			osgx::gltf::pbribl::resolveShaderLibs(CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER)
		)
	}};

	auto lighting = osgx::gltf::pbribl::PBRIBLLightingScene::create(
		gbuffer, environment, viewer.getCamera(), 1.0f, 1.0f, lightingOptions
	);

	if(!lighting.valid()) {
		std::cerr << "Failed to build the lighting pass" << std::endl;

		return 1;
	}

	// All of these live on the lighting pass's own StateSet - that's where
	// CUSTOM_DEFERRED_LIGHTING_FRAGMENT_SHADER's `uniform` declarations expect to find them, same
	// as osgx-gbuffer-comic.cpp's own uniform wiring. Kept as named pointers (not fire-and-forget)
	// so the ImGui section below can push live edits into the SAME osg::Uniform objects.
	auto* lightingSS = dynamic_cast<osg::Camera*>(lighting.node.get())->getOrCreateStateSet();

	// PBRIBLLightingScene::create()'s composite quad turns GL_DEPTH_TEST off but never touches
	// GL_BLEND (PBRIBL.cpp) - ambient GL_BLEND defaults to OFF, so `translucency`'s alpha was
	// being computed correctly and then silently dropped at the framebuffer write. This is plain
	// blending, not depth peeling: the quad only ever carries the single nearest-surface G-buffer
	// sample per pixel, so there's no second surface to reveal by peeling - "translucent" here
	// means fading that one known surface toward whatever the framebuffer already holds behind it
	// (the viewer's clear color for an empty scene, or real opaque geometry if this style is ever
	// layered on top of another pass). A genuine see-through-the-mesh x-ray would need multiple
	// depth layers - that's GBUFFER.md's separate X-Ray/Cutaway experiment, not this fix.
	lightingSS->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), osg::StateAttribute::ON
	);

	auto* baseColorUniform = new osg::Uniform("baseColor", baseColor);
	auto* edgeColorUniform = new osg::Uniform("edgeColor", edgeColor);
	auto* fresnelPowerUniform = new osg::Uniform("fresnelPower", fresnelPower);
	auto* contourFrequencyUniform = new osg::Uniform("contourFrequency", contourFrequency);
	auto* contourWidthUniform = new osg::Uniform("contourWidth", contourWidth);
	auto* contourCenterUniform = new osg::Uniform("contourCenter", contourCenter);
	// Model-space by default (rings fixed to the object) - see contourViewSpace's own comment in
	// the shader above for why view-space is kept as a toggle rather than dropped.
	auto* contourViewSpaceUniform = new osg::Uniform("contourViewSpace", false);
	auto* edgeThicknessUniform = new osg::Uniform("edgeThickness", 4.0f);
	auto* translucencyUniform = new osg::Uniform("translucency", translucency);
	auto* timeUniform = new osg::Uniform("time", 0.0f);
	auto* scanSpeedUniform = new osg::Uniform("scanSpeed", scanSpeedFor(scanSpeed));
	auto* scanWidthUniform = new osg::Uniform("scanWidth", safeRadius * 0.03f);
	auto* scanGlowUniform = new osg::Uniform("scanGlow", 1.5f);
	auto* scanRangeUniform = new osg::Uniform("scanRange", scanRange);
	auto* scanAxisUniform = new osg::Uniform("scanAxis", scanAxis);

	lightingSS->addUniform(baseColorUniform);
	lightingSS->addUniform(edgeColorUniform);
	lightingSS->addUniform(fresnelPowerUniform);
	lightingSS->addUniform(contourFrequencyUniform);
	lightingSS->addUniform(contourWidthUniform);
	lightingSS->addUniform(contourCenterUniform);
	lightingSS->addUniform(contourViewSpaceUniform);
	lightingSS->addUniform(edgeThicknessUniform);
	lightingSS->addUniform(translucencyUniform);
	lightingSS->addUniform(timeUniform);
	lightingSS->addUniform(scanSpeedUniform);
	lightingSS->addUniform(scanWidthUniform);
	lightingSS->addUniform(scanGlowUniform);
	lightingSS->addUniform(scanRangeUniform);
	lightingSS->addUniform(scanAxisUniform);

	auto root = osgx::make_ref<osg::Group>();

	// gbuffer.gbuffer.camera is the FIRST PRE_RENDER camera in this scene graph (added before
	// lighting.node below) - see osgx-gbuffer-comic.cpp's own UpdateLightingPassCallback comment
	// (and PBRIBLLightingScene::create()'s) for why the update() call has to land here.
	gbuffer.gbuffer.camera->setPreDrawCallback(
		new UpdateLightingPassCallback(&lighting, viewer.getCamera(), timeUniform)
	);

	root->addChild(gbuffer.gbuffer.camera);
	root->addChild(lighting.node);

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	// Near-black navy, not pure black - keeps the dark background from crushing to nothing
	// against the cyan glow, same "deep blue-black" the moodboard's own blueprint panel uses.
	viewer.getCamera()->setClearColor(osg::Vec4f(3.0f / 255.0f, 6.0f / 255.0f, 12.0f / 255.0f, 1.0f));

	std::cout
		<< "osgx-gbuffer-blueprint: osgx::Hook::DeferredLighting blueprint/holographic style - "
		<< "Fresnel rim, depth-ring contours, ink outlines, animated scan plane; "
		<< "no lights/shadow/IBL evaluation" << std::endl
	;

#ifdef OSGX_IMGUI
	// drawCamera pinned to lighting.node's own Camera explicitly - see osgx-gbuffer-comic.cpp's
	// own comment at this exact call site for why the default (nullptr) gets painted over.
	auto* gui = new osgx::imgui::Widget(viewer, dynamic_cast<osg::Camera*>(lighting.node.get()));

	gui->addSection("Blueprint Style", [
		fresnelPowerUniform, contourFrequencyUniform, contourWidthUniform, contourViewSpaceUniform,
		edgeThicknessUniform, translucencyUniform, scanSpeedUniform, scanWidthUniform,
		scanGlowUniform, scanRange, contourFrequencyFor, baseColorUniform, edgeColorUniform
	](osg::RenderInfo&) {
		static float fresnelPower_ = 2.5f;
		static float contourDensity_ = 1.0f;
		static float contourWidth_ = 0.08f;
		static bool contourViewSpace_ = false;
		static float edgeThickness_ = 4.0f;
		static float translucency_ = 0.25f;
		static float scanSpeedFraction_ = 0.3f;
		static float scanWidth_ = 0.0f;
		static float scanGlow_ = 1.5f;
		static float baseColor_[3] = {0.15f, 0.65f, 1.0f};
		static float edgeColor_[3] = {0.35f, 0.95f, 1.0f};
		static bool initialized = false;

		if(!initialized) {
			scanWidthUniform->get(scanWidth_);
			contourViewSpaceUniform->get(contourViewSpace_);

			initialized = true;
		}

		if(ImGui::SliderFloat("Fresnel Power", &fresnelPower_, 0.5f, 6.0f)) {
			fresnelPowerUniform->set(fresnelPower_);
		}

		// Model-space rings only ever range over [0, boundRadius] (contourFrequencyFor's own
		// K=5*density formula), so reaching a dense "topographic map" look there needs a much
		// higher density than view-space rings (which range over raw camera-distance depth, a
		// generally larger number) ever did - 60 gives up to 300 rings across the model's own
		// radius at max, comfortably past what looked "maxed out" at the old 5.0 ceiling.
		if(ImGui::SliderFloat("Contour Density", &contourDensity_, 0.1f, 60.0f)) {
			contourFrequencyUniform->set(contourFrequencyFor(contourDensity_));
		}

		if(ImGui::SliderFloat("Contour Width", &contourWidth_, 0.0f, 0.4f)) {
			contourWidthUniform->set(contourWidth_);
		}

		// See contourViewSpace's own comment in the shader source above - both looks are kept,
		// not one replacing the other.
		if(ImGui::Checkbox("View-Space Contours", &contourViewSpace_)) {
			contourViewSpaceUniform->set(contourViewSpace_);
		}

		if(ImGui::SliderFloat("Edge Thickness", &edgeThickness_, 1.0f, 8.0f)) {
			edgeThicknessUniform->set(edgeThickness_);
		}

		if(ImGui::SliderFloat("Translucency", &translucency_, 0.0f, 1.0f)) {
			translucencyUniform->set(translucency_);
		}

		if(ImGui::SliderFloat("Scan Speed", &scanSpeedFraction_, 0.0f, 2.0f)) {
			scanSpeedUniform->set(scanSpeedFraction_ * scanRange);
		}

		if(ImGui::SliderFloat("Scan Width", &scanWidth_, 0.0f, scanRange * 0.25f)) {
			scanWidthUniform->set(scanWidth_);
		}

		if(ImGui::SliderFloat("Scan Glow", &scanGlow_, 0.0f, 4.0f)) {
			scanGlowUniform->set(scanGlow_);
		}

		if(ImGui::ColorEdit3("Base Color", baseColor_)) {
			baseColorUniform->set(osg::Vec3(baseColor_[0], baseColor_[1], baseColor_[2]));
		}

		if(ImGui::ColorEdit3("Edge Color", edgeColor_)) {
			edgeColorUniform->set(osg::Vec3(edgeColor_[0], edgeColor_[1], edgeColor_[2]));
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();
}
