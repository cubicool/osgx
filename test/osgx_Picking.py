import array

import numpy as np
import pytest

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
	except ValueError:
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

def test_picking_catalog_expands_encode_pragma():
	resolved = osgx.resolveShaderLibs("#pragma osgx::picking encode\n")

	assert "osgx_encodePickID" in resolved
	# Bit layout must match decodePickID()'s R/G/B/A convention.
	assert "0xFFu" in resolved

PICK_RULES = (osgx.pickCenter, osgx.pickMostCoverage, osgx.pickNearestToCenter, osgx.spiralPick)

def rgba(pickID):
	"""The RGBA8 encoding decodePickID() expects: R=bits[7:0] ... A=bits[31:24]."""

	return [(pickID >> shift) & 0xff for shift in (0, 8, 16, 24)]

def center_region(n, pickID):
	"""A row-major n*n RGBA8 region that is empty (ID 0) everywhere but its center pixel."""

	ids = [0] * (n * n)
	ids[(n // 2) * n + n // 2] = pickID

	return np.array([rgba(i) for i in ids], dtype=np.uint8).reshape(n, n, 4)

def test_decode_pick_id_buffer_types():
	assert osgx.decodePickID(bytes(rgba(0x12345678))) == 0x12345678
	assert osgx.decodePickID(np.array(rgba(0x12345678), dtype=np.uint8)) == 0x12345678
	assert osgx.decodePickID(array.array("B", rgba(42))) == 42

@pytest.mark.parametrize("rule", PICK_RULES)
def test_pick_rule_finds_center_id(rule):
	assert rule(center_region(3, 7), 3) == 7
	assert rule(center_region(3, 7).tobytes(), 3) == 7

@pytest.mark.parametrize("rule", PICK_RULES)
def test_pick_rule_rejects_strided_view(rule):
	# A sub-region sliced out of a larger readback is the natural call shape, but it's strided:
	# reading it as packed n*n pixels would silently pick from the wrong pixels.
	big = np.zeros((6, 6, 4), dtype=np.uint8)
	big[1:4, 1:4] = center_region(3, 7)

	with pytest.raises(ValueError):
		rule(big[1:4, 1:4], 3)

	assert rule(np.ascontiguousarray(big[1:4, 1:4]), 3) == 7

@pytest.mark.parametrize("rule", PICK_RULES)
def test_pick_rule_rejects_wrong_dtype(rule):
	with pytest.raises(TypeError):
		rule(center_region(3, 7).astype(np.float32), 3)

@pytest.mark.parametrize("rule", PICK_RULES)
def test_pick_rule_rejects_non_rgba_pixels(rule):
	with pytest.raises(ValueError):
		rule(np.zeros((3, 3, 3), dtype=np.uint8), 3)

@pytest.mark.parametrize("rule", PICK_RULES)
def test_pick_rule_rejects_non_positive_n(rule):
	with pytest.raises(ValueError):
		rule(center_region(3, 7), 0)
