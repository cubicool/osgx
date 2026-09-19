import osgx

from OpenSceneGraph import *

def test_cursor_state_position_and_in_window():
	state = osgx.CursorState()

	assert (state.x, state.y) == (0, 0)
	assert state.inWindow == True # fail-safe default until the first refresh
	# fail-safe default until the first refresh -- (0, 0) at the bottom-left, matching GL/OSG's
	# own native convention.
	assert state.yIncreasingDownwards == False

	state.updateCursor(10, 20)

	assert (state.x, state.y) == (10, 20)

	state.inWindow = False

	assert state.inWindow == False

	state.yIncreasingDownwards = True

	assert state.yIncreasingDownwards == True

def test_cursor_handler_is_gui_event_handler():
	# osgGA.GUIEventAdapter has no Python constructor (upstream OpenSceneGraph.py binding), so
	# there's no way to synthesize a real MOVE/DRAG event from Python and drive handle()
	# directly -- headless coverage here is deliberately limited to construction/type, the same
	# limit osgx_Callbacks.py's DrawCallback tests already document for the same reason.
	handler = osgx.CursorHandler(osgx.CursorState())

	assert isinstance(handler, osgGA.GUIEventHandler)

def test_cursor_callback_fires_via_update_traversal():
	# Real C++ dispatch through Node.accept(UpdateVisitor()), not a direct Python call -- same
	# pattern osgx_Callbacks.py's NodeCallbacksGroup tests use, and for the same reason: proves
	# CursorCallback::operator() actually wires into a real update traversal.
	state = osgx.CursorState()

	state.updateCursor(3, 4)

	calls = []
	cb = osgx.CursorCallback(state, lambda x, y: calls.append((x, y)))

	n = osg.Node()
	n.updateCallback = cb
	n.accept(osgUtil.UpdateVisitor())

	assert calls == [(3, 4)]
	# No inWindowCheck was given -- inWindow must stay at its fail-safe default, untouched.
	assert state.inWindow == True

def test_cursor_callback_in_window_check_polled_via_update_traversal():
	state = osgx.CursorState()
	checks = []

	def in_window_check():
		checks.append(True)

		return False

	cb = osgx.CursorCallback(state, lambda x, y: None, in_window_check)

	n = osg.Node()
	n.updateCallback = cb
	n.accept(osgUtil.UpdateVisitor())

	assert len(checks) == 1
	assert state.inWindow == False

def test_cursor_capture_construction_and_properties():
	# handle()'s hide+warp+accumulate logic needs a real MOVE/DRAG GUIEventAdapter to drive it
	# (same osgGA.GUIEventAdapter constructor gap as CursorHandler above) -- coverage here is
	# limited to construction and the captured/consume() surface.
	view = osgViewer.Viewer()
	capture = osgx.CursorCapture(view)

	assert isinstance(capture, osgGA.GUIEventHandler)
	assert capture.captured == False

	capture.captured = True

	assert capture.captured == True

	delta = capture.consume()

	assert (delta.x, delta.y) == (0.0, 0.0)

def test_set_cursor_visible_and_warp_cursor_are_safe_without_a_realized_window():
	# Both are documented no-ops without a realized GraphicsWindow -- confirms that contract
	# holds from Python too, without needing a real window in a headless test run.
	view = osgViewer.Viewer()

	osgx.setCursorVisible(view, False)
	osgx.warpCursor(view, 10.0, 20.0)

def test_make_cursor_uniform_callback_two_arg_overload():
	state = osgx.CursorState()

	state.updateCursor(5, 7)

	uniform = osg.Uniform("osgx_CursorState", osg.Vec3f())
	cb = osgx.makeCursorUniformCallback(state, uniform)

	n = osg.Node()
	n.updateCallback = cb
	n.accept(osgUtil.UpdateVisitor())

	assert uniform.value == osg.Vec3f(5.0, 7.0, 1.0) # inWindow defaults True -> 1.0

def test_make_cursor_uniform_callback_three_arg_overload_with_in_window_check():
	state = osgx.CursorState()

	state.updateCursor(1, 2)

	uniform = osg.Uniform("osgx_CursorState", osg.Vec3f())
	cb = osgx.makeCursorUniformCallback(state, uniform, lambda: False)

	n = osg.Node()
	n.updateCallback = cb
	n.accept(osgUtil.UpdateVisitor())

	assert uniform.value == osg.Vec3f(1.0, 2.0, 0.0)
	assert state.inWindow == False
