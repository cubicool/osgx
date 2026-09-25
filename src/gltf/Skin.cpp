#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Skin.hpp"
#include "tg3_util.hpp"

#include "Log.hpp"

#include "osgx/Library.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>

OSGX_ENABLE_WARNINGS

#include "osgx/gltf/Shader.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace osgx::gltf::detail {

void Skin::initPalette() {
	if(inverseBindMatrices.size() > std::numeric_limits<unsigned int>::max()) {
		paletteMatrices = new osg::MatrixfArray();

		return;
	}

	paletteMatrices = new osg::MatrixfArray(
		static_cast<unsigned int>(inverseBindMatrices.size())
	);

	std::fill(
		paletteMatrices->begin(),
		paletteMatrices->end(),
		osg::Matrixf::identity()
	);

	paletteMatrices->setBufferObject(new osg::ShaderStorageBufferObject());
}

osg::Matrixd Skin::computeJointWorld(std::size_t jointIndex) {
	if(jointWorldComputed[jointIndex]) return jointWorldCache[jointIndex];

	osg::ref_ptr<osg::MatrixTransform> joint;

	jointNodes[jointIndex].lock(joint);

	osg::Matrixd local = joint ? joint->getMatrix() : osg::Matrixd::identity();
	int parent = parentJointIndex[jointIndex];
	osg::Matrixd world;

	if(parent >= 0) {
		world = local * computeJointWorld(static_cast<std::size_t>(parent));
	}
	else {
		osg::MatrixList worlds = joint ? joint->getWorldMatrices() : osg::MatrixList();

		world = worlds.empty() ? local : worlds.front();
	}

	jointWorldCache[jointIndex] = world;
	jointWorldComputed[jointIndex] = 1;

	return world;
}

bool Skin::updatePalette(osg::Node* skinnedNode) {
	if(!paletteMatrices || !skinnedNode) return false;

	osg::Matrixd meshWorld;
	osg::MatrixList meshWorlds = skinnedNode->getWorldMatrices();

	if(!meshWorlds.empty()) meshWorld = meshWorlds.front();

	osg::Matrixd worldToMesh;

	worldToMesh.invert(meshWorld);

	std::fill(jointWorldComputed.begin(), jointWorldComputed.end(), 0);

	bool anyUpdated = false;

	for(
		std::size_t i = 0;
		i < jointNodes.size() && i < paletteMatrices->size();
		i++
	) {
		if(!jointNodes[i].valid()) continue;

		osg::Matrixd jointWorld = computeJointWorld(i);
		osg::Matrixd inverseBind = inverseBindMatrices[i];
		osg::Matrixd jointMatrix = inverseBind * jointWorld * worldToMesh;

		(*paletteMatrices)[i] = osg::Matrixf(jointMatrix);
		anyUpdated = true;
	}

	if(anyUpdated) paletteMatrices->dirty();

	return anyUpdated;
}

SkinPaletteCallback::SkinPaletteCallback(Skin* skin, osg::ShaderStorageBufferBinding* binding):
_skin(skin),
_binding(binding) {}

void SkinPaletteCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	osgx::resolveBinding(_bindingResolved, _binding.get(), "osgx::gltf::joints");

	if(_skin.valid() && _skin->updatePalette(node) && !_loggedOnce) {
		_loggedOnce = true;

		GLTF_NOTIFY(1)
			<< "updated skin[" << _skin->index << "] palette "
			<< _skin->paletteMatrices->size()
			<< " matrix/matrices at buffer binding "
			<< _binding->getIndex() << std::endl
		;
	}

	traverse(node, nv);
}

std::vector<osg::ref_ptr<Skin>> prepareSkins(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays
) {
	std::vector<osg::ref_ptr<Skin>> skins;

	skins.reserve(model.skins_count);

	for(std::uint32_t skinIndex = 0; skinIndex < model.skins_count; skinIndex++) {
		const tg3_skin& source = model.skins[skinIndex];
		osg::ref_ptr<Skin> skin = new Skin();

		skin->index = static_cast<int>(skinIndex);
		skin->name = tg3_to_string(source.name);
		skin->joints.assign(source.joints, source.joints + source.joints_count);
		skin->skeleton = source.skeleton;
		skin->inverseBindMatrices.resize(source.joints_count, osg::Matrixf::identity());
		skin->jointNodes.resize(source.joints_count);

		if(
			source.inverse_bind_matrices >= 0 &&
			static_cast<std::uint32_t>(source.inverse_bind_matrices) < arrays.size() &&
			arrays[static_cast<std::size_t>(source.inverse_bind_matrices)]
		) {
			const std::size_t accessorIndex =
				static_cast<std::size_t>(source.inverse_bind_matrices);
			osg::Array* array = arrays[accessorIndex];
			auto* inverseBindMatrices = dynamic_cast<osg::MatrixfArray*>(array);

			if(inverseBindMatrices) {
				std::size_t count = std::min<std::size_t>(
					inverseBindMatrices->size(),
					skin->inverseBindMatrices.size()
				);

				std::copy_n(
					inverseBindMatrices->begin(),
					count,
					skin->inverseBindMatrices.begin()
				);

				if(count != skin->inverseBindMatrices.size()) {
					GLTF_NOTIFY(1)
						<< "skin[" << skinIndex << "] inverseBindMatrices count "
						<< count << " does not match joints count "
						<< skin->inverseBindMatrices.size() << std::endl
					;
				}
			}
			else {
				GLTF_NOTIFY(1)
					<< "skin[" << skinIndex << "] inverseBindMatrices accessor "
					<< source.inverse_bind_matrices << " is not a MatrixfArray" << std::endl
				;
			}
		}
		else if(source.inverse_bind_matrices >= 0) {
			GLTF_NOTIFY(1)
				<< "skin[" << skinIndex << "] inverseBindMatrices accessor "
				<< source.inverse_bind_matrices << " is unavailable" << std::endl
			;
		}

		skin->initPalette();

		GLTF_NOTIFY(1)
			<< "prepared skin[" << skinIndex << "] '" << skin->name << "'"
			<< " joints=" << skin->joints.size()
			<< " inverseBindMatrices=" << skin->inverseBindMatrices.size()
			<< " skeleton=" << skin->skeleton << std::endl
		;

		skins.push_back(skin);
	}

	return skins;
}

