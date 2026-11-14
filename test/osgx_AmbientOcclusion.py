import osgx

from OpenSceneGraph import *
from OpenSceneGraph.GL import *

# create() builds the scene graph only (no GL context needed); these tests cover the Python-side
# contract of osgx.SSAO/GTAO/CORTAO/ComputePass, not their rendered output.
W, H = 64, 32

def make_inputs():
	normal = osg.Texture2D()
	normal.size = (W, H)
	normal.internalFormat = GL_RGB16F

	position = osg.Texture2D()
	position.size = (W, H)
	position.internalFormat = GL_RGBA32F

	return normal, position, osg.Uniform("projectionMatrix", osg.Matrixf())

def test_ssao_gtao_cortao_share_the_create_signature():
	normal, position, projection = make_inputs()

	for cls in (osgx.SSAO, osgx.GTAO, osgx.CORTAO):
		ao = cls.create(normal, position, projection, W, H)

		assert ao.valid()
		assert isinstance(ao.aoTexture, osg.Texture2D)

def test_gtao_defaults_and_options():
	normal, position, projection = make_inputs()
	gtao = osgx.GTAO.create(normal, position, projection, W, H)

	assert gtao.quality == osgx.GTAO.Quality.HIGH
	assert gtao.denoisePasses == 1
	assert gtao.bentNormalTexture is None
	assert len(gtao.intermediateDenoisePasses) == 2
	assert isinstance(gtao.finalDenoisePass, osgx.ComputePass)

	options = osgx.GTAO.Options()
	options.quality = osgx.GTAO.Quality.ULTRA
	options.denoisePasses = 3
	options.bentNormal = True

	gtao = osgx.GTAO.create(normal, position, projection, W, H, options)

	assert gtao.quality == osgx.GTAO.Quality.ULTRA
	assert gtao.denoisePasses == 3
	assert isinstance(gtao.bentNormalTexture, osg.Texture2D)

def test_gtao_live_settings():
	normal, position, projection = make_inputs()
	gtao = osgx.GTAO.create(normal, position, projection, W, H)

	gtao.denoisePasses = 7

	assert gtao.denoisePasses == 3

	gtao.quality = osgx.GTAO.Quality.LOW

	assert gtao.quality == osgx.GTAO.Quality.LOW

	gtao.radius.value = 2.0

	assert gtao.radius.value == 2.0

def test_cortao_defaults_and_options():
	normal, position, projection = make_inputs()
	cortao = osgx.CORTAO.create(normal, position, projection, W, H)

	assert cortao.filter == osgx.CORTAO.Filter.TILED
	assert cortao.sampleCount == 16
	assert cortao.bentNormalTexture is None
	assert isinstance(cortao.occluderLookupTable, osg.Texture3D)

	options = osgx.CORTAO.Options()
	options.filter = osgx.CORTAO.Filter.SPARSE
	options.sampleCount = 8
	options.bentNormal = True

	cortao = osgx.CORTAO.create(normal, position, projection, W, H, options=options)

	assert cortao.filter == osgx.CORTAO.Filter.SPARSE
	assert cortao.sampleCount == 8
	assert isinstance(cortao.bentNormalTexture, osg.Texture2D)

	cortao.filter = osgx.CORTAO.Filter.NONE
	cortao.sampleCount = 32
	cortao.largeFilter = False

	assert cortao.filter == osgx.CORTAO.Filter.NONE
	assert cortao.sampleCount == 32
	assert not cortao.largeFilter

def test_cortao_occluder_lookup_table():
	image = osgx.CORTAO.createOccluderLookupTable()

	assert (image.s, image.t, image.r) == (24, 24, 16)

def test_computepass():
	cp = osgx.ComputePass("test")

	assert isinstance(cp, osg.DispatchCompute)
	assert cp.name == "test"
	assert cp.program is None
	assert cp.barriers == osgx.ComputePass.TEXTURE_BARRIERS

	cp.program = osgx.ComputePass.createProgram(
		"test",
		"#version 430\nlayout(local_size_x = 8, local_size_y = 8) in;\nvoid main() {}"
	)

	assert isinstance(cp.program, osg.Program)

	texture = osgx.ComputePass.createTexture("output", 16, 16, GL_R8)

	assert isinstance(texture, osg.Texture2D)

	cp.bindImage(0, texture, osg.BindImageTexture.WRITE_ONLY, GL_R8)
	cp.bindTexture(1, texture, "inputTexture")
	cp.setComputeGroupsFor(17, 16, 8, 8)

	assert cp.computeGroups == (3, 2, 1)

	cp.setRenderBin(-1)
	cp.setDefineEnabled("FOO", True)

	assert cp.isDefineEnabled("FOO")
	assert not cp.isDefineEnabled("BAR")

	cp.barriers = 0

	assert cp.barriers == 0
