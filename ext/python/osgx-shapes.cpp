#include "osgx-python.hpp"
#include "osgx/Shapes.hpp"

namespace osgx_python {

void bind_shapes(py::module_& m) {
	py::class_<osgx::VertexLayout>(
		m,
		"VertexLayout",
		"Generic vertex attribute locations a Polyhedron's Geometry is built with - position/"
		"normal/uv are installed both through Geometry's conventional arrays (for OSG's bounds "
		"and compatibility machinery) and at these explicit locations (for core-profile shaders)."
	)
		.def(py::init<>(), "Constructs the default layout (position=0, normal=1, uv=3).")
		.def_readwrite("position", &osgx::VertexLayout::position, "Generic attribute location for vertex position.")
		.def_readwrite("normal", &osgx::VertexLayout::normal, "Generic attribute location for vertex normal.")
		.def_readwrite("uv", &osgx::VertexLayout::uv, "Generic attribute location for vertex UV.")
	;

	py::class_<
		osgx::Polyhedron,
		osg::Geometry,
		osg::ref_ptr<osgx::Polyhedron>
	> polyhedron(
		m,
		"Polyhedron",
		"A polygonal, flat-shaded mesh with explicit GPU vertex attribute locations. Custom "
		"per-face streams are supplied in original face-corner order, not expanded triangle "
		"order - rebuild() duplicates them exactly as it duplicates positions for fan "
		"triangulation, so a per-face material ID, tangent, or decal attribute is equally "
		"straightforward to add."
	);

	py::class_<osgx::Polyhedron::Face>(
		polyhedron,
		"Face",
		"A face referring to vertices in its owning Polyhedron's base-vertex list. Winding must "
		"be outward: (v[1] - v[0]) x (v[2] - v[0]) points away from the solid. Each face is "
		"fan-triangulated and receives its own output vertices, so its normal is truly flat."
	)
		.def(py::init<>(), "Constructs an empty face with no vertices.")
		.def(
			py::init<std::vector<std::uint32_t>, std::vector<osg::Vec2>>(),
			"vertices"_a,
			"uv"_a=std::vector<osg::Vec2>{},
			"Constructs a face from base-vertex indices, with optional per-corner UVs."
		)
		.def_readwrite("vertices", &osgx::Polyhedron::Face::vertices, "Base-vertex indices, in winding order.")
		.def_readwrite("uv", &osgx::Polyhedron::Face::uv, "Optional per-corner UVs, parallel to `vertices`.")
		.def(
			"origin",
			&osgx::Polyhedron::Face::origin,
			"positions"_a,
			"The face's first vertex position, from the owning Polyhedron's `positions`."
		)
		.def(
			"center",
			&osgx::Polyhedron::Face::center,
			"positions"_a,
			"The planar polygon centroid, suitable for centering an inset decal on the face."
		)
		.def(
			"normal",
			&osgx::Polyhedron::Face::normal,
			"positions"_a,
			"The face's flat normal, derived from `positions` rather than cached."
		)
		.def(
			"right",
			&osgx::Polyhedron::Face::right,
			"positions"_a,
			"The face's local right basis vector, derived from `positions`."
		)
		.def(
			"up",
			&osgx::Polyhedron::Face::up,
			"positions"_a,
			"The face's local up basis vector, perpendicular to the first directed edge and "
			"pointing into the face."
		)
		.def(
			"planeCoordinates",
			&osgx::Polyhedron::Face::planeCoordinates,
			"positions"_a,
			"Every boundary vertex expressed relative to `origin()` in the face's right/up basis."
		)
	;

	polyhedron
		.def(py::init<>(), "Constructs an empty Polyhedron with no vertices or faces.")
		.def(
			py::init<
				std::vector<osg::Vec3>,
				std::vector<osgx::Polyhedron::Face>,
				osgx::VertexLayout
			>(),
			"vertices"_a,
			"faces"_a,
			"layout"_a=osgx::VertexLayout{},
			"Constructs a Polyhedron from a base-vertex list, a Face list referring into it, "
			"and an optional VertexLayout."
		)
		.def_property(
			"vertices",
			[](const osgx::Polyhedron& self) { return self.vertices(); },
			&osgx::Polyhedron::setVertices,
			"The base-vertex position list Face indices refer into."
		)
		.def_property(
			"faces",
			[](const osgx::Polyhedron& self) { return self.faces(); },
			&osgx::Polyhedron::setFaces,
			"The Face list, in original face-corner order (not expanded triangle order)."
		)
		.def_property(
			"layout",
			&osgx::Polyhedron::layout,
			&osgx::Polyhedron::setLayout,
			"The generic vertex attribute locations this Polyhedron's Geometry was built with."
		)
		.def(
			"setFaceVertexAttribute",
			&osgx::Polyhedron::setFaceVertexAttribute,
			"location"_a,
			"values"_a,
			"Installs a custom per-face-corner attribute at `location`. `values` has one element "
			"per input face corner (face 0's corners, then face 1's, etc.); the array type is "
			"preserved, so osg.Vec2Array, osg.Vec3Array, osg.FloatArray, etc. all work."
		)
		.def(
			"setFaceAttribute",
			&osgx::Polyhedron::setFaceAttribute,
			"location"_a,
			"values"_a,
			"Installs a custom per-face attribute at `location`. `values` has one element per "
			"face; each value is repeated for every triangle vertex emitted from that face - "
			"useful for face IDs and material parameters."
		)
		.def(
			"removeAttribute",
			&osgx::Polyhedron::removeAttribute,
			"location"_a,
			"Removes a previously installed per-face or per-face-vertex attribute at `location`."
		)
		.def(
			"rebuild",
			&osgx::Polyhedron::rebuild,
			"Regenerates the underlying Geometry's arrays from the current vertices/faces/"
			"attributes. Call after mutating them in place."
		)
		.def(
			"faceNormal",
			py::overload_cast<std::size_t>(&osgx::Polyhedron::faceNormal, py::const_),
			"faceIndex"_a,
			"The flat normal of face `faceIndex`, identifying a support face for ground alignment."
		)
		.def(
			"faceUp",
			&osgx::Polyhedron::faceUp,
			"faceIndex"_a,
			"The canonical up direction of face `faceIndex`, perpendicular to its first directed "
			"edge and pointing into the face - a dice/decal layer's text-up direction."
		)
		.def(
			"restingOffset",
			&osgx::Polyhedron::restingOffset,
			"orientation"_a=osg::Quat(),
			"Translation along world +Z needed to place the lowest oriented vertex at z=0."
		)
		.def(
			"faceRestingOffset",
			&osgx::Polyhedron::faceRestingOffset,
			"faceIndex"_a,
			"orientation"_a=osg::Quat(),
			"Translation along world +Z needed to place face `faceIndex`'s plane at z=0, "
			"preserving that support face even while a roll is still settling."
		)
		.def_static(
			"isometricFaceUV",
			&osgx::Polyhedron::isometricFaceUV,
			"vertices"_a,
			"face"_a,
			"Computes isometric-projection UVs for `face`'s boundary, so a square decal stays "
			"square on the real (possibly non-planar-friendly) face shape."
		)
	;

	py::class_<
		osgx::Cube,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::Cube>
	>(
		m,
		"Cube",
		"A six-faced Polyhedron, equivalent in dimensions to osg.Box but with face-unique "
		"vertices, explicit generic attributes, and a predictable UV square on every face."
	)
		.def(
			py::init<const osg::Vec3&, const osg::Vec3&, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(),
			"size"_a=osg::Vec3(1.0f, 1.0f, 1.0f),
			"layout"_a=osgx::VertexLayout{},
			"Constructs a cube of the given full (width, height, depth) `size` centered at `center`."
		)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(),
			"radius"_a=1.0f,
			"layout"_a=osgx::VertexLayout{},
			"Constructs a cube whose corners lie at distance `radius` from `center`."
		)
	;

