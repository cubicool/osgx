#include "osgx-python.hpp"
#include "osgx/ComputePass.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BindImageTexture>
#include <osg/DispatchCompute>
#include <osg/Program>
#include <osg/Texture>
#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_computepass(py::module_& m) {
	py::class_<osgx::ComputePass, osg::DispatchCompute, osg::ref_ptr<osgx::ComputePass>> pass(
		m,
		"ComputePass",
		"One compute dispatch (an osg::DispatchCompute), followed by the glMemoryBarrier() its "
		"consumers need - without that barrier the next pass may read stale image contents, a "
		"silent, driver-dependent failure. IS-A osg.DispatchCompute (computeGroups etc. stay "
		"available); culling is disabled. Passes under one camera run in setRenderBin() order."
	);

	pass.attr("TEXTURE_BARRIERS") = static_cast<unsigned int>(osgx::ComputePass::TEXTURE_BARRIERS);

	pass
		.def(
			py::init([](const std::string& name, osg::Program* program, unsigned int barriers) {
				return osg::ref_ptr<osgx::ComputePass>(
					new osgx::ComputePass(name, program, static_cast<GLbitfield>(barriers))
				);
			}),
			"name"_a,
			"program"_a=nullptr,
			"barriers"_a=static_cast<unsigned int>(osgx::ComputePass::TEXTURE_BARRIERS),
			"`program` may be None and set later. `barriers` is the glMemoryBarrier() bitfield issued "
			"after the dispatch (default ComputePass.TEXTURE_BARRIERS: texture fetch + shader image "
			"access); 0 skips the barrier."
		)
		.def_property(
			"barriers",
			[](const osgx::ComputePass& self) { return static_cast<unsigned int>(self.getBarriers()); },
			[](osgx::ComputePass& self, unsigned int barriers) {
				self.setBarriers(static_cast<GLbitfield>(barriers));
			},
			"The glMemoryBarrier() bitfield issued after the dispatch; 0 skips it."
		)
		.def_property(
			"program",
			&osgx::ComputePass::getProgram,
			&osgx::ComputePass::setProgram,
			"The compute osg.Program on this pass's own StateSet."
		)
		.def(
			"setComputeGroupsFor",
			&osgx::ComputePass::setComputeGroupsFor,
			"width"_a,
			"height"_a,
			"localSizeX"_a,
			"localSizeY"_a,
			"Dispatches enough localSizeX x localSizeY workgroups to cover width x height (rounded "
			"up); the shader bounds-checks the remainder."
		)
		.def(
			"setRenderBin",
			&osgx::ComputePass::setRenderBin,
			"number"_a,
			"setRenderBinDetails(number, \"RenderBin\") on this pass's StateSet; OSG draws bins in "
			"ascending number, which orders a chain of passes."
		)
		.def(
			"bindImage",
			&osgx::ComputePass::bindImage,
			"unit"_a,
			"texture"_a,
			"access"_a,
			"format"_a,
			"level"_a=0,
			"Binds `texture` (mip `level`) to image unit `unit` (the shader's layout(binding = unit)). "
			"`access` is an osg.BindImageTexture.Access; `format` the GL internal format the "
			"shader's image declaration uses (e.g. GL_R8 for `r8`)."
		)
		.def(
			"bindTexture",
			&osgx::ComputePass::bindTexture,
			"unit"_a,
			"texture"_a,
			"uniformName"_a,
			"Binds `texture` to texture unit `unit` plus the int uniform `uniformName` = unit for its "
			"sampler."
		)
		.def(
			"setDefineEnabled",
			&osgx::ComputePass::setDefineEnabled,
			"name"_a,
			"enabled"_a,
			"An on/off `#pragma import_defines` switch on this pass's StateSet: defined as \"1\" or "
			"absent. Each new combination compiles (and OSG caches) one more Program variant."
		)
		.def(
			"isDefineEnabled",
			&osgx::ComputePass::isDefineEnabled,
			"name"_a,
			"True if `name` is currently defined on this pass's StateSet (see setDefineEnabled())."
		)
		.def_static(
			"createProgram",
			&osgx::ComputePass::createProgram,
			"name"_a,
			"source"_a,
			"An osg.Program holding one COMPUTE shader built from `source`."
		)
		.def_static(
			"createTexture",
			[](const std::string& name, int width, int height, int internalFormat) {
				return osgx::ComputePass::createTexture(name, width, height, static_cast<GLint>(internalFormat));
			},
			"name"_a,
			"width"_a,
			"height"_a,
			"internalFormat"_a,
			"A width x height osg.Texture2D image-store target: no image, LINEAR filtering, "
			"CLAMP_TO_EDGE, DYNAMIC data variance."
		)
	;
}

}
