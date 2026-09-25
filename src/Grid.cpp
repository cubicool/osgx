#include "osgx/Grid.hpp"
#include "osgx/Array.hpp"
#include "osgx/Shader.hpp"

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/State>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace osgx {

namespace {

// Vertex shader hands the fragment shader a position in "grid space": UV (0..1) scaled by the
// logical canvas size. That keeps the grid's notion of "5 units apart" tied to the canvas the
// caller specified, independent of how big the quad actually is in NDC/world.
constexpr const char* GRID_VERTEX_SHADER = R"GLSL(
#version 430 core

#pragma osgx::grid INPUTS

uniform mat4 osg_ModelViewProjectionMatrix;

in vec4 osg_Vertex;
in vec2 osg_MultiTexCoord0;

out vec2 gridPos;

void main(void) {
	gl_Position = osg_ModelViewProjectionMatrix * osg_Vertex;

	gridPos = osg_MultiTexCoord0 * u_grid.canvasSize;
}
)GLSL";

constexpr const char* GRID_SHADER_INPUTS = R"GLSL(
struct osgx_GridSettings {
	vec2 canvasSize;
	float gridInterval;
	float gridIntervalStrong;
	float lineWidthPx;
	float lineWidth;
	int edgeMode;
	int lineMode;
	vec4 colorBg;
	vec4 colorLine;
	vec4 colorLineStrong;
};

layout(std430, binding = @osgx::grid@) readonly buffer osgx_GridSettingsBuffer {
	osgx_GridSettings u_grid;
};
)GLSL";

constexpr const char* GRID_SHADER_LIBRARY = R"GLSL(

struct osgx_GridSettings {
	vec2 canvasSize;
	float gridInterval;
	float gridIntervalStrong;
	float lineWidthPx;
	float lineWidth;
	int edgeMode;
	int lineMode;
	vec4 colorBg;
	vec4 colorLine;
	vec4 colorLineStrong;
};

layout(std430, binding = @osgx::grid@) readonly buffer osgx_GridSettingsBuffer {
	osgx_GridSettings u_grid;
};

// Lines that fall exactly on the canvas boundary (pos == 0 or pos == u_grid.canvasSize) only have
// geometry on one side, so their AA kernel is half-clipped and they render at roughly half
// intensity/width - u_grid.edgeMode picks how to handle that:
//   0 EDGE_ASIS  - leave the half-clipped line as rendered (matches raw geometry)
//   1 EDGE_HIDE  - don't draw boundary lines at all (clean edge, good for screen-aligned UI)
//   2 EDGE_NUDGE - shift boundary lines inward by half their width so they render at full
//                   strength (good for a true 3D ground plane, where there's no "UI edge")
const int EDGE_ASIS = 0;
const int EDGE_HIDE = 1;
const int EDGE_NUDGE = 2;

// Antialiased coverage (0..1) of gridlines spaced `interval` apart in `pos`'s units, with the
// line drawn `lineWidthPx` screen-pixels wide regardless of view distance/angle.
float osgx_GridLine(vec2 pos, float interval, float lineWidthPx) {
	vec2 coord = pos / interval;
	vec2 deriv = fwidth(coord);

	// Guards against NaN/Inf when the derivative collapses to zero (e.g. the quad edge-on
	// to the view, or an interval of 0 from a not-yet-initialized uniform).
	deriv = max(deriv, vec2(1e-6));

	// Which line (in units of `interval`) is nearest each axis, and is that line one of the
	// two canvas-boundary lines? Computed from the raw (pre-AA-bias, pre-nudge) coordinate so
	// it reflects true geometry, independent of the pixel-alignment tricks below.
	vec2 maxCoord = u_grid.canvasSize / interval;
	vec2 nearest = floor(coord + 0.5);
	bvec2 isMinEdge = lessThan(abs(nearest), vec2(0.5));
	bvec2 isMaxEdge = lessThan(abs(nearest - maxCoord), vec2(0.5));

	// The Cairo/Direct2D "+0.5" hairline trick: a mathematical line that lands exactly on a
	// pixel BOUNDARY gets its coverage split 50/50 across the two neighboring pixels instead
	// of drawn crisply into one, roughly doubling its apparent width. Biasing by half a screen
	// pixel re-centers that case onto a single pixel; harmless elsewhere since it's a constant
	// sub-pixel nudge of the whole grid pattern. Default bias for every non-edge fragment.
	vec2 bias = deriv * 0.5;

	// For edge lines under EDGE_NUDGE, REPLACE (not add to) the resonance bias above with the
	// nudge-inward amount - stacking both cancels to zero on min edges (line stays on the
	// boundary, still half-clipped) and doubles up on max edges (shifts a full pixel in), which
	// is exactly the bottom/left-vs-top/right asymmetry this branch is here to avoid.
	if(u_grid.edgeMode == EDGE_NUDGE) {
		vec2 halfWidthCoord = deriv * max(lineWidthPx, 0.0) * 0.5;

		if(isMinEdge.x) bias.x = -halfWidthCoord.x;
		else if(isMaxEdge.x) bias.x = halfWidthCoord.x;

		if(isMinEdge.y) bias.y = -halfWidthCoord.y;
		else if(isMaxEdge.y) bias.y = halfWidthCoord.y;
	}

	coord += bias;

	vec2 dist = abs(fract(coord + 0.5) - 0.5); // distance to nearest line, in grid cells
	vec2 distPx = dist / deriv; // same distance, converted to actual screen pixels
	vec2 halfWidth = vec2(max(lineWidthPx, 0.0) * 0.5);
	vec2 line = 1.0 - smoothstep(halfWidth, halfWidth + 1.0, distPx);

	if(u_grid.edgeMode == EDGE_HIDE) {
		if(isMinEdge.x || isMaxEdge.x) line.x = 0.0;
		if(isMinEdge.y || isMaxEdge.y) line.y = 0.0;
	}

	return clamp(max(line.x, line.y), 0.0, 1.0);
}

