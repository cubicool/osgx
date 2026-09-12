#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "ReaderImpl.hpp"
#include "Log.hpp"
#include "Scene.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osgDB/FileNameUtils>
#include <osgDB/Options>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using Progress = osgx::gltf::Reader::Progress;
using ProgressCallback = osgx::gltf::Reader::ProgressCallback;
using Stage = osgx::gltf::Reader::Stage;

// Per-section progress: tg3_stream_callbacks fires one of these per item, only after that
// section's array is fully parsed (TG3__STREAM_CB in tiny_gltf_v3.c parses the whole section,
// THEN loops the callback over it) - so ctx.model->X_count is already the real, final count
// by the time the first item of that section ticks, never a fabricated denominator. Covers the
// sections most worth showing progress for; buffers/bufferViews/accessors/samplers/cameras/
// scenes are typically tiny or uninteresting to report individually.
struct ParseProgressContext {
	const ProgressCallback* progress;
	const tg3_model* model;
};

tg3_stream_action tickSection(
	const ParseProgressContext& ctx,
	int32_t idx,
	std::uint32_t total,
	const char* section
) {
	std::uint64_t current = static_cast<std::uint64_t>(idx) + 1;

	(*ctx.progress)(Progress{
		Stage::Parsing,
		current,
		total,
		section,
		osgx::gltf::Reader::computeOverall(Stage::Parsing, current, total, section)
	});

	return TG3_STREAM_CONTINUE;
}

tg3_stream_action onMesh(const tg3_mesh*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->meshes_count, "meshes");
}

tg3_stream_action onNode(const tg3_node*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->nodes_count, "nodes");
}

tg3_stream_action onMaterial(const tg3_material*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->materials_count, "materials");
}

tg3_stream_action onTexture(const tg3_texture*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->textures_count, "textures");
}

tg3_stream_action onImage(const tg3_image*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->images_count, "images");
}

tg3_stream_action onAnimation(const tg3_animation*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->animations_count, "animations");
}

tg3_stream_action onSkin(const tg3_skin*, int32_t idx, void* ud) {
	auto* ctx = static_cast<ParseProgressContext*>(ud);

	return tickSection(*ctx, idx, ctx->model->skins_count, "skins");
}

void logAnimationBits(const tg3_model& model) {
	if(model.skins_count == 0 && model.animations_count == 0) return;

	GLTF_NOTIFY(0)
		<< model.skins_count << " skin(s), "
		<< model.animations_count << " animation(s)" << std::endl
	;

	for(std::uint32_t skinIdx = 0; skinIdx < model.skins_count; skinIdx++) {
		const tg3_skin& skin = model.skins[skinIdx];

		GLTF_NOTIFY(1)
			<< "skin[" << skinIdx << "] '" << osgx::gltf::detail::tg3_to_string(skin.name) << "'"
			<< " joints=" << skin.joints_count
			<< " skeleton=" << skin.skeleton
			<< " inverseBindMatrices=" << skin.inverse_bind_matrices << std::endl
		;

		for(std::uint32_t jointIdx = 0; jointIdx < skin.joints_count; jointIdx++) {
			int nodeIdx = skin.joints[jointIdx];
			std::string nodeName =
				nodeIdx >= 0 && static_cast<std::uint32_t>(nodeIdx) < model.nodes_count
				? osgx::gltf::detail::tg3_to_string(model.nodes[static_cast<std::uint32_t>(nodeIdx)].name)
				: std::string()
			;

			GLTF_NOTIFY(2)
				<< "joint[" << jointIdx << "]"
				<< " node=" << nodeIdx
				<< " '" << nodeName << "'" << std::endl
			;
		}
	}

	for(std::uint32_t animIdx = 0; animIdx < model.animations_count; animIdx++) {
		const tg3_animation& animation = model.animations[animIdx];

		GLTF_NOTIFY(1)
			<< "animation[" << animIdx << "] '"
			<< osgx::gltf::detail::tg3_to_string(animation.name) << "'"
			<< " channels=" << animation.channels_count
			<< " samplers=" << animation.samplers_count << std::endl
		;

		for(std::uint32_t samplerIdx = 0; samplerIdx < animation.samplers_count; samplerIdx++) {
			const tg3_animation_sampler& sampler = animation.samplers[samplerIdx];

			GLTF_NOTIFY(2)
				<< "sampler[" << samplerIdx << "]"
				<< " input=" << sampler.input
				<< " output=" << sampler.output
				<< " interpolation=" << osgx::gltf::detail::tg3_to_string(sampler.interpolation)
				<< std::endl
			;
		}

		for(std::uint32_t channelIdx = 0; channelIdx < animation.channels_count; channelIdx++) {
			const tg3_animation_channel& channel = animation.channels[channelIdx];
			std::string nodeName =
				channel.target.node >= 0 &&
				static_cast<std::uint32_t>(channel.target.node) < model.nodes_count
				? osgx::gltf::detail::tg3_to_string(
					model.nodes[static_cast<std::uint32_t>(channel.target.node)].name
				)
				: std::string()
			;

			GLTF_NOTIFY(2)
				<< "channel[" << channelIdx << "]"
				<< " sampler=" << channel.sampler
				<< " targetNode=" << channel.target.node
				<< " '" << nodeName << "'"
				<< " path=" << osgx::gltf::detail::tg3_to_string(channel.target.path) << std::endl
			;
		}
	}
}

}

namespace osgx::gltf::detail {

osgDB::ReaderWriter::ReadResult ReaderImpl::read(
	const std::string& location,
	bool /* isBinary - tg3_parse_file auto-sniffs JSON vs GLB, no longer needed internally */,
	const osgDB::Options* readOptions,
	const Reader::ProgressCallback& progress
) const {
	GLTF_NOTIFY(0) << "loading " << location << std::endl;

	tinygltf3::Model model;
	tinygltf3::ErrorStack errors;
	ParseProgressContext progressContext{&progress, model.get()};
	tg3_stream_callbacks stream{};

	stream.on_mesh = &onMesh;
	stream.on_node = &onNode;
	stream.on_material = &onMaterial;
	stream.on_texture = &onTexture;
	stream.on_image = &onImage;
	stream.on_animation = &onAnimation;
	stream.on_skin = &onSkin;
	stream.user_data = &progressContext;

	tg3_parse_options opts;

	tg3_parse_options_init(&opts);

	opts.stream = progress ? &stream : nullptr;
	opts.fs.read_file = &tg3_read_file;
	opts.fs.free_file = &tg3_free_file;

	tg3_error_code rc = tinygltf3::parse_file(model, errors, location.c_str(), &opts);

	for(std::uint32_t i = 0; i < errors.count(); i++) {
		const tg3_error_entry* entry = errors.entry(i);
		const char* message = entry->message ? entry->message : "";

		if(entry->severity == TG3_SEVERITY_ERROR) {
			OSG_WARN << location << ": " << message << std::endl;
		}
		else {
			GLTF_NOTIFY(0) << location << ": " << message << std::endl;
		}
	}

	if(rc != TG3_OK || errors.has_error()) {
		OSG_WARN << "failed to load " << location << std::endl;

		return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
	}

	GLTF_NOTIFY(0)
		<< model->meshes_count << " mesh(es), "
		<< model->accessors_count << " accessor(s), "
		<< model->buffer_views_count << " bufferView(s), "
		<< model->buffers_count << " buffer(s), "
		<< model->images_count << " image(s)" << std::endl
	;

	logAnimationBits(*model.get());

	return buildScene(
		*model.get(),
		location,
		readOptions,
		_textureCache,
		progress
	);
}

}
