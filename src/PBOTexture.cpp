#include "osgx/PBOTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/GLExtensions>
#include <osg/Notify>
#include <osg/State>
#include <osg/Timer>
#include <osg/buffered_value>

OSGX_ENABLE_WARNINGS

#include <cstring>
#include <vector>

#ifndef GL_MAP_PERSISTENT_BIT
# define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
# define GL_MAP_COHERENT_BIT 0x0080
#endif

namespace osgx {

namespace {

// Rolling-average sample depth for the wait/upload CPU timing buffers - matches
// osgx::debug::DEFAULT_BUFFER_SIZE's value without depending on Debug.hpp for it.
constexpr std::size_t PROFILE_SAMPLES = 60;

// GL_UNSIGNED_BYTE-only - the small set of formats a caller's FrameSource might reasonably hand
// us. Anything else is a caller error, not something to silently guess a size for.
std::size_t bytesPerPixel(GLenum format) {
	switch(format) {
		case GL_RED: return 1;
		case GL_RG: return 2;
		case GL_RGB:
		case GL_BGR: return 3;
		case GL_RGBA:
		case GL_BGRA: return 4;

		default:
			OSG_WARN << "osgx::PBOTexture: unsupported upload format 0x" << std::hex << format
				<< std::dec << std::endl;

			return 0;
	}
}

bool supportsPersistentMapping(const osg::GLExtensions* ext) {
	return
		ext->glGenBuffers &&
		ext->glBindBuffer &&
		ext->glDeleteBuffers &&
		ext->glBufferStorage &&
		ext->glMapBufferRange &&
		ext->isTexStorage2DSupported() &&
		ext->glFenceSync &&
		ext->glClientWaitSync &&
		ext->glDeleteSync
	;
}

}

class PBOTexture::Subload: public osg::Texture2D::SubloadCallback {
public:
	Subload(PBOTexture* owner, int width, int height, GLenum format, unsigned int ringCapacity):
	_owner(owner),
	_width(width),
	_height(height),
	_format(format),
	_ringCapacity(ringCapacity) {
	}

	void load(const osg::Texture2D&, osg::State& state) const override {
		const osg::GLExtensions* ext = state.get<osg::GLExtensions>();

		if(!supportsPersistentMapping(ext)) {
			OSG_WARN << "osgx::PBOTexture: persistent-mapped buffer storage/texture storage/sync "
				"is unavailable in the current GL context" << std::endl;

			return;
		}

		ext->glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, _width, _height);

		Ring& ring = _rings[state.getContextID()];

		_initRing(ext, ring);
		_upload(ext, ring);
	}

	void subload(const osg::Texture2D&, osg::State& state) const override {
		Ring& ring = _rings[state.getContextID()];

		if(!ring.initialized) return;

		_upload(state.get<osg::GLExtensions>(), ring);
	}

	double getAverageWaitMilliseconds(unsigned int contextID) const {
		if(contextID >= _rings.size()) return 0.0;

		return osg::Timer::instance()->delta_m(osg::Timer_t(0), _rings[contextID].waitTicks.average());
	}

	double getAverageUploadMilliseconds(unsigned int contextID) const {
		if(contextID >= _rings.size()) return 0.0;

		return osg::Timer::instance()->delta_m(osg::Timer_t(0), _rings[contextID].uploadTicks.average());
	}

	// Needs a current GL context matching contextID - same requirement as every other GL-object
	// release path in OSG. Called from PBOTexture::releaseGLObjects(state), never with state==0.
	void releaseGLObjects(unsigned int contextID) const {
		if(contextID >= _rings.size()) return;

		Ring& ring = _rings[contextID];

		if(!ring.initialized) return;

		const osg::GLExtensions* ext = osg::GLExtensions::Get(contextID, false);

		if(ext) {
			for(GLsync fence: ring.fence) if(fence) ext->glDeleteSync(fence);

			ext->glDeleteBuffers(static_cast<GLsizei>(ring.pbo.size()), ring.pbo.data());
		}

		ring = Ring();
	}

private:
	struct Ring {
		bool initialized = false;
		std::vector<GLuint> pbo;
		std::vector<void*> ptr;
		std::vector<GLsync> fence;
		unsigned int index = 0;
		osgx::aring_buffer<osg::Timer_t, PROFILE_SAMPLES> waitTicks;
		osgx::aring_buffer<osg::Timer_t, PROFILE_SAMPLES> uploadTicks;
	};

	std::size_t _frameSize() const {
		return static_cast<std::size_t>(_width) * static_cast<std::size_t>(_height) * bytesPerPixel(_format);
	}