// Adapted from Ben Golus' "Pristine Grid" technique:
// https://bgolus.medium.com/the-best-darn-grid-shader-yet-727f9278b9d8
// `lineWidth` is in the same units as `pos`/`interval`, so perspective naturally makes lines
// thinner; once they approach sub-pixel size, they are widened in derivative space and faded by
// coverage.
float osgx_PristineGridLine(vec2 pos, float interval, float lineWidth) {
	vec2 uv = pos / interval;
	vec2 uvDDX = dFdx(uv);
	vec2 uvDDY = dFdy(uv);
	vec2 uvDeriv = max(vec2(length(vec2(uvDDX.x, uvDDY.x)), length(vec2(uvDDX.y, uvDDY.y))), vec2(1e-6));

	vec2 lineWidthCell = clamp(vec2(lineWidth / interval), vec2(0.0), vec2(1.0));
	bvec2 invertLine = greaterThan(lineWidthCell, vec2(0.5));
	vec2 targetWidth = mix(lineWidthCell, vec2(1.0) - lineWidthCell, invertLine);
	vec2 drawWidth = clamp(targetWidth, uvDeriv, vec2(0.5));
	vec2 lineAA = uvDeriv * 1.5;
	vec2 gridUV = abs(fract(uv) * 2.0 - 1.0);

	gridUV = mix(vec2(1.0) - gridUV, gridUV, invertLine);

	vec2 grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, gridUV);

	grid2 *= clamp(targetWidth / drawWidth, vec2(0.0), vec2(1.0));
	grid2 = mix(grid2, targetWidth, clamp(uvDeriv * 2.0 - 1.0, vec2(0.0), vec2(1.0)));
	grid2 = mix(grid2, vec2(1.0) - grid2, invertLine);

	if(u_grid.edgeMode == EDGE_HIDE) {
		vec2 maxCoord = u_grid.canvasSize / interval;
		vec2 nearest = floor(uv + 0.5);
		bvec2 isMinEdge = lessThan(abs(nearest), vec2(0.5));
		bvec2 isMaxEdge = lessThan(abs(nearest - maxCoord), vec2(0.5));

		if(isMinEdge.x || isMaxEdge.x) grid2.x = 0.0;
		if(isMinEdge.y || isMaxEdge.y) grid2.y = 0.0;
	}

	return clamp(grid2.x * (1.0 - grid2.y) + grid2.y, 0.0, 1.0);
}

vec4 osgx_GridColor(vec2 gridPos) {
	vec4 color = u_grid.colorBg;

	float thin = u_grid.lineMode == 0 ?
		osgx_GridLine(gridPos, u_grid.gridInterval, u_grid.lineWidthPx) :
		osgx_PristineGridLine(gridPos, u_grid.gridInterval, u_grid.lineWidth);

	color = mix(color, u_grid.colorLine, thin);

	if(u_grid.gridIntervalStrong > 0.0) {
		float strong = u_grid.lineMode == 0 ?
			osgx_GridLine(gridPos, u_grid.gridIntervalStrong, u_grid.lineWidthPx * 1.5) :
			osgx_PristineGridLine(gridPos, u_grid.gridIntervalStrong, u_grid.lineWidth * 1.5);

		color = mix(color, u_grid.colorLineStrong, strong);
	}

	return color;
}
)GLSL";

