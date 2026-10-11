import gc
import sys
import weakref

import numpy as np
import pytest

import osgx

from OpenSceneGraph import *
from OpenSceneGraph.GL import *

# Same headless infrastructure as test/osgx_Headless.py - a real EGL pbuffer context, one
# viewer.frame(), then glReadPixels via osg.Image.readPixels(). PBOTexture's ring/subload/fence
# path only exists to get bytes onto the GPU correctly, so the only trustworthy way to confirm it
# worked is reading those bytes back off the GPU, not eyeballing a window.
if not hasattr(osgx, "headless") or not hasattr(osgx.headless, "createContext"):
	pytest.skip("osgx built without the osgx.headless Python binding", allow_module_level=True)

if not hasattr(osgx.platform, "createEGLWindow"):
	pytest.skip("osgx built without OSGX_WITH_EGL", allow_module_level=True)

W, H = 32, 32
TEX = 4 # small solid-fill texture - GL_LINEAR sampling of a uniform-color texture is exact
        # everywhere in its interior, so no wrap/filter edge cases to worry about.
CLEAR = (0, 0, 0, 255) # deliberately distinct from every test fill color below.

class SolidSource:
	"""A FrameSource returning a fresh solid-fill buffer each call, or None while paused. Tracks
	call count so tests can confirm the source is actually invoked once per subload, not just
	once at construction."""

	def __init__(self, color):
		self.color = color
		self.paused = False
		self.calls = 0

	def __call__(self):
		self.calls += 1

		if self.paused:
			return None

		return bytes(self.color) * (TEX * TEX)

def make_viewer():
	context = osgx.headless.createContext(W, H, None, osgx.headless.Backend.EGL)
	gc = context.graphicsContext

	if not gc.valid():
		pytest.skip("no EGL device could create a pbuffer context")

	viewer = osgViewer.Viewer()
	viewer.threadingModel = osgViewer.ViewerBase.SingleThreaded
	viewer.camera.graphicsContext = gc
	viewer.camera.viewport = (0, 0, W, H)
	viewer.camera.clearColor = osg.Vec4(*[c / 255.0 for c in CLEAR])
	viewer.camera.projectionMatrix = osg.Matrixd.ortho(0, W, 0, H, -1, 1)
	viewer.camera.viewMatrix = osg.Matrixd.identity()

	return viewer

def make_scene(viewer, texture):
	"""A single quad, exactly filling the viewport, textured with `texture`."""

	quad = osg.createTexturedQuadGeometry(osg.Vec3(0, 0, 0), osg.Vec3(W, 0, 0), osg.Vec3(0, H, 0))
	geode = osg.Geode()

	geode.drawables.append(quad)
	geode.stateSet.textureAttributes[0] = texture

	viewer.sceneData = geode

@pytest.fixture
def viewer(monkeypatch):
	# The EGL path must not need an X server at all.
	monkeypatch.delenv("DISPLAY", raising=False)

	return make_viewer()

def read_rgba():
	image = osg.Image()

	image.readPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE)

	return memoryview(image).tobytes()

