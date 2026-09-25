// vimrun! ./examples/osgx-pixel-text

#include "osgx/Core.hpp"
#include "osgx/Library.hpp"
#include "osgx/PixelText.hpp"
#include "osgx/Shapes.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/Viewport>
#include <osg/observer_ptr>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// A minimal, self-contained decal shader - deliberately NOT part of osgx itself (see
// osgx::PixelText::createAtlas()'s docstring: it only generalizes the atlas-building math dice
// already does; the per-shape decal-sampling shader stays an application concern, same as
// pyosg_dice.py's own decal fragment shader). The per-face diffuse lighting below is the whole
// point of this shader: it's what visually reads as "text painted onto a lit 3D surface,"
// distinguishing it from osgx::PixelText's flat, unlit, alpha-blended vector text.
//
// osgx_cellIndex (location 4) is a Face-domain custom attribute - see
// osgx::Polyhedron::setFaceAttribute()'s docstring - carrying which atlas cell each face should
// sample, one value per face, repeated for every triangle vertex that face's fan-triangulation
// emits. Location 4 is free of Polyhedron's own reserved position/normal/uv locations (0/1/3);
// OSG's own conventional-attribute aliasing may ALSO want location 4 for some unrelated name
// (osg_SecondaryColor, depending on build), but a glBindAttribLocation call for a name a given
// shader never declares is a harmless no-op, so there's no real collision here.
constexpr const char* DECAL_VERTEX_SHADER = R"GLSL(
#version 330 core

uniform mat4 osg_ModelViewProjectionMatrix;

layout(location = 4) in float osgx_cellIndex;

in vec4 osg_Vertex;
in vec3 osg_Normal;
in vec2 osg_MultiTexCoord0;

out vec3 vNormal;
out vec2 vUV;
out float vCellIndex;

void main(void) {
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
	vNormal = osg_Normal;
	vUV = osg_MultiTexCoord0;
	vCellIndex = osgx_cellIndex;
}
)GLSL";

constexpr const char* DECAL_FRAGMENT_SHADER = R"GLSL(
#version 330 core

in vec3 vNormal;
in vec2 vUV;
in float vCellIndex;

uniform sampler2D u_atlas;
uniform vec3 u_bodyColor;
uniform vec3 u_ink;
uniform float u_cellCount;

out vec4 fragColor;

void main(void) {
	const vec3 L = vec3(0.4, 0.6, 0.7);
	float diffuse = max(dot(normalize(vNormal), normalize(L)), 0.0);
	float light = 0.35 + 0.65 * diffuse;

	// Same LINEAR-filtered-coverage + smoothstep/fwidth technique osgx::PixelText uses - see
	// PixelText.cpp's PIXEL_TEXT_FRAGMENT_SHADER for the full rationale. No discard/blending
	// needed here: unlike a PixelText quad, this is painted directly onto an opaque surface,
	// so the "background" is just u_bodyColor mixed in below, not whatever's behind the quad.
	vec2 atlasUV = vec2((vCellIndex + vUV.x) / u_cellCount, vUV.y);
	float coverage = texture(u_atlas, atlasUV).r;
	float edge = max(fwidth(coverage) * 0.5, 1e-5);
	float alpha = smoothstep(0.5 - edge, 0.5 + edge, coverage);
	vec3 color = mix(u_bodyColor, u_ink, alpha);

	fragColor = vec4(color * light, 1.0);
}
)GLSL";

// One decal cell per face of an osgx::Cube (`faceCells.size()` must equal the cube's face count,
// i.e. 6) - proves osgx::PixelText::createAtlas()'s output is a usable real-world decal texture
// (the same shape pyosg_dice.py bakes per die face), and the diffuse lighting above makes it
// unmistakable as "painted onto a lit surface" rather than another flat PixelText-style quad.
osg::ref_ptr<osg::Node> makeDecalCube(const osg::Vec3& center, const std::vector<std::string>& faceCells) {
	auto cube = osgx::make_ref<osgx::Cube>(center, osg::Vec3(1.5f, 1.5f, 1.5f));

	if(faceCells.size() != cube->faces().size()) {
		throw std::invalid_argument("makeDecalCube: faceCells must have one entry per cube face");
	}

	auto atlas = osgx::PixelText::createAtlas(faceCells);
	auto texture = osgx::make_ref<osg::Texture2D>();

	texture->setImage(atlas);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setResizeNonPowerOfTwoHint(false);

	auto cellIndices = osgx::make_ref<osg::FloatArray>(static_cast<unsigned int>(faceCells.size()));

	for(std::size_t i = 0; i < faceCells.size(); i++) (*cellIndices)[i] = static_cast<float>(i);

	cube->setFaceAttribute(4, cellIndices);

	auto program = osgx::make_ref<osg::Program>();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, DECAL_VERTEX_SHADER));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, DECAL_FRAGMENT_SHADER));

	auto* ss = cube->getOrCreateStateSet();

	ss->setAttributeAndModes(program, osg::StateAttribute::ON);
	ss->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);
	ss->addUniform(new osg::Uniform("u_atlas", 0));
	ss->addUniform(new osg::Uniform("u_bodyColor", osg::Vec3(0.85f, 0.30f, 0.15f)));
	ss->addUniform(new osg::Uniform("u_ink", osg::Vec3(1.0f, 1.0f, 1.0f)));
	ss->addUniform(new osg::Uniform("u_cellCount", static_cast<float>(faceCells.size())));

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(cube);

	return geode;
}

