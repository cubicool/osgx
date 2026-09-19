// vimrun! ./examples/osgx-grid

#include "osgx/Core.hpp"
#include "osgx/Grid.hpp"
#include "osgx/Projection.hpp"
#include "osgx/Shapes.hpp"
#include "osgx/Shader.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Quat>
#include <osg/Shader>
#include <osg/Shape>
#include <osg/ShapeDrawable>
#include <osgGA/TrackballManipulator>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

namespace {

// A fullscreen triangle is the conventional infinite-floor primitive: its fragment shader
// reconstructs a world-space camera ray, intersects that ray with z=0, then procedurally draws
// grid lines at the hit point. There is no world-space quad to run out of.
constexpr const char* INFINITE_FLOOR_VERTEX_SHADER = R"GLSL(
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

constexpr const char* INFINITE_FLOOR_FRAGMENT_SHADER = R"GLSL(
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

	// The Z-up floor is the plane z=0. Parallel rays (the exact horizon) have no usable hit.
	if(abs(ray.z) < 1e-6) discard;

	float t = -nearPoint.z / ray.z;

	if(t <= 0.0 || t > 1.0) discard;

	vec3 hit = nearPoint + ray * t;
	vec4 clip = osg_ProjectionMatrix * osg_ViewMatrix * vec4(hit, 1.0);
	float ndcDepth = clip.z / clip.w;

	if(ndcDepth < -1.0 || ndcDepth > 1.0) discard;

	// A fullscreen primitive normally has no useful depth. Writing the ray/plane hit depth makes
	// this behave like real ground: geometry on the floor occludes it, and it occludes the clear.
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

// Grid's fragment library is independent of the mesh. This adapter maps Polyhedron's face-local
// UV chart into the same grid space that Grid's own vertex shader uses; the `dice` mode below is
// therefore a direct proof that the procedural line code is reusable rather than quad-specific.
constexpr const char* GRID_POLYHEDRON_VERTEX_SHADER = R"GLSL(
#version 430 core

#pragma osgx::grid INPUTS

in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;

uniform mat4 osg_ModelViewProjectionMatrix;

out vec2 gridPos;

void main() {
	gridPos = osg_MultiTexCoord0 * u_grid.canvasSize;
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;
}
)GLSL";

constexpr const char* GRID_POLYHEDRON_FRAGMENT_SHADER = R"GLSL(
#version 430 core

#pragma osgx::grid GRID

in vec2 gridPos;

out vec4 fragColor;

void main() {
	fragColor = osgx_GridColor(gridPos);
}
)GLSL";

osg::ref_ptr<osg::Node> makeInfiniteFloor() {
	auto vertices = osgx::make_ref<osg::Vec3Array>();

	// Oversized clip-space triangle covers the whole viewport without the diagonal seam a quad has.
	vertices->push_back(osg::Vec3(-1.0f, -1.0f, 0.0f));
	vertices->push_back(osg::Vec3(3.0f, -1.0f, 0.0f));
	vertices->push_back(osg::Vec3(-1.0f, 3.0f, 0.0f));

	auto floor = osgx::make_ref<osg::Geometry>();
	floor->setVertexArray(vertices);
	floor->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::TRIANGLES, 0, 3));

	osgx::registerProjectionShaderLibs();

	auto program = osgx::make_ref<osg::Program>();
	program->addShader(new osg::Shader(
		osg::Shader::VERTEX, osgx::resolveShaderLibs(INFINITE_FLOOR_VERTEX_SHADER)
	));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, INFINITE_FLOOR_FRAGMENT_SHADER));

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

