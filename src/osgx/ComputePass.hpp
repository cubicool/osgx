#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BindImageTexture>
#include <osg/DispatchCompute>
#include <osg/GL>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <string>

namespace osgx {

// ================================================================================================
// ComputePass - one compute dispatch, followed by the memory barrier its consumers need.
//
// The compute sibling of osgx::RTT: IS-A osg::DispatchCompute, so `group->addChild(pass)` just
// works and every DispatchCompute/Drawable setter stays directly available. What it adds:
//
// - glMemoryBarrier(barriers) right after the dispatch. OSG 3.6 has no StateAttribute for it, and
//   without it the next pass (or draw) may read stale image/buffer contents - a SILENT failure
//   that varies by driver and timing.
// - Culling disabled (a DispatchCompute has no meaningful bound).
// - Small helpers for the per-pass setup every multi-pass compute effect repeats: dispatch size
//   from a pixel size, RenderBin ordering, image unit and sampler bindings, on/off
//   `#pragma import_defines` switches.
//
// Ordering: OSG draws a render stage's RenderBins in ascending number, so a chain of passes under
// one camera runs in setRenderBin() order (e.g. -3, -2, -1, ahead of default-bin geometry at 0).
// The passes run in whatever render stage traverses them - for a screen-space effect, between a
// PRE_RENDER camera producing its inputs and the geometry consuming its outputs.
//
// No OSGX_META_Object / clone() support, same as RTT: it owns GPU-side bindings clone() can't
// meaningfully duplicate.
// ================================================================================================
class ComputePass: public osg::DispatchCompute {
public:
	// The barriers a texture-to-texture compute chain needs: image stores made visible to later
	// texture fetches (sampler reads) and to later image loads/stores.
	static constexpr GLbitfield TEXTURE_BARRIERS =
		GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;

	// `program` may be null and set later (setProgram()), e.g. when a pass swaps between programs
	// at runtime. barriers=0 skips glMemoryBarrier() entirely.
	explicit ComputePass(
		const std::string& name,
		osg::Program* program=nullptr,
		GLbitfield barriers=TEXTURE_BARRIERS
	);

	void drawImplementation(osg::RenderInfo& renderInfo) const override;

	void setBarriers(GLbitfield barriers) { _barriers = barriers; }
	GLbitfield getBarriers() const { return _barriers; }

	// The compute Program on this pass's own StateSet.
	void setProgram(osg::Program* program);
	osg::Program* getProgram() const;

	// Enough workgroups of localSizeX x localSizeY to cover width x height (rounded up); the shader
	// bounds-checks the remainder.
	void setComputeGroupsFor(int width, int height, int localSizeX, int localSizeY);

	// setRenderBinDetails(number, "RenderBin") on this pass's StateSet.
	void setRenderBin(int number);

	// A BindImageTexture on image unit `unit` (the shader's `layout(binding = unit)`).
	void bindImage(
		GLuint unit,
		osg::Texture* texture,
		osg::BindImageTexture::Access access,
		GLenum format,
		int level=0
	);

	// `texture` on texture unit `unit`, plus the int uniform `uniformName` = unit for its sampler.
	void bindTexture(unsigned int unit, osg::Texture* texture, const char* uniformName);

	// An on/off `#pragma import_defines` switch on this pass's StateSet: defined as "1" or absent.
	// Each new combination compiles (and OSG caches) one more Program variant.
	void setDefineEnabled(const std::string& name, bool enabled);
	bool isDefineEnabled(const std::string& name) const;

	// A Program holding one COMPUTE shader.
	static osg::ref_ptr<osg::Program> createProgram(const std::string& name, const std::string& source);

	// A width x height image-store target: no image, LINEAR filtering, CLAMP_TO_EDGE, DYNAMIC.
	static osg::ref_ptr<osg::Texture2D> createTexture(
		const std::string& name,
		int width,
		int height,
		GLint internalFormat
	);

protected:
	~ComputePass() override;

private:
	GLbitfield _barriers;
};

}
