// vimrun! ./examples/osgx-gbuffer-edgewear
//
// Stylized, exaggerated edge wear for a bare osgx::Icosahedron (a D20-like Polyhedron). This is
// deliberately a NEW experiment beside osgx-gbuffer-dice.cpp, not a continuation of it:
//
//   osgx-gbuffer-dice.cpp - derivative curvature, good for a physically restrained thin wear mask.
//   osgx-gbuffer-edgewear.cpp - a deliberately thick, broken screen-space deposit band, intended
//                                to read as graphic seam paint / worn dice edges.
//
// The target is the vivid bottom-right D20 in the user's moodboard: faces remain strongly colored,
// seams are thicker than a physical bevel would justify, the seam is imperfect rather than a clean
// vector outline, and places where several faces meet receive an extra deposit. It is NPR, not a
// claim that real abrasion is calculated this way.
//
// WHY A FINITE G-BUFFER NEIGHBORHOOD?
// A sharp Polyhedron has a discontinuous normal exactly ON an edge. Derivatives therefore expose
// only a nearly one-pixel signal, which is the useful but intentionally restrained approach in
// osgx-gbuffer-dice.cpp. Here each fragment looks through a configurable screen-space radius in
// gNormal. A fragment near a face boundary can see the normal discontinuity, so the resulting field
// is a controllable band several pixels wide. Counting discontinuities in several directions gives
// a separate corner-intensity approximation: a simple edge is hit from fewer directions than a
// vertex where multiple face boundaries converge.
//
// This is deliberately HYBRID rather than wholly screen-space. The G-buffer neighborhood LOCATES
// edges and gives the band a stable visual pixel width while the camera moves. The band width,
// breakup, and pigment variation are sampled in the Icosahedron's world coordinate, recovered from
// view-space gPosition. Thus those marks travel with the object rather than hovering on the screen.
// (This fixed untransformed subject has object space == world space; a reusable moving-object
// material would additionally need that object's inverse model matrix.)
//
// Debug modes are part of the example, not throwaway diagnostics:
//   0 final treatment, 1 thick edge field, 2 corner accumulation, 3 broken deposit mask, 4 normals.
// Tune those fields in that order. The final image should never be the only evidence for whether a
// slider works.

#include "osgx/Core.hpp"
#include "osgx/GBuffer.hpp"
#include "osgx/ImGui.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Shapes.hpp"
#include "osgx/gltf/PBRIBL.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/DisplaySettings>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

OSGX_ENABLE_WARNINGS

#include <iostream>

namespace {

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 800;

void attachFlatMaterial(osg::Geometry* geometry) {
	auto* material = new osgx::Material();

	material->setBaseColor(osg::Vec4(0.18f, 0.24f, 0.6f, 1.0f));
	material->setRoughness(0.42f);
	material->setMetallic(0.0f);
	geometry->getOrCreateStateSet()->setAttributeAndModes(material);
}

// PBRIBLLightingScene owns the view-space G-buffer contract even when our DeferredLighting hook
// supplies all visible shading. Refresh its matrices immediately before the G-buffer is drawn.
class UpdateLightingPassCallback: public osg::Camera::DrawCallback {
public:
	UpdateLightingPassCallback(osgx::gltf::pbribl::PBRIBLLightingScene* scene, osg::Camera* camera):
		_scene(scene), _camera(camera) {}

	void operator()(osg::RenderInfo&) const override {
		_scene->update(_camera.get());
	}

private:
	osgx::gltf::pbribl::PBRIBLLightingScene* _scene;
	osg::observer_ptr<osg::Camera> _camera;
};

constexpr const char EDGEWEAR_FRAGMENT_SHADER[] = R"GLSL(
#version 460 core
#pragma osgx::gltf DEFERRED_LIGHTING_INPUTS, GET_GBUFFER

uniform int viewMode;
uniform float edgeWidthPixels;
uniform float normalEdgeThreshold;
uniform float thicknessVariation;
uniform float wearScale;
uniform float cornerStrength;
uniform float breakupAmount;
uniform float depositStrength;
uniform vec3 faceShadowColor;
uniform vec3 faceLightColor;
uniform vec3 edgeColor;
uniform vec3 cornerColor;
uniform vec3 sunDirection;

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);

	return fract((p.x + p.y) * p.z);
}

