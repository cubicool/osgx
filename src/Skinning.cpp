#include "ShaderLibs.hpp"

#include "osgx/Library.hpp"
#include "osgx/Shader.hpp"
#include "osgx/Skinning.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferObject>

OSGX_ENABLE_WARNINGS

#include <algorithm>

namespace osgx {

namespace {

constexpr const char* SKINNING_DECL_SRC = R"GLSL(
struct osgx_SkinnedVertex {
	vec4 position;
	vec3 normal;
	vec3 tangent;
};

osgx_SkinnedVertex osgx_ApplySkin(vec4 position, vec3 normal, vec3 tangent);
)GLSL";

// Locations 8/9 match JOINT_INDICES_ATTRIBUTE/JOINT_WEIGHTS_ATTRIBUTE (Skinning.hpp).
constexpr const char* JOINT_INPUTS_SRC = R"GLSL(
layout(location = 8) in uvec4 osgx_JointIndices;
layout(location = 9) in vec4 osgx_JointWeights;

layout(std430, binding = @osgx::joints@) readonly buffer osgx_JointMatrixBuffer {
	mat4 osgx_jointMatrices[];
};
)GLSL";

}

void registerSkinningShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"SKINNING_DECL", "osgx_SkinnedVertex", SKINNING_DECL_SRC},
		{"JOINT_INPUTS", "osgx_JointMatrixBuffer", JOINT_INPUTS_SRC}
	};

	registerShaderLibs("osgx::skinning", libs);
}

void bindMeshAttributes(osg::Program& program) {
	program.addBindAttribLocation(TANGENT_ATTRIBUTE_NAME, TANGENT_ATTRIBUTE);
	program.addBindAttribLocation(JOINT_INDICES_ATTRIBUTE_NAME, JOINT_INDICES_ATTRIBUTE);
	program.addBindAttribLocation(JOINT_WEIGHTS_ATTRIBUTE_NAME, JOINT_WEIGHTS_ATTRIBUTE);
}

Skin::Skin(
	const std::vector<osg::MatrixTransform*>& joints,
	const std::vector<osg::Matrixf>& inverseBindMatrices
):
_joints(joints.begin(), joints.end()),
_inverseBindMatrices(inverseBindMatrices),
_jointWorlds(joints.size()),
_jointWorldComputed(joints.size()) {
	_inverseBindMatrices.resize(joints.size(), osg::Matrixf::identity());

	for(std::size_t i = 0; i < joints.size(); i++) {
		if(joints[i]) _jointIndices.emplace(joints[i], i);
	}
}

std::size_t Skin::getNumJoints() const {
	return _joints.size();
}

osg::Matrixd Skin::_computeJointWorld(std::size_t jointIndex) {
	if(_jointWorldComputed[jointIndex]) return _jointWorlds[jointIndex];

	osg::ref_ptr<osg::MatrixTransform> joint;

	_joints[jointIndex].lock(joint);

	osg::Matrixd world;

	if(joint) {
		auto parent = joint->getNumParents() ?
			_jointIndices.find(joint->getParent(0)) :
			_jointIndices.end()
		;

		if(parent != _jointIndices.end()) {
			world = joint->getMatrix() * _computeJointWorld(parent->second);
		}
		else {
			osg::MatrixList worlds = joint->getWorldMatrices();

			world = worlds.empty() ? joint->getMatrix() : worlds.front();
		}
	}

	_jointWorlds[jointIndex] = world;
	_jointWorldComputed[jointIndex] = 1;

	return world;
}

bool Skin::computePalette(const osg::Node* mesh, osg::MatrixfArray& palette) {
	if(!mesh) return false;

	osg::Matrixd worldToMesh;
	osg::MatrixList meshWorlds = mesh->getWorldMatrices();

	if(!meshWorlds.empty()) worldToMesh.invert(meshWorlds.front());

	if(palette.size() != _joints.size()) palette.resize(
		static_cast<unsigned int>(_joints.size()),
		osg::Matrixf::identity()
	);

	std::fill(_jointWorldComputed.begin(), _jointWorldComputed.end(), 0);

	for(std::size_t i = 0; i < _joints.size(); i++) {
		if(!_joints[i].valid()) continue;

		osg::Matrixd inverseBind = _inverseBindMatrices[i];

		palette[i] = osg::Matrixf(inverseBind * _computeJointWorld(i) * worldToMesh);
	}

	palette.dirty();

	return true;
}

osg::MatrixfArray* Skin::attach(osg::Node* mesh) {
	if(!mesh) return nullptr;

	auto* palette = new osg::MatrixfArray(static_cast<unsigned int>(_joints.size()));

	palette->setBufferObject(new osg::ShaderStorageBufferObject());

	computePalette(mesh, *palette);

	auto* binding = new osg::ShaderStorageBufferBinding(
		0,
		palette,
		0,
		static_cast<GLsizeiptr>(palette->getTotalDataSize())
	);

	mesh->addUpdateCallback(new SkinPaletteCallback(this, palette, binding));
	mesh->getOrCreateStateSet()->setAttributeAndModes(binding, osg::StateAttribute::ON);

	return palette;
}

SkinPaletteCallback::SkinPaletteCallback(
	Skin* skin,
	osg::MatrixfArray* palette,
	osg::ShaderStorageBufferBinding* binding
):
_skin(skin),
_palette(palette),
_binding(binding) {}

Skin* SkinPaletteCallback::getSkin() const {
	return _skin.get();
}

osg::MatrixfArray* SkinPaletteCallback::getPalette() const {
	return _palette.get();
}

void SkinPaletteCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	resolveBinding(_bindingResolved, _binding.get(), "osgx::joints");

	_skin->computePalette(node, *_palette);

	traverse(node, nv);
}

}
