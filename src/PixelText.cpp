#include "LibraryState.hpp"

#include "osgx/PixelText.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace osgx {

namespace {

// One glyph, one string per row (top to bottom), '#' = ink / '.' = empty - same convention as
// pyosg_dice.py's FONT_5X7, extended from digits-only to PixelText::CHARSET, and from 7 to 9
// rows to make room for a 2-row descender band (rows 7-8) below the row-6 baseline. Digit shapes
// (rows 0-6) are copied verbatim from FONT_5X7 so an eventual dice switch-over renders
// pixel-identically. Uppercase/digits/most punctuation never touch the descender band - their
// last two rows are always blank. Lowercase ascenders (b,d,f,h,k,l,t) reuse the full rows 0-6
// cap-height box; lowercase x-height letters (a,c,e,m,n,o,r,s,u,v,w,x,z) and 'i' sit in rows
// 2-6; lowercase descenders (g,j,p,q,y) and a few punctuation marks (,;_|) are the only glyphs
// that put ink in rows 7-8.
using Glyph = std::array<std::string_view, static_cast<std::size_t>(PixelText::GLYPH_ROWS)>;

const std::unordered_map<char, Glyph>& glyphTable() {
	static const std::unordered_map<char, Glyph> table = {
		{' ', {".....", ".....", ".....", ".....", ".....", ".....", ".....", ".....", "....."}},

		{'A', {"..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#", ".....", "....."}},
		{'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", ".....", "....."}},
		{'C', {".####", "#....", "#....", "#....", "#....", "#....", ".####", ".....", "....."}},
		{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####.", ".....", "....."}},
		{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####", ".....", "....."}},
		{'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#....", ".....", "....."}},
		{'G', {".####", "#....", "#....", "#.###", "#...#", "#...#", ".####", ".....", "....."}},
		{'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", ".....", "....."}},
		{'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####", ".....", "....."}},
		{'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", ".....", "....."}},
		{'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", ".....", "....."}},
		{'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####", ".....", "....."}},
		{'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#", ".....", "....."}},
		{'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#", ".....", "....."}},
		{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."}},
		{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", ".....", "....."}},
		{'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", ".....", "....."}},
		{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", ".....", "....."}},
		{'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####.", ".....", "....."}},
		{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "....."}},
		{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."}},
		{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", ".....", "....."}},
		{'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#", ".....", "....."}},
		{'X', {"#...#", ".#.#.", "..#..", "..#..", "..#..", ".#.#.", "#...#", ".....", "....."}},
		{'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", ".....", "....."}},
		{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", ".....", "....."}},

		// Lowercase - x-height letters occupy rows 2-6, ascenders 0-6, descenders 2-8.
		{'a', {".....", ".....", ".###.", "....#", ".####", "#...#", ".####", ".....", "....."}},
		{'b', {"#....", "#....", "#....", "####.", "#...#", "#...#", "####.", ".....", "....."}},
		{'c', {".....", ".....", ".####", "#....", "#....", "#....", ".####", ".....", "....."}},
		{'d', {"....#", "....#", "....#", ".####", "#...#", "#...#", ".####", ".....", "....."}},
		{'e', {".....", ".....", ".###.", "#...#", "#####", "#....", ".###.", ".....", "....."}},
		{'f', {"..##.", ".#...", "####.", ".#...", ".#...", ".#...", ".#...", ".....", "....."}},
		{'g', {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#", "####."}},
		{'h', {"#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#", ".....", "....."}},
		{'i', {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.", ".....", "....."}},
		{'j', {"...#.", ".....", "..##.", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
		{'k', {"#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", ".....", "....."}},
		{'l', {".#...", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", ".....", "....."}},
		{'m', {".....", ".....", "##.##", "#.#.#", "#.#.#", "#.#.#", "#.#.#", ".....", "....."}},
		{'n', {".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#", ".....", "....."}},
		{'o', {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.", ".....", "....."}},
		{'p', {".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
		{'q', {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#", "....#"}},
		{'r', {".....", ".....", "####.", "#....", "#....", "#....", "#....", ".....", "....."}},
		{'s', {".....", ".....", ".####", ".#...", "..##.", "...#.", "####.", ".....", "....."}},
		{'t', {".....", "..#..", "####.", "..#..", "..#..", "..#..", "...##", ".....", "....."}},
		{'u', {".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####", ".....", "....."}},
		{'v', {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", ".....", "....."}},
		{'w', {".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", ".....", "....."}},
		{'x', {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", ".....", "....."}},
		{'y', {".....", ".....", "#...#", "#...#", ".#.#.", "..#..", "..#..", ".#...", "#...."}},
		{'z', {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####", ".....", "....."}},

		// Digit shapes (rows 0-6) copied verbatim from pyosg_dice.py's FONT_5X7.
		{'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", ".....", "....."}},
		{'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", ".....", "....."}},
		{'3', {"####.", "....#", "...#.", "..##.", "....#", "....#", "####.", ".....", "....."}},
		{'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", ".....", "....."}},
		{'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", ".....", "....."}},
		{'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", ".....", "....."}},
		{'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", ".....", "....."}},
		{'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", ".....", "....."}},
		{'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", ".....", "....."}},
		{'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", ".....", "....."}},

		{'.', {".....", ".....", ".....", ".....", ".....", "..##.", "..##.", ".....", "....."}},
		// Real descender - the tail dips into rows 7-8, below the baseline, instead of the old
		// (pre-9-row) font's approximation of tucking it into row 6 itself.
		{',', {".....", ".....", ".....", ".....", ".....", "..##.", "..##.", ".##..", "#...."}},
		{':', {".....", "..##.", "..##.", ".....", "..##.", "..##.", ".....", ".....", "....."}},
		{';', {".....", "..##.", "..##.", ".....", "..##.", "..##.", ".....", ".##..", "#...."}},
		{'-', {".....", ".....", ".....", "#####", ".....", ".....", ".....", ".....", "....."}},
		// Genuinely below the baseline now (row 7), not sitting on it (the old row-6 placement).
		{'_', {".....", ".....", ".....", ".....", ".....", ".....", ".....", "#####", "....."}},
		{'/', {"....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#....", ".....", "....."}},
		{'\\', {"#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#", ".....", "....."}},
		{'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..", ".....", "....."}},
		{'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", ".....", "....."}},
		{'\"', {".#.#.", ".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....", "....."}},
		{'#', {".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", ".#.#.", ".....", "....."}},
		{'$', {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", ".....", "....."}},
		{'%', {"##..#", "##.#.", "..#..", ".#...", ".#.##", "#..##", ".....", ".....", "....."}},
		{'&', {".##..", "#..#.", ".##..", ".#.#.", "#..#.", "#...#", ".###.", ".....", "....."}},
		{'\'', {"..#..", "..#..", ".#...", ".....", ".....", ".....", ".....", ".....", "....."}},
		{'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", ".....", "....."}},
		{')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", ".....", "....."}},
		{'*', {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", ".....", ".....", "....."}},
		{'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", ".....", "....."}},
		{'=', {".....", "#####", ".....", "#####", ".....", ".....", ".....", ".....", "....."}},
		{'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", ".....", "....."}},
		{'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", ".....", "....."}},
		{'@', {".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####", ".....", "....."}},
		{'[', {".####", ".#...", ".#...", ".#...", ".#...", ".#...", ".####", ".....", "....."}},
		{']', {"####.", "...#.", "...#.", "...#.", "...#.", "...#.", "####.", ".....", "....."}},
		{'^', {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", ".....", "....."}},
		{'`', {".#...", "..#..", ".....", ".....", ".....", ".....", ".....", ".....", "....."}},
		{'{', {"...##", "..#..", "..#..", ".#...", "..#..", "..#..", "...##", ".....", "....."}},
		// Extended through the descender band too - a vertical bar reads better spanning the
		// full glyph height than stopping dead at the baseline like every other glyph here.
		{'|', {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
		{'}', {"##...", "..#..", "..#..", "...#.", "..#..", "..#..", "##...", ".....", "....."}},
		{'~', {".....", ".....", ".##..", "#..#.", "...##", ".....", ".....", ".....", "....."}},
	};

	return table;
}

// Passes `c` through unchanged if it's a supported (case-sensitive) character, falling back to
// space otherwise - the forgiving policy PixelText::setText() wants. createAtlas() validates
// (and throws) separately, before any character reaches this.
char normalizeChar(char c) {
	return PixelText::CHARSET.find(c) != std::string_view::npos ? c : ' ';
}

// Stamps one glyph's coverage into `pixels` (a `width`-wide, top-down GL_RED byte buffer: row 0
// is the glyph's top row), at pixel offset (x0, y0), scaled by `pixelScale`. The caller handles
// whatever y-flip its destination osg::Image (bottom-up) convention needs.
void stampGlyph(
	std::vector<unsigned char>& pixels,
	int width,
	int height,
	int x0,
	int y0,
	char c,
	int pixelScale
) {
	auto it = glyphTable().find(normalizeChar(c));

	if(it == glyphTable().end()) return;

	const Glyph& glyph = it->second;

	for(int row = 0; row < PixelText::GLYPH_ROWS; row++) {
		std::string_view line = glyph[static_cast<std::size_t>(row)];

		for(int col = 0; col < PixelText::GLYPH_COLS; col++) {
			if(line[static_cast<std::size_t>(col)] != '#') continue;

			for(int py = 0; py < pixelScale; py++) {
				int y = y0 + row * pixelScale + py;

				if(y < 0 || y >= height) continue;

				for(int px = 0; px < pixelScale; px++) {
					int x = x0 + col * pixelScale + px;

					if(x < 0 || x >= width) continue;

					pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 255;
				}
			}
		}
	}
}

// The tightest [minRow, maxRow] (inclusive) band of rows that actually carry ink across every
// character in `value` - the shared row range createAtlas() centers a cell's block against when
// verticallyCentered is true, so e.g. an all-digit cell (ink only in rows 0-6) centers exactly
// like the pre-descender-band 7-row font did, while a cell mixing ascenders/descenders (e.g.
// "Aq") still gives every character in it one shared baseline instead of each centering on its
// own. Falls back to the full glyph height if `value` is empty or every character in it is blank
// (e.g. an all-space cell) - nothing gets drawn either way, so the exact fallback range is moot,
// but minRow must not end up greater than maxRow.
std::pair<int, int> inkRowExtent(const std::string& value) {
	int minRow = PixelText::GLYPH_ROWS;
	int maxRow = -1;

	for(char c : value) {
		auto it = glyphTable().find(normalizeChar(c));

		if(it == glyphTable().end()) continue;

		const Glyph& glyph = it->second;

		for(int row = 0; row < PixelText::GLYPH_ROWS; row++) {
			if(glyph[static_cast<std::size_t>(row)].find('#') == std::string_view::npos) continue;

			minRow = std::min(minRow, row);
			maxRow = std::max(maxRow, row);
		}
	}

	if(maxRow < minRow) return {0, PixelText::GLYPH_ROWS - 1};

	return {minRow, maxRow};
}

// One glyph per cell, in PixelText::CHARSET order - the whole-charset atlas
// PixelText::_atlasTexture() wraps. Private: PixelText::createAtlas() (arbitrary cell lists,
// for decal-style use) is the one public "give me an atlas" entry point; nothing outside this
// file has ever needed a raw one-glyph-per-cell atlas of the whole charset directly.
osg::ref_ptr<osg::Image> createCharsetAtlas(int pixelScale=7) {
	std::vector<std::string> cells;

	cells.reserve(PixelText::CHARSET.size());

	for(char c : PixelText::CHARSET) cells.emplace_back(1, c);

	// PixelText::createAtlas() treats cellSize as BOTH the per-cell width and the whole atlas
	// height (square cells), so it must be sized off GLYPH_ROWS (9), not GLYPH_COLS (5) - a
	// glyph block is only GLYPH_COLS * pixelScale wide but GLYPH_ROWS * pixelScale tall, and
	// using the narrower dimension here would silently clip every glyph's top and bottom row.
	// multiPixelScale never comes into play regardless (every cell has exactly one character).
	// verticallyCentered=false: every glyph must sit on the same shared baseline across the whole
	// charset - see createAtlas()'s docstring in PixelText.hpp.
	return PixelText::createAtlas(
		cells, PixelText::GLYPH_ROWS * pixelScale, pixelScale, pixelScale, false
	);
}

// The glyph index SSBO is the "osgx::pixelText" slot (osgx::Bindings).
//
// No osg_Vertex/osg_MultiTexCoord0 (and correspondingly, PixelText::_build() binds no
// osg::Vec3Array/Vec2Array at all): the unit quad's four corners are procedural, indexed by
// gl_VertexID % 4 - the same "no vertex buffer, derive everything from gl_VertexID/
// gl_InstanceID" technique OpenSceneGraph.py/examples/pyosg-instanced-ssbo.py already
// demonstrates. `corner` doubles as both local position AND atlas UV without a second lookup,
// since the quad's local XY and its 0..1 UV are numerically identical either way.
constexpr const char* PIXEL_TEXT_VERTEX_SHADER = R"GLSL(
#version 460 core

uniform mat4 osg_ModelViewProjectionMatrix;
uniform float u_cellSize;
uniform float u_advance;
uniform int u_charsetCount;

layout(std430, binding = @osgx::pixelText@) readonly buffer osgx_PixelTextGlyphs {
	int osgx_glyphIndices[];
};

out vec2 vAtlasUV;

void main(void) {
	const vec2 corners[4] = vec2[4](
		vec2(0.0, 0.0),
		vec2(1.0, 0.0),
		vec2(1.0, 1.0),
		vec2(0.0, 1.0)
	);
	vec2 corner = corners[gl_VertexID % 4];
	int glyphIndex = osgx_glyphIndices[gl_InstanceID];
	vec3 pos = vec3(corner, 0.0) * u_cellSize;

	pos.x += float(gl_InstanceID) * u_advance;

	gl_Position = osg_ModelViewProjectionMatrix * vec4(pos, 1.0);
	vAtlasUV = vec2((float(glyphIndex) + corner.x) / float(u_charsetCount), corner.y);
}
)GLSL";

constexpr const char* PIXEL_TEXT_FRAGMENT_SHADER = R"GLSL(
#version 460 core

in vec2 vAtlasUV;

uniform sampler2D u_atlas;
uniform vec4 u_ink;

out vec4 fragColor;

void main(void) {
	// The atlas itself is a hard-edged 1-bit bitmap, but the texture is sampled with LINEAR
	// filtering (see PixelText::_atlasTexture()), so `coverage` ramps smoothly across roughly
	// one texel on either side of a glyph edge instead of jumping straight from 0 to 1.
	// smoothstep()'d back around the 0.5 midpoint - width driven by fwidth(coverage), i.e. how
	// fast that ramp moves per screen pixel - turns that ramp into a clean antialiased edge
	// instead of either a jaggy NEAREST edge or an untouched blurry LINEAR one. This only works
	// because the UV math is entirely our own (osgx_glyphIndices + osg_MultiTexCoord0), so every
	// glyph cell has a known, generous margin - LINEAR filtering never blends across cells into
	// unrelated glyphs.
	float coverage = texture(u_atlas, vAtlasUV).r;
	float edge = max(fwidth(coverage) * 0.5, 1e-5);
	float alpha = smoothstep(0.5 - edge, 0.5 + edge, coverage);

	if(alpha < 0.01) discard;

	fragColor = vec4(u_ink.rgb, u_ink.a * alpha);
}
)GLSL";

}

osg::ref_ptr<osg::Image> PixelText::createAtlas(
	std::span<const std::string> cells,
	int cellSize,
	int pixelScale,
	int multiPixelScale,
	bool verticallyCentered
) {
	if(cells.empty()) throw std::invalid_argument("osgx::PixelText::createAtlas: cells must not be empty");

	for(const auto& cell : cells) {
		for(char c : cell) {
			if(CHARSET.find(c) == std::string_view::npos) {
				throw std::invalid_argument(
					"osgx::PixelText::createAtlas: unsupported character '" + std::string(1, c) + "'"
				);
			}
		}
	}

	const int atlasW = cellSize * static_cast<int>(cells.size());
	const int atlasH = cellSize;
	std::vector<unsigned char> pixels(static_cast<std::size_t>(atlasW) * static_cast<std::size_t>(atlasH), 0);

	for(std::size_t cellIndex = 0; cellIndex < cells.size(); cellIndex++) {
		const std::string& value = cells[cellIndex];
		int scale = value.size() == 1 ? pixelScale : multiPixelScale;
		int charW = GLYPH_COLS * scale;
		int gap = scale;
		int blockW = static_cast<int>(value.size()) * charW + (static_cast<int>(value.size()) - 1) * gap;
		int minRow = 0;
		int maxRow = GLYPH_ROWS - 1;

		if(verticallyCentered) std::tie(minRow, maxRow) = inkRowExtent(value);

		int blockH = (maxRow - minRow + 1) * scale;
		int marginX = (cellSize - blockW) / 2;
		// Shifted by -minRow * scale so row `minRow` (the block's actual top edge) lands at the
		// computed top margin, rather than row 0 of the glyph's own GLYPH_ROWS-tall coordinate
		// space - see stampGlyph()'s row/y0 math below.
		int marginY = (cellSize - blockH) / 2 - minRow * scale;
		int cellOriginX = static_cast<int>(cellIndex) * cellSize;

		for(std::size_t charSlot = 0; charSlot < value.size(); charSlot++) {
			int x0 = cellOriginX + marginX + static_cast<int>(charSlot) * (charW + gap);

			stampGlyph(pixels, atlasW, atlasH, x0, marginY, value[charSlot], scale);
		}
	}

	auto image = make_ref<osg::Image>();

	image->allocateImage(atlasW, atlasH, 1, GL_RED, GL_UNSIGNED_BYTE);

	auto* dest = image->data();

	// osg::Image rows are stored bottom-up; `pixels` above was filled top-down (row 0 = glyph
	// top row) - same y-flip pyosg_dice.py's build_number_atlas() does via `atlas_h - 1 - y`.
	for(int y = 0; y < atlasH; y++) {
		std::copy(
			pixels.begin() + static_cast<std::ptrdiff_t>(y) * atlasW,
			pixels.begin() + static_cast<std::ptrdiff_t>(y + 1) * atlasW,
			dest + static_cast<std::ptrdiff_t>(atlasH - 1 - y) * atlasW
		);
	}

	return image;
}

osg::ref_ptr<osg::Texture2D> PixelText::_atlasTexture() {
	// Owned by the live osgx::Library (LibraryState::pixelTextAtlas), created on first use.
	auto& texture = detail::libraryState().pixelTextAtlas;

	if(texture) return texture;

	texture = [] {
		auto tex = make_ref<osg::Texture2D>();

		tex->setImage(createCharsetAtlas());
		// LINEAR, not NEAREST: the fragment shader's smoothstep/fwidth antialiasing needs a
		// smoothly-ramping coverage value near glyph edges to sharpen back up - NEAREST would
		// hand it a hard step with no ramp to work with. Every glyph cell carries a wide margin,
		// so this never blends across cells into an unrelated glyph.
		tex->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
		tex->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
		tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
		tex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
		// osg::Texture defaults to resizing any non-power-of-two image up to the next POT on
		// upload (a smoothing resize, meant for old fixed-function hardware) - this charset
		// atlas is essentially never POT-width, and that resize would blur this 1-bit bitmap
		// font into illegible noise. NPOT textures have been core GL since 2.0; disabling the
		// hint keeps the atlas pixel-exact.
		tex->setResizeNonPowerOfTwoHint(false);

		return tex;
	}();

	return texture;
}

// ------------------------------------------------------------------------------------------------

PixelText::PixelText() {
	_build(1.0f);
}

PixelText::PixelText(std::string_view text, float cellSize) {
	_build(cellSize);
	setText(text);
}

PixelText::PixelText(const PixelText& text, const osg::CopyOp& co):
osg::Geometry(text, co),
_text(text._text) {
	_instances = static_cast<osg::DrawArrays*>(getPrimitiveSetList()[0].get());

	_bindUniforms();
}

void PixelText::_build(float cellSize) {
	// No osg::Vec3Array/Vec2Array bound here at all - PIXEL_TEXT_VERTEX_SHADER builds the unit
	// quad's four corners procedurally from gl_VertexID, so there's no vertex/texcoord data for
	// this Geometry to own in the first place. osg::Geometry::drawImplementation() doesn't
	// require any bound arrays to issue its draw call, so this is a normal, supported shape for
	// a Geometry, not a workaround.
	_instances = new osg::DrawArrays(osg::PrimitiveSet::TRIANGLE_FAN, 0, 0, 0);

	addPrimitiveSet(_instances);

	_installState();

	setCellSize(cellSize);
	setAdvance(cellSize);
	setInk(osg::Vec4(0.05f, 0.05f, 0.05f, 1.0f));
	setText("");
}

void PixelText::_installState() {
	auto* ss = getOrCreateStateSet();
	auto program = make_ref<osg::Program>();

	program->addShader(new osg::Shader(osg::Shader::VERTEX, resolveShaderLibs(PIXEL_TEXT_VERTEX_SHADER)));
	program->addShader(new osg::Shader(osg::Shader::FRAGMENT, resolveShaderLibs(PIXEL_TEXT_FRAGMENT_SHADER)));

	ss->setAttributeAndModes(program, osg::StateAttribute::ON);
	ss->setMode(GL_BLEND, osg::StateAttribute::ON);
	// See osgx::Grid::_installState() - GL_BLEND alone leaves GL's default (ONE, ZERO) blend
	// func, which just overwrites the destination regardless of alpha.
	ss->setAttributeAndModes(
		new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
		osg::StateAttribute::ON
	);

	ss->setTextureAttributeAndModes(0, _atlasTexture(), osg::StateAttribute::ON);
	ss->addUniform(new osg::Uniform("u_atlas", 0));
	ss->addUniform(new osg::Uniform("u_charsetCount", static_cast<int>(CHARSET.size())));

	_cellSize = new osg::Uniform("u_cellSize", 1.0f);
	_advance = new osg::Uniform("u_advance", 1.0f);
	_ink = new osg::Uniform("u_ink", osg::Vec4(0.05f, 0.05f, 0.05f, 1.0f));

	ss->addUniform(_cellSize);
	ss->addUniform(_advance);
	ss->addUniform(_ink);
}

void PixelText::_bindUniforms() {
	auto* ss = getOrCreateStateSet();

	_cellSize = ss->getUniform("u_cellSize");
	_advance = ss->getUniform("u_advance");
	_ink = ss->getUniform("u_ink");
}

void PixelText::setText(std::string_view text) {
	_text.clear();
	_text.reserve(text.size());

	for(char c : text) _text.push_back(normalizeChar(c));

	auto count = static_cast<unsigned int>(_text.size());
	auto indices = make_ref<osg::IntArray>(std::max<unsigned int>(count, 1));

	for(unsigned int i = 0; i < count; i++) {
		(*indices)[i] = static_cast<int>(CHARSET.find(_text[i]));
	}

	indices->setBufferObject(new osg::ShaderStorageBufferObject());

	getOrCreateStateSet()->setAttributeAndModes(
		new osg::ShaderStorageBufferBinding(Library::instance().bindings().get("osgx::pixelText"), indices, 0, 0),
		osg::StateAttribute::ON
	);

	// count == 0 must draw literally nothing, not one degenerate instance: setNumInstances(0)
	// alone falls back to a single NON-instanced glDrawArrays call (see osg::DrawArrays::draw()),
	// which would still render one quad reading SSBO index 0. Zeroing the vertex count too makes
	// that fallback draw 0 vertices instead.
	_instances->setCount(count > 0 ? 4 : 0);
	_instances->setNumInstances(static_cast<int>(count));

	dirtyBound();
}

osg::BoundingBox PixelText::computeBoundingBox() const {
	auto count = static_cast<int>(_text.size());

	if(count == 0) return osg::BoundingBox();

	float cellSize = getCellSize();
	float advance = getAdvance();
	// The last glyph's quad spans [(count - 1) * advance, (count - 1) * advance + cellSize];
	// every earlier glyph's quad is fully contained within that same X range whenever
	// advance <= cellSize (the normal monospace case), so this is the real extent, not just an
	// approximation of it.
	float width = static_cast<float>(count - 1) * advance + cellSize;

	return osg::BoundingBox(
		osg::Vec3(0.0f, 0.0f, 0.0f),
		osg::Vec3(width, cellSize, 0.0f)
	);
}

}
