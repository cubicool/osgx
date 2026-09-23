#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Camera>
#include <osg/Plane>
#include <osg/Vec2>
#include <osg/Vec3>
#include <osg/Viewport>

OSGX_ENABLE_WARNINGS

namespace osgx {

// ================================================================================================
// CPU-side screen/world projection helpers - the C++ twin of the shared GLSL osgx_Unproject()
// below (#pragma osgx::projection UNPROJECT), and a general primitive for cursor/screen-driven
// world-space interaction (a mouse-dragged gizmo, a ground-plane pick, a HUD element following a
// 3D point, etc.) - not cursor-specific itself despite the motivating use case; nothing here
// touches osgx::CursorState/CursorCallback.
//
// Motivated by osgSlug's pyosgslug-cone-widget.py prototype, whose own make_unprojector()/
// ndc_from_event() reconstructed a camera basis from fovy/aspect by hand - a workaround only
// needed because the Python bindings there have no bound vec*matrix operator. The C++ side
// inverts view*projection directly instead, the same math examples/osgx-grid.cpp's and
// examples/osgx-turntable.cpp's own GLSL unproject() already did on the GPU (identically
// duplicated between the two files until this header existed) - and unlike fovy/aspect
// reconstruction, this also works for orthographic projections.
// ================================================================================================

// Window/event coordinates -> NDC ([-1, 1]^2, Y-up - OSG/GL's own convention), given `viewport`'s
// own rect. (x, y) must be in the SAME absolute space as GUIEventAdapter::getX()/getY() - if the
// 3D viewport doesn't start at the window's own (0, 0) (e.g. part of the window is a docked UI
// panel), subtract that origin first, the same correction PickReadback::setWindowOrigin() already
// solved for pick sub-frustums (see Picking.hpp). yIncreasingDownwards must match the SAME event's
// own GUIEventAdapter::getMouseYOrientation() (Y_INCREASING_DOWNWARDS) - real per-platform/
// per-event state, not something safe to default, so this takes it explicitly rather than guessing.
osg::Vec2d windowToNDC(
	const osg::Viewport* viewport, double x, double y, bool yIncreasingDownwards
);

// A world-space ray: origin + t*direction, t >= 0. direction is unit length.
struct Ray {
	osg::Vec3d origin;
	osg::Vec3d direction;
};

// Casts a world-space ray from `camera` through NDC point (ndcX, ndcY) (Y-up, [-1, 1]^2 - see
// windowToNDC() above), via inverse(view*projection) evaluated at the near and far planes - the
// exact CPU-side twin of the shared GLSL osgx_Unproject() below. Works for orthographic cameras
// too, unlike reconstructing a basis from fovy/aspect (which only has meaning for a symmetric
// perspective frustum).
Ray unprojectRay(const osg::Camera* camera, double ndcX, double ndcY);

// Ray/plane intersection: solves ray.origin + t*ray.direction for the point where `plane`'s own
// equation is zero. `plane` must be normalized (a^2+b^2+c^2=1) - see osg::Plane::distance()'s own
// precondition, which this reuses directly rather than re-deriving the plane equation by hand.
// Returns false for a ray parallel to the plane, or a hit behind the ray's origin (t < 0).
bool intersectRayPlane(const Ray& ray, const osg::Plane& plane, osg::Vec3d& outPoint);

// One-call glue: window/event coordinates -> world-space point on `plane`, combining
// windowToNDC() + unprojectRay() + intersectRayPlane(). This is exactly what
// pyosgslug-cone-widget.py's make_unprojector() + ndc_from_event() + its own z=0-plane solve did
// by hand, per MOVE event, in the prototype that motivated this file.
bool unprojectToPlane(
	const osg::Camera* camera,
	const osg::Viewport* viewport,
	double x, double y,
	bool yIncreasingDownwards,
	const osg::Plane& plane,
	osg::Vec3d& outPoint
);

// Single-point unproject at an explicit depth: the CPU-side twin of the shared GLSL
// `osgx_Unproject(vec2 ndc, float depth)` below, mirroring its signature exactly (unlike
// unprojectRay(), which only ever exposes the near/far pair together). Motivated by the
// osgx-aoe example's Mode 2 (decal on an arbitrary model surface via osgx::GBuffer): approach
// (a) there reads a single depth-buffer sample back to the CPU at the
// cursor's pixel and needs to turn that (screen x/y, depth) triple into a world-space point,
// which is exactly this call - no plane, no ray, just the one point.
osg::Vec3d unprojectPoint(const osg::Camera* camera, double ndcX, double ndcY, double ndcDepth);

// GLSL `#pragma osgx::projection UNPROJECT` / `#pragma osgx::projection DEPTH` catalog
// registration - see registerShaderLibs()/resolveShaderLibs() in Shader.hpp.
//
// UNPROJECT publishes `vec3 osgx_Unproject(vec2 ndc, float depth)`, matching this file's own
// unprojectRay() math exactly (inverse(osg_ProjectionMatrix) * ndc, then osg_ViewMatrixInverse),
// extracted from the identical, previously-duplicated GLSL function in examples/osgx-grid.cpp
// and examples/osgx-turntable.cpp.
//
// DEPTH publishes `float osgx_LinearizeDepth(float depth, mat4 projectionMatrix)` -
// depth-buffer sample -> distance from the camera along the view axis, deriving near/far
// implicitly from projectionMatrix's own [2][2]/[3][2] entries rather than needing a separate
// znear/zfar uniform PAIR the caller must decompose by hand (osg::Matrixd::getPerspective())
// and keep in sync - extracted from OpenSceneGraph.py/examples/pyosg-rtt.py's and
// pyosg-mrt.py's own identical, previously-duplicated `linearizeDepth(d, near, far)`.
//
// projectionMatrix is a REQUIRED parameter, deliberately not read ambiently from OSG's own
// automatic `osg_ProjectionMatrix` uniform the way osgx_Unproject() above does: that ambient
// value is only ever the CURRENTLY DRAWING camera's own projection, which is correct when
// linearizing depth inline in the same pass that produced it, but silently WRONG the moment the
// depth sample came from a DIFFERENT camera - exactly pyosg-rtt.py/pyosg-mrt.py's own real
// shape (a G-buffer geometry pass's real perspective projection, sampled later by a separate
// ABSOLUTE_RF composite/HUD pass whose own osg_ProjectionMatrix is identity, not the G-buffer
// camera's). That mismatch is also why those two files' own invProjectionMatrix/znear/zfar
// uniforms need a preDrawCallback bridging the ORIGINAL camera's live matrix into the composite
// pass's StateSet every frame in the first place (same shape as osgx-gbuffer.cpp's own
// UpdateLightingPassCallback) - passing projectionMatrix explicitly here doesn't remove that
// bridge (it can't be removed: OSG's automatic per-camera uniforms have no way to carry a
// DIFFERENT camera's matrix across passes), it just makes the one bridged uniform this function
// actually needs unambiguous at the call site instead of silently assumed.
void registerProjectionShaderLibs();

}
