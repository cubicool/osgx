import json
import struct
import zlib

import pytest

import osgx

from OpenSceneGraph import *


def _write_png(path, width, height, channels=1, fill=0):
	"""A minimal valid 8-bit PNG (grayscale for 1 channel, RGB for 3) - no imaging dependency."""
	color_type = {1: 0, 3: 2}[channels]
	raw = b"".join(b"\x00" + bytes([fill]) * (width * channels) for _ in range(height))

	def chunk(kind, data):
		body = kind + data

		return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

	png = (
		b"\x89PNG\r\n\x1a\n"
		+ chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
		+ chunk(b"IDAT", zlib.compress(raw))
		+ chunk(b"IEND", b"")
	)

	path.write_bytes(png)


def _manifest(data):
	return {
		"asset": {"version": "2.0", "generator": "test"},
		"extensionsUsed": ["osgx_sdf"],
		"extensions": {"osgx_sdf": {"data": data}},
	}


def _write_manifest(tmp_path, data, name="atlas.gltf"):
	path = tmp_path / name
	path.write_text(json.dumps(_manifest(data)))

	return str(path)


def _entry(type="SDF", uri="atlas.png", tiles=None):
	return {
		"type": type,
		"texture": {"uri": uri},
		"tiles": tiles if tiles is not None else {
			"a": {"rect": [16, 8, 32, 16], "pixelRange": 16.0},
			"b": {
				"rect": [0, 0, 8, 8], "pixelRange": 8.0,
				"range": 0.1, "texelsPerEm": 80.0, "emOrigin": [-0.1, -0.2],
			},
		},
	}


def test_load_reads_sdf_type_names_and_tiles(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry()]))

	assert ts.valid()
	assert ts.sdfType == osgx.SDF.SDFType.SDF
	assert ts.names() == ["a", "b"]
	assert ts.has("a") and not ts.has("nope")
	assert ts.texture is not None

	a = ts.tile("a")

	assert (a.x, a.y, a.w, a.h) == (16, 8, 32, 16)
	assert a.pixelRange == 16.0
	assert a.hasEmFrame is False

	b = ts.tile("b")

	assert b.hasEmFrame is True
	assert b.range == pytest.approx(0.1)
	assert b.texelsPerEm == pytest.approx(80.0)
	assert b.emOrigin == pytest.approx((-0.1, -0.2))


def test_uv_rect_flips_the_top_left_pixel_rect_into_texture_space(tmp_path):
	# 64x32 image, tile at pixel [16, 8, 32, 16] measured from the TOP-left:
	#   u = 16/64 .. 48/64 = 0.25 .. 0.75; rows 8..24 from the top are v = 1 - 24/32 .. 1 - 8/32.
	_write_png(tmp_path / "atlas.png", 64, 32)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry()]))

	assert ts.uvRect("a") == (0.25, 0.25, 0.75, 0.75)
	# A tile in the very top-left pixel corner ends up at the TOP-left of texture space (V near 1).
	assert ts.uvRect("b") == (0.0, 0.75, 0.125, 1.0)


def test_attribute_carries_sdf_type_pixelrange_and_uvrect(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry()]))
	sdf = ts.attribute("a")

	assert isinstance(sdf, osgx.SDF)
	assert sdf.sdfType == osgx.SDF.SDFType.SDF
	assert sdf.pixelRange == 16.0
	assert sdf.uvRect == osg.Vec4(0.25, 0.25, 0.75, 0.75)
	assert ts.attribute("nope") is None


def test_attributes_share_one_texture(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry()]))

	assert ts.attribute("a").texture is ts.attribute("b").texture
	assert ts.attribute("a").texture is ts.texture


def test_msdf_entry_with_an_rgb_image(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32, channels=3)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry(type="MSDF")]))

	assert ts.valid()
	assert ts.sdfType == osgx.SDF.SDFType.MSDF
	assert ts.attribute("a").sdfType == osgx.SDF.SDFType.MSDF


def test_msdf_entry_with_a_grayscale_image_is_rejected(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32, channels=1)

	assert not osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry(type="MSDF")])).valid()


def test_index_selects_the_data_entry(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)
	_write_png(tmp_path / "other.png", 64, 32, channels=3)

	path = _write_manifest(tmp_path, [
		_entry(type="SDF", uri="atlas.png", tiles={"first": {"rect": [0, 0, 8, 8], "pixelRange": 4.0}}),
		_entry(type="MSDF", uri="other.png", tiles={"second": {"rect": [0, 0, 8, 8], "pixelRange": 4.0}}),
	])

	assert osgx.gltf.sdf.TileSet.load(path).names() == ["first"]
	assert osgx.gltf.sdf.TileSet.load(path, 1).names() == ["second"]
	assert osgx.gltf.sdf.TileSet.load(path, 1).sdfType == osgx.SDF.SDFType.MSDF
	assert not osgx.gltf.sdf.TileSet.load(path, 2).valid()


def test_bad_tiles_are_skipped_not_fatal(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)

	tiles = {
		"good": {"rect": [0, 0, 8, 8], "pixelRange": 4.0},
		"no_pixelrange": {"rect": [0, 0, 8, 8]},
		"zero_pixelrange": {"rect": [0, 0, 8, 8], "pixelRange": 0.0},
		"short_rect": {"rect": [0, 0, 8], "pixelRange": 4.0},
		"empty_rect": {"rect": [0, 0, 0, 8], "pixelRange": 4.0},
		"outside_image": {"rect": [60, 0, 8, 8], "pixelRange": 4.0},
		"negative_origin": {"rect": [-1, 0, 8, 8], "pixelRange": 4.0},
		"not_an_object": 7,
	}

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry(tiles=tiles)]))

	assert ts.valid()
	assert ts.names() == ["good"]


@pytest.mark.parametrize("entry", [
	{"type": "BOGUS", "texture": {"uri": "atlas.png"}, "tiles": {"a": {"rect": [0, 0, 8, 8], "pixelRange": 4.0}}},
	{"type": "SDF", "tiles": {"a": {"rect": [0, 0, 8, 8], "pixelRange": 4.0}}},  # no texture
	{"type": "SDF", "texture": {"uri": "atlas.png"}},  # no tiles
	{"type": "SDF", "texture": {"uri": "atlas.png"}, "tiles": {}},
	{"type": "SDF", "texture": {"uri": "missing.png"}, "tiles": {"a": {"rect": [0, 0, 8, 8], "pixelRange": 4.0}}},
])
def test_invalid_entries_give_an_invalid_tileset(tmp_path, entry):
	_write_png(tmp_path / "atlas.png", 64, 32)

	assert not osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [entry])).valid()


def test_missing_file_and_missing_extension_are_invalid(tmp_path):
	assert not osgx.gltf.sdf.TileSet.load(str(tmp_path / "nope.gltf")).valid()

	path = tmp_path / "plain.gltf"
	path.write_text(json.dumps({"asset": {"version": "2.0"}}))

	assert not osgx.gltf.sdf.TileSet.load(str(path)).valid()

	path.write_text("{ this is not json")

	assert not osgx.gltf.sdf.TileSet.load(str(path)).valid()


def test_unknown_tile_name_raises_index_error(tmp_path):
	_write_png(tmp_path / "atlas.png", 64, 32)

	ts = osgx.gltf.sdf.TileSet.load(_write_manifest(tmp_path, [_entry()]))

	with pytest.raises(IndexError):
		ts.tile("nope")

	with pytest.raises(IndexError):
		ts.uvRect("nope")


def test_default_tileset_is_invalid():
	assert not osgx.gltf.sdf.TileSet().valid()
