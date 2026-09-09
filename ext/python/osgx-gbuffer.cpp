#include "osgx-python.hpp"
#include "osgx/GBuffer.hpp"

namespace osgx_python {

void bind_gbuffer(py::module_& m) {
	py::enum_<osgx::AttachmentFormat>(
		m,
		"AttachmentFormat",
		"Texture internal-format presets for one G-buffer color attachment: RGBA8 for ordinary "
		"LDR color/albedo, RGB16F for signed [-1,1] data (e.g. a view-space normal), RGBA16F for "
		"HDR color that can exceed 1.0 before tonemapping, RGBA32F for real eye-space position data."
	)
		.value("RGBA8", osgx::AttachmentFormat::RGBA8)
		.value("RGB16F", osgx::AttachmentFormat::RGB16F)
		.value("RGBA16F", osgx::AttachmentFormat::RGBA16F)
		.value("RGBA32F", osgx::AttachmentFormat::RGBA32F)
	;

	py::class_<osgx::GBuffer>(
		m,
		"GBuffer",
		"One populated G-buffer: `camera` is the PRE_RENDER FBO pass writing `colorTextures` "
		"(indexed exactly as passed to create(), i.e. colorTextures[i] is COLOR_BUFFERi) plus "
		"`depthTexture`. The caller still owns adding `camera` to the rendered scene graph."
	)
		.def(py::init<>(), "Constructs an empty GBuffer with no camera/textures set; see GBuffer.create().")
		.def_readwrite(
			"camera", &osgx::GBuffer::camera,
			"The PRE_RENDER FBO camera performing the geometry pass; add it to the scene graph."
		)
		.def_readwrite(
			"colorTextures", &osgx::GBuffer::colorTextures,
			"The color attachments, indexed exactly as passed to create() (colorTextures[i] is COLOR_BUFFERi)."
		)
		.def_readwrite(
			"depthTexture", &osgx::GBuffer::depthTexture,
			"The real GL_DEPTH_COMPONENT24 depth attachment."
		)
		.def("valid", &osgx::GBuffer::valid, "True if camera and every texture were successfully built.")
		// GBuffer::create() takes a std::span, which pybind11 has no built-in caster for -- same
		// pattern osgx-core.cpp's findDataFile() binding uses: wrap in a lambda taking a
		// std::vector (pybind11/stl.h converts a Python list automatically) and build the span
		// from that inside the call.
		.def_static(
			"create",
			[](
				osg::Node* node,
				int width,
				int height,
				const std::vector<osgx::AttachmentFormat>& colorFormats,
				osg::Transform::ReferenceFrame referenceFrame
			) {
				return osgx::GBuffer::create(
					node, width, height, colorFormats, referenceFrame
				);
			},
			"node"_a,
			"width"_a,
			"height"_a,
			"colorFormats"_a,
			"referenceFrame"_a=osg::Transform::RELATIVE_RF,
			"Builds a PRE_RENDER FBO camera writing len(colorFormats) simultaneous color "
			"attachments (COLOR_BUFFER0..N) plus a real depth attachment, with `node` (a real 3D "
			"scene subgraph) as its child."
		)
	;

	py::class_<osgx::SSAO>(
		m,
		"SSAO",
		"Hemisphere-kernel screen-space ambient occlusion, operating on any G-buffer's "
		"view-space normal+position channels. `aoTexture` (the blurred result) is a "
		"single-channel [0, 1] mask (1.0 = fully unoccluded)."
	)
		.def(py::init<>(), "Constructs an empty SSAO with no cameras/textures set; see SSAO.create().")
		.def_readwrite(
			"rawCamera", &osgx::SSAO::rawCamera,
			"The raw hemisphere-kernel occlusion pass, reading the G-buffer's normal/position textures directly."
		)
		.def_readwrite(
			"blurCamera", &osgx::SSAO::blurCamera,
			"The fixed-radius box-blur pass that denoises rawCamera's output into aoTexture."
		)
		.def_readwrite(
			"aoTexture", &osgx::SSAO::aoTexture,
			"The final blurred occlusion mask: single-channel GL_R8 in [0, 1], 1.0 = fully unoccluded."
		)
		.def_readwrite(
			"radius", &osgx::SSAO::radius,
			"Live-tunable sample-radius uniform; set() at any time, no pass rebuild needed."
		)
		.def_readwrite(
			"bias", &osgx::SSAO::bias,
			"Live-tunable depth-comparison bias uniform; set() at any time, no pass rebuild needed."
		)
		.def("valid", &osgx::SSAO::valid, "True if both cameras and aoTexture were successfully built.")
		.def_static(
			"create",
			&osgx::SSAO::create,
			"normalTexture"_a,
			"positionTexture"_a,
			"projectionMatrix"_a,
			"width"_a,
			"height"_a,
			"radius"_a=0.5f,
			"bias"_a=0.02f,
			"Generic hemisphere-kernel SSAO: reads any G-buffer's view-space normal+position "
			"textures directly, needs nothing else. `projectionMatrix` is a caller-owned "
			"osg.Uniform this pass reads every draw -- keep it refreshed from the same per-frame "
			"callback that updates PBRIBLLightingScene's own view-matrix uniforms (see that type's "
			"own doc comment for why it must be a PRE_RENDER preDrawCallback). `radius`/`bias` seed "
			"the returned live osg.Uniform-backed `radius`/`bias` attributes -- set() them at any "
			"time, no pass rebuild needed."
		)
	;
}

}
