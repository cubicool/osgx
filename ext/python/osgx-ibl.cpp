#include "osgx-python.hpp"
#include "osgx/GGXPrefilter.hpp"
#include "osgx/IBL.hpp"
#include "osgx/LambertianBake.hpp"

namespace osgx_python {

void bind_ibl(py::module_& m) {
	osgx::registerIBLShaderLibs();

	m.attr("FULLSCREEN_VERT") = osgx::FULLSCREEN_VERT;
	m.attr("BRDF_LUT_FRAG") = osgx::BRDF_LUT_FRAG;
	m.attr("SH_IRRADIANCE") = osgx::SH_IRRADIANCE;
	m.attr("LAMBERTIAN_IRRADIANCE") = osgx::LAMBERTIAN_IRRADIANCE;

	py::class_<
		osgx::RunOnceCallback,
		osg::NodeCallback,
		osg::ref_ptr<osgx::RunOnceCallback>
	>(
		m,
		"RunOnceCallback",
		"Disables a node after its update callback has fired exactly once - e.g. a PRE_RENDER "
		"bake camera that should render one frame at startup and then go idle. Call rebake() to "
		"re-arm it."
	)
		.def(py::init<>(), "Constructs a run-once callback (traverses children on its one firing).")
		.def(
			"rebake", &osgx::RunOnceCallback::rebake, "node"_a,
			"Re-arms this callback so `node` updates (renders, if it's a bake camera) one more "
			"time - e.g. after swapping the bake's source data."
		)
	;

	m
		.def(
			"loadPrefilterCubemap",
			&osgx::loadPrefilterCubemap,
			"path"_a,
			"Loads a pre-baked GGX-prefiltered cubemap (.ktx2); returns None (and logs OSG_WARN) "
			"if the path doesn't load as a TextureCubeMap."
		)
		.def(
			"makeBRDFLUTCamera",
			&osgx::makeBRDFLUTCamera,
			"lutSize"_a,
			"lut"_a,
			"Configures `lut` in-place (size/format/filters) and returns a PRE_RENDER camera that "
			"bakes the split-sum BRDF LUT into it exactly once (see RunOnceCallback)."
		)
	;

	py::class_<osgx::SH9>(
		m,
		"SH9",
		"9 RGB coefficients (L0-L2 spherical harmonics) standing in for a whole low-frequency "
		"diffuse environment. See computeSH() to build one from an HDR image, and SH_IRRADIANCE "
		"for its GLSL evaluation."
	)
		.def(py::init<>(), "Constructs an all-zero SH9 (9 black coefficients).")
		.def("__len__", [](const osgx::SH9&) { return 9; }, "Returns 9, the fixed number of SH9 coefficients.")
		.def("__getitem__", [](const osgx::SH9& self, size_t i) {
			if(i >= 9) throw py::index_error();

			return self.coeffs[i];
		}, "i"_a, "Returns the i'th RGB coefficient (0-8).")
		.def("__setitem__", [](osgx::SH9& self, size_t i, const osg::Vec3f& v) {
			if(i >= 9) throw py::index_error();

			self.coeffs[i] = v;
		}, "i"_a, "value"_a, "Sets the i'th RGB coefficient (0-8).")
	;

	m.def(
		"computeSH",
		&osgx::computeSH,
		"image"_a,
		"Projects an equirectangular HDR/LDR osg.Image onto SH9 diffuse irradiance coefficients."
	);

	m.def(
		"computeLambertianCubeMap",
		&osgx::computeLambertianCubeMap,
		"image"_a,
		"size"_a = 64,
		"samples"_a = 256,
		py::call_guard<py::gil_scoped_release>(),
		"Bakes a cosine-weighted Monte Carlo diffuse irradiance cubemap from an equirectangular "
		"HDR/LDR osg.Image - more accurate than SH9 (see computeSH), at the cost of a real bake "
		"instead of 9 coefficients. Sample with LAMBERTIAN_IRRADIANCE's osgx_LambertianIrradiance()."
	);

	py::class_<osgx::GGXPrefilterOptions>(
		m,
		"GGXPrefilterOptions",
		"Tuning knobs for GGXPrefilterScene.create()/rebake(): bake resolution, sample count, "
		"firefly suppression, and readback timing."
	)
		.def(
			py::init<>(),
			"Constructs default options (prefilterSize=128, sampleCount=1024, fireflyClamp=8.0, "
			"maxFrames=8, readbackFrame=2, syncReadback=True)."
		)
		.def_readwrite(
			"prefilterSize", &osgx::GGXPrefilterOptions::prefilterSize,
			"Cubemap face resolution (in texels) of the baked prefilter chain."
		)
		.def_readwrite(
			"sampleCount", &osgx::GGXPrefilterOptions::sampleCount,
			"Number of importance samples accumulated per texel."
		)
		.def_readwrite(
			"fireflyClamp", &osgx::GGXPrefilterOptions::fireflyClamp,
			"Caps per-sample luminance before accumulation, suppressing sun-disc/firefly noise at "
			"low roughness; tune per-HDRI."
		)
		.def_readwrite(
			"maxFrames", &osgx::GGXPrefilterOptions::maxFrames,
			"Number of frames the bake scene runs before the bake is considered complete."
		)
		.def_readwrite(
			"readbackFrame", &osgx::GGXPrefilterOptions::readbackFrame,
			"Frame at which GGXPrefilterReadback reads the baked cubemap back from the GPU."
		)
		.def_readwrite(
			"syncReadback", &osgx::GGXPrefilterOptions::syncReadback,
			"If True, the readback calls glFinish() before reading back (deterministic, stalls "
			"the pipeline); if False, it trusts that readbackFrame frames have already elapsed."
		)
	;

	py::class_<
		osgx::GGXPrefilterReadback,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgx::GGXPrefilterReadback>
	>(
		m,
		"GGXPrefilterReadback",
		"Post-draw callback that, once attached to a rendering camera, waits until its trigger "
		"frame has rendered and then reads the prefiltered cubemap back from the GPU."
	)
		.def_property_readonly("done", &osgx::GGXPrefilterReadback::isDone, "True once the triggered readback has completed.")
		.def_property_readonly("result", &osgx::GGXPrefilterReadback::getResult, "The read-back cubemap once `done` is True (None otherwise).")
		.def("reset", &osgx::GGXPrefilterReadback::reset, "Resets this readback to wait for another trigger/readback cycle.")
		.def(
			"finish", &osgx::GGXPrefilterReadback::finish,
			"Returns the finished, filtered/wrapped cubemap once `done` is True. Only valid to "
			"call once `done` reports True."
		)
	;

	py::class_<osgx::GGXPrefilterScene>(
		m,
		"GGXPrefilterScene",
		"The offscreen scene graph (PRE_RENDER cameras, one per cubemap face/mip) that "
		"GGX-prefilters an equirectangular HDR source. Building it renders nothing - the caller "
		"owns adding `root` to a rendered scene graph, attaching `readback` as a post-draw "
		"callback, and running frames until readback.done."
	)
		.def_readonly("root", &osgx::GGXPrefilterScene::root, "The offscreen bake scene graph; add it to a rendered scene graph.")
		.def_readonly("sourceTexture", &osgx::GGXPrefilterScene::sourceTexture, "The equirectangular source texture being prefiltered.")
		.def_readonly("prefilterTexture", &osgx::GGXPrefilterScene::prefilterTexture, "The GGX-prefiltered specular cubemap being baked into.")
		.def_readonly("readback", &osgx::GGXPrefilterScene::readback, "The GGXPrefilterReadback to attach as a post-draw callback on the rendering camera.")
		.def_static(
			"create",
			&osgx::GGXPrefilterScene::create,
			"equirectImage"_a,
			"options"_a = osgx::GGXPrefilterOptions(),
			"Builds the offscreen bake scene for `equirectImage`. Renders nothing itself - add "
			"`root` to the scene graph, attach `readback`, and run frames until readback.done."
		)
		.def(
			"rebake", &osgx::GGXPrefilterScene::rebake, "equirectImage"_a,
			"Reuses this bake scene for a new equirectangular source image, avoiding a full "
			"rebuild of every PRE_RENDER camera/FBO/program."
		)
	;
}

}