constexpr const char* GRID_FRAGMENT_SHADER = R"GLSL(
#version 430 core

#pragma osgx::grid GRID

in vec2 gridPos;

out vec4 fragColor;

void main(void) {
	fragColor = osgx_GridColor(gridPos);
}
)GLSL";

}

void registerGridShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"INPUTS", "osgx_GridSettings", GRID_SHADER_INPUTS},
		{"GRID", "osgx_GridColor", GRID_SHADER_LIBRARY}
	};

	::osgx::registerShaderLibs("osgx::grid", libs);
}

namespace {

constexpr std::size_t CANVAS_SIZE_OFFSET = 0;
constexpr std::size_t GRID_INTERVAL_OFFSET = 2;
constexpr std::size_t GRID_INTERVAL_STRONG_OFFSET = 3;
constexpr std::size_t LINE_WIDTH_PX_OFFSET = 4;
constexpr std::size_t LINE_WIDTH_OFFSET = 5;
constexpr std::size_t EDGE_MODE_OFFSET = 6;
constexpr std::size_t LINE_MODE_OFFSET = 7;
constexpr std::size_t COLOR_BG_OFFSET = 8;
constexpr std::size_t COLOR_LINE_OFFSET = 12;
constexpr std::size_t COLOR_LINE_STRONG_OFFSET = 16;
constexpr std::size_t GRID_SETTINGS_FLOATS = 20;

float intBitsToFloat(int value) { return std::bit_cast<float>(value); }
int floatBitsToInt(float value) { return std::bit_cast<int>(value); }

osg::Vec4 getVec4(const float* data, std::size_t offset) {
	return osg::Vec4(data[offset], data[offset + 1], data[offset + 2], data[offset + 3]);
}

}

GridSettings::GridSettings() {
	registerGridShaderLibs();
	_initBuffer();
}

GridSettings::GridSettings(const GridSettings& settings, const osg::CopyOp& copyop):
osg::StateAttribute(settings, copyop),
_buffer(static_cast<osgx::FloatArray*>(copyop(settings._buffer.get()))) {
	if(!_buffer) throw std::logic_error("GridSettings copy has no backing buffer");

	// Index 0 until apply() resolves the "osgx::grid" slot.
	_binding = new osg::ShaderStorageBufferBinding(
		0, _buffer, 0, static_cast<GLsizeiptr>(_buffer->getTotalDataSize())
	);
}

GridSettings::~GridSettings() {}

void GridSettings::_initBuffer() {
	_buffer = new osgx::FloatArray(static_cast<std::size_t>(GRID_SETTINGS_FLOATS));
	std::fill(_buffer->begin(), _buffer->end(), 0.0f);
	_buffer->setBufferObject(new osg::ShaderStorageBufferObject());
	// Index 0 until apply() resolves the "osgx::grid" slot.
	_binding = new osg::ShaderStorageBufferBinding(
		0, _buffer, 0, static_cast<GLsizeiptr>(_buffer->getTotalDataSize())
	);

	setCanvasSize(osg::Vec2(300.0f, 300.0f));
	setGridInterval(5.0f);
	setGridIntervalStrong(10.0f);
	setLineWidthPx(1.0f);
	setLineWidth(0.5f);
	setEdgeMode(EDGE_ASIS);
	setLineMode(LINE_SCREEN_PIXELS);
	setColorBg(osg::Vec4(0.10f, 0.10f, 0.12f, 0.0f));
	setColorLine(osg::Vec4(0.45f, 0.45f, 0.50f, 1.0f));
	setColorLineStrong(osg::Vec4(0.85f, 0.85f, 0.90f, 1.0f));
}

float* GridSettings::_data() const {
	if(!_buffer || !_binding) throw std::logic_error("GridSettings is invalid");

	return &(*_buffer)[0];
}

int GridSettings::compare(const osg::StateAttribute& sa) const {
	COMPARE_StateAttribute_Types(GridSettings, sa)
	COMPARE_StateAttribute_Parameter(_buffer)

	return 0;
}

void GridSettings::apply(osg::State& state) const {
	resolveBinding(_bindingResolved, _binding.get(), "osgx::grid");

	state.applyAttribute(_binding.get());
}

void GridSettings::setCanvasSize(const osg::Vec2& value) {
	_buffer->set({value.x(), value.y()}, CANVAS_SIZE_OFFSET);
	_buffer->dirty();
}