// sqrt(PixelText::CHARSET.size()) = sqrt(95) ~= 9.747 - 10 is the smallest column count that
// fits the whole charset in a square (10x10 = 100 cells, the last row's remaining 5 cells
// padded with blank space so every row comes out the same width).
constexpr int CHARSET_GRID_COLUMNS = 10;

// Lays out the ENTIRE osgx::PixelText::CHARSET (all 95 printable ASCII characters - the point of
// this example, now that lowercase and descenders are real glyphs and not just uppercase-folded
// aliases) as a square grid of PixelText rows. Anchored so the grid's TOP-RIGHT corner sits at
// `anchor` - the same relationship the old single-line "ABCDE01234" label had to the die (its
// right edge stopped just short of the die's left face), generalized from one row to a whole
// square block: row 0 (starting with a space) is the row nearest `anchor`, and each following
// row steps toward -Y (a little closer to the camera / lower on screen), so the die anchors the
// block's near-top-right corner exactly like it anchored the end of the original single line.
osg::ref_ptr<osg::Group> makeCharsetGrid(const osg::Vec3& anchor, float cellSize) {
	auto group = osgx::make_ref<osg::Group>();
	std::string_view charset = osgx::PixelText::CHARSET;
	int rows = (static_cast<int>(charset.size()) + CHARSET_GRID_COLUMNS - 1) / CHARSET_GRID_COLUMNS;
	float xOffset = anchor.x() - static_cast<float>(CHARSET_GRID_COLUMNS) * cellSize;

	for(int r = 0; r < rows; r++) {
		auto start = static_cast<std::size_t>(r) * static_cast<std::size_t>(CHARSET_GRID_COLUMNS);
		auto count = std::min<std::size_t>(CHARSET_GRID_COLUMNS, charset.size() - start);
		std::string rowText(charset.substr(start, count));

		rowText.resize(static_cast<std::size_t>(CHARSET_GRID_COLUMNS), ' ');

		auto label = osgx::make_ref<osgx::PixelText>(rowText, cellSize);
		auto geode = osgx::make_ref<osg::Geode>();

		geode->addDrawable(label);

		auto transform = osgx::make_ref<osg::MatrixTransform>();

		transform->setMatrix(osg::Matrix::translate(
			xOffset,
			anchor.y() - static_cast<float>(r + 1) * cellSize,
			anchor.z()
		));
		transform->addChild(geode);
		group->addChild(transform);
	}

	return group;
}

// Recomputes, every update traversal, both the HUD camera's pixel-space ortho projection and
// the label's screen-space position from the camera's CURRENT viewport - same "recompute a
// camera parameter live from a NodeCallback" idiom osgx::PickCameraSync already uses (see
// Picking.hpp) for keeping a camera in sync with something that can change frame to frame.
// `camera` is an observer_ptr, not a ref_ptr: the camera owns (transitively, via the scene
// graph) the transform this callback is attached to, so a strong ref back the other way would
// be a reference cycle.
struct HudLayoutCallback: public osg::NodeCallback {
	osg::observer_ptr<osg::Camera> camera;
	float margin;
	float labelHeight;

	HudLayoutCallback(osg::Camera* cam, float m, float h): camera(cam), margin(m), labelHeight(h) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		osg::ref_ptr<osg::Camera> cam;

		if(camera.lock(cam)) {
			const auto* viewport = cam->getViewport();

			if(viewport) {
				// Pixel-space ortho - 1 local unit = 1 screen pixel - is what makes this both
				// aspect-ratio-correct (no non-uniform stretch, unlike mapping onto a fixed
				// -1..1 NDC range on a non-square window) and exact-integer-scalable (a cellSize
				// of N * PixelText::GLYPH_ROWS puts exactly N screen pixels behind every source
				// font-pixel, an "Nx native resolution" pixel-font look with no blur).
				cam->setProjectionMatrix(osg::Matrix::ortho2D(
					0.0, viewport->width(), 0.0, viewport->height()
				));

				static_cast<osg::MatrixTransform*>(node)->setMatrix(osg::Matrix::translate(
					margin,
					static_cast<float>(viewport->height()) - margin - labelHeight,
					0.0f
				));
			}
		}

		traverse(node, nv);
	}
};

