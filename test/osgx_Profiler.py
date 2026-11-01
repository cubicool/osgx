import time

import pytest

import osgx

from OpenSceneGraph import *

if not hasattr(osgx, "headless") or not hasattr(osgx.headless, "createContext"):
	pytest.skip("osgx built without the osgx.headless Python binding", allow_module_level=True)

if not hasattr(osgx.platform, "createEGLWindow"):
	pytest.skip("osgx built without OSGX_WITH_EGL", allow_module_level=True)

W, H = 64, 64
FRAMES = 20
SPIN_MS = 1.0

Phase = osgx.debug.Phase

class SpinCallback(osg.NodeCallback):
	"""An update callback whose work is a Python TraceScope spinning SPIN_MS."""

	def __call__(self, node, nv):
		with osgx.debug.TraceScope("python update"):
			end = time.perf_counter() + SPIN_MS / 1000.0

			while time.perf_counter() < end:
				pass

		super().__call__(node, nv)

@pytest.fixture
def profiled(monkeypatch):
	"""A headless viewer with a Profiler attached, after FRAMES frames: (profiler, viewer)."""

	monkeypatch.delenv("DISPLAY", raising=False)

	try:
		context = osgx.headless.createContext(W, H, None, osgx.headless.Backend.EGL)

	except RuntimeError as exc:
		pytest.skip(f"could not create a pbuffer context: {exc}")

	if not context.graphicsContext.valid():
		pytest.skip("no EGL device could create a pbuffer context")

	root = osg.Group()
	box = osg.ShapeDrawable(osg.Box(osg.Vec3(0, 0, 0), 1.0))

	root.name = "root"
	box.name = "box"
	root.children.append(box)
	root.updateCallback = SpinCallback()

	viewer = osgViewer.Viewer()
	viewer.threadingModel = osgViewer.ViewerBase.SingleThreaded
	viewer.camera.graphicsContext = context.graphicsContext
	viewer.camera.viewport = (0, 0, W, H)
	viewer.camera.name = "main"
	viewer.camera.projectionMatrix = osg.Matrixd.perspective(30.0, W / H, 1.0, 100.0)
	viewer.camera.viewMatrix = osg.Matrixd.lookAt(
		osg.Vec3d(0, -8, 0), osg.Vec3d(0, 0, 0), osg.Vec3d(0, 0, 1)
	)
	viewer.sceneData = root

	profiler = osgx.debug.Profiler()

	profiler.attach(viewer)
	viewer.realize()

	for _ in range(FRAMES):
		viewer.frame()

	yield profiler, viewer

	del viewer
	del context

def find(node, label):
	"""The first node below `node` labeled `label`, or None."""

	for child in node.children:
		if child.label == label:
			return child

		found = find(child, label)

		if found:
			return found

	return None

def test_profile_has_four_phases(profiled):
	profiler, _ = profiled

	assert profiler.profileFrame is not None

	profile = profiler.profile

	assert [phase.phase for phase in profile] == [Phase.EVENT, Phase.UPDATE, Phase.CULL, Phase.DRAW]

def test_python_trace_scope_in_update_tree(profiled):
	profiler, _ = profiled

	update = profiler.profile[int(Phase.UPDATE)]
	root = find(update, "root")
	scope = find(update, "python update")

	assert root is not None and scope is not None
	assert find(root, "python update") is not None
	assert scope.location == "python update"
	assert scope.className is None
	assert scope.inclusiveMs >= SPIN_MS * 0.9

def test_drawable_in_draw_tree(profiled):
	profiler, _ = profiled

	draw = profiler.profile[int(Phase.DRAW)]
	main = find(draw, "main")
	box = find(draw, "box")

	assert main is not None and box is not None
	assert box.className == "ShapeDrawable"
	assert box.draws >= 1.0
	assert box.drawOrder >= 0

def test_spans_and_frame_load(profiled):
	profiler, _ = profiled

	profile = profiler.profile
	frame = profiler.profileFrame
	scope = find(profile[int(Phase.UPDATE)], "python update")
	span = osgx.debug.cpuSpan(scope, frame)
	load = osgx.debug.frameLoad(profile, frame)

	assert span is not None and span.endMs >= span.startMs
	assert osgx.debug.gpuSpan(scope, frame) is None
	assert load.cpuMs >= SPIN_MS * 0.9
	assert load.endMs(osgx.debug.WaterfallGpu.OFF) == load.cpuEndMs
	assert load.timelineEndMs >= load.cpuEndMs

def test_reports(profiled):
	profiler, _ = profiled

	assert "python update" in profiler.report()

	for gpu in (osgx.debug.WaterfallGpu.OFF, osgx.debug.WaterfallGpu.COST, osgx.debug.WaterfallGpu.TIMELINE):
		waterfall = profiler.reportWaterfall(gpu=gpu)

		assert "python update" in waterfall
		assert "box" in waterfall or "main" in waterfall

def test_mode_property(profiled):
	profiler, _ = profiled

	profiler.mode = osgx.debug.QueryMode.ASYNC

	assert profiler.mode == osgx.debug.QueryMode.ASYNC

def test_children_keep_their_tree_alive(profiled):
	profiler, _ = profiled

	children = profiler.profile[int(Phase.UPDATE)].children

	# The phase root is gone; its children must still be readable.
	assert all(isinstance(child.label, str) for child in children)
