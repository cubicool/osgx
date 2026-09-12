#include "osgx-python.hpp"
#include "osgx/PixelText.hpp"

#include <string>
#include <vector>

namespace osgx_python {

void bind_pixel_text(py::module_& m) {
	auto pixelText = py::class_<
		osgx::PixelText,
		osg::Geometry,
		osg::ref_ptr<osgx::PixelText>
	>(
		m,
		"PixelText",
		"A deliberately small, deliberately inflexible procedural 5x9 bitmap font - the full "
		"printable ASCII set (upper+lowercase, digits, space, punctuation), with a 2-row "
		"descender band for lowercase g/j/p/q/y and a few punctuation marks - for quick "
		"in-scene labels and decal text until slughorn/osgSlug (the real text renderer) is "
		"wired in. One gl_InstanceID-emitted quad per character, painted from a shared, "
		"process-wide atlas."
	);

	pixelText.attr("CHARSET") = std::string(osgx::PixelText::CHARSET);
	pixelText.attr("GLYPH_COLS") = osgx::PixelText::GLYPH_COLS;
	pixelText.attr("GLYPH_ROWS") = osgx::PixelText::GLYPH_ROWS;

	pixelText
		.def(py::init<>())
		.def(
			py::init<std::string_view, float>(),
			"text"_a,
			"cellSize"_a=1.0f,
			"In-scene text at any perspective/orientation - a gl_InstanceID-emitted quad per "
			"character, painted from a shared, process-wide atlas. Purely local-space geometry."
		)
		.def_static(
			"createAtlas",
			// pybind11's stl.h casts std::vector natively but has no std::span caster (it type-
			// erases the signature at bind time without actually failing until called - a real
			// TypeError at runtime, confirmed live). std::vector<std::string> -> std::span<const
			// std::string> converts implicitly on the way into the real function, so this stays
			// a thin bridge, not a reimplementation.
			[](
				const std::vector<std::string>& cells,
				int cellSize,
				int pixelScale,
				int multiPixelScale,
				bool verticallyCentered
			) {
				return osgx::PixelText::createAtlas(
					cells, cellSize, pixelScale, multiPixelScale, verticallyCentered
				);
			},
			"cells"_a,
			"cellSize"_a=77,
			"pixelScale"_a=7,
			"multiPixelScale"_a=5,
			"verticallyCentered"_a=true,
			"One atlas cell per entry in `cells`, each string centered within its own cell - for "
			"decal-style use (pyosg_dice.py calls this directly per die face). verticallyCentered "
			"(default True) centers each cell against the actual ink rows its own characters use "
			"(e.g. digits never dip into the descender band) rather than the font's full height."
		)
		.def_property(
			"text", &osgx::PixelText::getText, &osgx::PixelText::setText,
			"The rendered string. Case-sensitive - upper- and lowercase are distinct glyphs. "
			"Characters outside CHARSET render as a blank space rather than raising."
		)
		.def_property(
			"cellSize", &osgx::PixelText::getCellSize, &osgx::PixelText::setCellSize,
			"World-space size of one glyph's quad. Affects the label's extent, so it dirties bounds."
		)
		.def_property(
			"advance", &osgx::PixelText::getAdvance, &osgx::PixelText::setAdvance,
			"Distance between successive glyph origins along local +X; defaults to cellSize, so "
			"this is only touched to add letter-spacing. Affects the label's extent, so it "
			"dirties bounds."
		)
		.def_property(
			"ink", &osgx::PixelText::getInk, &osgx::PixelText::setInk,
			"Text color (RGBA), multiplied against the glyph atlas's coverage in the fragment shader."
		)
	;
}

}
