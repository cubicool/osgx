#include "osgx-python.hpp"
#include "osgx/PBOTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

#include <memory>

// osgx::PBOTexture IS-A osg::Texture2D, so its Python constructor should chain into Texture2D's
// own kwargs_init_own (wrap/filter/name/... - see pyosg/osg/Texture.cpp), exactly the way
// osgx::RTT chains into osg::Camera's (see ext/python/osgx-rtt.cpp for the fuller explanation this
// mirrors).
namespace pybind11x {
template<> struct kwargs_base<osgx::PBOTexture> { using type = osg::Texture2D; };
}

namespace osgx_python {

namespace {

// Bridges a Python callable into osgx::PBOTexture::FrameSource (std::function<const void*()>).
// Python has no raw-pointer type, so the callable is expected to return an object supporting the
// buffer protocol (bytes, bytearray, a numpy array, memoryview, ...) each call - or None, matching
// FrameSource's own "no new frame yet" contract. pybind11's std::function caster (functional.h)
// acquires the GIL automatically when it invokes a wrapped py::function, but this class calls
// py::function::operator() directly instead, so it acquires the GIL itself. The returned buffer is
// kept alive as a member across the call boundary - osgx::PBOTexture::Subload only ever reads
// through the returned pointer synchronously, immediately after calling this, so keeping the
// previous frame's object alive only until the next call overwrites it is sufficient.
//
// _buffer is a shared_ptr, not a plain py::buffer_info member: buffer_info's copy constructor is
// explicitly deleted (it owns a Py_buffer*), which would make PyFrameSource itself non-copy-
// constructible - and std::function<const void*()> (osgx::PBOTexture::FrameSource) requires its
// target callable type to be copy-constructible even though it's only ever moved-from here.
class PyFrameSource {
public:
	explicit PyFrameSource(py::function fn):
	_fn(std::move(fn)),
	_buffer(std::make_shared<py::buffer_info>()) {
	}

	const void* operator()() {
		py::gil_scoped_acquire gil;

		_frame = _fn();

		if(_frame.is_none()) return nullptr;

		*_buffer = _frame.cast<py::buffer>().request();

		return _buffer->ptr;
	}

private:
	py::function _fn;
	py::object _frame;
	std::shared_ptr<py::buffer_info> _buffer;
};

}

void bind_pbotexture(py::module_& m) {
	// Cross-module base-class inheritance: osg::Texture2D is bound in pyosg/osg/Texture.cpp, not
	// here. Works because osgx.cpp's py::module_::import("OpenSceneGraph") forces that
	// registration before any osgx py::class_<> that derives from an OSG type runs.
	py::class_<osgx::PBOTexture, osg::Texture2D, osg::ref_ptr<osgx::PBOTexture>>(
		m,
		"PBOTexture",
		"An osg.Texture2D that uploads new frame data through a persistent-mapped PBO ring plus "
		"immutable texture storage and a per-slot fence, instead of OSG's own osg.Image dirty-flag "
		"upload path. IS-A osg.Texture2D, not a wrapping object: every osg.Texture2D method stays "
		"directly available. source is a callable returning a new buffer-protocol object (bytes, "
		"bytearray, a numpy array, ...) of exactly width*height*bytesPerPixel(format) bytes each "
		"time it's called, or None if no new frame is ready yet."
	)
		.def(
			py::init([](int width, int height, GLenum format, py::function source, unsigned int ringCapacity) {
				return osg::ref_ptr<osgx::PBOTexture>(new osgx::PBOTexture(
					width, height, format, PyFrameSource(std::move(source)), ringCapacity
				));
			}),
			"width"_a,
			"height"_a,
			"format"_a,
			"source"_a,
			"ringCapacity"_a=3,
			"width/height: texture + PBO ring dimensions in pixels. format: the upload format handed "
			"to glTexSubImage2D (osg.GL_RGBA, osg.GL_BGRA, osg.GL_RGB, osg.GL_BGR, osg.GL_RG, or "
			"osg.GL_RED; always paired with GL_UNSIGNED_BYTE) - internal storage is always GL_RGBA8. "
			"ringCapacity: PBO ring depth (3, matching triple-buffering, unless measured otherwise)."
		)
		.def(
			py::init([](
				int width,
				int height,
				GLenum format,
				py::function source,
				unsigned int ringCapacity,
				py::kwargs kwargs
			) {
				osg::ref_ptr<osgx::PBOTexture> obj = new osgx::PBOTexture(
					width, height, format, PyFrameSource(std::move(source)), ringCapacity
				);

				pyx::kwargs_init(*obj, kwargs);

				return obj;
			}),
			"width"_a,
			"height"_a,
			"format"_a,
			"source"_a,
			"ringCapacity"_a=3,
			"Same as the plain constructor above, but any additional keyword arguments are applied "
			"via osg.Texture2D's own kwargs handling after construction."
		)
		.def(
			"setFrameSource",
			[](osgx::PBOTexture& self, py::function source) {
				self.setFrameSource(PyFrameSource(std::move(source)));
			},
			"source"_a,
			"Replaces the FrameSource callable given at construction time."
		)
		.def(
			"getAverageWaitMilliseconds",
			&osgx::PBOTexture::getAverageWaitMilliseconds,
			"contextID"_a=0,
			"Rolling average (last 60 subloads) of CPU wall time spent in the glClientWaitSync call "
			"that guards against overwriting a ring slot the GPU might still be reading from - the "
			"actual CPU-blocked-on-GPU sync stall. 0 until this texture has subloaded at least once "
			"under the given contextID."
		)
		.def(
			"getAverageUploadMilliseconds",
			&osgx::PBOTexture::getAverageUploadMilliseconds,
			"contextID"_a=0,
			"Rolling average (last 60 subloads) of the whole per-subload CPU cost under the given "
			"contextID: the glClientWaitSync wait, the memcpy into the mapped PBO, the "
			"glTexSubImage2D submission, and PBO bind churn."
		)
	;
}

}
