#include "osgx/Linux.hpp"

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osgViewer/Viewer>
#include <osgViewer/api/X11/GraphicsWindowX11>

OSGX_ENABLE_WARNINGS

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

namespace osgx::platform {

namespace {

// The EWMH `_NET_WM_STATE` ClientMessage protocol every X11 window manager is expected to
// honor; see: https://specifications.freedesktop.org/wm-spec/latest/ar01s05.html#NETWMSTATE
void sendNetWmState(Display* display, Window window, bool add, const char* stateName) {
	Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
	Atom state = XInternAtom(display, stateName, False);

	XEvent event{};

	event.xclient.type = ClientMessage;
	event.xclient.window = window;
	event.xclient.message_type = wmState;
	event.xclient.format = 32;
	event.xclient.data.l[0] = add ? 1 : 0; // _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE
	event.xclient.data.l[1] = static_cast<long>(state);
	event.xclient.data.l[2] = 0;
	event.xclient.data.l[3] = 1; // source indication: normal application
	event.xclient.data.l[4] = 0;

	XSendEvent(
		display,
		DefaultRootWindow(display),
		False,
		SubstructureRedirectMask | SubstructureNotifyMask,
		&event
	);

	XFlush(display);
}

}

void alwaysOnTop(osgViewer::Viewer& viewer, bool enabled) {
	auto* camera = viewer.getCamera();

	if(!camera) return;

	auto* gw = dynamic_cast<osgViewer::GraphicsWindowX11*>(camera->getGraphicsContext());

	if(!gw) return;

	sendNetWmState(gw->getEventDisplay(), gw->getWindow(), enabled, "_NET_WM_STATE_ABOVE");
}

std::vector<Monitor> listMonitors() {
	std::vector<Monitor> monitors;

	Display* display = XOpenDisplay(nullptr);

	if(!display) return monitors;

	int count = 0;
	XRRMonitorInfo* info = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);

	if(info) {
		for(int i = 0; i < count; i++) {
			Monitor monitor;

			char* name = XGetAtomName(display, info[i].name);

			monitor.name = name ? name : "";
			monitor.x = info[i].x;
			monitor.y = info[i].y;
			monitor.width = info[i].width;
			monitor.height = info[i].height;
			monitor.primary = info[i].primary != 0;

			if(name) XFree(name);

			monitors.push_back(monitor);
		}

		XRRFreeMonitors(info);
	}

	XCloseDisplay(display);

	return monitors;
}

void moveWindow(osgViewer::Viewer& viewer, int x, int y, int width, int height) {
	auto* camera = viewer.getCamera();

	if(!camera) return;

	auto* gw = dynamic_cast<osgViewer::GraphicsWindowX11*>(camera->getGraphicsContext());

	if(!gw) return;

	Display* display = gw->getEventDisplay();
	Window window = gw->getWindow();

	const osg::GraphicsContext::Traits* traits = gw->getTraits();
	int w = width > 0 ? width : traits->width;
	int h = height > 0 ? height : traits->height;

	XMoveResizeWindow(display, window, x, y, static_cast<unsigned int>(w), static_cast<unsigned int>(h));
	XFlush(display);

	gw->resized(x, y, w, h);
}

void setWindowTitle(osgViewer::Viewer& viewer, const std::string& title) {
	auto* camera = viewer.getCamera();

	if(!camera) return;

	auto* gw = dynamic_cast<osgViewer::GraphicsWindowX11*>(camera->getGraphicsContext());

	if(!gw) return;

	gw->setWindowName(title);
}

bool isCursorInWindow(osg::Camera* camera) {
	if(!camera) return true;

	auto* gw = dynamic_cast<osgViewer::GraphicsWindowX11*>(camera->getGraphicsContext());

	// !gw->valid() covers window-close teardown: GraphicsWindowX11::closeImplementation()
	// (src/osgViewer/GraphicsWindowX11.cpp) XCloseDisplay()s _eventDisplay, zeroes _window, and
	// sets _valid = false, in that order, but PickCameraSync's update callback (the only caller
	// of this) can still run one more traversal after close starts - confirmed via a real
	// coredump (2026-08-22): XQueryPointer() on the by-then-dangling Display* segfaulted hard,
	// not a catchable X error, the instant the window closed. Checked before touching
	// getEventDisplay()/getWindow() at all, not after - both are already zeroed by the time
	// _valid flips false, so this alone is the real guard, not defense in depth.
	if(!gw || !gw->valid()) return true;

	Display* display = gw->getEventDisplay();
	Window window = gw->getWindow();

	Window root, child;
	int rootX, rootY, winX, winY;
	unsigned int mask;

	// False here means "different screen," not "different window" - win_x/win_y are always
	// reported relative to `window`'s own origin regardless of what the pointer is actually
	// over, which is exactly the bounds check below wants.
	if(!XQueryPointer(display, window, &root, &child, &rootX, &rootY, &winX, &winY, &mask)) {
		return false;
	}

	const osg::GraphicsContext::Traits* traits = gw->getTraits();

	return winX >= 0 && winY >= 0 && winX < traits->width && winY < traits->height;
}

bool isCursorInWindow(osgViewer::Viewer& viewer) {
	return isCursorInWindow(viewer.getCamera());
}

}
