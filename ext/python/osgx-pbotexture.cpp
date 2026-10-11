#include "osgx-python.hpp"
#include "osgx/PBOTexture.hpp"

#include "pyosg/osg/callable.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

#include <optional>

// osgx::PBOTexture IS-A osg::Texture2D, so its Python constructor should chain into Texture2D's
// own kwargs_init_own (wrap/filter/name/... - see pyosg/osg/Texture.cpp), exactly the way
// osgx::RTT chains into osg::Camera's (see ext/python/osgx-rtt.cpp for the fuller explanation this
// mirrors).
namespace pybind11x {
template<> struct kwargs_base<osgx::PBOTexture> { using type = osg::Texture2D; };
}

namespace osgx_python {

namespace {

using FrameSource = osgx::PBOTexture::FrameSource;

// Python has no raw-pointer type, so Python frame sources return an object supporting the buffer
// protocol (bytes, bytearray, a numpy array, memoryview, ...) - or None, matching FrameSource's own
// "no new frame yet" contract. Every frame must be C-contiguous uint8 data of exactly
// texture.getFrameSize() bytes; osgx::PBOTexture::Subload copies that many bytes from the returned
// pointer, so the size check here is what keeps a short frame from being over-read.
//
// next() runs inside the draw traversal, where there's no Python caller to raise to (and, threaded,
// no safe way to unwind through OSG at all). Any error - from the Python code itself, or a frame
// that fails validation - is reported via sys.unraisablehook (the same channel CPython uses for
// errors in __del__) and that frame is skipped, exactly like returning None.
//
// The validated buffer is kept alive as a member until the next call: Subload only reads through
// the returned pointer synchronously, immediately after next() returns. OSG may destroy a
// FrameSource from a thread that doesn't hold the GIL (e.g. a threaded viewer's draw or teardown),
// so the destructor releases that buffer under the GIL itself.
class PyFrameSourceBase: public FrameSource {
protected:
	~PyFrameSourceBase() override {
		if(!_frame) return;

		// Same shutdown policy as pybind11x::release_with_gil(): once the interpreter is gone, leak
		// the buffer view (detach it so buffer_info's destructor skips PyBuffer_Release).
		if(!Py_IsInitialized()) {
			_frame->info.view() = nullptr;

			return;
		}

		py::gil_scoped_acquire gil;

		_frame.reset();
	}

	template<typename Produce>
	const void* produceFrame(const osgx::PBOTexture& texture, Produce&& produce) {
		py::gil_scoped_acquire gil;

		try {
			py::object result = produce();

			if(result.is_none()) return nullptr;

			if(!py::isinstance<py::buffer>(result)) throw py::type_error(
				"osgx.PBOTexture frame source must return a buffer-protocol object or None, got " +
				py::repr(py::type::of(result)).cast<std::string>()
			);

			auto frame = pyx::unpack_buffer<uint8_t>(py::reinterpret_borrow<py::buffer>(result));
			auto expected = texture.getFrameSize();

			if(frame.count != expected) throw py::value_error(
				"osgx.PBOTexture frame source returned " + std::to_string(frame.count) +
				" bytes, expected exactly " + std::to_string(expected)
			);

			_frame.emplace(std::move(frame));

			return _frame->data;
		}

		catch(py::error_already_set& e) {
			e.discard_as_unraisable("osgx.PBOTexture frame source");
		}

		catch(const py::builtin_exception& e) {
			e.set_error();

			py::error_already_set().discard_as_unraisable("osgx.PBOTexture frame source");
		}

		return nullptr;
	}

private:
	std::optional<pyx::BufferElements<uint8_t>> _frame;
};

// Wraps a plain Python callable (taking no arguments) as a FrameSource; the "Wrapper" half of
// pyosg's applyCallback() dual support for a FrameSource subclass instance OR any callable.
class PyFrameSource: public PyFrameSourceBase {
public:
	explicit PyFrameSource(py::object fn):
	_fn(std::move(fn)) {
	}