void resolveSkinJointNodes(
	const tg3_model& model,
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	const std::vector<osg::ref_ptr<Skin>>& skins
) {
	// Built once for the whole model: glTF node index to its parent's node index. tinygltf does not
	// expose parent pointers, so derive them from node.children.
	std::unordered_map<int, int> nodeParent;

	for(std::uint32_t nodeIndex = 0; nodeIndex < model.nodes_count; nodeIndex++) {
		const tg3_node& node = model.nodes[nodeIndex];

		for(std::uint32_t childSlot = 0; childSlot < node.children_count; childSlot++) {
			nodeParent[node.children[childSlot]] = static_cast<int>(nodeIndex);
		}
	}

	for(const auto& skinReference : skins) {
		Skin* skin = skinReference;

		if(!skin) continue;

		std::size_t resolved = 0;
		std::unordered_map<int, int> nodeIndexToJointIndex;

		for(std::size_t jointIndex = 0; jointIndex < skin->joints.size(); jointIndex++) {
			nodeIndexToJointIndex[skin->joints[jointIndex]] = static_cast<int>(jointIndex);
		}

		skin->parentJointIndex.assign(skin->joints.size(), -1);
		skin->jointWorldCache.resize(skin->joints.size());
		skin->jointWorldComputed.resize(skin->joints.size());

		for(std::size_t jointIndex = 0; jointIndex < skin->joints.size(); jointIndex++) {
			int nodeIndex = skin->joints[jointIndex];

			if(
				nodeIndex >= 0 &&
				nodeIndex < static_cast<int>(nodeTransforms.size()) &&
				nodeTransforms[static_cast<std::size_t>(nodeIndex)].valid()
			) {
				const std::size_t nodeOffset = static_cast<std::size_t>(nodeIndex);

				skin->jointNodes[jointIndex] = nodeTransforms[nodeOffset];
				resolved++;
			}

			auto parentNode = nodeParent.find(nodeIndex);

			if(parentNode != nodeParent.end()) {
				auto parentJoint = nodeIndexToJointIndex.find(parentNode->second);

				if(parentJoint != nodeIndexToJointIndex.end()) {
					skin->parentJointIndex[jointIndex] = parentJoint->second;
				}
			}
		}

		GLTF_NOTIFY(1)
			<< "resolved skin[" << skin->index << "] joint nodes "
			<< resolved << "/" << skin->joints.size() << std::endl
		;
	}
}

void installSkinPaletteCallbacks(const std::vector<osg::ref_ptr<Skin>>& skins) {
	for(const auto& skinReference : skins) {
		Skin* skin = skinReference;

		if(!skin || !skin->paletteMatrices) continue;

		const auto totalSize = static_cast<GLsizeiptr>(
			skin->paletteMatrices->getTotalDataSize()
		);

		for(const auto& skinnedNodeReference : skin->skinnedNodes) {
			osg::ref_ptr<osg::MatrixTransform> skinnedNode;

			skinnedNodeReference.lock(skinnedNode);

			if(!skinnedNode) continue;

			// Index 0 until SkinPaletteCallback resolves the "osgx::gltf::joints" slot.
			auto* binding = new osg::ShaderStorageBufferBinding(0, skin->paletteMatrices, 0, totalSize);

			skinnedNode->addUpdateCallback(new SkinPaletteCallback(skin, binding));
			skinnedNode->getOrCreateStateSet()->setAttributeAndModes(binding, osg::StateAttribute::ON);

			skin->updatePalette(skinnedNode);

			GLTF_NOTIFY(1)
				<< "installed skin[" << skin->index << "] palette callback on '"
				<< skinnedNode->getName() << "'"
				<< " buffer binding (resolved on first update)"
				<< " bytes=" << totalSize << std::endl
			;
		}
	}
}

}
