#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Quat>

OSGX_ENABLE_WARNINGS

#include <cstdint>
#include <tuple>
#include <vector>

namespace osgx {

// Attribute locations used by the generated Geometry.  Position, normal, and UV are installed
// both through Geometry's conventional arrays (for OSG's bounds and compatibility machinery)
// and through these explicit generic attributes (for core-profile shaders).
struct VertexLayout {
	unsigned int position = 0;
	unsigned int normal = 1;
	unsigned int uv = 3;
};

// A polygonal, flat-shaded mesh with explicit GPU vertex attribute locations.  Custom streams
// are supplied in the original face-corner order, rather than expanded triangle order: rebuild()
// duplicates them exactly as it duplicates positions for fan triangulation.  This makes a
// per-face material ID, tangent, or a future dice decal attribute equally straightforward.
class Polyhedron: public osg::Geometry {
public:
	OSGX_META_Object(osgx_shapes, Polyhedron)

	// A face refers to vertices in this Polyhedron's base-vertex list. Its winding must be outward:
	// (v[1] - v[0]) x (v[2] - v[0]) points away from the solid. Each face is fan-triangulated,
	// and receives its own output vertices so its normal is truly flat.
	struct Face {
		std::vector<std::uint32_t> vertices;
		std::vector<osg::Vec2> uv;

		// These are derived from the Polyhedron's base-vertex positions rather than cached here, so
		// changing those positions cannot leave a face with stale geometry. `origin` is the first
		// face vertex; planeCoordinates() expresses every boundary vertex relative to it in the
		// right/up basis. `center` is the planar polygon centroid, suitable for centering an inset
		// decal on any valid face.
		osg::Vec3 origin(const std::vector<osg::Vec3>& positions) const;
		osg::Vec3 center(const std::vector<osg::Vec3>& positions) const;
		osg::Vec3 normal(const std::vector<osg::Vec3>& positions) const;
		osg::Vec3 right(const std::vector<osg::Vec3>& positions) const;
		osg::Vec3 up(const std::vector<osg::Vec3>& positions) const;
		std::vector<osg::Vec2> planeCoordinates(const std::vector<osg::Vec3>& positions) const;

		// Computes origin/normal/right/up together, each intermediate reused rather than
		// rederived, for callers that would otherwise chain 4 separate calls that redundantly
		// redo each other's work (up() alone recomputes both normal() and right()).
		// Order: origin, normal, right, up - a tuple has no field names, so callers should
		// unpack it with a structured binding at the call site (or std::tie into existing
		// variables, which a structured binding cannot do).
		std::tuple<osg::Vec3, osg::Vec3, osg::Vec3, osg::Vec3> basis(
			const std::vector<osg::Vec3>& positions
		) const;

		// Reduces planeCoordinates()'s output to its (u,v) bounding box, since anchoring a decal
		// to a face is the only thing planeCoordinates() is ever used for next.
		// Order: minU, maxU, minV, maxV.
		std::tuple<float, float, float, float> extent(const std::vector<osg::Vec3>& positions) const;
	};

	Polyhedron() = default;
	Polyhedron(std::vector<osg::Vec3> vertices, std::vector<Face> faces, VertexLayout layout={});
	Polyhedron(const Polyhedron& polyhedron, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY);

	const std::vector<osg::Vec3>& vertices() const { return _vertices; }
	const std::vector<Face>& faces() const { return _faces; }
	const VertexLayout& layout() const { return _layout; }
	// The face winding establishes a stable local basis.  faceUp() is perpendicular to the first
	// directed edge and points into the face; a dice/decal layer can use it as its canonical text-up
	// direction, while faceNormal() identifies a support face for ground alignment.
	osg::Vec3 faceNormal(std::size_t faceIndex) const;
	osg::Vec3 faceUp(std::size_t faceIndex) const;

	// Translation along world +Z needed to place the lowest oriented vertex at z=0.  Keeping this
	// generic geometry calculation here lets an animation/dice layer choose its own support policy.
	float restingOffset(const osg::Quat& orientation=osg::Quat()) const;
	// Translation along world +Z needed to place this oriented face's plane at z=0.  Unlike
	// restingOffset(), this preserves a caller-selected support face even while a roll is still
	// settling; it is useful for any flat-based token or polyhedral game piece.
	float faceRestingOffset(std::size_t faceIndex, const osg::Quat& orientation=osg::Quat()) const;

