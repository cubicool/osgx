import gc
import os

import pytest

import osgx

from OpenSceneGraph import *
from OpenSceneGraph.GL import *

if not hasattr(osgx, "headless") or not hasattr(osgx.headless, "createContext"):
	pytest.skip("osgx built without the osgx.headless Python binding", allow_module_level=True)

if not hasattr(osgx.platform, "createEGLWindow"):
	pytest.skip("osgx built without OSGX_WITH_EGL", allow_module_level=True)

W, H = 160, 120
CLEAR = (0.1, 0.3, 0.2)

def make_viewer(samples=None, backend=osgx.headless.Backend.EGL):
	"""Headless viewer on an osgX-created pbuffer."""

	try:
		context = osgx.headless.createContext(W, H, samples, backend)

	except RuntimeError as exc:
		pytest.skip(f"could not create a pbuffer context: {exc}")

	gc = context.graphicsContext

	if not gc.valid():
		pytest.skip("no EGL device could create a pbuffer context")

	viewer = osgViewer.Viewer()
	viewer.threadingModel = osgViewer.ViewerBase.SingleThreaded
	viewer.camera.graphicsContext = gc

	return add_scene(viewer), context

def add_scene(viewer):
	"""The pixel-checked test scene: a unit sphere on CLEAR, framed identically everywhere."""

	root = osg.Group()

	root.children.append(osg.ShapeDrawable(osg.Sphere(osg.Vec3(0, 0, 0), 1.0)))

	viewer.sceneData = root
	viewer.camera.viewport = (0, 0, W, H)
	viewer.camera.clearColor = osg.Vec4(*CLEAR, 1.0)
	viewer.camera.projectionMatrix = osg.Matrixd.perspective(30.0, W / H, 1.0, 100.0)
	viewer.camera.viewMatrix = osg.Matrixd.lookAt(
		osg.Vec3d(0, -8, 0), osg.Vec3d(0, 0, 0), osg.Vec3d(0, 0, 1)
	)

	return viewer

@pytest.fixture
def viewer(monkeypatch):
	# The EGL path must not need an X server at all.
	monkeypatch.delenv("DISPLAY", raising=False)

	return make_viewer()[0]

def read_rgb():
	image = osg.Image()

	image.readPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE)

	return memoryview(image).tobytes()

def pixel(data, x, y):
	i = (y * W + x) * 3

	return tuple(data[i:i + 3])

def is_clear(rgb):
	return all(abs(c - round(v * 255)) <= 2 for c, v in zip(rgb, CLEAR))

def render_once(viewer):
	frames = []

	viewer.camera.finalDrawCallback = lambda ri: frames.append(read_rgb())
	viewer.frame()

	return frames[0]

