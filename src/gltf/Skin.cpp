#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Skin.hpp"
#include "tg3_util.hpp"

#include "Log.hpp"

#include "osgx/Skinning.hpp"

#include <algorithm>
#include <cstdint>

namespace osgx::gltf::detail {

std::vector<osg::ref_ptr<SkinData>> prepareSkins(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays
) {
	std::vector<osg::ref_ptr<SkinData>> skins;

	skins.reserve(model.skins_count);

	for(std::uint32_t skinIndex = 0; skinIndex < model.skins_count; skinIndex++) {
		const tg3_skin& source = model.skins[skinIndex];
		osg::ref_ptr<SkinData> skin = new SkinData();

		skin->index = static_cast<int>(skinIndex);
		skin->name = tg3_to_string(source.name);
		skin->joints.assign(source.joints, source.joints + source.joints_count);
		skin->inverseBindMatrices.resize(source.joints_count, osg::Matrixf::identity());

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

		GLTF_NOTIFY(1)
			<< "prepared skin[" << skinIndex << "] '" << skin->name << "'"
			<< " joints=" << skin->joints.size()
			<< " inverseBindMatrices=" << skin->inverseBindMatrices.size() << std::endl
		;

		skins.push_back(skin);
	}

	return skins;
}

void attachSkins(
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	const std::vector<osg::ref_ptr<SkinData>>& skins
) {
	for(const auto& skin : skins) {
		if(!skin) continue;

		std::vector<osg::MatrixTransform*> joints(skin->joints.size(), nullptr);
		std::size_t resolved = 0;

		for(std::size_t jointIndex = 0; jointIndex < skin->joints.size(); jointIndex++) {
			int nodeIndex = skin->joints[jointIndex];

			if(nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= nodeTransforms.size()) {
				continue;
			}

			osg::ref_ptr<osg::MatrixTransform> joint;

			nodeTransforms[static_cast<std::size_t>(nodeIndex)].lock(joint);

			if(joint) {
				joints[jointIndex] = joint.get();
				resolved++;
			}
		}

		GLTF_NOTIFY(1)
			<< "resolved skin[" << skin->index << "] joint nodes "
			<< resolved << "/" << skin->joints.size() << std::endl
		;

		osg::ref_ptr<osgx::Skin> osgxSkin = new osgx::Skin(joints, skin->inverseBindMatrices);

		for(const auto& skinnedNodeReference : skin->skinnedNodes) {
			osg::ref_ptr<osg::MatrixTransform> skinnedNode;

			skinnedNodeReference.lock(skinnedNode);

			if(!skinnedNode) continue;

			osgxSkin->attach(skinnedNode.get());

			GLTF_NOTIFY(1)
				<< "attached skin[" << skin->index << "] to '"
				<< skinnedNode->getName() << "'" << std::endl
			;
		}
	}
}

}
