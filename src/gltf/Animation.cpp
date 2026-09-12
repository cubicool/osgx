#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Animation.hpp"
#include "Log.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/FrameStamp>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace osgx::gltf::detail {

osg::Matrixd TRS::matrix() const {
	return
		osg::Matrixd::scale(scale) *
		osg::Matrixd::rotate(rotation) *
		osg::Matrixd::translate(translation)
	;
}

TRS nodeBaseTRS(const tg3_node& node) {
	TRS trs;

	// has_matrix is the explicit "was a matrix given" signal (v3 always populates
	// translation/rotation/scale with spec defaults, and matrix with an identity diagonal,
	// regardless of which the JSON actually specified) - see Scene.cpp's _createNode for
	// the same pattern.
	if(node.has_matrix) {
		osg::Quat so;
		osg::Matrixd matrix(node.matrix);

		matrix.decompose(trs.translation, trs.rotation, trs.scale, so);
	}
	else {
		trs.translation.set(node.translation[0], node.translation[1], node.translation[2]);
		trs.rotation.set(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
		trs.scale.set(node.scale[0], node.scale[1], node.scale[2]);
	}

	return trs;
}

namespace {

std::vector<float> readFloatTimes(
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	int accessorIndex
) {
	if(accessorIndex < 0) return {};

	const std::size_t arrayIndex = static_cast<std::size_t>(accessorIndex);

	if(arrayIndex >= arrays.size() || !arrays[arrayIndex]) return {};

	osg::Array* array = arrays[arrayIndex];
	auto* source = dynamic_cast<osg::FloatArray*>(array);

	if(!source) return {};

	return std::vector<float>(source->begin(), source->end());
}

std::vector<osg::Vec3d> readVec3Values(
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	int accessorIndex
) {
	if(accessorIndex < 0) return {};

	const std::size_t arrayIndex = static_cast<std::size_t>(accessorIndex);

	if(arrayIndex >= arrays.size() || !arrays[arrayIndex]) return {};

	osg::Array* array = arrays[arrayIndex];
	auto* source = dynamic_cast<osg::Vec3Array*>(array);

	if(!source) return {};

	std::vector<osg::Vec3d> values;

	values.reserve(source->size());

	for(const osg::Vec3& value : *source) {
		values.emplace_back(value.x(), value.y(), value.z());
	}

	return values;
}

std::vector<osg::Quat> readQuatValues(
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	int accessorIndex
) {
	if(accessorIndex < 0) return {};

	const std::size_t arrayIndex = static_cast<std::size_t>(accessorIndex);

	if(arrayIndex >= arrays.size() || !arrays[arrayIndex]) return {};

	osg::Array* array = arrays[arrayIndex];
	auto* source = dynamic_cast<osg::Vec4Array*>(array);

	if(!source) return {};

	std::vector<osg::Quat> values;

	values.reserve(source->size());

	for(const osg::Vec4& value : *source) {
		values.emplace_back(value.x(), value.y(), value.z(), value.w());
	}

	return values;
}

}

void installAnimationCallback(
	const tg3_model& model,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	const std::vector<osg::observer_ptr<osg::MatrixTransform>>& nodeTransforms,
	osg::Node* root,
	bool skipAnimation
) {
	if(skipAnimation) {
		GLTF_NOTIFY(1) << "animation disabled by gltfSkipAnimation option" << std::endl;

		return;
	}

	if(!root || model.animations_count == 0) return;

	osg::ref_ptr<AnimationCallback> callback = new AnimationCallback();

	for(
		std::uint32_t animationIndex = 0;
		animationIndex < model.animations_count;
		animationIndex++
	) {
		const tg3_animation& animation = model.animations[animationIndex];
		AnimationCallback::Clip clip;

		clip.name = animation.name.len == 0
			? std::string("animation[") + std::to_string(animationIndex) + "]"
			: tg3_to_string(animation.name)
		;

		for(
			std::uint32_t channelIndex = 0;
			channelIndex < animation.channels_count;
			channelIndex++
		) {
			const tg3_animation_channel& gltfChannel = animation.channels[channelIndex];

			if(gltfChannel.sampler < 0 || gltfChannel.target.node < 0) continue;

			const std::uint32_t samplerIndex = static_cast<std::uint32_t>(gltfChannel.sampler);
			const std::size_t targetNodeIndex = static_cast<std::size_t>(gltfChannel.target.node);

			if(samplerIndex >= animation.samplers_count) continue;
			if(targetNodeIndex >= nodeTransforms.size()) continue;
			if(targetNodeIndex >= model.nodes_count) continue;
			if(!nodeTransforms[targetNodeIndex].valid()) continue;

			const tg3_animation_sampler& gltfSampler =
				animation.samplers[samplerIndex];

			if(tg3_str_equals_cstr(gltfSampler.interpolation, "CUBICSPLINE")) {
				GLTF_NOTIFY(2)
					<< "animation '" << clip.name
					<< "' channel[" << channelIndex << "] CUBICSPLINE skipped" << std::endl
				;
				continue;
			}

			AnimationCallback::Channel channel;

			channel.target = nodeTransforms[targetNodeIndex];
			channel.targetNode = gltfChannel.target.node;
			channel.interpolation = gltfSampler.interpolation.len == 0
				? "LINEAR"
				: tg3_to_string(gltfSampler.interpolation)
			;
			channel.times = readFloatTimes(arrays, gltfSampler.input);

			if(tg3_str_equals_cstr(gltfChannel.target.path, "translation")) {
				channel.path = AnimationCallback::Path::Translation;
				channel.vec3Values = readVec3Values(arrays, gltfSampler.output);
			}
			else if(tg3_str_equals_cstr(gltfChannel.target.path, "rotation")) {
				channel.path = AnimationCallback::Path::Rotation;
				channel.quatValues = readQuatValues(arrays, gltfSampler.output);
			}
			else if(tg3_str_equals_cstr(gltfChannel.target.path, "scale")) {
				channel.path = AnimationCallback::Path::Scale;
				channel.vec3Values = readVec3Values(arrays, gltfSampler.output);
			}
			else {
				GLTF_NOTIFY(2)
					<< "animation '" << clip.name
					<< "' channel[" << channelIndex << "] path '"
					<< tg3_to_string(gltfChannel.target.path) << "' skipped" << std::endl
				;
				continue;
			}

			if(channel.times.empty()) continue;

			if(
				channel.path == AnimationCallback::Path::Rotation &&
				channel.quatValues.size() != channel.times.size()
			) continue;

			if(
				channel.path != AnimationCallback::Path::Rotation &&
				channel.vec3Values.size() != channel.times.size()
			) continue;

			clip.duration = std::max<double>(clip.duration, channel.times.back());
			callback->baseTRS.emplace(
				gltfChannel.target.node,
				nodeBaseTRS(model.nodes[targetNodeIndex])
			);
			clip.channels.push_back(std::move(channel));
		}

		if(clip.channels.empty()) {
			GLTF_NOTIFY(1)
				<< "animation '" << clip.name << "' has no supported channels" << std::endl
			;
		}
		else {
			GLTF_NOTIFY(1)
				<< "loaded animation '" << clip.name << "'"
				<< " channels=" << clip.channels.size()
				<< " duration=" << clip.duration << std::endl
			;
		}

		callback->clips.push_back(std::move(clip));
	}

	std::size_t initialAnimation = 0;

	for(std::uint32_t i = 0; i < model.animations_count; i++) {
		if(tg3_str_equals_cstr(model.animations[i].name, "Walk")) {
			initialAnimation = i;
			break;
		}
	}

	callback->playAnimation(initialAnimation);
	root->addUpdateCallback(callback);
}

std::size_t AnimationCallback::getNumAnimations() const {
	return clips.size();
}

std::string AnimationCallback::getAnimationName(std::size_t index) const {
	return index < clips.size() ? clips[index].name : std::string();
}

bool AnimationCallback::playAnimation(std::size_t index) {
	if(index >= clips.size()) return false;

	_activeClip = index;
	_playing = true;
	_restartRequested = true;

	return true;
}

bool AnimationCallback::playAnimation(const std::string& name) {
	for(std::size_t i = 0; i < clips.size(); i++) {
		if(clips[i].name == name) return playAnimation(i);
	}

	return false;
}

std::size_t AnimationCallback::getCurrentAnimationIndex() const {
	return _activeClip < clips.size() ? _activeClip : SimplePlayer::NoAnimation;
}

std::string AnimationCallback::getCurrentAnimationName() const {
	return getAnimationName(getCurrentAnimationIndex());
}

void AnimationCallback::setPlaying(bool playing) {
	_playing = playing;
}

bool AnimationCallback::getPlaying() const {
	return _playing;
}

void AnimationCallback::restart() {
	_restartRequested = true;
}

void AnimationCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(_activeClip >= clips.size()) {
		traverse(node, nv);

		return;
	}

	const Clip& clip = clips[_activeClip];

	if(clip.channels.empty() || clip.duration <= 0.0) {
		traverse(node, nv);

		return;
	}

	double simTime = 0.0;

	if(nv && nv->getFrameStamp()) simTime = nv->getFrameStamp()->getSimulationTime();

	if(!_started || _restartRequested) {
		_started = true;
		_restartRequested = false;
		_playTime = 0.0;
		_lastSimulationTime = simTime;
		_restoreBasePose();

		GLTF_NOTIFY(1)
			<< "playing animation '" << clip.name << "'"
			<< " duration=" << clip.duration
			<< " channel(s)=" << clip.channels.size() << std::endl
		;
	}

	if(_playing) _playTime += std::max(0.0, simTime - _lastSimulationTime);

	_lastSimulationTime = simTime;

	if(!_playing) {
		traverse(node, nv);

		return;
	}

	double t = std::fmod(_playTime, clip.duration);
	std::map<int, TRS> current = baseTRS;

	for(const Channel& channel : clip.channels) {
		auto it = current.find(channel.targetNode);

		if(it == current.end()) continue;

		if(channel.path == Path::Rotation) it->second.rotation = _sampleQuat(channel, t);
		else {
			osg::Vec3d v = _sampleVec3(channel, t);

			if(channel.path == Path::Translation) it->second.translation = v;
			else if(channel.path == Path::Scale) it->second.scale = v;
		}
	}

	for(const auto& [nodeIdx, trs] : current) {
		osg::ref_ptr<osg::MatrixTransform> target;

		for(const Channel& channel : clip.channels) {
			if(channel.targetNode == nodeIdx) {
				channel.target.lock(target);

				break;
			}
		}

		if(target) target->setMatrix(trs.matrix());
	}

	traverse(node, nv);
}

