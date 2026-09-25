#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/Matrixf>
#include <osg/MatrixTransform>
#include <osg/Referenced>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <string>
#include <vector>

struct tg3_model;

namespace osgx::gltf::detail {

// One glTF skin, gathered while the scene is built; attachSkins() turns it into an osgx::Skin
// (osgx/Skinning.hpp) once every node's transform exists.
struct SkinData: public osg::Referenced {
	int index = -1;
	std::string name;
	// glTF node indices, in palette order.
	std::vector<int> joints;
	std::vector<osg::Matrixf> inverseBindMatrices;
	// The transforms of the nodes that reference this skin.
	std::vector<osg::observer_ptr<osg::MatrixTransform>> skinnedNodes;
};

std::vector<osg::ref_ptr<SkinData>> prepareSkins(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays
);

// Builds an osgx::Skin per SkinData from the node transforms and attaches it to each skinned node.
void attachSkins(
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	const std::vector<osg::ref_ptr<SkinData>>& skins
);

}
