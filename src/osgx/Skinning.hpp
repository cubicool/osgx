#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/BufferIndexBinding>
#include <osg/Callback>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Referenced>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace osgx {

// ================================================================================================
// Mesh vertex attributes and linear blend skinning
//
// Position, normal, color, and texture coordinates use OSG's conventional arrays. A tangent and
// per-vertex joint indices/weights use these generic vertex attribute locations;
// bindMeshAttributes() binds their GLSL names to them on a Program. The glTF loader fills them for
// every primitive that has them.
//
// The "osgx::skinning" catalog (registered by osgx::Library, expanded by resolveShaderLibs() in
// Shader.hpp) has two entries:
//   SKINNING_DECL - struct osgx_SkinnedVertex {position, normal, tangent} plus the prototype
//                   osgx_SkinnedVertex osgx_ApplySkin(vec4 position, vec3 normal, vec3 tangent).
//                   A vertex shader calls osgx_ApplySkin() on the raw vertex/normal/tangent before
//                   any camera transform; the definition comes from a separate VERTEX shader
//                   object (the osgx::Hook::Skinning slot), the same "declaration + call site
//                   here, definition in a hook" pattern Light.hpp's DIRECT_LIGHTING_DECL uses.
//   JOINT_INPUTS  - the joint index/weight attributes and the joint-matrix storage buffer
//                   (mat4 osgx_jointMatrices[], at the "osgx::joints" SSBO slot).
//
// SKINNING_HOOK_IDENTITY defines osgx_ApplySkin() as a passthrough; SKINNING_HOOK_LINEAR_BLEND
// deforms by the weighted sum of four joint matrices. Both are complete shader sources: pass them
// through resolveShaderLibs() before wrapping them in an osg::Shader.
// ================================================================================================

inline constexpr unsigned int TANGENT_ATTRIBUTE = 7;
inline constexpr unsigned int JOINT_INDICES_ATTRIBUTE = 8;
inline constexpr unsigned int JOINT_WEIGHTS_ATTRIBUTE = 9;

inline constexpr char TANGENT_ATTRIBUTE_NAME[] = "osg_Tangent";
inline constexpr char JOINT_INDICES_ATTRIBUTE_NAME[] = "osgx_JointIndices";
inline constexpr char JOINT_WEIGHTS_ATTRIBUTE_NAME[] = "osgx_JointWeights";

inline constexpr char SKINNING_HOOK_IDENTITY[] = R"GLSL(
#version 460 core

#pragma osgx::skinning SKINNING_DECL

osgx_SkinnedVertex osgx_ApplySkin(vec4 position, vec3 normal, vec3 tangent) {
	return osgx_SkinnedVertex(position, normal, tangent);
}
)GLSL";

inline constexpr char SKINNING_HOOK_LINEAR_BLEND[] = R"GLSL(
#version 460 core

#pragma osgx::skinning SKINNING_DECL, JOINT_INPUTS

osgx_SkinnedVertex osgx_ApplySkin(vec4 position, vec3 normal, vec3 tangent) {
	mat4 skin =
		osgx_JointWeights.x * osgx_jointMatrices[osgx_JointIndices.x] +
		osgx_JointWeights.y * osgx_jointMatrices[osgx_JointIndices.y] +
		osgx_JointWeights.z * osgx_jointMatrices[osgx_JointIndices.z] +
		osgx_JointWeights.w * osgx_jointMatrices[osgx_JointIndices.w];

	return osgx_SkinnedVertex(
		skin * position,
		mat3(skin) * normal,
		mat3(skin) * tangent
	);
}
)GLSL";

// Binds TANGENT_ATTRIBUTE_NAME, JOINT_INDICES_ATTRIBUTE_NAME and JOINT_WEIGHTS_ATTRIBUTE_NAME to
// their attribute locations on `program`.
void bindMeshAttributes(osg::Program& program);

// True if any osg::Geometry under `node` has a JOINT_WEIGHTS_ATTRIBUTE array. Linear blend skinning
// applies to a whole Program, and a vertex with no joint arrays reads GL's default attribute values
// (and is deformed by an arbitrary joint matrix), so use SKINNING_HOOK_LINEAR_BLEND only when this
// is true.
bool hasJointWeights(osg::Node* node);

// ================================================================================================
// A skeleton's joint palette source: the joint transforms, in palette order, and one inverse bind
// matrix per joint. Palette entry i is
//
//   inverseBind[i] * jointWorld[i] * inverse(meshWorld)
//
// so a skinned mesh deforms by its joints' current world transforms and ignores its own. Joint
// world matrices are read from the scene graph as they are; whatever moves the joints (an
// animation callback, application code) is independent of the Skin.
//
// A joint whose first parent is another joint of the same Skin composes its local matrix onto that
// joint's world matrix; any other joint uses getWorldMatrices().front(). Joints are held by
// observer_ptr: a joint deleted from the graph keeps its last palette entry.
// ================================================================================================
class Skin: public osg::Referenced {
public:
	// `inverseBindMatrices` entries missing past its end are identity.
	Skin(
		const std::vector<osg::MatrixTransform*>& joints,
		const std::vector<osg::Matrixf>& inverseBindMatrices
	);

	std::size_t getNumJoints() const;

	// Writes this Skin's palette for `mesh` into `palette` (resized to getNumJoints()). Returns
	// false if `mesh` is null.
	bool computePalette(const osg::Node* mesh, osg::MatrixfArray& palette);

	// Makes `mesh` a skinned mesh of this Skin: gives it its own palette (a MatrixfArray backed by
	// a ShaderStorageBufferObject), bound in its StateSet at the "osgx::joints" slot, and a
	// SkinPaletteCallback update callback that recomputes it every update traversal. Geometry under
	// `mesh` supplies JOINT_INDICES_ATTRIBUTE/JOINT_WEIGHTS_ATTRIBUTE and draws with a
	// SKINNING_HOOK_LINEAR_BLEND (or equivalent) Hook::Skinning shader. Returns the palette.
	osg::MatrixfArray* attach(osg::Node* mesh);

private:
	osg::Matrixd _computeJointWorld(std::size_t jointIndex);

	std::vector<osg::observer_ptr<osg::MatrixTransform>> _joints;
	std::vector<osg::Matrixf> _inverseBindMatrices;
	std::unordered_map<const osg::Node*, std::size_t> _jointIndices;

	// Scratch reused by every computePalette() call.
	std::vector<osg::Matrixd> _jointWorlds;
	std::vector<char> _jointWorldComputed;
};

// The update callback Skin::attach() installs. The palette's storage buffer binding is created with
// index 0 and set to the "osgx::joints" slot on the first update (a Skin may be attached before
// osgx::initialize()).
class SkinPaletteCallback: public osg::NodeCallback {
public:
	SkinPaletteCallback(
		Skin* skin,
		osg::MatrixfArray* palette,
		osg::ShaderStorageBufferBinding* binding
	);

	Skin* getSkin() const;
	osg::MatrixfArray* getPalette() const;

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	osg::ref_ptr<Skin> _skin;
	osg::ref_ptr<osg::MatrixfArray> _palette;
	osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
	std::once_flag _bindingResolved;
};

}