void AnimationCallback::_restoreBasePose() {
	for(const Clip& clip : clips) {
		for(const Channel& channel : clip.channels) {
			auto it = baseTRS.find(channel.targetNode);
			osg::ref_ptr<osg::MatrixTransform> target;

			channel.target.lock(target);

			if(it != baseTRS.end() && target) {
				target->setMatrix(it->second.matrix());
			}
		}
	}
}

std::size_t AnimationCallback::_sampleIndex(
	const std::vector<float>& times,
	double t,
	double& mix
) {
	if(times.size() < 2) {
		mix = 0.0;

		return 0;
	}

	auto hi = std::upper_bound(times.begin(), times.end(), static_cast<float>(t));

	if(hi == times.begin()) {
		mix = 0.0;

		return 0;
	}

	if(hi == times.end()) {
		mix = 0.0;

		return times.size() - 1;
	}

	std::size_t i0 = static_cast<std::size_t>(std::distance(times.begin(), hi) - 1);
	float t0 = times[i0];
	float t1 = times[i0 + 1];

	mix = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;

	return i0;
}

osg::Vec3d AnimationCallback::_sampleVec3(const Channel& channel, double t) {
	if(channel.vec3Values.empty()) return osg::Vec3d();

	double mix = 0.0;
	std::size_t i = _sampleIndex(channel.times, t, mix);

	if(channel.interpolation == "STEP" || i + 1 >= channel.vec3Values.size()) {
		return channel.vec3Values[std::min(i, channel.vec3Values.size() - 1)];
	}

	const osg::Vec3d& a = channel.vec3Values[i];
	const osg::Vec3d& b = channel.vec3Values[i + 1];

	return a * (1.0 - mix) + b * mix;
}

osg::Quat AnimationCallback::_sampleQuat(const Channel& channel, double t) {
	if(channel.quatValues.empty()) return osg::Quat();

	double mix = 0.0;
	std::size_t i = _sampleIndex(channel.times, t, mix);

	if(channel.interpolation == "STEP" || i + 1 >= channel.quatValues.size()) {
		return channel.quatValues[std::min(i, channel.quatValues.size() - 1)];
	}

	osg::Quat q;

	q.slerp(mix, channel.quatValues[i], channel.quatValues[i + 1]);

	return q;
}

}
