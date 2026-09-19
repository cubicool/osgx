#pragma once

#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/NodeCallback>
#include <osg/Referenced>
#include <osg/Uniform>
#include <osg/Vec2>
#include <osg/Vec3>
#include <osg/observer_ptr>
#include <osg/ref_ptr>
#include <osgGA/GUIEventHandler>
#include <osgViewer/View>

OSGX_ENABLE_WARNINGS

#include <atomic>
#include <functional>

namespace osgx {

// Shows/hides the OS cursor for the view's current window. No-op if the view has no realized
// GraphicsWindow yet. A plain action, not a get/set pair - OSG's GraphicsWindow has no visibility
// getter of its own (useCursor()/setCursor() are write-only), and faking one via a shadow value
// would only be tracking osgx's own writes, not the window's real state.
void setCursorVisible(osgViewer::View& view, bool visible=true);

// Warps the OS cursor to (x, y) in view/event coordinates (the same space as
// GUIEventAdapter::getX()/getY(), NOT window-local pixels) without the jump itself being reported
// as motion to whatever next reads a delta against the pre-warp position.
void warpCursor(osgViewer::View& view, float x, float y);

// Thread-safe, event-driven cursor position tracking - the generalized, picking-agnostic
// version of osgx::PickReadback's positional half (see osgx/Picking.hpp: atomic x/y,
// updateCursor(), x()/y()). Pure state; nothing here requires a pick camera, scene graph, or
// rendering of any kind.
class CursorState: public osg::Referenced {
public:
	// Updates the tracked position, in view/event coordinates (the same space as
	// GUIEventAdapter::getX()/getY()). Called from CursorHandler on every MOVE/DRAG event; safe
	// from any thread.
	void updateCursor(int x, int y) {
		_x.store(x, std::memory_order_relaxed);
		_y.store(y, std::memory_order_relaxed);
	}

	int x() const { return _x.load(std::memory_order_relaxed); }
	int y() const { return _y.load(std::memory_order_relaxed); }

	// Whether the last known position is still inside the window. Refreshed every update
	// traversal by CursorCallback via its own inWindowCheck functor (see CursorCallback's own
	// comment) - OSG's own event stream has no "pointer left the window" event at all
	// (GraphicsWindowX11 only ever requests EnterWindowMask, never LeaveWindowMask - see
	// osgx::platform::isCursorInWindow()'s own comment in Linux.hpp), so this has to be polled
	// rather than reacted to. True (fail-safe) until the first refresh.
	bool inWindow() const { return _inWindow.load(std::memory_order_relaxed); }
	void setInWindow(bool inWindow) { _inWindow.store(inWindow, std::memory_order_relaxed); }

	// Whether x()/y() increase downwards (top of window = smaller y) or upwards - the same
	// per-platform/per-event state osgx::windowToNDC()/unprojectToPlane() (Projection.hpp) require
	// explicitly and document as "not something safe to default": it comes from the real
	// GUIEventAdapter::getMouseYOrientation() of whichever event last updated this state, not a
	// guess. Refreshed by CursorHandler on every MOVE/DRAG event, same cadence as updateCursor().
	// A caller unprojecting x()/y() onto a world plane should pass THIS, not a hardcoded literal -
	// the osgx-aoe example's Mode 1 hardcoded true here first and got a Y-inverted decal, which is
	// exactly the failure this accessor exists to prevent. False (Y_INCREASING_UPWARDS - (0, 0) at
	// the bottom-left, matching GL/OSG's own native NDC/framebuffer convention) is the fail-safe
	// placeholder until the first real event refreshes it.
	bool yIncreasingDownwards() const {
		return _yIncreasingDownwards.load(std::memory_order_relaxed);
	}

