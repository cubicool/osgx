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
