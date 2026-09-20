#include "osgx-python.hpp"
#include "osgx/SDF.hpp"

namespace osgx_python {

// osgx::SDF - the GLSL shader-library catalog (SHAPES/SAMPLING/TEXTURE, no CPU-side functions,
// unlike Projection.hpp's CPU/GLSL twins - see SDF.hpp's own comment for why) plus the SDF
// StateAttribute for baked distance-field textures.
void bind_sdf(py::module_& m) {
	m.def(
		"registerSDFShaderLibs",
		&osgx::registerSDFShaderLibs,
		"Registers the '#pragma osgx::sdf' shader-library entries: SHAPES (nine closed-form 2D "
		"signed-distance functions, negative = inside), SAMPLING (osgx_SDF_Median/"
		"ScreenPixelRange/CoverageFromDistance for baked fields), and TEXTURE (the SDF "
		"attribute's own inputs + osgx_SDF_Coverage(uv); list it AFTER SAMPLING). See SDF.hpp's "
		"own doc comment for full per-function signatures."
	);

	py::class_<osgx::SDF, osg::StateAttribute, osg::ref_ptr<osgx::SDF>> sdf(
		m,
		"SDF",
		"A real osg.StateAttribute carrying one baked distance-field texture (single-channel "
		"SDF or 3-channel MSDF) plus its pixelRange, sdfType, and uvRect, applied via a std430 shader "
		"storage buffer and a fixed texture unit (SDF_TEXTURE_UNIT). Use with "
		"'#pragma osgx::sdf SAMPLING,TEXTURE' and osgx_SDF_Coverage(uv). osgx only CONSUMES "
		"distance fields - it never generates them."
	);

	py::enum_<osgx::SDF::SDFType>(sdf, "SDFType", "Channel layout of the baked distance field.")
		.value("SDF", osgx::SDF::SDFType::SDF)
		.value("MSDF", osgx::SDF::SDFType::MSDF)
		.export_values()
	;

	sdf
		.def(py::init<>(), "Constructs an SDF with no texture, SDFType.SDF, pixelRange 4, full-texture uvRect.")
		.def_static(
			"makeTexture", &osgx::SDF::makeTexture, "image"_a,
			"Builds an osg.Texture2D configured the way a distance field must be sampled (bilinear, no "
			"mipmaps, clamped); a single-channel grayscale image is relabeled GL_RED/GL_R8. None for a "
			"None image."
		)
		.def_property(
			"texture", &osgx::SDF::getTexture, &osgx::SDF::setTexture,
			"The baked distance-field texture (osg.Texture2D), bound at SDF_TEXTURE_UNIT."
		)
		.def_property(
			"sdfType", &osgx::SDF::getSDFType, &osgx::SDF::setSDFType,
			"SDFType.SDF (read .r) or SDFType.MSDF (median of .rgb)."
		)
		.def_property(
			"pixelRange", &osgx::SDF::getPixelRange, &osgx::SDF::setPixelRange,
			"The distance range, in TEXELS, the field was baked with (msdfgen's -pxrange)."
		)
		.def_property(
			"uvRect", &osgx::SDF::getUVRect, &osgx::SDF::setUVRect,
			"(u0, v0, u1, v1) in whole-texture space selecting the drawn tile; v0 > v1 flips it."
		)
		.def_readonly_static("SDF_TEXTURE_UNIT", &osgx::SDF::SDF_TEXTURE_UNIT)
		.def_readonly_static("SDF_BINDING", &osgx::SDF::SDF_BINDING)
	;
}

}
