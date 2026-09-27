#include "osgx-python.hpp"
#include "osgx/Headless.hpp"

namespace osgx_python {

void bind_headless(py::module_& m_headless) {
	py::enum_<osgx::headless::Backend>(m_headless, "Backend")
		.value(
			"Auto",
			osgx::headless::Backend::AUTO,
			"Prefer EGL when available, otherwise use OSG's native pbuffer."
		)
		.value(
			"EGL",
			osgx::headless::Backend::EGL,
			"Require osgX's EGL pbuffer backend."
		)
		.value(
			"Native",
			osgx::headless::Backend::NATIVE,
			"Require OSG's native WGL/GLX/Cocoa pbuffer backend."
		)
	;

	py::class_<osgx::headless::Context>(
		m_headless,
		"Context",
		"A created offscreen pbuffer context. `graphicsContext` is ready to assign to an osg.Camera; "
		"`backend` reports the backend actually selected."
	)
		.def_readonly(
			"graphicsContext",
			&osgx::headless::Context::graphicsContext,
			"The valid pbuffer GraphicsContext. Assign it to camera.graphicsContext before realizing "
			"a viewer."
		)
		.def_readonly(
			"backend",
			&osgx::headless::Context::backend,
			"The actual backend used; Auto resolves to EGL or Native."
		)
	;

	m_headless.def(
		"createContext",
		&osgx::headless::createContext,
		"width"_a,
		"height"_a,
		"samples"_a=std::nullopt,
		"backend"_a=osgx::headless::Backend::AUTO,
		"Create an offscreen pbuffer Context. Auto prefers osgX's EGL backend (no X server on Linux) "
		"and otherwise uses OSG's platform-native pbuffer. `samples=None` inherits DisplaySettings; "
		"0 disables MSAA and a positive value requests that many samples. Raises RuntimeError when "
		"the requested context cannot be created."
	);
}

}