	void _initRing(const osg::GLExtensions* ext, Ring& ring) const {
		if(ring.initialized) return;

		const std::size_t frameSize = _frameSize();

		ring.pbo.assign(_ringCapacity, 0);
		ring.ptr.assign(_ringCapacity, nullptr);
		ring.fence.assign(_ringCapacity, nullptr);

		for(unsigned int i = 0; i < _ringCapacity; i++) {
			ext->glGenBuffers(1, &ring.pbo[i]);
			ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ring.pbo[i]);

			ext->glBufferStorage(
				GL_PIXEL_UNPACK_BUFFER,
				static_cast<GLintptr>(frameSize),
				nullptr,
				GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
			);

			ring.ptr[i] = ext->glMapBufferRange(
				GL_PIXEL_UNPACK_BUFFER,
				0,
				static_cast<GLsizeiptr>(frameSize),
				GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_UNSYNCHRONIZED_BIT
			);
		}

		ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

		ring.initialized = true;
	}

	void _upload(const osg::GLExtensions* ext, Ring& ring) const {
		const FrameSource& source = _owner->getFrameSource();
		const void* data = source ? source() : nullptr;

		if(!data) return;

		const osg::Timer_t uploadStart = osg::Timer::instance()->tick();

		GLsync& fence = ring.fence[ring.index];

		// Guard against overwriting a slot the GPU might still be reading from _ringCapacity
		// frames ago. This wait is the actual CPU-blocked-on-GPU sync stall.
		if(fence) {
			const osg::Timer_t waitStart = osg::Timer::instance()->tick();

			ext->glClientWaitSync(fence, 0, GL_TIMEOUT_IGNORED);

			ring.waitTicks.add(osg::Timer::instance()->tick() - waitStart);

			ext->glDeleteSync(fence);
			fence = nullptr;
		}

		else ring.waitTicks.add(osg::Timer_t(0));

		std::memcpy(ring.ptr[ring.index], data, _frameSize());

		ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ring.pbo[ring.index]);

		// Offset-as-pointer: reads from the bound GL_PIXEL_UNPACK_BUFFER instead of client memory.
		// The texture itself is already bound to GL_TEXTURE_2D by Texture2D::apply() before this
		// callback runs.
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _width, _height, _format, GL_UNSIGNED_BYTE, nullptr);

		// GL_PIXEL_UNPACK_BUFFER is a single global binding, not scoped to this texture/unit - OSG's
		// own PixelDataBufferObject always rebinds it to 0 right after use for exactly this reason.
		// Leaving our PBO bound here would corrupt the next unrelated glTexSubImage2D-style upload
		// this frame (e.g. StatsHandler's font atlas glyph upload silently reading our mapped PBO).
		ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

		fence = ext->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		ring.index = (ring.index + 1) % _ringCapacity;

		ring.uploadTicks.add(osg::Timer::instance()->tick() - uploadStart);
	}

	PBOTexture* _owner;
	int _width;
	int _height;
	GLenum _format;
	unsigned int _ringCapacity;
	mutable osg::buffered_object<Ring> _rings;
};

PBOTexture::PBOTexture(
	int width,
	int height,
	GLenum format,
	FrameSource source,
	unsigned int ringCapacity
):
_source(std::move(source)) {
	setInternalFormat(static_cast<GLint>(GL_RGBA8));
	setTextureSize(width, height);
	setNumMipmapLevels(1);
	setUseHardwareMipMapGeneration(false);

	// osg::Texture's default MIN_FILTER is LINEAR_MIPMAP_LINEAR (trilinear) - a silently
	// incomplete texture here, since there's exactly one mip level and no mipmap generation.
	setFilter(MIN_FILTER, LINEAR);

	setSubloadCallback(new Subload(this, width, height, format, ringCapacity));
}

PBOTexture::~PBOTexture() {}

void PBOTexture::setFrameSource(FrameSource source) {
	_source = std::move(source);
}

double PBOTexture::getAverageWaitMilliseconds(unsigned int contextID) const {
	const auto* subload = static_cast<const Subload*>(getSubloadCallback());

	return subload ? subload->getAverageWaitMilliseconds(contextID) : 0.0;
}

double PBOTexture::getAverageUploadMilliseconds(unsigned int contextID) const {
	const auto* subload = static_cast<const Subload*>(getSubloadCallback());

	return subload ? subload->getAverageUploadMilliseconds(contextID) : 0.0;
}

void PBOTexture::releaseGLObjects(osg::State* state) const {
	Texture2D::releaseGLObjects(state);

	if(state) {
		if(const auto* subload = static_cast<const Subload*>(getSubloadCallback())) {
			subload->releaseGLObjects(state->getContextID());
		}
	}
}

}