	void setYIncreasingDownwards(bool yIncreasingDownwards) {
		_yIncreasingDownwards.store(yIncreasingDownwards, std::memory_order_relaxed);
	}

private:
	std::atomic<int> _x{0};
	std::atomic<int> _y{0};
	std::atomic<bool> _inWindow{true};
	std::atomic<bool> _yIncreasingDownwards{false};
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
// inWindowCheck is an optional std::function<bool()>, polled every traversal to refresh
// CursorState::inWindow() - see that method's own comment for why polling (rather than reacting
// to an event) is necessary at all. Deliberately NOT a hard call into
// osgx::platform::isCursorInWindow(): this file is core (always compiled), while
// isCursorInWindow() lives in the optional, X11-only osgx::platform module (Linux.hpp) - a core
// module must not reach into an optional one (ai/todo-picking.md tracks the same coupling
// PickCameraSync still has today). A caller built with OSGX_PLATFORM supplies its own
// `[cam]{ return osgx::platform::isCursorInWindow(cam); }`; a caller without it, or one that
// doesn't care about the in-window edge case, just omits the argument, and inWindow() stays at
// its fail-safe default forever.
class CursorCallback: public osg::NodeCallback {
public:
	CursorCallback(
		CursorState* state,
		std::function<void(int, int)> fn,
		std::function<bool()> inWindowCheck = nullptr
	):
	_state(state),
	_fn(std::move(fn)),
	_inWindowCheck(std::move(inWindowCheck)) {}

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	osg::ref_ptr<CursorState> _state;
	std::function<void(int, int)> _fn;
	std::function<bool()> _inWindowCheck;
};

// Convenience factory: builds a CursorCallback that pushes state's live position and in-window
// flag into `uniform` every update traversal, as osg::Vec3(x, y, inWindow ? 1.0 : 0.0) in the same
// view/event coordinate space as CursorState::x()/y() - `uniform` must be FLOAT_VEC3. No new class
// here, and deliberately not a StateAttribute: osgx::LightSet::apply() (see PBR.cpp) already tried
// pushing a uniform live from inside a StateAttribute via OSG's Program-targeted uniform-push
// machinery (applyShaderCompositionUniform()/getLastAppliedProgramObject()) for osgx_lightCount,
// and both broke the moment any sibling Program elsewhere in the same frame used
// StateAttribute::OVERRIDE (confirmed live 2026-09-03 - see PBR.hpp's own history comment).
// A plain Uniform refreshed by an ordinary update callback goes through OSG's normal per-StateSet
// uniform stack instead and never shares that failure mode, so that's what this builds - just a
// named CursorCallback construction, not a new mechanism.
//
// The caller owns `uniform`: create it and addUniform() it onto whatever StateSet needs to read
// it (picking its own name), then install the returned callback via setUpdateCallback() same as
// any other CursorCallback. inWindowCheck is forwarded as-is - see CursorCallback's own comment.
osg::ref_ptr<CursorCallback> makeCursorUniformCallback(
	CursorState* state,
	osg::Uniform* uniform,
	std::function<bool()> inWindowCheck = nullptr
);

// Software "soft capture" of the cursor: hides it and re-centers it every time it moves,
// accumulating the raw motion as a delta instead of exposing absolute screen position -- the
// standard hide+warp+accumulate trick for turntable/FPS-style look controls that need unbounded
// relative motion regardless of physical screen size. Motivated by osgx::OrbitAxisManipulator
// (see osgx/Manipulators.hpp), which today maps cursor position directly to orbit/height and so
// can't turn past the physical screen edge - but this is a general primitive, not
// manipulator-specific; compose it with anything that wants relative-only input.
//
// This is NOT true OS-level pointer confinement: nothing stops the cursor from visibly darting to
// the edge of the screen for one frame between the warp and the next event on some window
// managers/compositors. Real confinement needs XGrabPointer (Linux) and platform equivalents
// elsewhere; not yet implemented - see TODO.md's "osgx::platform later work".
//
// Deliberately NOT wired into OrbitAxisManipulator itself, even though both are plain osgx:: now:
// composing the two at the application level (add both as event handlers, feed consume()'d
// deltas into the manipulator via orbitByDelta()) keeps OrbitAxisManipulator ignorant of any one
// specific input-capture scheme, so a different one can be swapped in without touching
// Manipulators.hpp. See docs/CORE.md's osgx/Cursor.hpp section for the worked composition.
//
// Usage: add as an ordinary event handler (addEventHandler()), toggle setCaptured() (e.g. on a
// button press), and poll consume() once per update traversal for the accumulated delta.
class CursorCapture: public osgGA::GUIEventHandler {
public:
	explicit CursorCapture(osgViewer::View& view): _view(&view) {}

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

	osg::Vec2 _center{0.0f, 0.0f};
	osg::Vec2 _accum{0.0f, 0.0f};
};

}
