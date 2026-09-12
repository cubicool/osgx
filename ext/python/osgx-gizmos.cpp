#include "osgx-python.hpp"
#include "osgx/Gizmos.hpp"
#include "osgx/PBR.hpp"

namespace osgx_python {

void bind_gizmos(py::module_& m) {
	py::class_<
		osgx::LightMarkers,
		osg::Group,
		osg::ref_ptr<osgx::LightMarkers>
	>(
		m,
		"LightMarkers",
		"Depth-tested, real scene-space markers for point/sphere/spot lights (up to "
		"osgx.MAX_LIGHTS), rebuilt from the live LightSet every update traversal. A directional "
		"light has no position and is never drawn here - see LightGizmos, which pairs this with "
		"a directional-only overlay. Contributes nothing to any ancestor's bounding sphere."
	)
		.def(
			py::init<const osgx::LightSet&, float, float>(),
			"lights"_a,
			"minMarkerRadius"_a=0.05f,
			"spotConeLength"_a=1.0f,
			"`lights` is the live LightSet this marker set visualizes; markers track it every frame."
		)
	;

	py::class_<
		osgx::LightGizmos,
		osg::Group,
		osg::ref_ptr<osgx::LightGizmos>
	>(
		m,
		"LightGizmos",
		"Bundles LightMarkers (depth-tested point/spot/sphere markers) and a directional-only "
		"overlay camera for a LightSet into one addable node. Deliberately contributes nothing to "
		"any ancestor's bounding sphere - a gizmo annotates a scene, it must never influence how "
		"that scene is framed or clipped."
	)
		.def(
			py::init<const osgx::LightSet&, osg::Node*, float, float>(),
			"lights"_a,
			"scene"_a,
			"minMarkerRadius"_a=0.05f,
			"spotConeLength"_a=1.0f,
			"Bundles LightMarkers (point/sphere/spot) and a directional-only overlay camera for a "
			"LightSet into one addable node."
		)
		.def_property_readonly(
			"markers", &osgx::LightGizmos::getMarkers,
			"The LightMarkers child visualizing point/sphere/spot lights."
		)
		.def_property_readonly(
			"overlay", &osgx::LightGizmos::getOverlay,
			"The non-depth-tested POST_RENDER camera drawing directional-light plane/arrow overlays."
		)
	;
}

}
