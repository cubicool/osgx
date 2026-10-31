#include "ShaderLibs.hpp"

#include "osgx/Library.hpp"
#include "osgx/Shader.hpp"
#include "osgx/Skinning.hpp"
#include "osgx/Visitors.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferObject>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geometry>

OSGX_ENABLE_WARNINGS

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

bool hasJointWeights(osg::Node* node) {
	if(!node) return false;

	bool found = false;

	LambdaVisitor<osg::Geometry> visitor([&found](osg::Geometry& geometry) {
		if(geometry.getVertexAttribArray(JOINT_WEIGHTS_ATTRIBUTE)) found = true;
	});

	node->accept(visitor);

	return found;
}

Skin::Skin(
	const std::vector<osg::MatrixTransform*>& joints,
	const std::vector<osg::Matrixf>& inverseBindMatrices
):
_joints(joints.begin(), joints.end()),
_inverseBindMatrices(inverseBindMatrices) {
	_inverseBindMatrices.resize(joints.size(), osg::Matrixf::identity());
}

std::size_t Skin::getNumJoints() const {
	return _joints.size();
}

const osg::Matrixd& Skin::_computeWorld(const osg::Node* node) {
	auto it = _worlds.find(node);

	if(it != _worlds.end()) return it->second;

	osg::Matrixd world;
	const osg::Camera* camera = node->asCamera();

	// An absolute or root Camera ends the path without contributing, as in
	// osg::computeLocalToWorld(path, ignoreCameras=true).
	if(
		!camera ||
		(camera->getReferenceFrame() == osg::Transform::RELATIVE_RF && camera->getNumParents())
	) {
		if(node->getNumParents()) world = _computeWorld(node->getParent(0));

		if(const osg::Transform* transform = node->asTransform()) {
			transform->computeLocalToWorldMatrix(world, nullptr);
		}
	}

	return _worlds.emplace(node, world).first->second;
}

bool Skin::computePalette(const osg::Node* mesh, osg::MatrixfArray& palette) {
	_worlds.clear();
	_worldsCached = false;

	return _writePalette(mesh, palette);
}

bool Skin::computePalette(
	const osg::Node* mesh,
	osg::MatrixfArray& palette,
	unsigned int frameNumber
) {
	if(!_worldsCached || _worldsFrame != frameNumber) {
		_worlds.clear();
		_worldsCached = true;
		_worldsFrame = frameNumber;
	}

	return _writePalette(mesh, palette);
}

bool Skin::_writePalette(const osg::Node* mesh, osg::MatrixfArray& palette) {
	if(!mesh) return false;

	osg::Matrixd worldToMesh = osg::Matrixd::inverse(_computeWorld(mesh));

	if(palette.size() != _joints.size()) palette.resize(
		static_cast<unsigned int>(_joints.size()),
		osg::Matrixf::identity()
	);

	for(std::size_t i = 0; i < _joints.size(); i++) {
		osg::ref_ptr<osg::MatrixTransform> joint;

		_joints[i].lock(joint);

		if(!joint) continue;

		osg::Matrixd inverseBind = _inverseBindMatrices[i];

		palette[i] = osg::Matrixf(inverseBind * _computeWorld(joint.get()) * worldToMesh);
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

	const osg::FrameStamp* frameStamp = nv ? nv->getFrameStamp() : nullptr;

	if(frameStamp) _skin->computePalette(node, *_palette, frameStamp->getFrameNumber());
	else _skin->computePalette(node, *_palette);

	traverse(node, nv);
}

}
