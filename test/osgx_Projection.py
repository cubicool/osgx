import osgx

from OpenSceneGraph import *

def make_camera(eye=osg.Vec3d(0, 0, 10), center=osg.Vec3d(0, 0, 0), up=osg.Vec3d(0, 1, 0)):
	cam = osg.Camera()

	cam.viewMatrix = osg.Matrixd.lookAt(eye, center, up)
	cam.projectionMatrix = osg.Matrixd.perspective(45.0, 800.0 / 600.0, 0.1, 1000.0)

	return cam

def test_window_to_ndc_center_and_corner():
	vp = osg.Viewport(0, 0, 800, 600)

	center = osgx.windowToNDC(vp, 400, 300, True)

	assert abs(center.x) < 1e-9
	assert abs(center.y) < 1e-9

	# Window-space top-right (800, 0), Y_INCREASING_DOWNWARDS -> NDC (1, 1): the flip is what
	# turns a top-of-window Y=0 into NDC's own bottom-left-origin, Y-up convention.
	top_right = osgx.windowToNDC(vp, 800, 0, True)

	assert abs(top_right.x - 1.0) < 1e-9
	assert abs(top_right.y - 1.0) < 1e-9

	# Without the flip (Y already increasing upwards), the same (800, 0) window point is instead
	# NDC's bottom-right.
	bottom_right = osgx.windowToNDC(vp, 800, 0, False)

	assert abs(bottom_right.x - 1.0) < 1e-9
	assert abs(bottom_right.y - (-1.0)) < 1e-9

def test_window_to_ndc_respects_viewport_origin():
	# A viewport that doesn't start at the window's own (0, 0) -- e.g. the 3D view is confined to
	# part of a larger window alongside some other docked UI.
	vp = osg.Viewport(100, 50, 800, 600)
	center = osgx.windowToNDC(vp, 500, 350, True)

	assert abs(center.x) < 1e-9
	assert abs(center.y) < 1e-9

def test_unproject_ray_center_of_screen_looks_down_view_direction():
	cam = make_camera()
	ray = osgx.unprojectRay(cam, 0.0, 0.0)

	# The near-plane point for the center NDC pixel lies on the camera's own look axis (straight
	# down -Z from an eye at (0,0,10) looking at the origin), and the ray direction matches it.
	assert abs(ray.origin.x) < 1e-6
	assert abs(ray.origin.y) < 1e-6
	assert abs(ray.direction.x) < 1e-9
	assert abs(ray.direction.y) < 1e-9
	assert ray.direction.z < 0.0

	# direction must be unit length
	length = (ray.direction.x**2 + ray.direction.y**2 + ray.direction.z**2) ** 0.5

	assert abs(length - 1.0) < 1e-9

def test_unproject_ray_off_center_diverges_outward():
	cam = make_camera()
	ray = osgx.unprojectRay(cam, 0.5, 0.5)

	# A top-right NDC point casts a ray angled up and to the right of the view axis.
	assert ray.direction.x > 0.0
	assert ray.direction.y > 0.0

def test_unproject_ray_works_for_orthographic_projection():
	# fovy/aspect reconstruction (the cone-widget prototype's own workaround) has no meaning for
	# an orthographic frustum -- this is exactly the case unprojectRay() was designed to still
	# get right, via inverse(view*projection) instead.
	cam = osg.Camera()

	cam.viewMatrix = osg.Matrixd.lookAt(osg.Vec3d(0, 0, 10), osg.Vec3d(0, 0, 0), osg.Vec3d(0, 1, 0))
	cam.projectionMatrix = osg.Matrixd.ortho(-10, 10, -10, 10, 0.1, 1000.0)

	center = osgx.unprojectRay(cam, 0.0, 0.0)

	assert abs(center.origin.x) < 1e-6
	assert abs(center.origin.y) < 1e-6
	assert abs(center.direction.x) < 1e-9
	assert abs(center.direction.y) < 1e-9

	# Unlike perspective, an orthographic ray through an off-center NDC point stays PARALLEL to
	# the view axis -- only its origin shifts, not its direction.
	offset = osgx.unprojectRay(cam, 0.5, 0.5)

	assert abs(offset.direction.x) < 1e-9
	assert abs(offset.direction.y) < 1e-9
	assert offset.origin.x > 0.0
	assert offset.origin.y > 0.0

def test_intersect_ray_plane_hit():
	ray = osgx.Ray()

	ray.origin = osg.Vec3d(0, 0, 5)
	ray.direction = osg.Vec3d(0, 0, -1)

	plane = osg.Plane(osg.Vec3d(0, 0, 1), 0.0) # z=0

	hit = osgx.intersectRayPlane(ray, plane)

	assert hit is not None
	assert abs(hit.x) < 1e-9
	assert abs(hit.y) < 1e-9
	assert abs(hit.z) < 1e-9

