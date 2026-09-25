import osgx

def test_sdf_catalog_expands_shapes_pragma():
	resolved = osgx.resolveShaderLibs("#pragma osgx::sdf SHAPES\n")

	# One entry, nine functions - see SDF.hpp's own comment for why these aren't split into
	# individually-selectable tags the way Projection.hpp's UNPROJECT/DEPTH are.
	for name in (
		"osgx_SDF_Circle",
		"osgx_SDF_Rect",
		"osgx_SDF_Capsule",
		"osgx_SDF_Arc",
		"osgx_SDF_ArcBand",
		"osgx_SDF_Rotate",
		"osgx_SDF_Hexagon",
		"osgx_SDF_Octagon",
		"osgx_SDF_Star",
	):
		assert name in resolved

	# The two renamed-during-extraction names (matching slughorn::Mask::Type's own vocabulary,
	# not osgSlug's previously-independent GLSL names) must NOT reappear under their old spelling.
	assert "osgx_SDF_Box" not in resolved
	assert "osgx_SDF_Pie" not in resolved

def test_sampling_and_texture_expand_in_order():
	resolved = osgx.resolveShaderLibs("#pragma osgx::sdf SAMPLING,TEXTURE\n")

	for name in (
		"osgx_SDF_Median",
		"osgx_SDF_ScreenPixelRange",
		"osgx_SDF_CoverageFromDistance",
		"osgx_SDF_Coverage",
		"osgx_sdfTexture",
	):
		assert name in resolved

	# TEXTURE calls into SAMPLING, so SAMPLING must be spliced first (GLSL needs declare-before-use).
	assert resolved.index("osgx_SDF_Median") < resolved.index("osgx_SDF_Coverage(vec2 uv)")

def test_sdf_attribute_roundtrip_and_defaults():
	sdf = osgx.SDF()

	assert sdf.sdfType == osgx.SDF.SDFType.SDF
	assert sdf.pixelRange == 4.0

	sdf.sdfType = osgx.SDF.SDFType.MSDF
	sdf.pixelRange = 8.0

	assert sdf.sdfType == osgx.SDF.SDFType.MSDF
	assert sdf.pixelRange == 8.0
