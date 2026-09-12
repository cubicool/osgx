#pragma once

#include <string>
#include <vector>

namespace osg { class Camera; }
namespace osgViewer { class Viewer; }

namespace osgx::platform {

// Pins the viewer's native X11 window above other windows via the EWMH `_NET_WM_STATE`
// ClientMessage protocol every window manager is expected to honor; see:
// https://specifications.freedesktop.org/wm-spec/latest/ar01s05.html#NETWMSTATE
void alwaysOnTop(osgViewer::Viewer& viewer, bool enabled=true);

// A single XRandR monitor, in real (possibly non-adjacent/overlapping) root-window coordinates --
// do not assume a flush left-to-right layout.
struct Monitor {
	std::string name;

	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;

	bool primary = false;
};

// Queries the real XRandR monitor layout (position/size in root-window coordinates). Monitors
// are NOT assumed to be flush/adjacent - use these rects directly for placement math.
std::vector<Monitor> listMonitors();

// Repositions (and optionally resizes) an already-realized X11 window in one call: moves the real
// X11 window via XMoveResizeWindow, then calls GraphicsContext::resized() so OSG's own viewport/
// camera bookkeeping stays in sync. Pass width/height <= 0 to keep the window's current size.
void moveWindow(osgViewer::Viewer& viewer, int x, int y, int width=-1, int height=-1);

// Retitles the real X11 window (title bar + icon/taskbar name), via
// GraphicsWindowX11::setWindowName() - osg::GraphicsContext already exposes this as a
// real cross-platform virtual (osgViewer::GraphicsWindow::setWindowName(), implemented
// for X11 via XStoreName/XSetIconName), but GraphicsWindowX11 itself is not registered
// in pyosg's Python bindings (only the methodless GraphicsWindow base is), so pybind11's
// RTTI-based polymorphic dispatch can't reach it from `camera.graphicsContext` - this
// does the dynamic_cast in C++ instead, the same shape as alwaysOnTop()/moveWindow()
// above, so nothing X11-specific needs to be exposed to Python at all.
void setWindowTitle(osgViewer::Viewer& viewer, const std::string& title);

// True if the mouse pointer is currently within the given camera's own X11 window bounds.
// Queries X11 directly (XQueryPointer) rather than tracking GUIEventAdapter MOVE events,
// because GraphicsWindowX11 only ever requests EnterWindowMask, never LeaveWindowMask - there
// is no "pointer left the window" event in OSG's own event stream at all to hook off of.
//
// `camera` overload exists so osgx::PickCameraSync (Picking.cpp, #ifdef OSGX_PLATFORM) can call
// this directly on the viewer-camera pointer it already holds, to transparently invalidate
// continuous/hover picking when the pointer leaves - see PickReadback::invalidate()'s own
// comment for why that's needed at all (the pick camera's sub-frustum otherwise keeps
// re-rendering at the last MOVE event's position forever, reporting stale hover with the cursor
// nowhere near the window). Callers with just a Viewer can use the second overload instead.
//
// Both return true (fail open, don't false-trigger an invalidation) if the graphics context
// isn't a real X11 window, or the query can't be answered (pointer on a different screen
// entirely).
bool isCursorInWindow(osg::Camera* camera);
bool isCursorInWindow(osgViewer::Viewer& viewer);

}