def count_blended(data):
	"""Pixels that are neither the clear color nor the flat, unlit sphere color."""

	sphere = pixel(data, W // 2, H // 2)
	count = 0

	for y in range(H):
		for x in range(W):
			rgb = pixel(data, x, y)

			if is_clear(rgb) or all(abs(a - b) <= 2 for a, b in zip(rgb, sphere)):
				continue

			count += 1

	return count

def query_context(viewer, *names):
	"""Read glGetIntegerv values from the live context inside a draw callback."""

	os.environ.setdefault("PYOPENGL_PLATFORM", "egl")

	gl = pytest.importorskip("OpenGL.GL")
	values = {}

	def query(ri):
		for name in names:
			values[name] = int(gl.glGetIntegerv(getattr(gl, name)))

	viewer.camera.finalDrawCallback = query
	viewer.frame()

	return values

def test_create_context_selects_egl(monkeypatch):
	monkeypatch.delenv("DISPLAY", raising=False)

	_, context = make_viewer()

	assert context.backend == osgx.headless.Backend.EGL
	assert context.graphicsContext.valid()
	assert not context.graphicsContext.traits.doubleBuffer

def test_auto_context_prefers_egl(monkeypatch):
	monkeypatch.delenv("DISPLAY", raising=False)

	_, context = make_viewer(backend=osgx.headless.Backend.Auto)

	assert context.backend == osgx.headless.Backend.EGL

def test_pbuffer_renders_scene(viewer):
	frames = []

	viewer.camera.finalDrawCallback = lambda ri: frames.append(read_rgb())
	viewer.frame()

	assert len(frames) == 1
	assert len(frames[0]) == W * H * 3
	assert is_clear(pixel(frames[0], 0, 0))
	assert not is_clear(pixel(frames[0], W // 2, H // 2))

def test_closing_one_pbuffer_context_keeps_others_alive(viewer):
	# Every pbuffer context on a device shares one EGLDisplay; closing one must not terminate
	# the display out from under the others.
	other, _ = make_viewer()

	other.frame()

	del other
	gc.collect()

	frames = []

	viewer.camera.finalDrawCallback = lambda ri: frames.append(read_rgb())
	viewer.frame()

	assert len(frames) == 1
	assert is_clear(pixel(frames[0], 0, 0))
	assert not is_clear(pixel(frames[0], W // 2, H // 2))

def test_cpp_camera_draw_callback_is_callable(viewer):
	# osgx.CameraDrawCallbacksGroup is a pure C++ osg::Camera::DrawCallback; calling it from a
	# Python callback with the real RenderInfo dispatches into its C++ operator(), which fans
	# out to its members.
	calls = []

	class Member(osg.Camera.DrawCallback):
		def __call__(self, ri):
			calls.append(ri.contextID)

	group = osgx.CameraDrawCallbacksGroup()
	group.add(Member())

	viewer.camera.finalDrawCallback = lambda ri: group(ri)
	viewer.frame()

	assert len(calls) == 1

def test_msaa_samples_blend_silhouette(monkeypatch):
	monkeypatch.delenv("DISPLAY", raising=False)

	aliased = count_blended(render_once(make_viewer()[0]))
	smoothed = count_blended(render_once(make_viewer(4)[0]))

	assert smoothed > max(10, 2 * aliased)

def test_default_context_is_compatibility_profile(monkeypatch):
	monkeypatch.delenv("DISPLAY", raising=False)

	values = query_context(make_viewer()[0], "GL_CONTEXT_PROFILE_MASK")

	assert values["GL_CONTEXT_PROFILE_MASK"] & 0x2 # GL_CONTEXT_COMPATIBILITY_PROFILE_BIT

def test_display_settings_request_core_profile_and_debug_flag(monkeypatch):
	monkeypatch.delenv("DISPLAY", raising=False)
	settings = osg.DisplaySettings.instance
	old_version = settings.glContextVersion
	old_profile = settings.glContextProfileMask
	old_flags = settings.glContextFlags

	try:
		# Traits use the GLX/EGL create_context bit values: profile core=0x1, flags debug=0x1.
		settings.glContextVersion = "3.3"
		settings.glContextProfileMask = 0x1
		settings.glContextFlags = 0x1
		values = query_context(
			make_viewer()[0],
			"GL_CONTEXT_PROFILE_MASK", "GL_CONTEXT_FLAGS", "GL_MAJOR_VERSION", "GL_MINOR_VERSION"
		)

	finally:
		settings.glContextVersion = old_version
		settings.glContextProfileMask = old_profile
		settings.glContextFlags = old_flags

	assert values["GL_CONTEXT_PROFILE_MASK"] & 0x1 # GL_CONTEXT_CORE_PROFILE_BIT
	assert values["GL_CONTEXT_FLAGS"] & 0x2 # GL_CONTEXT_FLAG_DEBUG_BIT
	assert (values["GL_MAJOR_VERSION"], values["GL_MINOR_VERSION"]) >= (3, 3)

def test_native_context_renders_when_display_is_available():
	# On Linux this is GLX's PixelBufferX11, so it needs a real X display (unlike EGL).
	if not os.environ.get("DISPLAY"):
		pytest.skip("native pbuffer on Linux needs an X display")

	viewer, context = make_viewer(backend=osgx.headless.Backend.Native)

	assert context.backend == osgx.headless.Backend.Native
	assert not viewer.camera.graphicsContext.traits.doubleBuffer

	data = render_once(viewer)

	assert is_clear(pixel(data, 0, 0))
	assert not is_clear(pixel(data, W // 2, H // 2))
