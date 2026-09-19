import osgx

def test_register_sdf_shader_libs_expands_shapes_pragma():
	osgx.registerSDFShaderLibs()

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

def test_register_sdf_shader_libs_is_idempotent():
	# registerShaderLibs() only throws on a genuine content conflict -- re-registering the same
	# catalog (e.g. called from more than one module/example) must be a safe no-op.
	osgx.registerSDFShaderLibs()
	osgx.registerSDFShaderLibs()
