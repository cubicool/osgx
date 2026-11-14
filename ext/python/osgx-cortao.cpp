#include "osgx-python.hpp"
#include "osgx/CORTAO.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/Texture3D>
#include <osg/Uniform>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_cortao(py::module_& m) {
	py::class_<osgx::CORTAO> cortao(
		m,
		"CORTAO",
		"Circular Occluder Ray-Traced Ambient Occlusion (Daniel Isheden, MIT, "
		"https://github.com/theagentd/CORTAO), three compute passes (prepass, main, filter). Same "
		"inputs as osgx.SSAO (a G-buffer's view-space normal + eye-space position textures and the "
		"projection uniform) and the same output contract: `aoTexture`, single-channel GL_R8, "
		"1.0 = unoccluded. Add `node` under a camera that runs after the G-buffer camera and "
		"before whatever samples aoTexture."
	);

	py::enum_<osgx::CORTAO::Filter>(
		cortao,
		"Filter",
		"The filter pass's mode. TILED (default): the main pass's noise repeats every 4x4 pixels "
		"and a depth/normal-aware 4x4 average cancels it - smooth and frame-stable without temporal "
		"filtering. SPARSE: CORTAO's own 5- or 13-tap prefilter, intended to be followed by "
		"temporal filtering. NONE: a copy of rawAoTexture."
	)
		.value("NONE", osgx::CORTAO::Filter::NONE)
		.value("SPARSE", osgx::CORTAO::Filter::SPARSE)
		.value("TILED", osgx::CORTAO::Filter::TILED)
	;

	py::class_<osgx::CORTAO::Options>(cortao, "Options", "CORTAO.create()'s optional settings.")
		.def(py::init<>(), "Defaults: maxRadius=0.5, sampleCount=16, filter=TILED, bentNormal=False.")
		.def_readwrite(
			"maxRadius", &osgx::CORTAO::Options::maxRadius,
			"World-space (scene units) maximum occlusion radius; scale-dependent like SSAO's radius."
		)
		.def_readwrite(
			"sampleCount", &osgx::CORTAO::Options::sampleCount,
			"Initial CORTAO_SAMPLE_COUNT; see CORTAO.sampleCount."
		)
		.def_readwrite("filter", &osgx::CORTAO::Options::filter, "The filter pass's initial mode.")
		.def_readwrite(
			"bentNormal", &osgx::CORTAO::Options::bentNormal,
			"Also write bentNormalTexture (GL_RGBA8_SNORM: xyz = view-space bent normal, w = "
			"occlusion; unfiltered)."
		)
	;

	cortao
		.def(py::init<>(), "Constructs an empty CORTAO with nothing built; see CORTAO.create().")
		.def_readwrite("node", &osgx::CORTAO::node, "The osg.Group holding every pass; add it to the scene graph.")
		.def_readwrite("prepass", &osgx::CORTAO::prepass, "Packs linear depth + a spheremap normal into packedTexture.")
		.def_readwrite("mainPass", &osgx::CORTAO::mainPass, "Traces the rays, writing rawAoTexture (and bentNormalTexture).")
		.def_readwrite("filterPass", &osgx::CORTAO::filterPass, "rawAoTexture -> aoTexture, per CORTAO.filter.")
		.def_readwrite(
			"sparseFilterProgram", &osgx::CORTAO::sparseFilterProgram,
			"The SPARSE filter's program (swapped onto filterPass by the filter property)."
		)
		.def_readwrite(
			"tiledFilterProgram", &osgx::CORTAO::tiledFilterProgram,
			"The TILED filter's program (also NONE's copy variant)."
		)
		.def_readwrite("aoTexture", &osgx::CORTAO::aoTexture, "The result to consume: GL_R8, 1.0 = unoccluded.")
		.def_readwrite(
			"rawAoTexture", &osgx::CORTAO::rawAoTexture,
			"The main pass's unfiltered output, quantized to 1/16 (16 rays per pixel)."
		)
		.def_readwrite(
			"bentNormalTexture", &osgx::CORTAO::bentNormalTexture,
			"Only with Options.bentNormal: xyz = view-space bent normal, w = occlusion."
		)
		.def_readwrite("packedTexture", &osgx::CORTAO::packedTexture, "The prepass output (GL_R32UI, up to 8 mips).")
		.def_readwrite(
			"occluderLookupTable", &osgx::CORTAO::occluderLookupTable,
			"The osg.Texture3D occluder lookup table (built by createOccluderLookupTable())."
		)
		.def_readwrite("maxRadius", &osgx::CORTAO::maxRadius, "Live float uniform; assign .value at any time.")
		.def_readwrite(
			"maxScreenspaceRadiusHeightRatio", &osgx::CORTAO::maxScreenspaceRadiusHeightRatio,
			"Live float uniform: the largest screen-space radius, as a fraction of the screen height."
		)
		.def_readwrite("mipBias", &osgx::CORTAO::mipBias, "Live float uniform: bias on the packed-depth mip selection.")
		.def_readwrite("maxSlopeCompensation", &osgx::CORTAO::maxSlopeCompensation, "Live float uniform.")
		.def_readwrite("backfaceThreshold", &osgx::CORTAO::backfaceThreshold, "Live float uniform.")
		.def_readwrite("backfaceBoost", &osgx::CORTAO::backfaceBoost, "Live float uniform.")
		.def_readwrite(
			"filterDepthSensitivity", &osgx::CORTAO::filterDepthSensitivity,
			"Live float uniform (SPARSE and TILED filters)."
		)
		.def_readwrite(
			"filterNormalSensitivity", &osgx::CORTAO::filterNormalSensitivity,
			"Live float uniform (SPARSE and TILED filters)."
		)
		.def_readwrite(
			"randomSeed", &osgx::CORTAO::randomSeed,
			"Live uint uniform; keep constant unless a temporal filter follows."
		)
		.def("valid", &osgx::CORTAO::valid, "True if every pass and output texture was built.")
		.def_property(
			"sampleCount",
			&osgx::CORTAO::getSampleCount,
			&osgx::CORTAO::setSampleCount,
			"CORTAO_SAMPLE_COUNT, the primary quality/cost knob (4-32 recommended). A define: each "
			"first use of a new value compiles a Program variant."
		)
		.def_property(
			"filter",
			&osgx::CORTAO::getFilter,
			&osgx::CORTAO::setFilter,
			"The filter pass's mode (CORTAO.Filter)."
		)
		.def_property(
			"largeFilter",
			&osgx::CORTAO::getLargeFilter,
			&osgx::CORTAO::setLargeFilter,
			"SPARSE only: CORTAO_LARGE_FILTER, 13 taps instead of 5 (default True)."
		)
		.def_property(
			"filterSymmetry",
			&osgx::CORTAO::getFilterSymmetry,
			&osgx::CORTAO::setFilterSymmetry,
			"SPARSE only: CORTAO_FILTER_SYMMETRY, centered kernel weights (default True)."
		)
		.def_static(
			"create",
			py::overload_cast<
				osg::Texture2D*,
				osg::Texture2D*,
				osg::Uniform*,
				int,
				int,
				const osgx::CORTAO::Options&
			>(&osgx::CORTAO::create),
			"normalTexture"_a,
			"positionTexture"_a,
			"projectionMatrix"_a,
			"width"_a,
			"height"_a,
			"options"_a=osgx::CORTAO::Options{},
			"`normalTexture` view-space, `positionTexture` eye-space with background pixels left at 0, "
			"`projectionMatrix` the caller-owned, draw-time-captured \"projectionMatrix\" uniform (same "
			"contract as SSAO.create()). Perspective projections only."
		)
		.def_static(
			"createOccluderLookupTable",
			&osgx::CORTAO::createOccluderLookupTable,
			"rayCount"_a=16u,
			"directionResolution"_a=24u,
			"radiusResolution"_a=16u,
			"The occluder lookup table as a 3D GL_R{8,16,32}UI osg.Image (rayCount 8/16/32): for each "
			"spheremap-encoded occluder direction and radius, the bitmask of rays it blocks. A port of "
			"the reference OccluderLookupTableGenerator.py; deterministic."
		)
	;
}

}