	const void* next(const osgx::PBOTexture& texture) override {
		return produceFrame(texture, [this] { return _fn(); });
	}

protected:
	~PyFrameSource() override {
		pyx::release_with_gil(_fn);
	}

private:
	py::object _fn;
};

// Trampoline for a Python subclass of osgx.PBOTexture.FrameSource overriding next(texture).
class PyFrameSourceTrampoline: public PyFrameSourceBase {
public:
	const void* next(const osgx::PBOTexture& texture) override {
		return produceFrame(texture, [&]() -> py::object {
			auto override = py::get_override(static_cast<const FrameSource*>(this), "next");

			if(!override) throw py::type_error(
				"osgx.PBOTexture.FrameSource subclass must implement next(texture)"
			);

			return override(py::cast(&texture, py::return_value_policy::reference));
		});
	}
};

// The Python object given as a texture's frame source (a FrameSource subclass instance or a plain
// callable) is held in a slot on the texture itself: alive exactly as long as it's set (which is
// what keeps a Python subclass's next() override reachable from C++), released as soon as it's
// replaced, and returned as-is by the frameSource getter.
using PBOTextureSlots = pyx::PropertySlots<osgx::PBOTexture, 1>;

constexpr size_t FrameSourceSlot = 0;
constexpr auto FrameSourceGetter = py::overload_cast<>(&osgx::PBOTexture::getFrameSource);

void setFrameSource(osgx::PBOTexture& self, py::object source) {
	pyosg::detail::applyCallback<&osgx::PBOTexture::setFrameSource, FrameSource, PyFrameSource>(
		self,
		source
	);

	auto& slots = PBOTextureSlots::storage_type::get(self)->template proxy<PBOTextureSlots>();

	slots.set(FrameSourceSlot, source, self.getFrameSource());
}

osg::ref_ptr<osgx::PBOTexture> createPBOTexture(
	int width,
	int height,
	GLenum format,
	py::object source,
	unsigned int ringCapacity
) {
	osg::ref_ptr<osgx::PBOTexture> obj = new osgx::PBOTexture(
		width,
		height,
		format,
		static_cast<FrameSource*>(nullptr),
		ringCapacity
	);

	setFrameSource(*obj, std::move(source));

	return obj;
}

}

void bind_pbotexture(py::module_& m) {
	// Cross-module base-class inheritance: osg::Texture2D is bound in pyosg/osg/Texture.cpp, not
	// here. Works because osgx.cpp's py::module_::import("OpenSceneGraph") forces that
	// registration before any osgx py::class_<> that derives from an OSG type runs.
	auto texture = py::class_<osgx::PBOTexture, osg::Texture2D, osg::ref_ptr<osgx::PBOTexture>>(
		m,
		"PBOTexture",
		"An osg.Texture2D that uploads new frame data through a persistent-mapped PBO ring plus "
		"immutable texture storage and a per-slot fence, instead of OSG's own osg.Image dirty-flag "
		"upload path. IS-A osg.Texture2D, not a wrapping object: every osg.Texture2D method stays "
		"directly available. frameSource is either a PBOTexture.FrameSource subclass instance "
		"(overriding next(texture)) or a plain callable taking no arguments; either returns a new "
		"buffer-protocol object (bytes, bytearray, a numpy array, ...) of exactly frameSize "
		"(width*height*bytesPerPixel(format)) C-contiguous uint8 bytes, or None if no new frame is "
		"ready yet. An exception from the source, or a frame of the wrong size or type, is reported "
		"via sys.unraisablehook and that frame is skipped."
	);

	py::class_<FrameSource, osg::Referenced, PyFrameSourceTrampoline, osg::ref_ptr<FrameSource>>(
		texture,
		"FrameSource",
		"Base class for a Python frame source: override next(texture) to return a new "
		"buffer-protocol object of exactly texture.frameSize C-contiguous uint8 bytes, or None if "
		"no new frame is ready yet. Called at most once per GL context per frame, from the draw "
		"traversal."
	)
		.def(py::init<>(), "Create a FrameSource; subclasses must override next(texture).")
	;

	texture
		.def(
			py::init(&createPBOTexture),
			"width"_a,
			"height"_a,
			"format"_a,
			"source"_a=py::none(),
			"ringCapacity"_a=3,
			"width/height: texture + PBO ring dimensions in pixels. format: the upload format handed "
			"to glTexSubImage2D (osg.GL_RGBA, osg.GL_BGRA, osg.GL_RGB, osg.GL_BGR, osg.GL_RG, or "
			"osg.GL_RED; always paired with GL_UNSIGNED_BYTE) - internal storage is always GL_RGBA8. "
			"source: the initial frameSource (a FrameSource subclass instance, a plain callable, or "
			"None). ringCapacity: PBO ring depth (3, matching triple-buffering, unless measured "
			"otherwise)."
		)
		.def(
			py::init([](
				int width,
				int height,
				GLenum format,
				py::object source,
				unsigned int ringCapacity,
				py::kwargs kwargs
			) {
				auto obj = createPBOTexture(width, height, format, std::move(source), ringCapacity);

				pyx::kwargs_init(*obj, kwargs);

				return obj;
			}),
			"width"_a,
			"height"_a,
			"format"_a,
			"source"_a=py::none(),
			"ringCapacity"_a=3,
			"Same as the plain constructor above, but any additional keyword arguments are applied "
			"via osg.Texture2D's own kwargs handling after construction."
		)
		.def_property(
			"frameSource",
			PBOTextureSlots::getter<FrameSourceSlot>(FrameSourceGetter),
			&setFrameSource,
			"The current frame source (a FrameSource subclass instance, a plain callable, or None), "
			"returned as the same Python object it was set to."
		)
		.def(
			"setFrameSource",
			&setFrameSource,
			"source"_a,
			"Same as assigning the frameSource property."
		)
		.def_property_readonly(
			"frameSize",
			&osgx::PBOTexture::getFrameSize,
			"The exact number of bytes every frame the source returns must contain: "
			"width*height*bytesPerPixel(format)."
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
