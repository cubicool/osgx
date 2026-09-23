#pragma once

#include "Array.hpp"
#include "Core.hpp"
#include "Light.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/NodeCallback>
#include <osg/observer_ptr>
#include <osg/StateSet>

OSGX_ENABLE_WARNINGS

namespace osgx {

// Debug visualization for osgx::LightSet lights - deliberately not part of osgx::debug, which is
// specifically GL_KHR_debug integration, not visual scene gizmos. Two mechanisms, since a
// directional light and a point/spot/sphere light are genuinely different visualization problems
// (see LightGizmos below): only the latter has a real position to place depth-tested geometry at.

// Depth-tested, real scene-space markers for point/sphere/spot lights (up to osgx::MAX_LIGHTS),
// added as an ordinary child of the lit scene. One osg::Geometry, rebuilt in place every update
// traversal from the live LightSet uniforms (an osg::NodeCallback installed on this Group) --
// combines Shapes.hpp's Polyhedron::rebuild() "mutate the existing arrays, don't replace them"
// pattern with the per-frame-uniform-read NodeCallback idiom already used by PBR.hpp's
// OrbitLightRig and Picking.hpp's PickCameraSync. Marker shape per active light: three orthogonal
// wireframe circles sized to max(lightSourceRadius, minMarkerRadius) for a point/sphere light (so
// an ideal point light still shows a small marker, a sphere light shows its true physical size); a
// wireframe cone (ring + spokes from the apex) for a spot light, sized by its outer cone angle and
// spotConeLength. A directional light has no position and is never drawn here - see LightGizmos
// below, which pairs this with a directional-only overlay.
class LightMarkers: public osg::Group {
public:
	OSGX_META_Object(osgx_gizmo, LightMarkers)

	LightMarkers() = default;
	// `lights` is the live osgx::LightSet this marker set visualizes.
	explicit LightMarkers(
		const osgx::LightSet& lights, float minMarkerRadius=0.05f, float spotConeLength=1.0f
	);
	LightMarkers(const LightMarkers& rhs, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	osg::Group(rhs, co) {}

	// Contributes nothing to any ancestor's bounding sphere - see LightGizmos::computeBound().
	osg::BoundingSphere computeBound() const override { return osg::BoundingSphere(); }

private:
	class UpdateCallback: public osg::NodeCallback {
	public:
		UpdateCallback(const osgx::LightSet& lights, float minMarkerRadius, float spotConeLength):
		_lights(const_cast<osgx::LightSet*>(&lights)),
		_minMarkerRadius(minMarkerRadius),
		_spotConeLength(spotConeLength) {}

		void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

	private:
		osg::ref_ptr<osgx::LightSet> _lights;
		float _minMarkerRadius;
		float _spotConeLength;
	};

	void rebuild(const osgx::LightSet& lights, float minMarkerRadius, float spotConeLength);

	osg::ref_ptr<osg::Geometry> _geometry;
};

// Bundles both LightMarkers (depth-tested point/spot/sphere markers) and a directional-only
// overlay camera into one addable node - `root->addChild(gizmos)` instead of a caller
// hand-wiring two separate pieces into every example. The overlay is a non-depth-tested
// POST_RENDER child camera (a directional light has no position, so there is no real depth to
// test its marker against); it ports create_light_gizmo()/LightGizmoCallback/
// GIZMO_VERTEX_SHADER/GIZMO_FRAGMENT_SHADER from
// OpenSceneGraph.py/examples/pyosg-lighting/11-sketchfab-lambertian.py (wireframe plane
// perpendicular to the light direction plus a direction arrow) essentially unchanged, generalized
// from one hardcoded light_dir_u/light_color_u pair to LightSet's up-to-MAX_LIGHTS directional
// slots. `scene`'s bounding sphere (computed once, at construction time, same as the Python
// original) sizes and places every directional light's plane/arrow proportionally to the scene.
//
// `minMarkerRadius`/`spotConeLength` forward straight to LightMarkers - their defaults are
// unit-scene-scale, and a caller whose lights sit much farther from the target than that (a spot
// light standing well back from its subject, say) needs to size them up or the cone/sphere
// markers draw too small/short to visually reach anything.
class LightGizmos: public osg::Group {
public:
	OSGX_META_Object(osgx_gizmo, LightGizmos)

	LightGizmos() = default;
	explicit LightGizmos(
		const osgx::LightSet& lights,
		osg::Node* scene,
		float minMarkerRadius=0.05f,
		float spotConeLength=1.0f
	);
	LightGizmos(const LightGizmos& rhs, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	osg::Group(rhs, co) {}

	// Deliberately contributes NOTHING to any ancestor's bounding sphere. A gizmo annotates a
	// scene; it must never influence how that scene is framed or clipped. Left to the default
	// Group::computeBound(), this node actively fights the thing it is annotating: the overlay's
	// plane/arrow are sized off `scene`'s own bound at construction and are therefore always
	// LARGER than the scene, so a root bound including them makes TrackballManipulator's home
	// framing pull back further than the model needs, and pushes CULL's computed near/far out to
	// cover geometry the viewer does not care about. Worse, the markers track live light
	// positions, so the root bound would shift every time a light is dragged.
	//
	// The tradeoff is explicit: a scene containing ONLY gizmos has no bound to frame. That is the
	// correct reading - there would be nothing being annotated.
	osg::BoundingSphere computeBound() const override { return osg::BoundingSphere(); }

	LightMarkers* getMarkers() const { return _markers.get(); }
	osg::Camera* getOverlay() const { return _overlay.get(); }

private:
	osg::ref_ptr<LightMarkers> _markers;
	osg::ref_ptr<osg::Camera> _overlay;
};

}
