#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/Callback>
#include <osg/Matrixd>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Quat>
#include <osg/Vec3d>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include "osgx/gltf/SimplePlayer.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct tg3_model;
struct tg3_node;

namespace osgx::gltf::detail {

struct TRS {
	osg::Vec3d translation = osg::Vec3d(0.0, 0.0, 0.0);
	osg::Quat rotation;
	osg::Vec3d scale = osg::Vec3d(1.0, 1.0, 1.0);

	osg::Matrixd matrix() const;
};

TRS nodeBaseTRS(const tg3_node& node);

void installAnimationCallback(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	osg::Node* root,
	bool skipAnimation
);

// Drives glTF node animation (translation/rotation/scale channels only - no morph-target/weights
// path exists here) UNCONDITIONALLY: installAnimationCallback() attaches one of these directly as
// an ordinary osg::NodeCallback, and `_playing` below defaults to true, so keyframe sampling and
// setMatrix() calls happen every update traversal the instant the application starts calling
// frame() - no SimplePlayer, and no other application code, is required to make this run.
// SimplePlayer (SimplePlayer.hpp) is a CONTROL surface bolted on top of an already-running
// instance of this class (found via dynamic_cast<SimplePlayerControl*> on the model's update-
// callback chain) - constructing one lets a caller pause/select/restart, but never causes
// animation to start; it was already happening. This is also why a caller who never touches
// SimplePlayer at all (e.g. a plain viewer/loader) still sees any model with node animation move.
//
// For a skinned/rigged model, this callback only ever moves the JOINT nodes' own transforms --
// the mesh deformation those transforms produce is a second, entirely separate mechanism; see
// osgx::SkinPaletteCallback (osgx/Skinning.hpp), which has no dependency on this class or on
// SimplePlayer either.
class AnimationCallback:
	public osg::NodeCallback,
	public SimplePlayerControl {
public:
	enum class Path {
		Translation,
		Rotation,
		Scale
	};

	struct Channel {
		osg::observer_ptr<osg::MatrixTransform> target;
		int targetNode = -1;
		Path path = Path::Translation;
		std::string interpolation = "LINEAR";
		std::vector<float> times;
		std::vector<osg::Vec3d> vec3Values;
		std::vector<osg::Quat> quatValues;
	};

	struct Clip {
		std::string name;
		std::vector<Channel> channels;
		double duration = 0.0;
	};

	std::vector<Clip> clips;
	std::map<int, TRS> baseTRS;

	std::size_t getNumAnimations() const override;
	std::string getAnimationName(std::size_t index) const override;
	bool playAnimation(std::size_t index) override;
	bool playAnimation(const std::string& name) override;
	std::size_t getCurrentAnimationIndex() const override;
	std::string getCurrentAnimationName() const override;
	void setPlaying(bool playing) override;
	bool getPlaying() const override;
	void restart() override;
	void operator()(osg::Node* node, osg::NodeVisitor* nv) override;

private:
	bool _started = false;
	bool _playing = true;
	bool _restartRequested = false;
	std::size_t _activeClip = 0;
	double _playTime = 0.0;
	double _lastSimulationTime = 0.0;

	void _restoreBasePose();
	static std::size_t _sampleIndex(const std::vector<float>& times, double t, double& mix);
	static osg::Vec3d _sampleVec3(const Channel& channel, double t);
	static osg::Quat _sampleQuat(const Channel& channel, double t);
};

}
