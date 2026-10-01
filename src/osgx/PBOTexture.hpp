#pragma once

#include "Core.hpp"
#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <functional>

namespace osgx {

// PBOTexture - an osg::Texture2D that uploads new frame data through a persistent-mapped PBO
// ring (GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT via glBufferStorage) plus glTexStorage2D
// immutable storage and a per-slot fence, instead of OSG's own osg::Image dirty-flag ->
// glTexSubImage2D path (ordinary Texture2D::apply() re-checking Image::getModifiedCount() every
// frame and doing a client-memory upload). Generalizes the pattern proven in mbew's own
// mbew-example-video-osg.cpp (~/dev/mbew) - video is the motivating case, but nothing here is
// video-specific; any high-frequency CPU->GPU source (camera feed, procedural data) is a valid
// FrameSource.
//
// Deliberately IS-A osg::Texture2D (not a wrapping helper), same reasoning as osgx::RTT: every
// existing Texture2D consumer (StateSet::setTextureAttributeAndModes, shader sampler binding,
// ...) keeps working unmodified, and the caller never juggles a separate "here's your real
// texture" handle. Never has an osg::Image attached - the PBO ring IS the storage, and the
// constructor forces MIN_FILTER to LINEAR (the osg::Texture default is LINEAR_MIPMAP_LINEAR,
// which is a silently incomplete texture here since there's exactly one mip level and no
// hardware mipmap generation).
//
// Ring/fence state is buffered per GL context (osg::buffered_object), same pattern as
// osgx::debug::FrameAccumulator - a PBOTexture applied under multiple graphics contexts
// (multi-window) gets its own independent ring in each, matching how OSG already keeps a
// separate texture object per context.
//
// releaseGLObjects(state) frees that context's ring (PBOs + any pending fence) in addition to the
// base Texture2D cleanup; like the rest of OSG's GL-object teardown, it needs a State with a
// current context to issue the GL calls, so releaseGLObjects(nullptr) - e.g. from the destructor,
// where no context is guaranteed current - only dirties the base texture object and leaves any
// context's ring for that context's own eventual teardown to catch.
//
// No OSGX_META_Object / clone() support - like osgx::RTT, a PBOTexture owns real per-context GL
// buffer/fence state that clone() can't meaningfully duplicate.
class PBOTexture: public osg::Texture2D {
public:
	// Returns a pointer to exactly width*height*bytesPerPixel(format) bytes of new frame data, or
	// nullptr if no new frame is ready yet (a no-op for that context this subload - e.g. the
	// source hasn't produced a frame since the last one was consumed, so the texture keeps
	// showing whatever it last uploaded). Called at most once per unique GL context this texture
	// is applied under, per frame - the caller is responsible for its own thread-safety/framing
	// (mbew's own iterate()/sync() contract is exactly this shape).
	using FrameSource = std::function<const void*()>;

	// width/height: texture + PBO ring dimensions in pixels. format: the upload format handed to
	// glTexSubImage2D (GL_RGBA, GL_BGRA, GL_RGB, GL_BGR, GL_RG, or GL_RED; always paired with
	// GL_UNSIGNED_BYTE) - internal storage format is always GL_RGBA8 regardless. ringCapacity: PBO
	// ring depth (3, matching triple-buffering, unless a caller has measured a reason to change
	// it).
	explicit PBOTexture(
		int width,
		int height,
		GLenum format,
		FrameSource source,
		unsigned int ringCapacity=3
	);

	void setFrameSource(FrameSource source);
	const FrameSource& getFrameSource() const { return _source; }

	// Rolling averages (last 60 subloads) of CPU wall time in this context's upload path.
	// getAverageWaitMilliseconds() isolates just the glClientWaitSync call - the actual CPU-
	// blocked-on-GPU sync stall, guarding against overwriting a ring slot the GPU might still be
	// reading from. getAverageUploadMilliseconds() is the whole per-subload cost (that wait, plus
	// the memcpy into the mapped PBO, the glTexSubImage2D submission, and PBO bind churn). Both
	// read 0 until this texture has subloaded at least once under the given contextID (0 - the
	// common single-window case - by default).
	double getAverageWaitMilliseconds(unsigned int contextID=0) const;
	double getAverageUploadMilliseconds(unsigned int contextID=0) const;

	void releaseGLObjects(osg::State* state=nullptr) const override;

protected:
	~PBOTexture() override;

private:
	class Subload;

	FrameSource _source;
};

}