osg::Vec2 GridSettings::getCanvasSize() const {
	const auto* data = _data();

	return osg::Vec2(data[CANVAS_SIZE_OFFSET], data[CANVAS_SIZE_OFFSET + 1]);
}

void GridSettings::setGridInterval(float value) {
	_data()[GRID_INTERVAL_OFFSET] = value;
	_buffer->dirty();
}

float GridSettings::getGridInterval() const { return _data()[GRID_INTERVAL_OFFSET]; }

void GridSettings::setGridIntervalStrong(float value) {
	_data()[GRID_INTERVAL_STRONG_OFFSET] = value;
	_buffer->dirty();
}

float GridSettings::getGridIntervalStrong() const { return _data()[GRID_INTERVAL_STRONG_OFFSET]; }

void GridSettings::setLineWidthPx(float value) {
	_data()[LINE_WIDTH_PX_OFFSET] = value;
	_buffer->dirty();
}

float GridSettings::getLineWidthPx() const { return _data()[LINE_WIDTH_PX_OFFSET]; }

void GridSettings::setLineWidth(float value) {
	_data()[LINE_WIDTH_OFFSET] = value;
	_buffer->dirty();
}

float GridSettings::getLineWidth() const { return _data()[LINE_WIDTH_OFFSET]; }

void GridSettings::setEdgeMode(EdgeMode value) {
	_data()[EDGE_MODE_OFFSET] = intBitsToFloat(static_cast<int>(value));
	_buffer->dirty();
}

GridSettings::EdgeMode GridSettings::getEdgeMode() const { return static_cast<EdgeMode>(floatBitsToInt(_data()[EDGE_MODE_OFFSET])); }

void GridSettings::setLineMode(LineMode value) {
	_data()[LINE_MODE_OFFSET] = intBitsToFloat(static_cast<int>(value));
	_buffer->dirty();
}

GridSettings::LineMode GridSettings::getLineMode() const { return static_cast<LineMode>(floatBitsToInt(_data()[LINE_MODE_OFFSET])); }

void GridSettings::setColorBg(const osg::Vec4& value) {
	_buffer->set({value.r(), value.g(), value.b(), value.a()}, COLOR_BG_OFFSET);
	_buffer->dirty();
}

osg::Vec4 GridSettings::getColorBg() const { return getVec4(_data(), COLOR_BG_OFFSET); }

void GridSettings::setColorLine(const osg::Vec4& value) {
	_buffer->set({value.r(), value.g(), value.b(), value.a()}, COLOR_LINE_OFFSET);
	_buffer->dirty();
}

osg::Vec4 GridSettings::getColorLine() const { return getVec4(_data(), COLOR_LINE_OFFSET); }

void GridSettings::setColorLineStrong(const osg::Vec4& value) {
	_buffer->set({value.r(), value.g(), value.b(), value.a()}, COLOR_LINE_STRONG_OFFSET);
	_buffer->dirty();
}

osg::Vec4 GridSettings::getColorLineStrong() const { return getVec4(_data(), COLOR_LINE_STRONG_OFFSET); }

void Grid::_build(const osg::Vec3& corner, const osg::Vec3& widthVec, const osg::Vec3& heightVec) {
	auto vertices = make_ref<Vec3Array>();

	vertices->push_back(corner);
	vertices->push_back(corner + widthVec);
	vertices->push_back(corner + widthVec + heightVec);
	vertices->push_back(corner + heightVec);

	auto texCoords = make_ref<Vec2Array>();

	texCoords->push_back(osg::Vec2(0.0f, 0.0f));
	texCoords->push_back(osg::Vec2(1.0f, 0.0f));
	texCoords->push_back(osg::Vec2(1.0f, 1.0f));
	texCoords->push_back(osg::Vec2(0.0f, 1.0f));

	auto normals = make_ref<Vec3Array>();
	auto normal = widthVec ^ heightVec;

	normal.normalize();
	normals->push_back(normal);

	setVertexArray(vertices);
	setTexCoordArray(0, texCoords);
	setNormalArray(normals, osg::Array::BIND_OVERALL);
	// GL_QUADS is invalid in a core-profile context (including the 4.6 core
	// profile used by pyosg-lighting). A four-vertex triangle fan describes the
	// same rectangle without falling back to that removed legacy primitive.
	addPrimitiveSet(new DrawArrays(osg::PrimitiveSet::TRIANGLE_FAN, 0, 4));

	_installState();
}