def test_intersect_ray_plane_parallel_misses():
	ray = osgx.Ray()

	ray.origin = osg.Vec3d(0, 0, 5)
	ray.direction = osg.Vec3d(1, 0, 0) # parallel to the z=0 plane

	plane = osg.Plane(osg.Vec3d(0, 0, 1), 0.0)

	assert osgx.intersectRayPlane(ray, plane) is None

def test_intersect_ray_plane_behind_origin_misses():
	ray = osgx.Ray()

	ray.origin = osg.Vec3d(0, 0, 5)
	ray.direction = osg.Vec3d(0, 0, 1) # pointing away from the z=0 plane

	plane = osg.Plane(osg.Vec3d(0, 0, 1), 0.0)

	assert osgx.intersectRayPlane(ray, plane) is None

def test_intersect_ray_plane_angled_hit_on_offset_plane():
	ray = osgx.Ray()

	ray.origin = osg.Vec3d(0, 0, 10)
	ray.direction = osg.Vec3d(1, 0, -1)
	ray.direction.normalize()

	plane = osg.Plane(osg.Vec3d(0, 0, 1), -4.0) # z=4 plane

	hit = osgx.intersectRayPlane(ray, plane)

	assert hit is not None
	assert abs(hit.z - 4.0) < 1e-9
	assert abs(hit.x - 6.0) < 1e-9 # 45-degree ray traveling 6 in z (10 -> 4) covers 6 in x too

def test_unproject_to_plane_center_of_screen():
	cam = make_camera()
	vp = osg.Viewport(0, 0, 800, 600)
	plane = osg.Plane(osg.Vec3d(0, 0, 1), 0.0)

	hit = osgx.unprojectToPlane(cam, vp, 400, 300, True, plane)

	assert hit is not None
	assert abs(hit.x) < 1e-6
	assert abs(hit.y) < 1e-6
	assert abs(hit.z) < 1e-6

def test_unproject_point_matches_unproject_ray_origin_at_near_plane():
	cam = make_camera()
	ray = osgx.unprojectRay(cam, 0.3, -0.4)
	point = osgx.unprojectPoint(cam, 0.3, -0.4, -1.0)

	assert abs(point.x - ray.origin.x) < 1e-9
	assert abs(point.y - ray.origin.y) < 1e-9
	assert abs(point.z - ray.origin.z) < 1e-9

def test_unproject_point_center_of_screen_near_plane():
	cam = make_camera()
	point = osgx.unprojectPoint(cam, 0.0, 0.0, -1.0)

	assert abs(point.x) < 1e-6
	assert abs(point.y) < 1e-6
	# Near-plane point on the look axis, in front of the eye at (0, 0, 10).
	assert point.z < 10.0

def test_unproject_to_plane_returns_none_on_miss():
	cam = make_camera()
	vp = osg.Viewport(0, 0, 800, 600)
	# A plane the camera's own view ray, from this eye/center setup, never reaches.
	plane = osg.Plane(osg.Vec3d(0, 0, 1), -1000.0) # z=1000, behind the camera

	assert osgx.unprojectToPlane(cam, vp, 400, 300, True, plane) is None

def test_register_projection_shader_libs_expands_unproject_pragma():
	osgx.registerProjectionShaderLibs()

	resolved = osgx.resolveShaderLibs("#pragma osgx::projection UNPROJECT\n")

	assert "osgx_Unproject" in resolved
	assert "osg_ProjectionMatrix" in resolved
	assert "osg_ViewMatrixInverse" in resolved

def test_register_projection_shader_libs_expands_depth_pragma():
	osgx.registerProjectionShaderLibs()

	resolved = osgx.resolveShaderLibs("#pragma osgx::projection DEPTH\n")

	# projectionMatrix is a required function PARAMETER, not an ambient osg_ProjectionMatrix
	# uniform declaration - see Projection.hpp's own comment for why (correct in the same pass
	# that produced the depth, silently wrong the moment a different camera produced it).
	assert "osgx_LinearizeDepth" in resolved
	assert "mat4 projectionMatrix" in resolved
	assert "uniform mat4 osg_ProjectionMatrix" not in resolved

def test_register_projection_shader_libs_is_idempotent():
	# registerShaderLibs() only throws on a genuine content conflict -- re-registering the same
	# catalog (e.g. called from more than one module/example) must be a safe no-op.
	osgx.registerProjectionShaderLibs()
	osgx.registerProjectionShaderLibs()