def center_pixel(data):
	i = ((H // 2) * W + (W // 2)) * 4

	return tuple(data[i:i + 4])

def render_once(viewer):
	frames = []

	viewer.camera.finalDrawCallback = lambda ri: frames.append(read_rgba())
	viewer.frame()

	return frames[0]

def warm_up(viewer, source):
	"""osgViewer::Renderer runs a one-time compile() pass before its real first draw
	(Renderer::_compileOnNextDraw, true from construction - see src/osgViewer/Renderer.cpp),
	which applies every StateSet once, including this texture - so a viewer's very first frame()
	call sees FrameSource invoked twice (compile pass + real draw), one more than every later
	frame. Render one throwaway frame and reset the counter so tests measure steady-state
	per-frame behavior instead of hardcoding that one-time OSG quirk into every assertion."""

	viewer.frame()

	source.calls = 0

def test_solid_fill_renders_exact_color(viewer):
	source = SolidSource((255, 0, 0, 255))
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	make_scene(viewer, texture)
	warm_up(viewer, source)

	data = render_once(viewer)

	assert center_pixel(data) == (255, 0, 0, 255)
	assert source.calls == 1

def test_reuploads_new_color_every_frame(viewer):
	# Proves the SUBLOAD path (not just the one-time load() at texture creation) actually runs
	# each frame - a second render with a different color must show the new color, not the first.
	source = SolidSource((0, 255, 0, 255))
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	make_scene(viewer, texture)
	warm_up(viewer, source)

	first = render_once(viewer)

	assert center_pixel(first) == (0, 255, 0, 255)

	source.color = (0, 0, 255, 255)
	second = render_once(viewer)

	assert center_pixel(second) == (0, 0, 255, 255)
	assert source.calls == 2

def test_none_frame_keeps_previous_frame(viewer):
	# FrameSource returning None means "no new frame yet" - a no-op upload, not a blank/crash.
	source = SolidSource((255, 255, 0, 255))
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	make_scene(viewer, texture)
	warm_up(viewer, source)

	first = render_once(viewer)

	assert center_pixel(first) == (255, 255, 0, 255)

	source.paused = True
	second = render_once(viewer)

	assert center_pixel(second) == (255, 255, 0, 255)
	assert source.calls == 2

def test_set_frame_source_replaces_callable(viewer):
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidSource((255, 0, 255, 255)))

	make_scene(viewer, texture)
	render_once(viewer)

	replacement = SolidSource((0, 255, 255, 255))

	texture.setFrameSource(replacement)

	data = render_once(viewer)

	assert center_pixel(data) == (0, 255, 255, 255)
	assert replacement.calls == 1

def test_profiler_timings_are_nonnegative_after_a_real_upload(viewer):
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidSource((128, 128, 128, 255)))

	make_scene(viewer, texture)
	render_once(viewer)

	contextID = viewer.camera.graphicsContext.state.contextID

	assert texture.getAverageUploadMilliseconds(contextID) >= 0.0
	assert texture.getAverageWaitMilliseconds(contextID) >= 0.0
	# Unexercised contexts report 0, not an error.
	assert texture.getAverageUploadMilliseconds(contextID + 1) == 0.0

class ScriptedSource:
	"""A FrameSource returning whatever `frame` currently holds, or raising it if it's an exception."""

	def __init__(self, frame):
		self.frame = frame

	def __call__(self):
		if isinstance(self.frame, Exception):
			raise self.frame

		return self.frame

def test_frame_size():
	assert osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidSource(CLEAR)).frameSize == TEX * TEX * 4
	assert osgx.PBOTexture(TEX, TEX, GL_RGB, SolidSource(CLEAR)).frameSize == TEX * TEX * 3
	assert osgx.PBOTexture(TEX, TEX, GL_RED, SolidSource(CLEAR)).frameSize == TEX * TEX

def test_numpy_frame_renders_exact_color(viewer):
	source = ScriptedSource(np.full((TEX, TEX, 4), (255, 0, 0, 255), dtype=np.uint8))

	make_scene(viewer, osgx.PBOTexture(TEX, TEX, GL_RGBA, source))
	warm_up(viewer, source)

	assert center_pixel(render_once(viewer)) == (255, 0, 0, 255)

def _assert_bad_frame_skipped(viewer, monkeypatch, bad, exc_type):
	# Errors from the frame source happen inside the draw traversal, with no Python caller to raise
	# to, so they're reported via sys.unraisablehook instead. Patched here in the test body, not in
	# a fixture: pytest installs its own hook at the start of each test's call phase.
	unraisable = []

	monkeypatch.setattr(sys, "unraisablehook", unraisable.append)

	good = bytes((0, 255, 0, 255)) * (TEX * TEX)
	source = ScriptedSource(good)

	make_scene(viewer, osgx.PBOTexture(TEX, TEX, GL_RGBA, source))
	warm_up(viewer, source)

	assert center_pixel(render_once(viewer)) == (0, 255, 0, 255)

	source.frame = bad

	assert center_pixel(render_once(viewer)) == (0, 255, 0, 255)
	assert len(unraisable) == 1
	assert unraisable[0].exc_type is exc_type

	# The source recovers as soon as it returns a valid frame again.
	source.frame = bytes((0, 0, 255, 255)) * (TEX * TEX)

	assert center_pixel(render_once(viewer)) == (0, 0, 255, 255)
	assert len(unraisable) == 1

def test_short_frame_is_skipped(viewer, monkeypatch):
	_assert_bad_frame_skipped(viewer, monkeypatch, bytes(TEX * TEX * 4 - 1), ValueError)

