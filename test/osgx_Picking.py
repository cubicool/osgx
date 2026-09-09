import osgx

from OpenSceneGraph import *
from OpenSceneGraph.GL import *

def make_image(w=4, h=4):
	img = osg.Image()

	img.allocateImage(w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE)

	return img

def test_shape_drawable_color():
	sd = osg.ShapeDrawable(osg.Sphere(osg.Vec3(), 1.0))

	sd.color = osg.Vec4(1.0, 0.2, 0.2, 1.0)

	assert sd.color == osg.Vec4(1.0, 0.2, 0.2, 1.0)

def test_pick_readback_sync_construction():
	rb = osgx.PickReadbackSync(
		1, make_image(), 800, 600,
		rule=osgx.PickRule.SPIRAL,
		mode=osgx.PickReadbackSync.Mode.CLICK,
	)

	assert isinstance(rb, osg.NodeCallback)
	assert isinstance(rb, osgx.PickReadback)
	# PickReadback's `virtual osg::Object` base (see osgx/Picking.hpp) is what makes this
	# multiple-inheritance shape work at all -- confirm it's really reachable.
	assert isinstance(rb, osg.Object)

	hits = []

	rb.onPick = lambda pid, action: hits.append((pid, action))
	rb.requestPick(10, 20)

	assert (rb.mouseX, rb.mouseY) == (10, 20)
	assert rb.lastID == 0

def test_pick_readback_async_construction():
	tex = osg.Texture2D()
	rba = osgx.PickReadbackAsync(tex, 1, 1, mode=osgx.PickReadbackAsync.Mode.CONTINUOUS)

	assert isinstance(rba, osg.Camera.DrawCallback)
	assert isinstance(rba, osgx.PickReadback)

def test_pick_callbacks_accept_pick_readback_base():
	# The actual point of PickReadback's virtual-Object fix: PickCameraSync/PickHoverCallback/
	# PickHandler all take a plain `PickReadback*` in C++ -- prove a PickReadbackSync Python
	# object satisfies that despite being a totally separate multiple-inheritance branch.
	rb = osgx.PickReadbackSync(1, make_image(), 800, 600)
	cam = osg.Camera()

	sync = osgx.PickCameraSync(cam, False, 0, 0, rb)
	hover = osgx.PickHoverCallback(rb)
	handler = osgx.PickHandler(rb, False, False)

	assert sync is not None
	assert hover is not None
	assert handler is not None

def test_make_pick_camera_overloads():
	cam_image = osgx.makePickCamera(64, 64, make_image())

	assert isinstance(cam_image, osg.Camera)

	cam_tex = osgx.makePickCamera(64, 64, osg.Texture2D())

	assert isinstance(cam_tex, osg.Camera)

def test_make_pick_camera_install_program_false():
	# installProgram=False -- caller (e.g. a wrapper Group between this camera and shared scene
	# content) installs its own Program instead of fighting this camera's OVERRIDE with
	# PROTECTED. Just confirms both overloads still construct a usable camera with the generic
	# pick Program skipped; the StateSet isn't inspected here, that's covered at the osgSlug
	# integration level (pickScope no longer needs PROTECTED against this camera).
	cam_image = osgx.makePickCamera(64, 64, make_image(), installProgram=False)

	assert isinstance(cam_image, osg.Camera)

	cam_tex = osgx.makePickCamera(64, 64, osg.Texture2D(), installProgram=False)

	assert isinstance(cam_tex, osg.Camera)

def test_decode_pick_id():
	assert osgx.decodePickID(bytes([5, 0, 0, 0])) == 5
	assert osgx.decodePickID(bytes([0, 1, 0, 0])) == 256

def test_pick_rules_spiral_and_center():
	# 3x3 region, row-major, Y=0 at bottom-left. Put ID 5 one ring out from center (index 3,
	# NOT the true center at index 4) so pickCenter and spiralPick disagree -- proves spiralPick
	# actually searches outward instead of just re-reading the center pixel.
	region = bytearray(9 * 4)

	region[3 * 4] = 5 # pixel index 3 = row 1, col 0

	region = bytes(region)

	assert osgx.pickCenter(region, 3) == 0
	assert osgx.spiralPick(region, 3) == 5

def test_pick_rule_buffer_too_small_raises():
	try:
		osgx.pickCenter(bytes([0, 0, 0, 0]), 3) # needs 3*3*4=36 bytes, only got 4

		assert False, "expected an exception for an undersized buffer"
	except RuntimeError:
		pass

def test_pick_id_allocator_hands_out_contiguous_ranges():
	alloc = osgx.PickIDAllocator()

	# ID 0 stays reserved for "nothing hit" -- the first real ID is 1.
	assert alloc.alloc() == 1
	assert alloc.alloc() == 2
	# A 3-layer CompositeShape reserves a contiguous block; the caller adds its own
	# 0-based layer index on top of the returned base.
	assert alloc.alloc(3) == 3
	assert alloc.alloc() == 6

def test_pick_id_allocator_instances_are_independent():
	a = osgx.PickIDAllocator()
	b = osgx.PickIDAllocator()

	a.alloc(5)

	# A fresh allocator still starts at 1 -- state lives on the instance, not process-wide.
	assert b.alloc() == 1

def test_register_pick_shader_libs_expands_encode_pragma():
	osgx.registerPickShaderLibs()

	resolved = osgx.resolveShaderLibs("#pragma osgx::picking encode\n")

	assert "osgx_encodePickID" in resolved
	# Bit layout must match decodePickID()'s R/G/B/A convention.
	assert "0xFFu" in resolved

def test_register_pick_shader_libs_is_idempotent():
	# registerShaderLibs() only throws on a genuine content conflict -- re-registering the
	# same catalog (e.g. called from more than one module import) must be a safe no-op.
	osgx.registerPickShaderLibs()
	osgx.registerPickShaderLibs()