// Smooth value noise and a nearest-cell field give edgewear an organic, irregular vocabulary.
// Neither is tied to vUV: all calls below use recovered world position, so turning the camera does
// not make the marks slide across the Polyhedron.
float valueNoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);

	f = f * f * (3.0 - 2.0 * f);

	return mix(
		mix(mix(hash13(i + vec3(0.0, 0.0, 0.0)), hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(hash13(i + vec3(0.0, 1.0, 0.0)), hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(hash13(i + vec3(0.0, 0.0, 1.0)), hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(hash13(i + vec3(0.0, 1.0, 1.0)), hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z
	);
}

float fbm(vec3 p) {
	float value = 0.0;
	float amplitude = 0.5;

	for(int i = 0; i < 4; i++) {
		value += amplitude * valueNoise(p);
		p = p * 2.03 + vec3(17.1, 31.7, 11.3);
		amplitude *= 0.5;
	}

	return value;
}

float cellularNoise(vec3 p) {
	vec3 cell = floor(p);
	vec3 local = fract(p);
	float nearestDistance = 10.0;

	for(int z = -1; z <= 1; z++) {
		for(int y = -1; y <= 1; y++) {
			for(int x = -1; x <= 1; x++) {
				vec3 offset = vec3(float(x), float(y), float(z));
				vec3 seed = cell + offset;
				vec3 feature = vec3(
					hash13(seed + vec3(11.0, 0.0, 0.0)),
					hash13(seed + vec3(0.0, 23.0, 0.0)),
					hash13(seed + vec3(0.0, 0.0, 37.0))
				);

				nearestDistance = min(nearestDistance, length(offset + feature - local));
			}
		}
	}

	return 1.0 - smoothstep(0.18, 0.95, nearestDistance);
}

float discontinuity(vec3 centerNormal, vec2 uv) {
	vec3 neighborNormal = texture(gNormal, uv).rgb;

	// A cleared G-buffer neighbor is the outer silhouette. It is a valid target for this graphic
	// treatment, unlike osgx-gbuffer-dice.cpp's physical-curvature estimator.
	if(dot(neighborNormal, neighborNormal) < 0.0001) return 1.0;

	float normalDifference = 0.5 * (1.0 - dot(centerNormal, normalize(neighborNormal)));

	// Adjacent icosahedron faces differ by about 0.13 in this metric. Keeping the transition
	// narrow makes the slider practical for the deliberately sharp, faceted subject.
	return smoothstep(normalEdgeThreshold, normalEdgeThreshold + 0.15, normalDifference);
}

void main() {
	osgx_GBuffer gb = osgx_GetGBuffer(vUV);

	if(dot(gb.normal, gb.normal) < 0.0001) discard;

	vec3 N = normalize(gb.normal);
	vec2 texel = 1.0 / vec2(textureSize(gNormal, 0));
	vec3 worldPosition = (inverse(osgx_mainViewMatrix) * vec4(gb.position, 1.0)).xyz;
	float broadWear = fbm(worldPosition * wearScale);
	float cellularWear = cellularNoise(worldPosition * wearScale * 0.72);
	float wearPattern = clamp(0.62 * broadWear + 0.38 * cellularWear, 0.0, 1.0);
	float localEdgeWidth = edgeWidthPixels * mix(
		1.0 - thicknessVariation, 1.0 + thicknessVariation, wearPattern
	);
	float edgeField = 0.0;

	// A fixed 9x9 grid keeps shader cost understandable. edgeWidthPixels scales the grid's reach,
	// rather than changing loop bounds, so it is a real artistic width control and not a compile-time
	// specialization. A weighted maximum produces one broad, tapered band rather than a Sobel line.
	for(int y = -4; y <= 4; y++) {
		for(int x = -4; x <= 4; x++) {
			if(x == 0 && y == 0) continue;

			vec2 gridOffset = vec2(float(x), float(y));
			float gridDistance = length(gridOffset) / 4.0;

			if(gridDistance > 1.0) continue;

			vec2 sampleUV = vUV + gridOffset * texel * (localEdgeWidth / 4.0);
			float proximity = 1.0 - smoothstep(0.0, 1.0, gridDistance);

			edgeField = max(edgeField, discontinuity(N, sampleUV) * proximity);
		}
	}

	// Eight directions at the full band radius. An ordinary edge is reached by a limited set of
	// directions; a vertex reaches several. This is intentionally a readable approximation, not a
	// topology query, and the separate debug view makes its behavior immediately inspectable.
	const vec2 directions[8] = vec2[](
		vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
		normalize(vec2(1.0, 1.0)), normalize(vec2(-1.0, 1.0)),
		normalize(vec2(1.0, -1.0)), normalize(vec2(-1.0, -1.0))
	);
	float directionalHits = 0.0;

	for(int i = 0; i < 8; i++) {
		directionalHits += discontinuity(N, vUV + directions[i] * texel * localEdgeWidth);
	}

	float cornerField = smoothstep(2.0, 5.5, directionalHits) * edgeField;
	float breakup = mix(1.0, smoothstep(0.30, 0.66, wearPattern), breakupAmount);
	float depositMask = edgeField * breakup;
	float deposit = clamp(depositMask * depositStrength + cornerField * cornerStrength, 0.0, 1.0);

	if(viewMode == 1) {
		fragColor = vec4(vec3(edgeField), gb.alphaCoverage);
		return;
	}

	if(viewMode == 2) {
		fragColor = vec4(vec3(cornerField), gb.alphaCoverage);
		return;
	}

	if(viewMode == 3) {
		fragColor = vec4(vec3(depositMask), gb.alphaCoverage);
		return;
	}

	if(viewMode == 4) {
		fragColor = vec4(N * 0.5 + 0.5, gb.alphaCoverage);
		return;
	}

	// A compact, intentionally non-PBR face treatment. The screen-space deposit is the subject of
	// this example, so this avoids introducing IBL/material controls that would obscure the signal.
	vec3 L = normalize(mat3(osgx_mainViewMatrix) * sunDirection);
	float NdotL = clamp(dot(N, L), 0.0, 1.0);
	vec3 face = mix(faceShadowColor, faceLightColor, 0.22 + 0.78 * NdotL);
	vec3 seam = mix(edgeColor, cornerColor, cornerField);
	vec3 color = mix(face, seam, deposit);

	fragColor = vec4(pow(max(color, vec3(0.0)), vec3(1.0 / 2.2)), gb.alphaCoverage);
}
)GLSL";

}

int main() {
	osg::DisplaySettings::instance()->setNumMultiSamples(4);
	osgViewer::Viewer viewer;

	viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
	viewer.setUpViewInWindow(50, 50, WIDTH, HEIGHT);

	auto shape = osgx::make_ref<osgx::Icosahedron>(osg::Vec3(), 1.8f);

	attachFlatMaterial(shape);

	auto model = osgx::make_ref<osg::Geode>();

	model->addDrawable(shape);

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
			osgx::gltf::pbribl::resolveShaderLibs(EDGEWEAR_FRAGMENT_SHADER)
		)
	}};

	auto lighting = osgx::gltf::pbribl::PBRIBLLightingScene::create(
		gbuffer, nullptr, viewer.getCamera(), lightingOptions
	);

	if(!lighting.valid()) {
		std::cerr << "Failed to build the edgewear lighting pass" << std::endl;

		return 1;
	}

	auto* lightingSS = dynamic_cast<osg::Camera*>(lighting.node.get())->getOrCreateStateSet();
	auto* viewModeUniform = new osg::Uniform("viewMode", 0);
	auto* edgeWidthUniform = new osg::Uniform("edgeWidthPixels", 7.0f);
	auto* normalThresholdUniform = new osg::Uniform("normalEdgeThreshold", 0.05f);
	auto* thicknessVariationUniform = new osg::Uniform("thicknessVariation", 0.55f);
	auto* wearScaleUniform = new osg::Uniform("wearScale", 1.45f);
	auto* cornerStrengthUniform = new osg::Uniform("cornerStrength", 0.7f);
	auto* breakupAmountUniform = new osg::Uniform("breakupAmount", 0.65f);
	auto* depositStrengthUniform = new osg::Uniform("depositStrength", 0.9f);
	auto* faceShadowColorUniform = new osg::Uniform(
		"faceShadowColor", osg::Vec3(0.035f, 0.055f, 0.16f)
	);
	auto* faceLightColorUniform = new osg::Uniform(
		"faceLightColor", osg::Vec3(0.08f, 0.38f, 0.95f)
	);
	auto* edgeColorUniform = new osg::Uniform("edgeColor", osg::Vec3(1.0f, 0.70f, 0.12f));
	auto* cornerColorUniform = new osg::Uniform("cornerColor", osg::Vec3(1.0f, 0.18f, 0.04f));
	auto* sunDirectionUniform = new osg::Uniform("sunDirection", osg::Vec3(0.35f, 0.4f, 0.85f));

	lightingSS->addUniform(viewModeUniform);
	lightingSS->addUniform(edgeWidthUniform);
	lightingSS->addUniform(normalThresholdUniform);
	lightingSS->addUniform(thicknessVariationUniform);
	lightingSS->addUniform(wearScaleUniform);
	lightingSS->addUniform(cornerStrengthUniform);
	lightingSS->addUniform(breakupAmountUniform);
	lightingSS->addUniform(depositStrengthUniform);
	lightingSS->addUniform(faceShadowColorUniform);
	lightingSS->addUniform(faceLightColorUniform);
	lightingSS->addUniform(edgeColorUniform);
	lightingSS->addUniform(cornerColorUniform);
	lightingSS->addUniform(sunDirectionUniform);

	auto root = osgx::make_ref<osg::Group>();

	gbuffer.gbuffer.camera->setPreDrawCallback(
		new UpdateLightingPassCallback(&lighting, viewer.getCamera())
	);
	root->addChild(gbuffer.gbuffer.camera);
	root->addChild(lighting.node);

	viewer.setSceneData(root);
	viewer.setCameraManipulator(new osgGA::TrackballManipulator());
	viewer.addEventHandler(new osgViewer::StatsHandler());
	viewer.getCamera()->setClearColor(osg::Vec4(0.012f, 0.012f, 0.018f, 1.0f));

	std::cout
		<< "osgx-gbuffer-edgewear: 0=final 1=edge field 2=corner field 3=broken deposit 4=normal"
		<< std::endl
	;