osg::ref_ptr<Grid> Grid::createSphere(float radius, unsigned int slices, unsigned int stacks) {
	auto grid = make_ref<Grid>();

	grid->_buildSphere(radius, slices, stacks);

	return grid;
}

void Grid::_buildSphere(float radius, unsigned int slices, unsigned int stacks) {
	if(radius <= 0.0f) throw std::invalid_argument("Grid sphere radius must be positive");
	if(slices < 3) throw std::invalid_argument("Grid sphere needs at least three slices");
	if(stacks < 2) throw std::invalid_argument("Grid sphere needs at least two stacks");

	auto vertices = make_ref<Vec3Array>();
	auto texCoords = make_ref<Vec2Array>();
	auto normals = make_ref<Vec3Array>();
	auto elements = make_ref<DrawElementsUInt>(osg::PrimitiveSet::TRIANGLES);

	vertices->reserve(static_cast<std::size_t>(slices + 1) * (stacks + 1));
	texCoords->reserve(vertices->capacity());
	normals->reserve(vertices->capacity());
	elements->reserve(static_cast<std::size_t>(slices) * stacks * 6);

	for(unsigned int stack = 0; stack <= stacks; stack++) {
		const auto v = static_cast<float>(stack) / static_cast<float>(stacks);
		const auto phi = v * static_cast<float>(osg::PI);
		const auto sinPhi = std::sin(phi);
		const auto cosPhi = std::cos(phi);

		for(unsigned int slice = 0; slice <= slices; slice++) {
			const auto u = static_cast<float>(slice) / static_cast<float>(slices);
			const auto theta = u * static_cast<float>(2.0 * osg::PI);
			const osg::Vec3 normal(
				sinPhi * std::cos(theta),
				sinPhi * std::sin(theta),
				cosPhi
			);

			vertices->push_back(normal * radius);
			texCoords->push_back(osg::Vec2(u, v));
			normals->push_back(normal);
		}
	}

	for(unsigned int stack = 0; stack < stacks; stack++) {
		for(unsigned int slice = 0; slice < slices; slice++) {
			const auto a = stack * (slices + 1) + slice;
			const auto b = a + 1;
			const auto c = a + slices + 1;
			const auto d = c + 1;

			elements->append(a, c, b, b, c, d);
		}
	}

	setVertexArray(vertices);
	setTexCoordArray(0, texCoords);
	setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
	removePrimitiveSet(0, getNumPrimitiveSets());
	addPrimitiveSet(elements);
	dirtyDisplayList();
	dirtyBound();
}

void Grid::_installState() {
	auto* ss = getOrCreateStateSet();
	auto program = make_ref<osg::Program>();
	auto settings = make_ref<GridSettings>();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, resolveShaderLibs(GRID_VERTEX_SHADER)));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, resolveShaderLibs(GRID_FRAGMENT_SHADER)));

	ss->setAttributeAndModes(program, osg::StateAttribute::ON);
	setSettings(settings);
	ss->setMode(GL_BLEND, osg::StateAttribute::ON);
	// Enabling GL_BLEND alone leaves GL's default blend func (ONE, ZERO), which just
	// overwrites the destination regardless of alpha; SRC_ALPHA/ONE_MINUS_SRC_ALPHA is what
	// actually makes the zero-alpha background pixels transparent.
	ss->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
		osg::StateAttribute::ON
	);
}

void Grid::setSettings(GridSettings* settings) {
	if(!settings) throw std::invalid_argument("Grid settings cannot be null");

	getOrCreateStateSet()->setAttributeAndModes(settings, osg::StateAttribute::ON);
	_settings = settings;
}

void Grid::_bindSettings() {
	auto* settings = dynamic_cast<GridSettings*>(getOrCreateStateSet()->getAttribute(
		GridSettings::GRID_SETTINGS_TYPE,
		GridSettings::GRID_SETTINGS_MEMBER
	));

	if(!settings) throw std::logic_error("Grid StateSet has no GridSettings");

	_settings = settings;
}

osg::ref_ptr<osg::Camera> Grid::orthoCamera() {
	auto geode = make_ref<osg::Geode>();

	geode->addDrawable(this);

	auto camera = make_ref<osg::Camera>();

	camera->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
	camera->setRenderOrder(osg::Camera::PRE_RENDER);
	camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	camera->setClearColor(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
	camera->setProjectionMatrix(osg::Matrix::ortho2D(-1.0, 1.0, -1.0, 1.0));
	camera->setViewMatrix(osg::Matrix::identity());
	camera->setAllowEventFocus(false);
	camera->setCullingActive(false);

	camera->addChild(geode);

	return camera;
}

}
