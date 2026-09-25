#include "ShaderLibs.hpp"

#include "osgx/Projection.hpp"
#include "osgx/Shader.hpp"

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Matrixd>

OSGX_ENABLE_WARNINGS

#include <cmath>

namespace osgx {

namespace {

// Self-contained (declares its own osg_ProjectionMatrix/osg_ViewMatrixInverse) so a caller's
// shader needs nothing but the #pragma line and a call site - matching the same pattern
// the "osgx::picking" catalog's PICK_ENCODE_SRC uses (see Picking.cpp). Extracted verbatim from
// examples/osgx-grid.cpp's and examples/osgx-turntable.cpp's own identical, previously-duplicated
// unproject(ndc, depth) function - see unprojectRay()'s own comment for why the CPU side doesn't
// need this same text-splicing trick to stay in sync with it (it's hand-mirrored instead, and both
// are short enough that drift would be obvious on review).
constexpr const char* PROJECTION_UNPROJECT_SRC = R"GLSL(
uniform mat4 osg_ProjectionMatrix;
uniform mat4 osg_ViewMatrixInverse;

vec3 osgx_Unproject(vec2 ndc, float depth) {
	vec4 view = inverse(osg_ProjectionMatrix) * vec4(ndc, depth, 1.0);

	view /= view.w;

	vec4 world = osg_ViewMatrixInverse * view;

	return world.xyz / world.w;
}
)GLSL";

// See the "osgx::projection" catalog comment (Projection.hpp) for the derivation: solving
// ndcZ = clip.z/clip.w for eye.z using only projectionMatrix's [2][2]/[3][2] entries (a standard
// symmetric perspective matrix's own near/far encoding), then negating - eye-space Z is negative
// in front of the camera, and every caller of the previous, duplicated linearizeDepth(d, near,
// far) expected a positive distance instead. projectionMatrix is a required parameter, NOT read
// from the ambient osg_ProjectionMatrix uniform - see that same comment for why (it's only ever
// the CURRENTLY DRAWING camera's own projection, silently wrong the moment the depth sample came
// from a different camera, which is the real, motivating case here).
constexpr const char* PROJECTION_DEPTH_SRC = R"GLSL(
float osgx_LinearizeDepth(float depth, mat4 projectionMatrix) {
	float ndcZ = depth * 2.0 - 1.0;
	float a = projectionMatrix[2][2];
	float b = projectionMatrix[3][2];

	return b / (ndcZ + a);
}
)GLSL";

}

void registerProjectionShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"UNPROJECT", "osgx_Unproject", PROJECTION_UNPROJECT_SRC},
		{"DEPTH", "osgx_LinearizeDepth", PROJECTION_DEPTH_SRC}
	};

	registerShaderLibs("osgx::projection", libs);
}

osg::Vec2d windowToNDC(
	const osg::Viewport* viewport, double x, double y, bool yIncreasingDownwards
) {
	double ndcX = (x - viewport->x()) / viewport->width() * 2.0 - 1.0;
	double ndcY = (y - viewport->y()) / viewport->height() * 2.0 - 1.0;

	if(yIncreasingDownwards) ndcY = -ndcY;

	return osg::Vec2d(ndcX, ndcY);
}

Ray unprojectRay(const osg::Camera* camera, double ndcX, double ndcY) {
	osg::Matrixd invViewProj;

	invViewProj.invert(camera->getViewMatrix() * camera->getProjectionMatrix());

	osg::Vec3d nearPoint = osg::Vec3d(ndcX, ndcY, -1.0) * invViewProj;
	osg::Vec3d farPoint = osg::Vec3d(ndcX, ndcY, 1.0) * invViewProj;

	Ray ray;

	ray.origin = nearPoint;
	ray.direction = farPoint - nearPoint;
	ray.direction.normalize();

	return ray;
}

bool intersectRayPlane(const Ray& ray, const osg::Plane& plane, osg::Vec3d& outPoint) {
	double denom = plane.dotProductNormal(ray.direction);

	if(std::abs(denom) < 1e-9) return false;

	double t = -plane.distance(ray.origin) / denom;

	if(t < 0.0) return false;

	outPoint = ray.origin + ray.direction * t;

	return true;
}

bool unprojectToPlane(
	const osg::Camera* camera,
	const osg::Viewport* viewport,
	double x, double y,
	bool yIncreasingDownwards,
	const osg::Plane& plane,
	osg::Vec3d& outPoint
) {
	osg::Vec2d ndc = windowToNDC(viewport, x, y, yIncreasingDownwards);
	Ray ray = unprojectRay(camera, ndc.x(), ndc.y());

	return intersectRayPlane(ray, plane, outPoint);
}

osg::Vec3d unprojectPoint(const osg::Camera* camera, double ndcX, double ndcY, double ndcDepth) {
	osg::Matrixd invViewProj;

	invViewProj.invert(camera->getViewMatrix() * camera->getProjectionMatrix());

	return osg::Vec3d(ndcX, ndcY, ndcDepth) * invViewProj;
}

}
