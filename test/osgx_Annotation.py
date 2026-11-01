import os

import pytest

import osgx

from OpenSceneGraph import *

if not hasattr(osgx, "headless") or not hasattr(osgx.headless, "createContext"):
	pytest.skip("osgx built without the osgx.headless Python binding", allow_module_level=True)

if not hasattr(osgx.platform, "createEGLWindow"):
	pytest.skip("osgx built without OSGX_WITH_EGL", allow_module_level=True)

W, H = 64, 64

@pytest.fixture
def viewer(monkeypatch):
	"""Headless viewer whose context has the GL_KHR_debug functions bound (osgx.debug.initialize())."""

	monkeypatch.delenv("DISPLAY", raising=False)

	try:
		context = osgx.headless.createContext(W, H, None, osgx.headless.Backend.EGL)

	except RuntimeError as exc:
		pytest.skip(f"could not create a pbuffer context: {exc}")

	if not context.graphicsContext.valid():
		pytest.skip("no EGL device could create a pbuffer context")

	viewer = osgViewer.Viewer()
	viewer.threadingModel = osgViewer.ViewerBase.SingleThreaded
	viewer.camera.graphicsContext = context.graphicsContext
	viewer.camera.viewport = (0, 0, W, H)
	viewer.sceneData = osg.Group()
	viewer.realizeOperation = osgx.debug.GraphicsOperation()

	# The pbuffer context is realized already, so frame() would not realize the viewer (and run the
	# realize operation) on its own.
	viewer.realize()

	# The context must outlive the viewer's frames.
	yield viewer

	del viewer
	del context

def in_draw(viewer, fn):
	"""Run `fn(depth)` inside a draw callback; `depth()` reads GL_DEBUG_GROUP_STACK_DEPTH."""

	os.environ.setdefault("PYOPENGL_PLATFORM", "egl")

	gl = pytest.importorskip("OpenGL.GL")
	errors = []

	def depth():
		return int(gl.glGetIntegerv(gl.GL_DEBUG_GROUP_STACK_DEPTH))

	def callback(ri):
		try:
			fn(depth)

		except BaseException as exc:
			errors.append(exc)

	viewer.camera.finalDrawCallback = callback
	viewer.frame()

	if errors:
		raise errors[0]

def test_annotation_scope_pops_on_exit(viewer):
	depths = []

	def body(depth):
		depths.append(depth())

		with osgx.debug.AnnotationScope(0, "scope"):
			depths.append(depth())

		depths.append(depth())

	in_draw(viewer, body)

	assert depths == [depths[0], depths[0] + 1, depths[0]]

def test_annotation_scope_pushes_on_enter_not_init(viewer):
	depths = []

	def body(depth):
		scope = osgx.debug.AnnotationScope(0, "scope")

		depths.append(depth())

		with scope:
			depths.append(depth())

		depths.append(depth())

	in_draw(viewer, body)

	assert depths == [depths[0], depths[0] + 1, depths[0]]

def test_annotation_scope_pops_on_exception(viewer):
	depths = []

	def body(depth):
		base = depth()

		with pytest.raises(ValueError):
			with osgx.debug.AnnotationScope(0, "scope"):
				raise ValueError("inside the group")

		depths.extend([base, depth()])

	in_draw(viewer, body)

	assert depths[0] == depths[1]

def test_annotation_group_begin_end(viewer):
	depths = []

	def body(depth):
		group = osgx.debug.AnnotationGroup(0, "group")

		depths.append(depth())

		group.begin()
		group.begin()

		depths.append(depth())

		group.end()
		group.end()

		depths.append(depth())

	in_draw(viewer, body)

	assert depths == [depths[0], depths[0] + 1, depths[0]]

def test_trace_scope_inside_drawing_is_a_group(viewer):
	depths = []

	def body(depth):
		base = depth()

		# No open DRAW span (no profiler, no annotation): CPU-only.
		with osgx.debug.TraceScope("outside"):
			depths.append(depth() - base)

		with osgx.debug.AnnotationScope(0, "annotation"):
			with osgx.debug.TraceScope("inside"):
				depths.append(depth() - base)

			# An explicit phase: CPU-only.
			with osgx.debug.TraceScope("explicit", osgx.debug.Phase.DRAW):
				depths.append(depth() - base)

		depths.append(depth() - base)

	in_draw(viewer, body)

	assert depths == [0, 2, 1, 0]