def test_long_frame_is_skipped(viewer, monkeypatch):
	_assert_bad_frame_skipped(viewer, monkeypatch, bytes(TEX * TEX * 4 + 4), ValueError)

def test_strided_frame_is_skipped(viewer, monkeypatch):
	strided = np.zeros((TEX, TEX, 8), dtype=np.uint8)[:, :, :4]

	_assert_bad_frame_skipped(viewer, monkeypatch, strided, ValueError)

def test_wrong_dtype_frame_is_skipped(viewer, monkeypatch):
	_assert_bad_frame_skipped(viewer, monkeypatch, np.zeros(TEX * TEX * 4, dtype=np.float32), TypeError)

def test_non_buffer_frame_is_skipped(viewer, monkeypatch):
	_assert_bad_frame_skipped(viewer, monkeypatch, "not a buffer", TypeError)

def test_raising_source_is_skipped(viewer, monkeypatch):
	_assert_bad_frame_skipped(viewer, monkeypatch, RuntimeError("boom"), RuntimeError)

def test_source_released_with_texture():
	source = SolidSource(CLEAR)
	ref = weakref.ref(source)
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	del source
	gc.collect()

	assert ref() is not None

	del texture
	gc.collect()

	assert ref() is None

def test_replaced_source_released(viewer):
	# Rendered first, so the source's last frame buffer is held too, not just the callable.
	source = SolidSource((255, 0, 0, 255))
	ref = weakref.ref(source)
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	make_scene(viewer, texture)
	render_once(viewer)

	del source

	texture.setFrameSource(SolidSource((0, 255, 0, 255)))
	gc.collect()

	assert ref() is None

class SolidFrameSource(osgx.PBOTexture.FrameSource):
	"""The FrameSource-subclass equivalent of SolidSource, recording the texture next() receives."""

	def __init__(self, color):
		super().__init__()

		self.color = color
		self.textures = []

	def next(self, texture):
		self.textures.append(texture)

		return bytes(self.color) * (TEX * TEX)

def test_frame_source_subclass_renders(viewer):
	source = SolidFrameSource((255, 0, 255, 255))
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	make_scene(viewer, texture)

	assert center_pixel(render_once(viewer)) == (255, 0, 255, 255)
	assert source.textures and all(t is texture for t in source.textures)

def test_frame_source_subclass_kept_alive_while_set(viewer):
	# No Python reference besides the texture's own: the slot must keep the subclass instance (and
	# so its next() override) alive, or C++ would find no override to call.
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidFrameSource((0, 255, 255, 255)))

	gc.collect()

	make_scene(viewer, texture)

	assert center_pixel(render_once(viewer)) == (0, 255, 255, 255)

def test_frame_source_property_identity():
	source = SolidFrameSource(CLEAR)
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	assert texture.frameSource is source

	fn = SolidSource(CLEAR)
	texture.frameSource = fn

	assert texture.frameSource is fn

	texture.frameSource = None

	assert texture.frameSource is None
	assert osgx.PBOTexture(TEX, TEX, GL_RGBA).frameSource is None

def test_cleared_frame_source_keeps_previous_frame(viewer):
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidSource((255, 255, 0, 255)))

	make_scene(viewer, texture)
	render_once(viewer)

	texture.frameSource = None

	assert center_pixel(render_once(viewer)) == (255, 255, 0, 255)

def test_frame_source_subclass_without_next_is_skipped(viewer, monkeypatch):
	class NoNext(osgx.PBOTexture.FrameSource):
		pass

	unraisable = []

	monkeypatch.setattr(sys, "unraisablehook", unraisable.append)

	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, SolidSource((0, 255, 0, 255)))

	make_scene(viewer, texture)
	render_once(viewer)

	texture.frameSource = NoNext()

	assert center_pixel(render_once(viewer)) == (0, 255, 0, 255)
	assert len(unraisable) == 1
	assert unraisable[0].exc_type is TypeError

def test_frame_source_subclass_released_when_replaced():
	source = SolidFrameSource(CLEAR)
	ref = weakref.ref(source)
	texture = osgx.PBOTexture(TEX, TEX, GL_RGBA, source)

	del source
	gc.collect()

	assert ref() is not None

	texture.frameSource = None
	gc.collect()

	assert ref() is None

def test_invalid_frame_source_raises():
	with pytest.raises(ValueError):
		osgx.PBOTexture(TEX, TEX, GL_RGBA, 42)
