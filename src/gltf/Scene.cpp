#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Scene.hpp"
#include "Accessor.hpp"
#include "Animation.hpp"
#include "Log.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
#include "Skin.hpp"
#include "Texture.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/CullFace>
#include <osg/MatrixTransform>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <osgDB/Options>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <cstdint>
#include <vector>

namespace osgx::gltf::detail {

class SceneBuilder {
public:
	SceneBuilder(
		const tg3_model& model,
		const std::string& referrer,
		const osgDB::Options* readOptions,
		TextureCache* textureCache,
		const Reader::ProgressCallback& progress
	):
	_model(model),
	_readOptions(readOptions),
	_textureLoader(model, referrer, readOptions, textureCache),
	_materialBuilder(model, referrer, readOptions, _textureLoader),
	_progress(progress),
	_meshBuilder(model, readOptions, _materialBuilder, _arrays, _skins) {
		_nodeTransforms.resize(model.nodes_count);

		_arrays = extractArrays(model);
		_skins = prepareSkins(model, _arrays);
	}

	osg::Node* build() {
		// glTF is Y-up; rotate to Z-up unless the caller passes "gltfZUp".
		bool zUp =
			_readOptions &&
			_readOptions->getOptionString().find("gltfZUp") != std::string::npos
		;

		osg::MatrixTransform* root = new osg::MatrixTransform();

		if(!zUp) root->setMatrix(osg::Matrixd::rotate(
			osg::Vec3d(0, 1, 0),
			osg::Vec3d(0, 0, 1)
		));

		for(std::uint32_t sceneIndex = 0; sceneIndex < _model.scenes_count; sceneIndex++) {
			const tg3_scene& scene = _model.scenes[sceneIndex];

			for(std::uint32_t nodeSlot = 0; nodeSlot < scene.nodes_count; nodeSlot++) {
				if(osg::Node* node = _createNode(scene.nodes[nodeSlot])) root->addChild(node);
			}
		}

		resolveSkinJointNodes(_model, _nodeTransforms, _skins);
		_installAnimationCallback(root);
		installSkinPaletteCallbacks(_skins);

		root->getOrCreateStateSet()->setAttributeAndModes(
			new osg::CullFace(osg::CullFace::BACK),
			osg::StateAttribute::ON
		);

		return root;
	}

private:
	const tg3_model& _model;
	const osgDB::Options* _readOptions;
	TextureLoader _textureLoader;
	MaterialBuilder _materialBuilder;
	Reader::ProgressCallback _progress;
	std::vector<osg::ref_ptr<osg::Array>> _arrays;
	std::vector<osg::ref_ptr<Skin>> _skins;
	MeshBuilder _meshBuilder;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> _nodeTransforms;
	std::uint64_t _nodesBuilt = 0;

	osg::Node* _createNode(int nodeIdx, unsigned depth = 0) {
		if(nodeIdx < 0 || static_cast<std::uint32_t>(nodeIdx) >= _model.nodes_count) return nullptr;

		const std::uint32_t nodeIndex = static_cast<std::uint32_t>(nodeIdx);
		const tg3_node& node = _model.nodes[nodeIndex];
		std::string nodeName = tg3_to_string(node.name);

		GLTF_NOTIFY(depth)
			<< "createNode '" << nodeName << "'"
			<< " node=" << nodeIdx
			<< " mesh=" << node.mesh
			<< " skin=" << node.skin
			<< " children=" << node.children_count << std::endl
		;

		// One tick per node regardless of depth. model.nodes_count includes
		// unreferenced nodes, so it is an upper-bound denominator.
		_nodesBuilt++;

		if(_progress) _progress(Reader::Progress{
			Reader::Stage::BuildingNodes,
			_nodesBuilt,
			_model.nodes_count,
			{},
			Reader::computeOverall(Reader::Stage::BuildingNodes, _nodesBuilt, _model.nodes_count, {})
		});

		osg::MatrixTransform* transform = new osg::MatrixTransform();

		// v3 always populates translation/rotation/scale with spec defaults (and matrix with
		// an identity diagonal) regardless of whether the JSON specified them - has_matrix is
		// the real, explicit "was a matrix given" signal, replacing the old size()==16/3/3/4
		// presence-check idiom entirely.
		if(node.has_matrix) {
			transform->setMatrix(osg::Matrixd(node.matrix));
		}
		else {
			osg::Matrixd scale = osg::Matrixd::scale(node.scale[0], node.scale[1], node.scale[2]);
			osg::Matrixd rotation;

			rotation.makeRotate(osg::Quat(
				node.rotation[0],
				node.rotation[1],
				node.rotation[2],
				node.rotation[3]
			));

			osg::Matrixd translation = osg::Matrixd::translate(
				node.translation[0],
				node.translation[1],
				node.translation[2]
			);

			transform->setMatrix(scale * rotation * translation);
		}

		_nodeTransforms[nodeIndex] = transform;

		if(node.skin >= 0) {
			const std::size_t skinIndex = static_cast<std::size_t>(node.skin);

			if(skinIndex < _skins.size() && _skins[skinIndex].valid()) {
				_skins[skinIndex]->skinnedNodes.push_back(transform);
			}
		}

		if(node.mesh >= 0) {
			const std::uint32_t meshIndex = static_cast<std::uint32_t>(node.mesh);

			if(meshIndex < _model.meshes_count) transform->addChild(_meshBuilder.makeMesh(
				_model.meshes[meshIndex],
				node.skin
			));
		}

		for(std::uint32_t childSlot = 0; childSlot < node.children_count; childSlot++) {
			if(osg::Node* child = _createNode(node.children[childSlot], depth + 1)) {
				transform->addChild(child);
			}
		}

		transform->setName(nodeName);

		return transform;
	}

	void _installAnimationCallback(osg::Node* root) const {
		const bool skipAnimation =
			_readOptions &&
			_readOptions->getOptionString().find("gltfSkipAnimation") != std::string::npos
		;

		installAnimationCallback(
			_model,
			_arrays,
			_nodeTransforms,
			root,
			skipAnimation
		);
	}
};

osg::Node* buildScene(
	const tg3_model& model,
	const std::string& referrer,
	const osgDB::Options* readOptions,
	TextureCache* textureCache,
	const Reader::ProgressCallback& progress
) {
	SceneBuilder builder(model, referrer, readOptions, textureCache, progress);

	return builder.build();
}

}
