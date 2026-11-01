import threading

import pytest

import osgx

Phase = osgx.debug.Phase

def on_new_thread(fn):
	"""Run `fn()` on a new thread (a trace buffer with nothing recorded) and return its result."""

	result = {}

	def run():
		try:
			result["value"] = fn()

		except BaseException as exc:
			result["error"] = exc

	thread = threading.Thread(target=run)

	thread.start()
	thread.join()

	if "error" in result:
		raise result["error"]

	return result["value"]

def test_trace_scope_with_phase_opens_a_span():
	def body():
		before = osgx.debug.threadTraceState()

		with osgx.debug.TraceScope("update work", Phase.UPDATE):
			inside = osgx.debug.threadTraceState()

		after = osgx.debug.threadTraceState()

		return before, inside, after

	before, inside, after = on_new_thread(body)

	assert before.frame is None and before.phase is None and before.depth == 0
	assert inside.phase == Phase.UPDATE and inside.depth == 1
	assert inside.frame == 0
	assert after.phase == Phase.UPDATE and after.depth == 0

def test_trace_scope_without_phase_takes_the_open_spans():
	def body():
		with osgx.debug.TraceScope("outer", Phase.CULL):
			with osgx.debug.TraceScope("inner"):
				inner = osgx.debug.threadTraceState()

				with osgx.debug.TraceScope("draw", Phase.DRAW):
					draw = osgx.debug.threadTraceState()

				back = osgx.debug.threadTraceState()

		return inner, draw, back

	inner, draw, back = on_new_thread(body)

	assert inner.phase == Phase.CULL and inner.depth == 2
	assert draw.phase == Phase.DRAW and draw.depth == 3
	assert back.phase == Phase.CULL and back.depth == 2

def test_trace_scope_without_phase_or_records_is_not_recorded():
	def body():
		with osgx.debug.TraceScope("nothing yet"):
			inside = osgx.debug.threadTraceState()

		return inside, osgx.debug.threadTraceState()

	inside, after = on_new_thread(body)

	assert inside.phase is None and inside.depth == 0
	assert after.phase is None and after.depth == 0

def test_trace_scope_without_phase_after_a_record_takes_its_phase():
	def body():
		with osgx.debug.TraceScope("first", Phase.EVENT):
			pass

		with osgx.debug.TraceScope("between"):
			return osgx.debug.threadTraceState()

	state = on_new_thread(body)

	assert state.phase == Phase.EVENT and state.depth == 1

def test_trace_scope_closes_on_exception():
	def body():
		with pytest.raises(ValueError):
			with osgx.debug.TraceScope("raises", Phase.UPDATE):
				raise ValueError("inside the scope")

		return osgx.debug.threadTraceState()

	assert on_new_thread(body).depth == 0

def test_trace_scope_enters_on_with_not_init():
	def body():
		scope = osgx.debug.TraceScope("later", Phase.UPDATE)
		before = osgx.debug.threadTraceState()

		with scope:
			inside = osgx.debug.threadTraceState()

		return before, inside

	before, inside = on_new_thread(body)

	assert before.depth == 0 and before.phase is None
	assert inside.depth == 1
