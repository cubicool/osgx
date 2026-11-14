#include "osgx-python.hpp"
#include "osgx/GTAO.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>
#include <osg/Uniform>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_gtao(py::module_& m) {
	py::class_<osgx::GTAO> gtao(
		m,
		"GTAO",
		"Ground Truth Ambient Occlusion (Jimenez et al. 2016) as implemented by Intel's XeGTAO "
		"(MIT, https://github.com/GameTechDev/XeGTAO): horizon-based, a few screen-space slices "
		"per pixel, integrated analytically with cosine weighting. Compute passes: depth "
		"prefilter, main, and edge-aware denoise. Same inputs and output contract as osgx.SSAO / "
		"osgx.CORTAO: `aoTexture`, single-channel GL_R8, 1.0 = unoccluded."
	);

	py::enum_<osgx::GTAO::Quality>(
		gtao,
		"Quality",
		"Slices x steps per slice: LOW 1x2, MEDIUM 2x2, HIGH 3x3, ULTRA 9x3."
	)
		.value("LOW", osgx::GTAO::Quality::LOW)
		.value("MEDIUM", osgx::GTAO::Quality::MEDIUM)
		.value("HIGH", osgx::GTAO::Quality::HIGH)
		.value("ULTRA", osgx::GTAO::Quality::ULTRA)
	;

	py::class_<osgx::GTAO::Options>(gtao, "Options", "GTAO.create()'s optional settings.")
		.def(py::init<>(), "Defaults: radius=0.5, quality=HIGH, denoisePasses=1, bentNormal=False.")
		.def_readwrite(
			"radius", &osgx::GTAO::Options::radius,
			"World-space (scene units) occlusion radius; scale-dependent like SSAO's radius."
		)
		.def_readwrite("quality", &osgx::GTAO::Options::quality, "The initial GTAO.Quality.")
		.def_readwrite("denoisePasses", &osgx::GTAO::Options::denoisePasses, "0-3; see GTAO.denoisePasses.")
		.def_readwrite(
			"bentNormal", &osgx::GTAO::Options::bentNormal,
			"Also write bentNormalTexture (GL_RGBA8_SNORM: xyz = view-space bent normal, w = visibility)."
		)
	;

	gtao
		.def(py::init<>(), "Constructs an empty GTAO with nothing built; see GTAO.create().")
		.def_readwrite("node", &osgx::GTAO::node, "The osg.Group holding every pass; add it to the scene graph.")
		.def_readwrite("prefilterPass", &osgx::GTAO::prefilterPass, "View-space depth into the 5-mip depthTexture.")
		.def_readwrite("mainPass", &osgx::GTAO::mainPass, "The horizon search, writing the working term and edges.")
		.def_readwrite(
			"intermediateDenoisePasses", &osgx::GTAO::intermediateDenoisePasses,
			"The two intermediate denoise ComputePasses (ping-ponging workingTextures), as a list."
		)
		.def_readwrite(
			"finalDenoisePass", &osgx::GTAO::finalDenoisePass,
			"The last denoise pass, writing aoTexture (and bentNormalTexture)."
		)
		.def_readwrite("aoTexture", &osgx::GTAO::aoTexture, "The result to consume: GL_R8, 1.0 = unoccluded.")
		.def_readwrite(
			"bentNormalTexture", &osgx::GTAO::bentNormalTexture,
			"Only with Options.bentNormal: xyz = view-space bent normal, w = visibility."
		)
		.def_readwrite(
			"depthTexture", &osgx::GTAO::depthTexture,
			"View-space depth, 5 mips, GL_R32F; rows top-down (XeGTAO's layout)."
		)
		.def_readwrite("edgesTexture", &osgx::GTAO::edgesTexture, "Packed depth edges (GL_R8), rows top-down.")
		.def_readwrite(
			"workingTextures", &osgx::GTAO::workingTextures,
			"The packed working term ping-pong pair (GL_R8UI, or GL_R32UI with bent normals), as a list."
		)
		.def_readwrite("radius", &osgx::GTAO::radius, "Live float uniform; assign .value at any time.")
		.def_readwrite(
			"radiusMultiplier", &osgx::GTAO::radiusMultiplier,
			"Live float uniform (XeGTAO heuristic, default 1.457)."
		)
		.def_readwrite("falloffRange", &osgx::GTAO::falloffRange, "Live float uniform (XeGTAO heuristic, default 0.615).")
		.def_readwrite(
			"sampleDistributionPower", &osgx::GTAO::sampleDistributionPower,
			"Live float uniform (XeGTAO heuristic, default 2.0)."
		)
		.def_readwrite(
			"thinOccluderCompensation", &osgx::GTAO::thinOccluderCompensation,
			"Live float uniform (XeGTAO heuristic, default 0.0)."
		)
		.def_readwrite(
			"finalValuePower", &osgx::GTAO::finalValuePower,
			"Live float uniform (XeGTAO heuristic, default 2.2): the overall darkness."
		)
		.def_readwrite(
			"depthMipSamplingOffset", &osgx::GTAO::depthMipSamplingOffset,
			"Live float uniform (XeGTAO heuristic, default 3.3)."
		)
		.def_readwrite(
			"noiseIndex", &osgx::GTAO::noiseIndex,
			"Live uint uniform; 0 by default, frameNumber % 64 only with a temporal filter downstream."
		)
		.def("valid", &osgx::GTAO::valid, "True if every pass and output texture was built.")
		.def_property(
			"quality",
			&osgx::GTAO::getQuality,
			&osgx::GTAO::setQuality,
			"The GTAO.Quality (a define on the main pass: one Program variant per level)."
		)
		.def_property(
			"denoisePasses",
			&osgx::GTAO::getDenoisePasses,
			&osgx::GTAO::setDenoisePasses,
			"0 (no blur), 1 (sharp), 2 (medium) or 3 (soft); clamped to 3."
		)
		.def_static(
			"create",
			py::overload_cast<
				osg::Texture2D*,
				osg::Texture2D*,
				osg::Uniform*,
				int,
				int,
				const osgx::GTAO::Options&
			>(&osgx::GTAO::create),
			"normalTexture"_a,
			"positionTexture"_a,
			"projectionMatrix"_a,
			"width"_a,
			"height"_a,
			"options"_a=osgx::GTAO::Options{},
			"`normalTexture` view-space, `positionTexture` eye-space with background pixels left at 0, "
			"`projectionMatrix` the caller-owned, draw-time-captured \"projectionMatrix\" uniform (same "
			"contract as SSAO.create()). Perspective projections only (off-center is fine)."
		)
	;
}

}