	void setVertices(std::vector<osg::Vec3> vertices);
	void setFaces(std::vector<Face> faces);
	void setLayout(VertexLayout layout);

	// `values` has one element per input face corner: face 0's corners, then face 1's, etc.
	// The array type is preserved, so osg::Vec2Array, osg::Vec3Array, osg::FloatArray, and so on
	// can all become shader inputs without Shapes having to know their element type.
	void setFaceVertexAttribute(unsigned int location, osg::Array* values);

	// `values` has one element per face.  Each value is repeated for every triangle vertex emitted
	// from that face; useful for face IDs and material parameters.
	void setFaceAttribute(unsigned int location, osg::Array* values);
	void removeAttribute(unsigned int location);

	void rebuild();

	static osg::Vec3 faceNormal(const std::vector<osg::Vec3>& vertices, const Face& face);
	static std::vector<osg::Vec2> isometricFaceUV(
		const std::vector<osg::Vec3>& vertices,
		const Face& face
	);

private:
	enum class AttributeDomain {
		Face,
		FaceVertex
	};

	struct Attribute {
		unsigned int location;
		AttributeDomain domain;
		osg::ref_ptr<osg::Array> values;
	};

	std::size_t faceVertexCount() const;
	void setAttribute(unsigned int location, AttributeDomain domain, osg::Array* values);

	std::vector<osg::Vec3> _vertices;
	std::vector<Face> _faces;
	VertexLayout _layout;
	std::vector<Attribute> _attributes;
};

// A six-faced Polyhedron, equivalent in dimensions to osg::Box but with face-unique vertices,
// explicit generic attributes, and a predictable UV square on every face.
class Cube: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, Cube)

	Cube(const osg::Vec3& center=osg::Vec3(), const osg::Vec3& size=osg::Vec3(1.0f, 1.0f, 1.0f), VertexLayout layout={});
	// This overload matches the other named polyhedra: `radius` is the distance from `center` to
	// every corner. The size-based constructor remains available for box-style uses.
	Cube(const osg::Vec3& center, float radius, VertexLayout layout={});
	Cube(const Cube& cube, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	Polyhedron(cube, co) {}
};

// A regular, four-faced Polyhedron.  `radius` is the distance from `center` to every corner.
class Tetrahedron: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, Tetrahedron)

	Tetrahedron(const osg::Vec3& center=osg::Vec3(), float radius=1.0f, VertexLayout layout={});
	Tetrahedron(const Tetrahedron& tetrahedron, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	Polyhedron(tetrahedron, co) {}
};

// A regular, eight-faced Polyhedron.  `radius` is the distance from `center` to every corner.
class Octahedron: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, Octahedron)

	Octahedron(const osg::Vec3& center=osg::Vec3(), float radius=1.0f, VertexLayout layout={});
	Octahedron(const Octahedron& octahedron, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	Polyhedron(octahedron, co) {}
};

// A regular, twenty-faced Polyhedron.  `radius` is the distance from `center` to every corner.
class Icosahedron: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, Icosahedron)

	Icosahedron(const osg::Vec3& center=osg::Vec3(), float radius=1.0f, VertexLayout layout={});
	Icosahedron(const Icosahedron& icosahedron, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	Polyhedron(icosahedron, co) {}
};

// A regular, twelve-faced Polyhedron.  `radius` is the distance from `center` to every corner.
// Its pentagonal UVs are isometric projections of the actual face, rather than guessed UVs.
class Dodecahedron: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, Dodecahedron)

	Dodecahedron(const osg::Vec3& center=osg::Vec3(), float radius=1.0f, VertexLayout layout={});
	Dodecahedron(const Dodecahedron& dodecahedron, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	Polyhedron(dodecahedron, co) {}
};

// The ten-faced solid commonly used for a D10.  Its kite faces are given isometric UVs, so a
// square decal stays square on the real face.  `radius` is the distance from center to either apex.
class PentagonalTrapezohedron: public Polyhedron {
public:
	OSGX_META_Object(osgx_shapes, PentagonalTrapezohedron)

	PentagonalTrapezohedron(const osg::Vec3& center=osg::Vec3(), float radius=1.0f, VertexLayout layout={});
	PentagonalTrapezohedron(
		const PentagonalTrapezohedron& pentagonalTrapezohedron,
		const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY
	):
	Polyhedron(pentagonalTrapezohedron, co) {}
};

}