	py::class_<
		osgx::Tetrahedron,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::Tetrahedron>
	>(
		m,
		"Tetrahedron",
		"A regular, four-faced Polyhedron. `radius` is the distance from `center` to every corner."
	)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(), "radius"_a=1.0f, "layout"_a=osgx::VertexLayout{},
			"Constructs a tetrahedron whose corners lie at distance `radius` from `center`."
		)
	;

	py::class_<
		osgx::Octahedron,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::Octahedron>
	>(
		m,
		"Octahedron",
		"A regular, eight-faced Polyhedron. `radius` is the distance from `center` to every corner."
	)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(), "radius"_a=1.0f, "layout"_a=osgx::VertexLayout{},
			"Constructs an octahedron whose corners lie at distance `radius` from `center`."
		)
	;

	py::class_<
		osgx::Icosahedron,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::Icosahedron>
	>(
		m,
		"Icosahedron",
		"A regular, twenty-faced Polyhedron. `radius` is the distance from `center` to every corner."
	)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(), "radius"_a=1.0f, "layout"_a=osgx::VertexLayout{},
			"Constructs an icosahedron whose corners lie at distance `radius` from `center`."
		)
	;

	py::class_<
		osgx::Dodecahedron,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::Dodecahedron>
	>(
		m,
		"Dodecahedron",
		"A regular, twelve-faced Polyhedron. `radius` is the distance from `center` to every "
		"corner. Its pentagonal UVs are isometric projections of the actual face, rather than "
		"guessed UVs."
	)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(), "radius"_a=1.0f, "layout"_a=osgx::VertexLayout{},
			"Constructs a dodecahedron whose corners lie at distance `radius` from `center`."
		)
	;

	py::class_<
		osgx::PentagonalTrapezohedron,
		osgx::Polyhedron,
		osg::ref_ptr<osgx::PentagonalTrapezohedron>
	>(
		m,
		"PentagonalTrapezohedron",
		"The ten-faced solid commonly used for a D10. Its kite faces are given isometric UVs, so "
		"a square decal stays square on the real face. `radius` is the distance from center to "
		"either apex."
	)
		.def(
			py::init<const osg::Vec3&, float, osgx::VertexLayout>(),
			"center"_a=osg::Vec3(), "radius"_a=1.0f, "layout"_a=osgx::VertexLayout{},
			"Constructs a pentagonal trapezohedron whose apexes lie at distance `radius` from `center`."
		)
	;
}

}
