#include "osgx-python.hpp"
#include "osgx/Projection.hpp"

#include <optional>

namespace osgx_python {

// osgx::Projection - CPU-side screen/world projection helpers (Ray, windowToNDC, unprojectRay,
// intersectRayPlane, unprojectToPlane) plus the shared GLSL osgx_Unproject() registration. Plain
// osgx:: from the start (unlike pbr/shadow/gbuffer/ibl/picking/cursor above, this one never lived
// under a namespaced submodule), so its bindings go straight onto the top-level module too. The
// two "solves for a point" functions take a bool+out-Vec3d& shape in C++ (ordinary OSG idiom) but
// return std::optional<osg::Vec3d> here instead - Python has no reference out-parameters.
void bind_projection(py::module_& m) {
	py::class_<
		osgx::DepthProjectionCallback,
		osg::Camera::DrawCallback,
		osg::ref_ptr<osgx::DepthProjectionCallback>
	>(
		m,
		"DepthProjectionCallback",
		"Records the projection matrix a camera actually draws with into two mat4 uniforms, "
		"`name` and `name` + \"Inverse\", for a later pass that samples that camera's depth "
		"(osgx_LinearizeDepth()/osgx_ViewPositionFromDepth(), #pragma osgx::projection DEPTH, "
		"VIEW_POSITION). Install it as the depth-producing camera's postDrawCallback and add "
		"projection/projectionInverse to the consuming pass's StateSet."
	)
		.def(py::init<const std::string&>(), "name"_a="osgx_depthProjection")
		.def_property_readonly(
			"projection",
			&osgx::DepthProjectionCallback::getProjection,
			"The FLOAT_MAT4 projection uniform."
		)
		.def_property_readonly(
			"projectionInverse",
			&osgx::DepthProjectionCallback::getProjectionInverse,
			"The FLOAT_MAT4 inverse-projection uniform."
		)
		.def_property_readonly(
			"projectionMatrix",
			&osgx::DepthProjectionCallback::getProjectionMatrix,
			"The most recently captured projection (identity until the camera first draws), for "
			"CPU-side use such as unprojectPoint(view, projection, ...)."
		)
	;

	py::class_<osgx::Ray>(
		m,
		"Ray",
		"A world-space ray: origin + t*direction, t >= 0. direction is unit length."
	)
		.def(py::init<>())
		.def_readwrite("origin", &osgx::Ray::origin)
		.def_readwrite("direction", &osgx::Ray::direction)
	;

	m.def(
		"windowToNDC",
		&osgx::windowToNDC,
		"viewport"_a,
		"x"_a,
		"y"_a,
		"yIncreasingDownwards"_a,
		"Window/event coordinates -> NDC ([-1, 1]^2, Y-up). (x, y) must be in the same absolute "
		"space as GUIEventAdapter.x/y - subtract the viewport's own origin first if it doesn't "
		"start at the window's (0, 0). yIncreasingDownwards must match the same event's own "
		"GUIEventAdapter.mouseYOrientation (Y_INCREASING_DOWNWARDS)."
	);

	m.def(
		"unprojectRay",
		&osgx::unprojectRay,
		"camera"_a,
		"ndcX"_a,
		"ndcY"_a,
		"Casts a world-space Ray from `camera` through NDC point (ndcX, ndcY) (Y-up, [-1, 1]^2 - "
		"see windowToNDC()), via inverse(view*projection) - works for orthographic cameras too."
	);

	m.def(
		"intersectRayPlane",
		[](const osgx::Ray& ray, const osg::Plane& plane) -> std::optional<osg::Vec3d> {
			osg::Vec3d point;

			if(!osgx::intersectRayPlane(ray, plane, point)) return std::nullopt;

			return point;
		},
		"ray"_a,
		"plane"_a,
		"Ray/plane intersection; `plane` must be normalized (a^2+b^2+c^2=1) - see "
		"osg.Plane.distance()'s own precondition. Returns None for a ray parallel to the plane, "
		"or a hit behind the ray's origin."
	);

	m.def(
		"unprojectToPlane",
		[](
			const osg::Camera* camera,
			const osg::Viewport* viewport,
			double x, double y,
			bool yIncreasingDownwards,
			const osg::Plane& plane
		) -> std::optional<osg::Vec3d> {
			osg::Vec3d point;

			if(
				!osgx::unprojectToPlane(camera, viewport, x, y, yIncreasingDownwards, plane, point)
			) return std::nullopt;

			return point;
		},
		"camera"_a,
		"viewport"_a,
		"x"_a,
		"y"_a,
		"yIncreasingDownwards"_a,
		"plane"_a,
		"One-call glue: window/event coordinates -> world-space point on `plane`, combining "
		"windowToNDC() + unprojectRay() + intersectRayPlane(). Returns None on a miss - see "
		"intersectRayPlane()'s own docstring for why."
	);

	m.def(
		"unprojectPoint",
		py::overload_cast<const osg::Camera*, double, double, double>(&osgx::unprojectPoint),
		"camera"_a,
		"ndcX"_a,
		"ndcY"_a,
		"ndcDepth"_a,
		"Single-point unproject at an explicit depth - the CPU-side twin of the shared GLSL "
		"osgx_Unproject(vec2 ndc, float depth), mirroring its signature exactly. Unlike "
		"unprojectRay() (near/far pair only), this takes one specific depth - e.g. a "
		"depth-buffer sample read back at the cursor's pixel."
	);

	m.def(
		"unprojectPoint",
		py::overload_cast<const osg::Matrixd&, const osg::Matrixd&, double, double, double>(
			&osgx::unprojectPoint
		),
		"view"_a,
		"projection"_a,
		"ndcX"_a,
		"ndcY"_a,
		"ndcDepth"_a,
		"The same, through explicit view/projection matrices - e.g. a camera's viewMatrix and "
		"the DepthProjectionCallback.projectionMatrix its depth was actually written with."
	);

}

}
