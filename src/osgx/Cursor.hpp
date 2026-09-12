#pragma once

#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/NodeCallback>
#include <osg/Referenced>
#include <osg/Vec2>
#include <osg/observer_ptr>
#include <osg/ref_ptr>
#include <osgGA/GUIEventHandler>
#include <osgViewer/View>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <functional>

namespace osgx::platform {

// Shows/hides the OS cursor for the view's current window. No-op if the view has no realized
// GraphicsWindow yet. A plain action, not a get/set pair - OSG's GraphicsWindow has no visibility
// getter of its own (useCursor()/setCursor() are write-only), and faking one via a shadow value
// would only be tracking osgx's own writes, not the window's real state.
void setCursorVisible(osgViewer::View& view, bool visible=true);

// Warps the OS pointer to (x, y) in view/event coordinates (the same space as
// GUIEventAdapter::getX()/getY(), NOT window-local pixels) without the jump itself being reported
// as motion to whatever next reads a delta against the pre-warp position.
void warpPointer(osgViewer::View& view, float x, float y);

// Thread-safe, event-driven cursor position tracking - the generalized, picking-agnostic
// version of osgx::PickReadback's positional half (see osgx/Picking.hpp: atomic x/y,
// updateMouse(), x()/y()). Pure state; nothing here requires a pick camera, scene graph, or
// rendering of any kind.
class CursorState: public osg::Referenced {
public:
	// Updates the tracked position, in view/event coordinates (the same space as
	// GUIEventAdapter::getX()/getY()). Called from CursorHandler on every MOVE/DRAG event; safe
	// from any thread.
	void updateMouse(int x, int y) {
		_x.store(x, std::memory_order_relaxed);
		_y.store(y, std::memory_order_relaxed);
	}

	int x() const { return _x.load(std::memory_order_relaxed); }
	int y() const { return _y.load(std::memory_order_relaxed); }

	// Whether the last known position is still inside the window. Refreshed every update
	// traversal by CursorCallback via isCursorInWindow() - OSG's own event stream has no
	// "pointer left the window" event at all (GraphicsWindowX11 only ever requests
	// EnterWindowMask, never LeaveWindowMask - see isCursorInWindow()'s own comment in
	// Linux.hpp), so this has to be polled rather than reacted to. True (fail-safe) until the
	// first refresh.
	bool inWindow() const { return _inWindow.load(std::memory_order_relaxed); }
	void setInWindow(bool inWindow) { _inWindow.store(inWindow, std::memory_order_relaxed); }

private:
	std::atomic<int> _x{0};
	std::atomic<int> _y{0};
	std::atomic<bool> _inWindow{true};
};

// GUIEventHandler that forwards MOVE/DRAG events into a CursorState. Same shape as
// osgx::PickHandler's continuous-mode branch, minus everything about IDs/clicks. Always returns
// false so the active manipulator (or any other handler) still sees the event.
class CursorHandler: public osgGA::GUIEventHandler {
public:
	// Retains state for this handler's lifetime.
	explicit CursorHandler(CursorState* state): _state(state) {}

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&) override;

private:
	osg::ref_ptr<CursorState> _state;
};

// NodeCallback that fires a std::function<void(int x, int y)> every update traversal
// (unconditionally, same style as osgx::PickCameraSync) with the current cursor position. The
// consumer decides what "follow" means - reposition a HUD quad, unproject onto a world plane,
// drive a rendered software cursor, whatever. Not cursor-specific despite living here: nothing
// about this requires the thing being driven to look like a cursor.
//
// Also refreshes state's inWindow() via isCursorInWindow() every traversal, the same way
// osgx::PickCameraSync does for osgx::PickReadback - see CursorState::inWindow()'s own comment
// for why polling is necessary. viewerCam is non-owning (observer_ptr); a null/expired camera
// just skips the inWindow() refresh, leaving it at its last known value.
class CursorCallback: public osg::NodeCallback {
public:
	CursorCallback(CursorState* state, osg::Camera* viewerCam, std::function<void(int, int)> fn):
	_state(state),
	_viewerCam(viewerCam),
	_fn(std::move(fn)) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	osg::ref_ptr<CursorState> _state;
	osg::observer_ptr<osg::Camera> _viewerCam;
	std::function<void(int, int)> _fn;
};

// Software "soft capture" of the mouse: hides the OS cursor and re-centers the pointer every time
// it moves, accumulating the raw motion as a delta instead of exposing absolute screen position --
// the standard hide+warp+accumulate trick for turntable/FPS-style look controls that need
// unbounded relative motion regardless of physical screen size. Motivated by
// osgx::OrbitAxisManipulator (see osgx/Manipulators.hpp), which today maps mouse position directly
// to orbit/height and so can't turn past the physical screen edge - but this is a general
// primitive, not manipulator-specific; compose it with anything that wants relative-only input.
//
// This is NOT true OS-level pointer confinement: nothing stops the cursor from visibly darting to
// the edge of the screen for one frame between the warp and the next event on some window
// managers/compositors. Real confinement needs XGrabPointer (Linux) and platform equivalents
// elsewhere; not yet implemented - see TODO.md's "osgx::platform later work".
//
// Deliberately NOT wired into OrbitAxisManipulator itself: Manipulators.hpp is part of the
// always-available osgx.hpp umbrella, while osgx::platform is opt-in (X11/EGL/GBM), so the
// manipulator must not gain a hard dependency on it. Compose the two at the application level
// instead - add both as event handlers and feed consume()'d deltas into the manipulator.
//
// Usage: add as an ordinary event handler (addEventHandler()), toggle setCaptured() (e.g. on a
// mouse-button press), and poll consume() once per update traversal for the accumulated delta.
class PointerCapture: public osgGA::GUIEventHandler {
public:
	explicit PointerCapture(osgViewer::View& view): _view(&view) {}

	// Hides the cursor and starts warp+accumulate when true; restores the cursor and stops when
	// false. Disabled by default - callers opt in explicitly.
	void setCaptured(bool captured);
	bool isCaptured() const { return _captured; }

	// Accumulated (dx, dy) since the last call, in view/event coordinate units (the same units as
	// GUIEventAdapter::getX()/getY()). Resets the accumulator to zero so repeated polling - e.g.
	// once per update traversal - never double-counts.
	osg::Vec2 consume();

	bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa) override;

private:
	osg::observer_ptr<osgViewer::View> _view;

	bool _captured = false;
	bool _recenterPending = false; // need to actively warp on the next MOVE/DRAG (capture-start/resize)
	bool _echoPending = false; // next MOVE/DRAG is presumed to be this handler's own warp echo

	float _centerX = 0.0f;
	float _centerY = 0.0f;

	osg::Vec2 _accum{0.0f, 0.0f};
};

}
