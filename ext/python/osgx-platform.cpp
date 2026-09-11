#include "osgx-python.hpp"
#include "osgx/Cursor.hpp"
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

// osgx::platform -- X11/XRandr window helpers (alwaysOnTop, listMonitors, moveWindow) plus, when
// built (see OSGX_WITH_EGL/OSGX_WITH_GBM in CMakeLists.txt), the EGL- and GBM/DRM-backed
// GraphicsWindow factories. Moved here from OSG.py's pyosg/linux -- was never actually
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
		"flush/adjacent -- see listMonitors()."
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
		"are NOT assumed to be flush/adjacent -- use these rects directly for placement math."
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
		"True if the mouse pointer is currently within this viewer's real X11 window bounds. "
		"Queries X11 directly -- there is no 'pointer left the window' GUIEventAdapter event in "
		"OSG's own event stream. osgx.picking.PickCameraSync already checks this automatically "
		"every frame to invalidate continuous/hover picking when the pointer leaves the window "
		"entirely -- this standalone binding is for anything else that wants the same check."
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

	m_platform.def(
		"setCursorVisible",
		&osgx::platform::setCursorVisible,
		"view"_a,
		"visible"_a=true,
		"Show/hide the OS cursor for the view's current window."
	);

	m_platform.def(
		"warpPointer",
		&osgx::platform::warpPointer,
		"view"_a,
		"x"_a,
		"y"_a,
		"Warp the OS pointer to (x, y) in view/event coordinates (GUIEventAdapter.x/y space, not "
		"window-local pixels) without the jump itself registering as motion."
	);

	// Thread-safe, event-driven cursor position tracking -- the generalized, picking-agnostic
	// version of osgx.picking.PickReadback's positional half. CursorHandler/CursorCallback retain
	// it in C++, matching the osg::ref_ptr holder used here.
	py::class_<osgx::platform::CursorState, osg::ref_ptr<osgx::platform::CursorState>>(
		m_platform,
		"CursorState",
		"Thread-safe, event-driven cursor position tracking -- the generalized, picking-agnostic "
		"version of osgx.picking.PickReadback's positional half. Pure state; nothing here "
		"requires a pick camera, scene graph, or rendering of any kind."
	)
		.def(py::init<>())
		.def(
			"updateMouse", &osgx::platform::CursorState::updateMouse, "x"_a, "y"_a,
			"Updates the tracked position, in view/event coordinates (GUIEventAdapter.x/y "
			"space). Called from CursorHandler on every MOVE/DRAG event; safe from any thread."
		)
		.def_property_readonly(
			"x", &osgx::platform::CursorState::x, "Last tracked cursor X position."
		)
		.def_property_readonly(
			"y", &osgx::platform::CursorState::y, "Last tracked cursor Y position."
		)
		.def_property(
			"inWindow",
			&osgx::platform::CursorState::inWindow,
			&osgx::platform::CursorState::setInWindow,
			"Whether the last known position is still inside the window. Refreshed every update "
			"traversal by CursorCallback via isCursorInWindow() -- there is no 'pointer left the "
			"window' GUIEventAdapter event to react to instead. True (fail-safe) until the first "
			"refresh."
		)
	;

	py::class_<
		osgx::platform::CursorHandler,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::platform::CursorHandler>
	>(
		m_platform,
		"CursorHandler",
		"Forwards MOVE/DRAG events into a CursorState. Same shape as osgx.picking.PickHandler's "
		"continuous branch, minus everything about IDs/clicks."
	)
		.def(
			py::init<osgx::platform::CursorState*>(),
			"state"_a,
			"Retains state. "
			"Every MOVE/DRAG event updates its tracked position. Always returns False so the "
			"active manipulator (or any other handler) still sees the event."
		)
	;

	py::class_<
		osgx::platform::CursorCallback,
		osg::NodeCallback,
		osg::ref_ptr<osgx::platform::CursorCallback>
	>(
		m_platform,
		"CursorCallback",
		"Fires a callable(x, y) every update traversal (unconditionally, same style as "
		"osgx.picking.PickCameraSync) with the current cursor position. The consumer decides "
		"what 'follow' means -- reposition a HUD quad, unproject onto a world plane, drive a "
		"rendered software cursor, whatever."
	)
		.def(
			py::init<osgx::platform::CursorState*, osg::Camera*, std::function<void(int, int)>>(),
			"state"_a,
			"viewerCam"_a,
			"fn"_a,
			"Retains state. Install via setUpdateCallback(). Also refreshes state.inWindow via "
			"isCursorInWindow() against viewerCam every traversal, the same way PickCameraSync "
			"does for PickReadback."
		)
	;

	// Software hide+warp+accumulate mouse capture for turntable/FPS-style relative-motion look
	// controls. NOT true OS-level pointer confinement -- see osgx/Cursor.hpp.
	py::class_<
		osgx::platform::PointerCapture,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::platform::PointerCapture>
	>(
		m_platform,
		"PointerCapture",
		"Software hide+warp+accumulate mouse capture for turntable/FPS-style relative-motion look "
		"controls. NOT true OS-level pointer confinement -- see osgx/Cursor.hpp."
	)
		.def(
			py::init<osgViewer::View&>(), "view"_a,
			"Wraps `view`; capture starts disabled -- set .captured = True to begin hiding+warping+accumulating."
		)
		.def_property(
			"captured",
			&osgx::platform::PointerCapture::isCaptured,
			&osgx::platform::PointerCapture::setCaptured,
			"Whether capture is active: while True, hides the cursor and re-centers it on every "
			"MOVE/DRAG event, accumulating the raw delta. Disabled by default."
		)
		.def(
			"consume", &osgx::platform::PointerCapture::consume,
			"Returns the accumulated delta since the last consume() call and resets it to zero. "
			"Poll once per update traversal."
		)
	;
}

}