osg::ref_ptr<osg::Node> makeInfiniteFloorScene() {
	auto root = osgx::make_ref<osg::Group>();
	auto objects = osgx::make_ref<osg::Geode>();

	auto addBox = [&](const osg::Vec3& center, const osg::Vec3& size, const osg::Vec4& color) {
		auto box = osgx::make_ref<osg::ShapeDrawable>(new osg::Box(center, size.x(), size.y(), size.z()));

		box->setColor(color);
		objects->addDrawable(box);
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

	root->addChild(objects);
	root->addChild(makeInfiniteFloor());

	return root;
}

osg::ref_ptr<osg::Node> makeGridDie() {
	// Icosahedron supplies a separate equilateral-triangle UV chart for every face. The grid
	// remains continuous within a face while its deliberate chart seams make every facet legible.
	auto die = osgx::make_ref<osgx::Icosahedron>(osg::Vec3(), 3.0f);
	auto program = osgx::make_ref<osg::Program>();
	auto settings = osgx::make_ref<osgx::GridSettings>();

	program->addShader(new osg::Shader(
		osg::Shader::VERTEX,
		osgx::resolveShaderLibs(GRID_POLYHEDRON_VERTEX_SHADER)
	));
	program->addShader(new osg::Shader(
		osg::Shader::FRAGMENT,
		osgx::resolveShaderLibs(GRID_POLYHEDRON_FRAGMENT_SHADER)
	));

	auto* stateSet = die->getOrCreateStateSet();

	stateSet->setAttributeAndModes(program, osg::StateAttribute::ON);
	stateSet->setAttributeAndModes(settings, osg::StateAttribute::ON);
	settings->setCanvasSize(osg::Vec2(12.0f, 12.0f));
	settings->setGridInterval(2.0f);
	settings->setGridIntervalStrong(6.0f);
	settings->setLineWidthPx(1.0f);
	settings->setLineWidth(0.055f);
	settings->setEdgeMode(osgx::GridSettings::EDGE_ASIS);
	settings->setLineMode(osgx::GridSettings::LINE_GRID_UNITS);
	settings->setColorBg(osg::Vec4(0.025f, 0.040f, 0.070f, 1.0f));
	settings->setColorLine(osg::Vec4(0.20f, 0.42f, 0.74f, 1.0f));
	settings->setColorLineStrong(osg::Vec4(0.72f, 0.90f, 1.0f, 1.0f));
	stateSet->setMode(GL_BLEND, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);

	auto geode = osgx::make_ref<osg::Geode>();

	geode->addDrawable(die);

	return geode;
}

osg::ref_ptr<osg::Node> makeFrameRod(
	const osg::Vec3& start,
	const osg::Vec3& end,
	float radius,
	const osg::Vec4& color
) {
	const osg::Vec3 direction = end - start;
	const float length = direction.length();
	const osg::Vec3 midpoint = (start + end) * 0.5f;

	auto rod = osgx::make_ref<osg::ShapeDrawable>(
		new osg::Cylinder(osg::Vec3(), radius, length)
	);

	rod->setColor(color);

	auto transform = osgx::make_ref<osg::MatrixTransform>();
	osg::Quat rotation;

	rotation.makeRotate(osg::Vec3(0.0f, 0.0f, 1.0f), direction);

	// osg::Cylinder is aligned to local Z.  Rotate it to span the requested edge, then
	// move its center to the midpoint; the spheres below cover the joins cleanly.
	transform->setMatrix(
		osg::Matrix::rotate(rotation) *
		osg::Matrix::translate(midpoint)
	);

	transform->addChild(rod);

	return transform;
}

void addFrameJoint(
	osg::Group* group,
	const osg::Vec3& position,
	float radius,
	const osg::Vec4& color
) {
	auto joint = osgx::make_ref<osg::ShapeDrawable>(new osg::Sphere(position, radius));

	joint->setColor(color);
	group->addChild(joint);
}

void addGridRoom(osg::Group* root) {
	constexpr float halfWidth = 5.0f;
	constexpr float height = 8.0f;
	constexpr float rodRadius = 0.10f;
	const osg::Vec4 frameColor(0.55f, 0.60f, 0.70f, 1.0f);

	auto configureGrid = [](osgx::Grid* grid) {
		grid->setCanvasSize(osg::Vec2(500.0f, 500.0f));
		grid->setGridInterval(50.0f);
		grid->setGridIntervalStrong(250.0f);
		grid->setLineMode(osgx::Grid::LINE_GRID_UNITS);
		grid->setLineWidth(1.0f);
		grid->setEdgeMode(osgx::Grid::EDGE_HIDE);
		grid->setColorBg(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
		grid->setColorLine(osg::Vec4(0.22f, 0.30f, 0.42f, 1.0f));
		grid->setColorLineStrong(osg::Vec4(0.52f, 0.68f, 0.88f, 1.0f));
	};

	// An open Z-up room: the floor, rear wall, and right wall all share their seams exactly.
	// The model belongs around the origin, resting just above the floor at z = 0.
	auto floor = osgx::make_ref<osgx::Grid>(
		osg::Vec3(-halfWidth, -halfWidth, 0.0f),
		osg::Vec3(2.0f * halfWidth, 0.0f, 0.0f),
		osg::Vec3(0.0f, 2.0f * halfWidth, 0.0f)
	);
	auto backWall = osgx::make_ref<osgx::Grid>(
		osg::Vec3(-halfWidth, halfWidth, 0.0f),
		osg::Vec3(2.0f * halfWidth, 0.0f, 0.0f),
		osg::Vec3(0.0f, 0.0f, height)
	);
	auto rightWall = osgx::make_ref<osgx::Grid>(
		osg::Vec3(halfWidth, -halfWidth, 0.0f),
		osg::Vec3(0.0f, 2.0f * halfWidth, 0.0f),
		osg::Vec3(0.0f, 0.0f, height)
	);

	configureGrid(floor);
	configureGrid(backWall);
	configureGrid(rightWall);

	auto panels = osgx::make_ref<osg::Geode>();
	panels->addDrawable(floor);
	panels->addDrawable(backWall);
	panels->addDrawable(rightWall);
	root->addChild(panels);

	auto frame = osgx::make_ref<osg::Group>();
	const osg::Vec3 frontLeft(-halfWidth, -halfWidth, 0.0f);
	const osg::Vec3 frontRight(halfWidth, -halfWidth, 0.0f);
	const osg::Vec3 backLeft(-halfWidth, halfWidth, 0.0f);
	const osg::Vec3 backRight(halfWidth, halfWidth, 0.0f);
	const osg::Vec3 backLeftTop(-halfWidth, halfWidth, height);
	const osg::Vec3 backRightTop(halfWidth, halfWidth, height);
	const osg::Vec3 frontRightTop(halfWidth, -halfWidth, height);

	// Nine unique edges: six outside edges plus the three seams shared by two panels.
	for(const auto& [start, end] : std::initializer_list<std::pair<osg::Vec3, osg::Vec3>>{
		{frontLeft, frontRight}, {frontLeft, backLeft},
		{backLeft, backRight}, {frontRight, backRight}, {backRight, backRightTop},
		{backLeft, backLeftTop}, {backLeftTop, backRightTop},
		{frontRight, frontRightTop}, {frontRightTop, backRightTop}
	}) frame->addChild(makeFrameRod(start, end, rodRadius, frameColor));

	for(const auto& position : {
		frontLeft, frontRight, backLeft, backRight,
		backLeftTop, backRightTop, frontRightTop
	}) addFrameJoint(frame, position, rodRadius * 1.25f, frameColor);

	root->addChild(frame);

}

}

int main(int argc, char** argv) {
	bool orthoMode = argc > 1 && std::string(argv[1]) == "ortho";
	bool floorMode = argc > 1 && std::string(argv[1]) == "floor";
	bool diceMode = argc > 1 && std::string(argv[1]) == "dice";

	osg::ref_ptr<osg::Node> root;

	if(orthoMode) {
		// Fullscreen NDC quad, always drawn first via a PRE_RENDER camera. Configure the Grid
		// itself before wrapping it, since createOrthoCamera() would only hand back the camera.
		auto grid = osgx::make_ref<osgx::Grid>();

		grid->setCanvasSize(osg::Vec2(800.0f, 600.0f));
		grid->setGridInterval(10.0f);
		grid->setGridIntervalStrong(100.0f);
		grid->setLineWidthPx(1.0f);
		grid->setEdgeMode(osgx::Grid::EDGE_NUDGE);

		root = grid->orthoCamera();
	}

	else if(floorMode) root = makeInfiniteFloorScene();

	else if(diceMode) root = makeGridDie();

	else {
		auto room = osgx::make_ref<osg::Group>();

		addGridRoom(room);
		root = room;
	}

	osgViewer::Viewer viewer;

	viewer.setSceneData(root);
	viewer.getCamera()->setClearColor(osg::Vec4(0.015f, 0.020f, 0.035f, 1.0f));

	if(floorMode || diceMode) {
		auto manipulator = new osgGA::TrackballManipulator();

		manipulator->setHomePosition(
			diceMode ? osg::Vec3d(9.0, -11.0, 7.0) : osg::Vec3d(12.0, -16.0, 10.0),
			osg::Vec3d(0.0, 0.0, 0.0),
			osg::Vec3d(0.0, 0.0, 1.0)
		);

		viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
		viewer.setCameraManipulator(manipulator);
		viewer.home();
	}

	// The ortho camera's own clear IS the frame's first paint; stop the viewer's main
	// camera from immediately stomping it with a second COLOR_BUFFER_BIT clear.
	if(orthoMode) viewer.getCamera()->setClearMask(GL_DEPTH_BUFFER_BIT);

	auto r = viewer.run();

	return r;
}
