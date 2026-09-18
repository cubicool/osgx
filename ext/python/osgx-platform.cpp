#include "osgx-python.hpp"
#include "osgx/Linux.hpp"

OSGX_DISABLE_WARNINGS

#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#ifdef OSGX_EGL
#include "osgx/GraphicsWindowEGL.hpp"
#endif

#ifdef OSGX_GBM
#include "osgx/GraphicsWindowGBM.hpp"
#endif

using namespace std::string_literals;

namespace osgx_python {

// osgx::platform - X11/XRandr window helpers (alwaysOnTop, listMonitors, moveWindow) plus, when
// built (see OSGX_WITH_EGL/OSGX_WITH_GBM in CMakeLists.txt), the EGL- and GBM/DRM-backed
// GraphicsWindow factories. Moved here from OSG.py's pyosg/linux - was never actually
// OSG.py-specific, so osgx is the right home; see osgx/Linux.hpp.
void bind_platform(py::module_& m_platform) {
	m_platform.def(
		"alwaysOnTop",
		&osgx::platform::alwaysOnTop,
		"viewer"_a,
		"enabled"_a=true,
		"Pin the viewer's native X11 window above other windows (EWMH _NET_WM_STATE_ABOVE)."
	);

	py::class_<osgx::platform::Monitor>(
		m_platform,
		"Monitor",
		"One XRandR monitor, in real root-window coordinates. Monitors are NOT assumed to be "
		"flush/adjacent - see listMonitors()."
	)
		.def_readonly("name", &osgx::platform::Monitor::name, "This monitor's XRandR output name (e.g. 'DP-1', 'HDMI-0').")
		.def_readonly("x", &osgx::platform::Monitor::x, "Left edge, in root-window pixels.")
		.def_readonly("y", &osgx::platform::Monitor::y, "Top edge, in root-window pixels.")
		.def_readonly("width", &osgx::platform::Monitor::width, "Width in pixels.")
		.def_readonly("height", &osgx::platform::Monitor::height, "Height in pixels.")
		.def_readonly("primary", &osgx::platform::Monitor::primary, "True if this is the XRandR-designated primary monitor.")
		.def("__repr__", [](const osgx::platform::Monitor& self) {
			return
				"Monitor(name='"s + self.name + "', "
				"x="s + std::to_string(self.x) + ", "
				"y="s + std::to_string(self.y) + ", "
				"width="s + std::to_string(self.width) + ", "
				"height="s + std::to_string(self.height) + ", "
				"primary="s + (self.primary ? "True"s : "False"s) + ")"s
			;
		}, "A readable Monitor(name=..., x=..., y=..., width=..., height=..., primary=...) representation.")
	;

	m_platform.def(
		"listMonitors",
		&osgx::platform::listMonitors,
		"Query the real XRandR monitor layout (position/size in root-window coordinates). Monitors "
		"are NOT assumed to be flush/adjacent - use these rects directly for placement math."
	);

	m_platform.def(
		"moveWindow",
		&osgx::platform::moveWindow,
		"viewer"_a,
		"x"_a,
		"y"_a,
		"width"_a=-1,
		"height"_a=-1,
		"Reposition (and optionally resize) an already-realized X11 window, keeping OSG's own "
		"viewport bookkeeping in sync. Pass width/height <= 0 to keep the current size."
	);

	m_platform.def(
		"setWindowTitle",
		&osgx::platform::setWindowTitle,
		"viewer"_a,
		"title"_a,
		"Retitle the viewer's native X11 window (title bar + icon/taskbar name)."
	);

	m_platform.def(
		"isCursorInWindow",
		py::overload_cast<osgViewer::Viewer&>(&osgx::platform::isCursorInWindow),
		"viewer"_a,
		"True if the cursor is currently within this viewer's real X11 window bounds. Queries "
		"X11 directly - there is no 'pointer left the window' GUIEventAdapter event in OSG's own "
		"event stream. osgx.picking.PickCameraSync already checks this automatically every frame "
		"to invalidate continuous/hover picking when the cursor leaves the window entirely - this "
		"standalone binding is for anything else that wants the same check."
	);

#ifdef OSGX_EGL
	m_platform.def(
		"createEGLWindow",
		&osgx::platform::createEGLWindow,
		"traits"_a,
		"Create an X11 window driven by EGL (instead of GLX). Skeleton/proof-of-concept: assign "
		"the result to `camera.graphicsContext`."
	);
#endif

#ifdef OSGX_GBM
	m_platform.def(
		"createGBMWindow",
		&osgx::platform::createGBMWindow,
		"traits"_a,
		"Create a direct-scanout DRM/KMS+GBM window (no X11, no window manager). Skeleton/proof-"
		"of-concept: requires exclusive DRM master access, so it will fail under a running X "
		"server. Assign the result to `camera.graphicsContext`."
	);
#endif

}

}