// Cycles `baseText` through 0..3 trailing dots every `interval` seconds, live, via
// PixelText::setText() - proof that updating a PixelText "on the fly" genuinely works, not just
// at construction: every tick rebuilds the per-glyph SSBO and re-derives the instance count and
// bounds from scratch (see PixelText::setText()/computeBoundingBox()), and it has to keep
// rendering correctly every frame after that, not just the one it changed on. Attached directly
// to the PixelText Drawable's own setUpdateCallback() - Drawable is-a Node in this OSG build, so
// the standard NodeCallback shape applies to it exactly like any other node.
struct LoadingDotsCallback: public osg::NodeCallback {
	std::string baseText;
	double interval;
	double lastChangeTime = -1.0;
	int dots = 0;

	LoadingDotsCallback(std::string base, double intervalSeconds):
	baseText(std::move(base)), interval(intervalSeconds) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override {
		const auto* frameStamp = nv->getFrameStamp();
		double simTime = frameStamp ? frameStamp->getSimulationTime() : 0.0;

		if(lastChangeTime < 0.0) lastChangeTime = simTime;

		if(simTime - lastChangeTime >= interval) {
			lastChangeTime = simTime;
			dots = (dots + 1) % 4;

			static_cast<osgx::PixelText*>(node)->setText(baseText + std::string(static_cast<std::size_t>(dots), '.'));
		}

		traverse(node, nv);
	}
};

// A screen-aligned PixelText in the window's top-left corner - the ortho-camera-wrapping
// technique osgx::Grid::orthoCamera() already uses for a fullscreen background overlay, just
// POST_RENDER (drawn last, on TOP of everything) instead of PRE_RENDER, and pixel-space instead
// of NDC so a specific on-screen pixel size/position is possible at all. Deliberately built
// here, not as an osgx::PixelText method: HUD placement is exactly the kind of camera-
// composition policy that stays at the call site, same reasoning as PixelText not baking in
// camera-facing/billboard behavior itself.
osg::ref_ptr<osg::Camera> makeHudLabel(std::string_view text, float scale=3.0f, float margin=12.0f) {
	float cellSize = static_cast<float>(osgx::PixelText::GLYPH_ROWS) * scale;
	auto label = osgx::make_ref<osgx::PixelText>(text, cellSize);
	auto labelGeode = osgx::make_ref<osg::Geode>();

	labelGeode->addDrawable(label);

	label->setUpdateCallback(new LoadingDotsCallback(std::string(text), 0.3));

	auto transform = osgx::make_ref<osg::MatrixTransform>();

	transform->addChild(labelGeode);

	auto camera = osgx::make_ref<osg::Camera>();

	camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	camera->setRenderOrder(osg::Camera::POST_RENDER);
	// Depth only - preserve the 3D scene's already-rendered color, and reset depth so the HUD
	// can never be occluded by anything drawn earlier, wherever it happens to land on screen.
	camera->setClearMask(GL_DEPTH_BUFFER_BIT);
	camera->setViewMatrix(osg::Matrix::identity());
	camera->setAllowEventFocus(false);
	camera->setCullingActive(false);
	camera->addChild(transform);

	transform->setUpdateCallback(new HudLayoutCallback(camera.get(), margin, cellSize));

	return camera;
}

}

int main(int, char**) {
	auto lib = osgx::initialize();

	auto root = osgx::make_ref<osg::Group>();
	osg::Vec3 dieCenter(6.0f, 0.0f, 0.75f);
	osg::Vec3 dieSize(1.5f, 1.5f, 1.5f);
	float gridCellSize = 0.5f;
	float gridMargin = 0.25f;

	// Grid's top-right corner sits `gridMargin` short of the die's left face, at the die's own
	// y-center - same gap the old single-line "ABCDE01234" label left before the die.
	osg::Vec3 gridAnchor(
		dieCenter.x() - dieSize.x() * 0.5f - gridMargin,
		dieCenter.y(),
		0.0f
	);

	root->addChild(makeCharsetGrid(gridAnchor, gridCellSize));
	root->addChild(makeDecalCube(dieCenter, {"1", "2", "3", "4", "5", "6"}));

	auto hud = makeHudLabel("LOADING");

	root->addChild(hud);

	osgViewer::Viewer viewer;

	viewer.setSceneData(root);
	viewer.getCamera()->setClearColor(osg::Vec4(0.92f, 0.92f, 0.90f, 1.0f));

	auto manipulator = new osgGA::TrackballManipulator();

	manipulator->setHomePosition(
		osg::Vec3d(3.0, -14.0, 5.0),
		osg::Vec3d(3.0, 0.0, 0.5),
		osg::Vec3d(0.0, 0.0, 1.0)
	);
	viewer.setCameraManipulator(manipulator);
	viewer.home();

	// hud's own viewport is left unset until now, then shared (not copied) with the main
	// camera's - only valid once realize() has set the main camera's viewport up from the real
	// window traits, and sharing rather than copying is what lets HudLayoutCallback see future
	// resizes too, via osg::GraphicsContext::resizedImplementation() updating that same shared
	// Viewport object in place every time the window actually changes size.
	viewer.realize();

	if(auto* mainViewport = viewer.getCamera()->getViewport()) hud->setViewport(mainViewport);

	return viewer.run();
}
