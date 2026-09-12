#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/Callback>
#include <osg/MatrixTransform>
#include <osg/Referenced>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <string>
#include <vector>

struct tg3_model;

namespace osgx::gltf::detail {

struct Skin: public osg::Referenced {
	int index = -1;
	std::string name;
	std::vector<int> joints;
	std::vector<osg::Matrixf> inverseBindMatrices;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> jointNodes;
	std::vector<osg::observer_ptr<osg::MatrixTransform>> skinnedNodes;
	int skeleton = -1;
	osg::ref_ptr<osg::MatrixfArray> paletteMatrices;

	// Index of each joint's parent joint within this skin, or -1 when its parent lies outside it.
	std::vector<int> parentJointIndex;

	// Scratch buffers reused for every palette update to avoid per-frame allocation.
	std::vector<osg::Matrixd> jointWorldCache;
	std::vector<char> jointWorldComputed;

	void initPalette();
	osg::Matrixd computeJointWorld(std::size_t jointIndex);
	bool updatePalette(osg::Node* skinnedNode);
};

// Reflects, not drives: every update traversal this reads whatever the CURRENT world matrices of
// the skin's joint nodes happen to be (Skin::computeJointWorld()) and rewrites the joint-matrix
// SSBO the vertex shader deforms the mesh against. It has no play/pause state and no idea what (if
// anything) is moving those joints - installSkinPaletteCallbacks() attaches this unconditionally
// on every skinned mesh node, independent of AnimationCallback (Animation.hpp) or SimplePlayer.
// A joint that has its own animation channels visibly deforms the mesh purely because
// AnimationCallback moves the joint transform and this callback picks up whatever that transform
// currently is; a joint with no animation just has this recompute the same bind-pose matrix every
// frame, harmlessly. The two callbacks never call into each other.
class SkinPaletteCallback: public osg::NodeCallback {
public:
	explicit SkinPaletteCallback(Skin* skin);

	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	osg::ref_ptr<Skin> _skin;
	bool _loggedOnce = false;
};

std::vector<osg::ref_ptr<Skin>> prepareSkins(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays
);

void resolveSkinJointNodes(
	const tg3_model& model,
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	const std::vector<osg::ref_ptr<Skin>>& skins
);

void installSkinPaletteCallbacks(const std::vector<osg::ref_ptr<Skin>>& skins);

}
