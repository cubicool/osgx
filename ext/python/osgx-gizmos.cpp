#include "osgx-python.hpp"
#include "osgx/Gizmos.hpp"

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

	py::class_<
		osgx::FrustumGizmo,
		osg::Group,
		osg::ref_ptr<osgx::FrustumGizmo>
	>(
		m,
		"FrustumGizmo",
		"Depth-tested wireframe box reconstructed every update traversal from a live view/"
		"projection pair (unprojecting the 8 NDC corners) - works identically for an orthographic "
		"frustum (ShadowMap.create()'s directional camera) or a perspective one "
		"(ShadowMap.createSpot()'s spot camera). Not tied to ShadowMap or LightSet - compose it "
		"alongside LightGizmos yourself. Two constructors: pass an osg.Camera directly for the "
		"common case (tracks it live, so it stays correct across ShadowMap.reposition()/"
		"repositionSpot() with no extra wiring), or pass a zero-argument callable returning "
		"(view, projection) each call for any other live source - a hand-built frustum, a portal/"
		"mirror plane, anything that isn't a real rendering camera at all. Contributes nothing to "
		"any ancestor's bounding sphere."
	)
		.def(
			py::init<osg::Camera*, const osg::Vec3&>(),
			"camera"_a,
			"color"_a=osg::Vec3(1.0f, 1.0f, 1.0f),
			"`camera` is read fresh every update traversal - most often a ShadowMap's own `camera`."
		)
		.def(
			py::init<osgx::FrustumGizmo::MatrixSource, const osg::Vec3&>(),
			"source"_a,
			"color"_a=osg::Vec3(1.0f, 1.0f, 1.0f),
			"`source` is called fresh every update traversal and must return a (view, projection) "
			"tuple of osg.Matrixd - the generic form for any frustum source that isn't a real "
			"osg.Camera."
		)
	;

	py::class_<
		osgx::CaptureCubeGizmo,
		osg::Group,
		osg::ref_ptr<osgx::CaptureCubeGizmo>
	>(
		m,
		"CaptureCubeGizmo",
		"Depth-tested wireframe AXIS-ALIGNED CUBE visualizing a point light's shadow CAPTURE range "
		"- not its illumination falloff (physically boundless, no hard edge to draw), but the real "
		"hard boundary a CaptureCubeMap enforces: geometry beyond the shared far plane is never "
		"rendered into any of its six faces, so nothing out there can cast a shadow regardless of "
		"how bright the light is. Two constructors, same split as FrustumGizmo: pass any single "
		"face camera from a CaptureCubeMap directly for the common case (center/half-size are "
		"decomposed from it live every update traversal), or pass a zero-argument callable "
		"returning (center, halfSize) each call for any other live source. Contributes nothing to "
		"any ancestor's bounding sphere."
	)
		.def(
			py::init<osg::Camera*, const osg::Vec3&>(),
			"cubeFaceCamera"_a,
			"color"_a=osg::Vec3(1.0f, 1.0f, 1.0f),
			"`cubeFaceCamera` is any single element of CaptureCubeMap.cameras, e.g. "
			"shadowMap.cubeCapture.cameras[0]."
		)
		.def(
			py::init<osgx::CaptureCubeGizmo::RangeSource, const osg::Vec3&>(),
			"source"_a,
			"color"_a=osg::Vec3(1.0f, 1.0f, 1.0f),
			"`source` is called fresh every update traversal and must return a (center, halfSize) "
			"tuple - halfSize <= 0 skips that update's rebuild."
		)
	;
}

}
