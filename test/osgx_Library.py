import osgx
import pytest

def test_slots_lists_declarations_and_assigned_indices(osgx_library):
	index = osgx_library.binding("osgx::joints")
	slots = {slot.name: slot for slot in osgx_library.slots()}

	assert slots["osgx::joints"].type == osgx.Bindings.Type.SSBO
	assert slots["osgx::joints"].index == index
	assert slots["osgx::material"].type == osgx.Bindings.Type.UBO
	assert slots["osgx::material.baseColor"].type == osgx.Bindings.Type.TextureUnit

def test_declare_after_first_lookup_raises(osgx_library):
	osgx_library.binding("osgx::joints")

	with pytest.raises(Exception):
		osgx_library.declare(osgx.Bindings.Type.UBO, "test::late")

	assert "test::late" not in {slot.name for slot in osgx_library.slots()}
