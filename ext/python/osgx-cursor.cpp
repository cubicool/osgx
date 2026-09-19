#include "osgx-python.hpp"
#include "osgx/Cursor.hpp"

OSGX_DISABLE_WARNINGS

#include <osgViewer/View>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

// osgx::Cursor.hpp used to bind onto the osgx.platform submodule, back when it lived in the
// osgx::platform C++ namespace - it never actually needed anything platform-specific
// (setCursorVisible()/warpCursor()/CursorState/CursorHandler/CursorCallback/CursorCapture all
// touch only core OSG/osgGA/osgViewer APIs), so it collapsed into plain osgx:: this week and its
// bindings move here, straight onto the top-level module, same as pbr/shadow/gbuffer/ibl/picking
// did before it (see osgx.cpp). osgx.platform.isCursorInWindow() remains the one genuinely
// platform-specific piece (X11-only) - see ext/python/osgx-platform.cpp.
void bind_cursor(py::module_& m) {
	m.def(
		"setCursorVisible",
		&osgx::setCursorVisible,
		"view"_a,
		"visible"_a=true,
		"Show/hide the OS cursor for the view's current window."
	);

	m.def(
		"warpCursor",
		&osgx::warpCursor,
		"view"_a,
		"x"_a,
		"y"_a,
		"Warp the OS pointer to (x, y) in view/event coordinates (GUIEventAdapter.x/y space, not "
		"window-local pixels) without the jump itself registering as motion."
	);

	// Thread-safe, event-driven cursor position tracking - the generalized, picking-agnostic
	// version of osgx.picking.PickReadback's positional half. CursorHandler/CursorCallback retain
	// it in C++, matching the osg::ref_ptr holder used here.
	py::class_<osgx::CursorState, osg::ref_ptr<osgx::CursorState>>(
		m,
		"CursorState",
		"Thread-safe, event-driven cursor position tracking - the generalized, picking-agnostic "
		"version of osgx.picking.PickReadback's positional half. Pure state; nothing here "
		"requires a pick camera, scene graph, or rendering of any kind."
	)
		.def(py::init<>())
		.def(
			"updateCursor", &osgx::CursorState::updateCursor, "x"_a, "y"_a,
			"Updates the tracked position, in view/event coordinates (GUIEventAdapter.x/y "
			"space). Called from CursorHandler on every MOVE/DRAG event; safe from any thread."
		)
		.def_property_readonly(
			"x", &osgx::CursorState::x, "Last tracked cursor X position."
		)
		.def_property_readonly(
			"y", &osgx::CursorState::y, "Last tracked cursor Y position."
		)
		.def_property(
			"inWindow",
			&osgx::CursorState::inWindow,
			&osgx::CursorState::setInWindow,
			"Whether the last known position is still inside the window. Refreshed every update "
			"traversal by CursorCallback via an optional inWindowCheck callable - there is no "
			"'pointer left the window' GUIEventAdapter event to react to instead. True (fail-safe) "
			"until the first refresh."
		)
		.def_property(
			"yIncreasingDownwards",
			&osgx::CursorState::yIncreasingDownwards,
			&osgx::CursorState::setYIncreasingDownwards,
			"Whether x/y increase downwards or upwards - the real GUIEventAdapter.mouseYOrientation "
			"of whichever event last updated this state, refreshed by CursorHandler on every "
			"MOVE/DRAG event. Pass this (not a hardcoded literal) as windowToNDC()/"
			"unprojectToPlane()'s own yIncreasingDownwards argument. False (fail-safe - (0, 0) at "
			"the bottom-left, matching GL/OSG's own native convention) until the first refresh."
		)
	;

	py::class_<
		osgx::CursorHandler,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::CursorHandler>
	>(
		m,
		"CursorHandler",
		"Forwards MOVE/DRAG events into a CursorState. Same shape as osgx.picking.PickHandler's "
		"continuous branch, minus everything about IDs/clicks."
	)
		.def(
			py::init<osgx::CursorState*>(),
			"state"_a,
			"Retains state. "
			"Every MOVE/DRAG event updates its tracked position. Always returns False so the "
			"active manipulator (or any other handler) still sees the event."
		)
	;

	py::class_<
		osgx::CursorCallback,
		osg::NodeCallback,
		osg::ref_ptr<osgx::CursorCallback>
	>(
		m,
		"CursorCallback",
		"Fires a callable(x, y) every update traversal (unconditionally, same style as "
		"osgx.picking.PickCameraSync) with the current cursor position. The consumer decides "
		"what 'follow' means - reposition a HUD quad, unproject onto a world plane, drive a "
		"rendered software cursor, whatever."
	)
		.def(
			py::init<osgx::CursorState*, std::function<void(int, int)>>(),
			"state"_a,
			"fn"_a,
			"Retains state. Install via setUpdateCallback(). state.inWindow is never refreshed "
			"without an inWindowCheck callable - see the other overload."
		)
		.def(
			py::init<osgx::CursorState*, std::function<void(int, int)>, std::function<bool()>>(),
			"state"_a,
			"fn"_a,
			"inWindowCheck"_a,
			"Retains state. Install via setUpdateCallback(). inWindowCheck is polled every "
			"traversal to refresh state.inWindow - e.g. "
			"lambda: osgx.platform.isCursorInWindow(viewer) when osgx.platform is available."
		)
	;

	// Convenience factory, not a new class: builds a CursorCallback that pushes state's live
	// position + in-window flag into `uniform` (must be FLOAT_VEC3: x, y, inWindow ? 1.0 : 0.0)
	// every update traversal. See its own C++ comment (osgx/Cursor.hpp) for why this is a plain
	// Uniform + update callback rather than a StateAttribute pushing the uniform directly
	// (osgx.LightSet.apply()'s own history already proved that unreliable under a sibling
	// Program's StateAttribute.OVERRIDE elsewhere in the same frame).
	m.def(
		"makeCursorUniformCallback",
		&osgx::makeCursorUniformCallback,
		"state"_a,
		"uniform"_a,
		"inWindowCheck"_a,
		"Builds a CursorCallback pushing state's live position + in-window flag into `uniform` "
		"(FLOAT_VEC3) every update traversal. Install the result via setUpdateCallback()."
	);

	m.def(
		"makeCursorUniformCallback",
		[](osgx::CursorState* state, osg::Uniform* uniform) {
			return osgx::makeCursorUniformCallback(state, uniform);
		},
		"state"_a,
		"uniform"_a,
		"Same as the other overload, without an inWindowCheck - the uniform's in-window component "
		"then just reflects state.inWindow's last known value (true until first set)."
	);

	// Software hide+warp+accumulate cursor capture for turntable/FPS-style relative-motion look
	// controls. NOT true OS-level pointer confinement - see osgx/Cursor.hpp.
	py::class_<
		osgx::CursorCapture,
		osgGA::GUIEventHandler,
		osg::ref_ptr<osgx::CursorCapture>
	>(
		m,
		"CursorCapture",
		"Software hide+warp+accumulate cursor capture for turntable/FPS-style relative-motion "
		"look controls. NOT true OS-level pointer confinement - see osgx/Cursor.hpp."
	)
		.def(
			py::init<osgViewer::View&>(), "view"_a,
			"Wraps `view`; capture starts disabled - set .captured = True to begin hiding+warping+accumulating."
		)
		.def_property(
			"captured",
			&osgx::CursorCapture::isCaptured,
			&osgx::CursorCapture::setCaptured,
			"Whether capture is active: while True, hides the cursor and re-centers it on every "
			"MOVE/DRAG event, accumulating the raw delta. Disabled by default."
		)
		.def(
			"consume", &osgx::CursorCapture::consume,
			"Returns the accumulated delta since the last consume() call and resets it to zero. "
			"Poll once per update traversal."
		)
	;
}

}