#ifdef OSGX_IMGUI
	auto* gui = new osgx::imgui::Widget(viewer, dynamic_cast<osg::Camera*>(lighting.node.get()));

	gui->addSection("Edgewear", [
		viewModeUniform, edgeWidthUniform, normalThresholdUniform, cornerStrengthUniform,
		thicknessVariationUniform, wearScaleUniform, breakupAmountUniform, depositStrengthUniform,
		faceShadowColorUniform, faceLightColorUniform, edgeColorUniform, cornerColorUniform,
		sunDirectionUniform
	](osg::RenderInfo&) {
		static int viewMode = 0;
		static float edgeWidth = 7.0f;
		static float normalThreshold = 0.05f;
		static float thicknessVariation = 0.55f;
		static float wearScale = 1.45f;
		static float cornerStrength = 0.7f;
		static float breakupAmount = 0.65f;
		static float depositStrength = 0.9f;
		static float faceShadowColor[3] = {0.035f, 0.055f, 0.16f};
		static float faceLightColor[3] = {0.08f, 0.38f, 0.95f};
		static float edgeColor[3] = {1.0f, 0.70f, 0.12f};
		static float cornerColor[3] = {1.0f, 0.18f, 0.04f};
		static osg::Vec3 sunDirection(0.35f, 0.4f, 0.85f);

		if(ImGui::RadioButton("Final", &viewMode, 0)) viewModeUniform->set(viewMode);

		ImGui::SameLine();

		if(ImGui::RadioButton("Edge field", &viewMode, 1)) viewModeUniform->set(viewMode);

		ImGui::SameLine();

		if(ImGui::RadioButton("Corner field", &viewMode, 2)) viewModeUniform->set(viewMode);

		ImGui::SameLine();

		if(ImGui::RadioButton("Deposit mask", &viewMode, 3)) viewModeUniform->set(viewMode);

		ImGui::SameLine();

		if(ImGui::RadioButton("Normal", &viewMode, 4)) viewModeUniform->set(viewMode);

		ImGui::TextUnformatted("Tune debug views in order: edge band, corners, breakup, then final.");

		if(ImGui::SliderFloat("Edge Width (pixels)", &edgeWidth, 1.0f, 18.0f)) {
			edgeWidthUniform->set(edgeWidth);
		}

		if(ImGui::SliderFloat("Normal Edge Threshold", &normalThreshold, 0.0f, 0.5f)) {
			normalThresholdUniform->set(normalThreshold);
		}

		if(ImGui::SliderFloat("Thickness Variation", &thicknessVariation, 0.0f, 0.9f)) {
			thicknessVariationUniform->set(thicknessVariation);
		}

		if(ImGui::SliderFloat("Wear Pattern Scale", &wearScale, 0.1f, 6.0f)) {
			wearScaleUniform->set(wearScale);
		}

		if(ImGui::SliderFloat("Corner Deposit", &cornerStrength, 0.0f, 2.0f)) {
			cornerStrengthUniform->set(cornerStrength);
		}

		if(ImGui::SliderFloat("Breakup", &breakupAmount, 0.0f, 1.0f)) {
			breakupAmountUniform->set(breakupAmount);
		}

		if(ImGui::SliderFloat("Edge Deposit", &depositStrength, 0.0f, 1.5f)) {
			depositStrengthUniform->set(depositStrength);
		}

		if(ImGui::ColorEdit3("Face Shadow", faceShadowColor)) {
			faceShadowColorUniform->set(
				osg::Vec3(faceShadowColor[0], faceShadowColor[1], faceShadowColor[2])
			);
		}

		if(ImGui::ColorEdit3("Face Light", faceLightColor)) {
			faceLightColorUniform->set(
				osg::Vec3(faceLightColor[0], faceLightColor[1], faceLightColor[2])
			);
		}

		if(ImGui::ColorEdit3("Edge Color", edgeColor)) {
			edgeColorUniform->set(osg::Vec3(edgeColor[0], edgeColor[1], edgeColor[2]));
		}

		if(ImGui::ColorEdit3("Corner Color", cornerColor)) {
			cornerColorUniform->set(osg::Vec3(cornerColor[0], cornerColor[1], cornerColor[2]));
		}

		if(ImGui::SliderFloat3("Sun Direction", sunDirection.ptr(), -1.0f, 1.0f)) {
			if(sunDirection.length2() > 1e-8f) {
				sunDirection.normalize();
				sunDirectionUniform->set(sunDirection);
			}
		}
	}, osgx::imgui::SectionOptions::create(false, true));
#endif

	return viewer.run();
}
