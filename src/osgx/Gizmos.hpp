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

#include <functional>
#include <utility>

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

// Depth-tested wireframe box reconstructed every update traversal from a live view/projection
// pair, via `ndcCorner * inverse(view * projection)` unprojection - `osg::Vec3 * osg::Matrixd`
// already performs the homogeneous divide (`Matrixd::preMult()`), so the SAME code correctly
// handles an ORTHOGRAPHIC frustum (osgx::ShadowMap::create()'s directional camera) and a
// PERSPECTIVE one (ShadowMap::createSpot()'s spot camera) - no light-kind-specific branch anywhere
// in this class. NOT tied to osgx::ShadowMap or osgx::LightSet at all - compose it alongside
// LightGizmos at the application level, same "composition stays at the app level" choice as
// osgx::platform::PointerCapture + OrbitAxisManipulator (see Cursor.hpp's own header comment for
// the full rationale) - and see this file's own header comment for why that composition choice is
// NOT the same split as "light gizmo vs. shadow gizmo": a light's own marker/cone/arrow (LightSet
// data) exists independently of whether that light has a ShadowMap at all, while this class draws
// the SHADOW CAMERA's own setup, which only exists once one has actually been built for it.
class FrustumGizmo: public osg::Group {
public:
	OSGX_META_Object(osgx_gizmo, FrustumGizmo)

	// Returns {view, projection} fresh each call - the generic customization point: ANY live
	// source of a view/projection pair can drive this gizmo, not just a real osg::Camera (a
	// hand-built culling volume, a portal/mirror plane, a frustum never actually attached to a
	// real rendering camera at all). Returns BY VALUE rather than by-reference out-params
	// specifically so this signature also works as a Python callable - pybind11's std::function
	// caster can correctly marshal a Python function's RETURN value, but cannot propagate
	// mutations to C++ reference out-params back across the boundary.
	using MatrixSource = std::function<std::pair<osg::Matrixd, osg::Matrixd>()>;

	FrustumGizmo() = default;
	explicit FrustumGizmo(MatrixSource source, const osg::Vec3& color=osg::Vec3(1.0f, 1.0f, 1.0f));
	// Convenience overload for the common case - a real osg::Camera (most often
	// ShadowMap::camera). Tracks `camera` live, re-reading its matrices every update traversal, so
	// it stays correct across ShadowMap::reposition()/repositionSpot() calls with no extra wiring.
	explicit FrustumGizmo(osg::Camera* camera, const osg::Vec3& color=osg::Vec3(1.0f, 1.0f, 1.0f));
	FrustumGizmo(const FrustumGizmo& rhs, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	osg::Group(rhs, co) {}

	// Contributes nothing to any ancestor's bounding sphere - same reasoning as LightGizmos'/
	// LightMarkers' own computeBound() override (see LightGizmos' own comment): a gizmo annotates a
	// scene, it must never influence how that scene is framed or clipped, and the frustum this
	// draws is frequently LARGER than the scene it shadows.
	osg::BoundingSphere computeBound() const override { return osg::BoundingSphere(); }

private:
	class UpdateCallback: public osg::NodeCallback {
	public:
		UpdateCallback(MatrixSource source, const osg::Vec3& color):
		_source(std::move(source)), _color(color) {}

		void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

	private:
		MatrixSource _source;
		osg::Vec3 _color;
	};

	void rebuild(const osg::Matrixd& view, const osg::Matrixd& proj, const osg::Vec3& color);

	osg::ref_ptr<osg::Geometry> _geometry;
};

// Depth-tested wireframe AXIS-ALIGNED CUBE visualizing a point light's shadow CAPTURE range - not
// its illumination falloff (physically boundless/inverse-square, no hard edge to draw), but the
// real hard boundary osgx::CaptureCubeMap enforces: geometry beyond the shared far plane is never
// rendered into any of the six faces at all, so nothing out there can ever cast a shadow regardless
// of how bright the light is. Same "light gizmo vs. shadow gizmo" split as FrustumGizmo's own
// header comment - this only exists once a CaptureCubeMap has actually been built, not a property
// of the point light itself.
class CaptureCubeGizmo: public osg::Group {
public:
	OSGX_META_Object(osgx_gizmo, CaptureCubeGizmo)

	// Returns {center, halfSize} fresh each call - the same "any live source, not just a real
	// osg::Camera" customization point as FrustumGizmo::MatrixSource, and for the identical
	// reason: return-by-value so this signature also works as a Python callable. `halfSize <= 0`
	// skips that update's rebuild entirely (used by the osg::Camera convenience overload below
	// while its projection matrix isn't yet a valid perspective one to decompose).
	using RangeSource = std::function<std::pair<osg::Vec3, float>()>;

	CaptureCubeGizmo() = default;
	explicit CaptureCubeGizmo(
		RangeSource source, const osg::Vec3& color=osg::Vec3(1.0f, 1.0f, 1.0f)
	);
	// Convenience overload for the common case - any single face camera from a CaptureCubeMap,
	// e.g. `shadowMap.cubeCapture.cameras[0]`; center/halfSize are decomposed from it live every
	// update traversal via Matrixd::getLookAt()/getPerspective() (all six faces share the same eye
	// position and far plane, only their look direction differs, so any one works).
	explicit CaptureCubeGizmo(
		osg::Camera* cubeFaceCamera, const osg::Vec3& color=osg::Vec3(1.0f, 1.0f, 1.0f)
	);
	CaptureCubeGizmo(const CaptureCubeGizmo& rhs, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	osg::Group(rhs, co) {}

	// See FrustumGizmo::computeBound()'s own comment - identical reasoning.
	osg::BoundingSphere computeBound() const override { return osg::BoundingSphere(); }

private:
	class UpdateCallback: public osg::NodeCallback {
	public:
		UpdateCallback(RangeSource source, const osg::Vec3& color):
		_source(std::move(source)), _color(color) {}

		void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

	private:
		RangeSource _source;
		osg::Vec3 _color;
	};

	void rebuild(const osg::Vec3& center, float halfSize, const osg::Vec3& color);

	osg::ref_ptr<osg::Geometry> _geometry;
};

}
