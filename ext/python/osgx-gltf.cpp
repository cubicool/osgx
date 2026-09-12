#include "osgx-python.hpp"

// This whole file only compiles (and is only added to the source list, see CMakeLists.txt) when
// OSGX_BUILD_GLTF is on. It deliberately owns its tinygltf/osgx::gltf includes locally instead of
// pulling them into the shared osgx-python.hpp umbrella header - tinygltf's headers are heavy
// and previously forced every single binding file in the old monolithic ext/osgx-python.cpp to
// pay their parse cost, even ones that never touch glTF at all.
#ifdef OSGX_GLTF

#include "osgx/gltf/PBRIBL.hpp"
#include "osgx/gltf/Reader.hpp"
#include "osgx/gltf/Shader.hpp"
#include "osgx/gltf/SimplePlayer.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osgDB/FileNameUtils>
#include <osgDB/Options>

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "gltf/tg3_util.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace {

using osgx::gltf::detail::tg3_to_string;

const char* componentTypeName(int componentType) {
	switch(componentType) {
		case TG3_COMPONENT_TYPE_BYTE: return "BYTE";
		case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: return "UNSIGNED_BYTE";
		case TG3_COMPONENT_TYPE_SHORT: return "SHORT";
		case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
		case TG3_COMPONENT_TYPE_INT: return "INT";
		case TG3_COMPONENT_TYPE_UNSIGNED_INT: return "UNSIGNED_INT";
		case TG3_COMPONENT_TYPE_FLOAT: return "FLOAT";
		case TG3_COMPONENT_TYPE_DOUBLE: return "DOUBLE";
		default: return "UNKNOWN";
	}
}

const char* accessorTypeName(int type) {
	switch(type) {
		case TG3_TYPE_SCALAR: return "SCALAR";
		case TG3_TYPE_VEC2: return "VEC2";
		case TG3_TYPE_VEC3: return "VEC3";
		case TG3_TYPE_VEC4: return "VEC4";
		case TG3_TYPE_MAT2: return "MAT2";
		case TG3_TYPE_MAT3: return "MAT3";
		case TG3_TYPE_MAT4: return "MAT4";
		default: return "UNKNOWN";
	}
}

py::object maybeString(tg3_str value) {
	return value.len == 0 ? py::none() : py::cast(tg3_to_string(value));
}

py::object maybeNodeName(const tg3_model& model, int nodeIdx) {
	if(nodeIdx < 0 || static_cast<std::uint32_t>(nodeIdx) >= model.nodes_count) return py::none();

	return maybeString(model.nodes[static_cast<std::uint32_t>(nodeIdx)].name);
}

py::dict accessorInfo(const tg3_model& model, int accessorIdx) {
	py::dict out;

	out["index"] = accessorIdx;

	if(accessorIdx < 0 || static_cast<std::uint32_t>(accessorIdx) >= model.accessors_count) {
		out["valid"] = false;
		return out;
	}

	const tg3_accessor& accessor = model.accessors[static_cast<std::uint32_t>(accessorIdx)];

	out["valid"] = true;
	out["name"] = maybeString(accessor.name);
	out["count"] = accessor.count;
	out["componentType"] = componentTypeName(accessor.component_type);
	out["componentTypeValue"] = accessor.component_type;
	out["type"] = accessorTypeName(accessor.type);
	out["typeValue"] = accessor.type;
	out["normalized"] = accessor.normalized;
	out["bufferView"] = accessor.buffer_view;
	out["byteOffset"] = accessor.byte_offset;

	py::list minValues;
	for(std::uint32_t i = 0; i < accessor.min_values_count; i++) minValues.append(accessor.min_values[i]);
	out["min"] = minValues;

	py::list maxValues;
	for(std::uint32_t i = 0; i < accessor.max_values_count; i++) maxValues.append(accessor.max_values[i]);
	out["max"] = maxValues;

	return out;
}

py::dict textureInfo(const tg3_model& model, int textureIdx, int texCoord) {
	py::dict out;

	out["index"] = textureIdx;
	out["texCoord"] = texCoord;

	if(textureIdx >= 0 && static_cast<std::uint32_t>(textureIdx) < model.textures_count) {
		const tg3_texture& texture = model.textures[static_cast<std::uint32_t>(textureIdx)];

		out["valid"] = true;
		out["name"] = maybeString(texture.name);
		out["source"] = texture.source;
		out["sampler"] = texture.sampler;

		if(texture.source >= 0 && static_cast<std::uint32_t>(texture.source) < model.images_count) {
			const tg3_image& image = model.images[static_cast<std::uint32_t>(texture.source)];

			out["imageName"] = maybeString(image.name);
			out["imageUri"] = maybeString(image.uri);
			out["imageWidth"] = image.width;
			out["imageHeight"] = image.height;
			// tinygltf v3.0.1 never populates image.image itself (see Texture.cpp - osgx
			// self-decodes embedded/data-URI images via OSG's own plugins instead), so
			// "embedded" here means "has no external uri", not "tinygltf already decoded it".
			out["embedded"] = image.uri.len == 0;
		}
	}

	else out["valid"] = false;

	return out;
}

py::object textureInfoOrNone(const tg3_model& model, const tg3_texture_info& info) {
	if(info.index < 0) return py::none();

	return textureInfo(model, info.index, info.tex_coord);
}

py::object normalTextureInfoOrNone(
	const tg3_model& model,
	const tg3_normal_texture_info& info
) {
	if(info.index < 0) return py::none();

	py::dict out = textureInfo(model, info.index, info.tex_coord);

	out["scale"] = info.scale;

	return out;
}

py::object occlusionTextureInfoOrNone(
	const tg3_model& model,
	const tg3_occlusion_texture_info& info
) {
	if(info.index < 0) return py::none();

	py::dict out = textureInfo(model, info.index, info.tex_coord);

	out["strength"] = info.strength;

	return out;
}

py::list numberList(const double* values, std::uint32_t count) {
	py::list out;

	for(std::uint32_t i = 0; i < count; i++) out.append(values[i]);

	return out;
}

py::dict materialInfo(const tg3_model& model, int materialIdx) {
	const tg3_material& material = model.materials[static_cast<std::uint32_t>(materialIdx)];
	const tg3_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;

	py::dict out;

	out["index"] = materialIdx;
	out["name"] = maybeString(material.name);
	out["alphaMode"] = tg3_to_string(material.alpha_mode);
	out["alphaCutoff"] = material.alpha_cutoff;
	out["doubleSided"] = static_cast<bool>(material.double_sided);
	out["emissiveFactor"] = numberList(material.emissive_factor, 3);
	out["normalTexture"] = normalTextureInfoOrNone(model, material.normal_texture);
	out["occlusionTexture"] = occlusionTextureInfoOrNone(model, material.occlusion_texture);
	out["emissiveTexture"] = textureInfoOrNone(model, material.emissive_texture);

	py::dict pbrDict;

	pbrDict["baseColorFactor"] = numberList(pbr.base_color_factor, 4);
	pbrDict["metallicFactor"] = pbr.metallic_factor;
	pbrDict["roughnessFactor"] = pbr.roughness_factor;
	pbrDict["baseColorTexture"] = textureInfoOrNone(model, pbr.base_color_texture);
	pbrDict["metallicRoughnessTexture"] = textureInfoOrNone(model, pbr.metallic_roughness_texture);

	out["pbrMetallicRoughness"] = pbrDict;

	bool hasSpecGloss =
		osgx::gltf::detail::tg3_find_extension(
			material.ext, "KHR_materials_pbrSpecularGlossiness"
		) != nullptr
	;

	out["hasSpecGloss"] = hasSpecGloss;
	out["requiresSpecGlossBake"] = hasSpecGloss;
	out["hasBaseColorMap"] = pbr.base_color_texture.index >= 0;
	out["hasMetallicRoughnessMap"] = pbr.metallic_roughness_texture.index >= 0;
	out["hasNormalMap"] = material.normal_texture.index >= 0;
	out["hasOcclusionMap"] = material.occlusion_texture.index >= 0;
	out["hasEmissiveMap"] = material.emissive_texture.index >= 0;
	out["factorOnlyPBR"] =
		pbr.base_color_texture.index < 0 &&
		pbr.metallic_roughness_texture.index < 0 &&
		!hasSpecGloss
	;
	out["textureDrivenPBR"] =
		pbr.base_color_texture.index >= 0 ||
		pbr.metallic_roughness_texture.index >= 0 ||
		hasSpecGloss
	;
	out["usesLowRoughnessFactor"] = pbr.roughness_factor < 0.35;
	out["likelyReflectiveFromFactors"] =
		pbr.roughness_factor < 0.35 &&
		pbr.metallic_factor > 0.5
	;

	py::list extensions;

	for(std::uint32_t i = 0; i < material.ext.extensions_count; i++) {
		extensions.append(tg3_to_string(material.ext.extensions[i].name));
	}

	out["extensions"] = extensions;

	return out;
}

py::dict primitiveInfo(const tg3_model& model, const tg3_primitive& primitive, int primitiveIdx) {
	py::dict out;
	py::dict attributes;
	bool hasJoints0 = false, hasWeights0 = false, hasPosition = false, hasNormal = false, hasTangent = false;

	out["index"] = primitiveIdx;
	out["mode"] = primitive.mode;
	out["indices"] = accessorInfo(model, primitive.indices);
	out["material"] = primitive.material;

	if(
		primitive.material >= 0 &&
		static_cast<std::uint32_t>(primitive.material) < model.materials_count
	) {
		out["materialName"] = maybeString(
			model.materials[static_cast<std::uint32_t>(primitive.material)].name
		);
	}

	for(std::uint32_t i = 0; i < primitive.attributes_count; i++) {
		const tg3_str& name = primitive.attributes[i].key;

		attributes[py::str(tg3_to_string(name))] = accessorInfo(model, primitive.attributes[i].value);

		if(tg3_str_equals_cstr(name, "JOINTS_0")) hasJoints0 = true;
		else if(tg3_str_equals_cstr(name, "WEIGHTS_0")) hasWeights0 = true;
		else if(tg3_str_equals_cstr(name, "POSITION")) hasPosition = true;
		else if(tg3_str_equals_cstr(name, "NORMAL")) hasNormal = true;
		else if(tg3_str_equals_cstr(name, "TANGENT")) hasTangent = true;
	}

	out["attributes"] = attributes;
	out["hasJoints0"] = hasJoints0;
	out["hasWeights0"] = hasWeights0;
	out["hasPosition"] = hasPosition;
	out["hasNormal"] = hasNormal;
	out["hasTangent"] = hasTangent;

	return out;
}

py::dict meshInfo(const tg3_model& model, int meshIdx) {
	const tg3_mesh& mesh = model.meshes[static_cast<std::uint32_t>(meshIdx)];
	py::dict out;
	py::list primitives;

	out["index"] = meshIdx;
	out["name"] = maybeString(mesh.name);

	for(std::uint32_t i = 0; i < mesh.primitives_count; i++) {
		primitives.append(primitiveInfo(model, mesh.primitives[i], static_cast<int>(i)));
	}

	out["primitives"] = primitives;
	out["primitiveCount"] = mesh.primitives_count;

	return out;
}

py::dict skinInfo(const tg3_model& model, int skinIdx) {
	const tg3_skin& skin = model.skins[static_cast<std::uint32_t>(skinIdx)];
	py::dict out;
	py::list joints;
	py::list users;

	out["index"] = skinIdx;
	out["name"] = maybeString(skin.name);
	out["skeleton"] = skin.skeleton;
	out["skeletonName"] = maybeNodeName(model, skin.skeleton);
	out["inverseBindMatrices"] = accessorInfo(model, skin.inverse_bind_matrices);

	for(std::uint32_t jointIdx = 0; jointIdx < skin.joints_count; jointIdx++) {
		int nodeIdx = skin.joints[jointIdx];
		py::dict joint;

		joint["index"] = jointIdx;
		joint["node"] = nodeIdx;
		joint["nodeName"] = maybeNodeName(model, nodeIdx);

		joints.append(joint);
	}

	for(std::uint32_t nodeIdx = 0; nodeIdx < model.nodes_count; nodeIdx++) {
		const tg3_node& node = model.nodes[nodeIdx];

		if(node.skin != skinIdx) continue;

		py::dict user;

		user["node"] = nodeIdx;
		user["nodeName"] = maybeString(node.name);
		user["mesh"] = node.mesh;

		if(node.mesh >= 0 && static_cast<std::uint32_t>(node.mesh) < model.meshes_count) {
			user["meshName"] = maybeString(
				model.meshes[static_cast<std::uint32_t>(node.mesh)].name
			);
		}

		users.append(user);
	}

	out["joints"] = joints;
	out["jointCount"] = skin.joints_count;
	out["users"] = users;
	out["userCount"] = py::len(users);

	const int ibm = skin.inverse_bind_matrices >= 0 &&
		static_cast<std::uint32_t>(skin.inverse_bind_matrices) < model.accessors_count
		? skin.inverse_bind_matrices
		: -1
	;

	out["inverseBindMatricesMatchJointCount"] =
		ibm >= 0 &&
		model.accessors[static_cast<std::uint32_t>(ibm)].count == skin.joints_count
	;

	return out;
}

py::dict animationInfo(const tg3_model& model, int animationIdx) {
	const tg3_animation& animation =
		model.animations[static_cast<std::uint32_t>(animationIdx)];
	py::dict out;
	py::list samplers;
	py::list channels;
	std::set<std::string> paths;
	std::set<std::string> interpolations;
	double duration = 0.0;

	out["index"] = animationIdx;
	out["name"] = maybeString(animation.name);

	for(std::uint32_t samplerIdx = 0; samplerIdx < animation.samplers_count; samplerIdx++) {
		const tg3_animation_sampler& sampler = animation.samplers[samplerIdx];
		std::string interpolation = sampler.interpolation.len == 0
			? "LINEAR"
			: tg3_to_string(sampler.interpolation)
		;
		py::dict item;

		item["index"] = samplerIdx;
		item["input"] = accessorInfo(model, sampler.input);
		item["output"] = accessorInfo(model, sampler.output);
		item["interpolation"] = interpolation;

		if(
			sampler.input >= 0 &&
			static_cast<std::uint32_t>(sampler.input) < model.accessors_count &&
			model.accessors[static_cast<std::uint32_t>(sampler.input)].max_values_count > 0
		) {
			const tg3_accessor& input =
				model.accessors[static_cast<std::uint32_t>(sampler.input)];

			duration = std::max(duration, input.max_values[0]);
			item["endTime"] = input.max_values[0];
		}

		interpolations.insert(interpolation);
		samplers.append(item);
	}

	for(std::uint32_t channelIdx = 0; channelIdx < animation.channels_count; channelIdx++) {
		const tg3_animation_channel& channel = animation.channels[channelIdx];
		std::string targetPath = tg3_to_string(channel.target.path);
		py::dict item;

		item["index"] = channelIdx;
		item["sampler"] = channel.sampler;
		item["targetNode"] = channel.target.node;
		item["targetNodeName"] = maybeNodeName(model, channel.target.node);
		item["targetPath"] = targetPath;
		item["supportedByCurrentLoader"] =
			targetPath == "translation" ||
			targetPath == "rotation" ||
			targetPath == "scale"
		;

		paths.insert(targetPath);
		channels.append(item);
	}

	py::list pathList;
	for(const auto& path : paths) pathList.append(path);

	py::list interpolationList;
	for(const auto& interpolation : interpolations) interpolationList.append(interpolation);

	out["samplers"] = samplers;
	out["samplerCount"] = animation.samplers_count;
	out["channels"] = channels;
	out["channelCount"] = animation.channels_count;
	out["targetPaths"] = pathList;
	out["interpolations"] = interpolationList;
	out["duration"] = duration;
	out["hasMorphTargetAnimation"] = paths.find("weights") != paths.end();
	out["hasCubicSpline"] = interpolations.find("CUBICSPLINE") != interpolations.end();

	return out;
}

py::dict inspectGLTF(const std::string& path, bool /* loadImages - tinygltf v3.0.1 never decodes
	image pixel data regardless of this flag (see Texture.cpp's self-decode comment); kept for
	Python API source compatibility only, no effect here */) {
	tinygltf3::Model model;
	tinygltf3::ErrorStack errors;
	tg3_parse_options opts;

	tg3_parse_options_init(&opts);

	// inspectGLTF() doesn't go through osgx::gltf::Reader, so it needs its own fs.read_file
	// (tg3_parse_file has no fallback beyond TINYGLTF3_ENABLE_FS-style opt-in macros this
	// project doesn't define) - reuse the same plain-read implementation ReaderImpl.cpp uses.
	opts.fs.read_file = &osgx::gltf::detail::tg3_read_file;
	opts.fs.free_file = &osgx::gltf::detail::tg3_free_file;

	tg3_error_code rc = tinygltf3::parse_file(model, errors, path.c_str(), &opts);

	if(rc != TG3_OK || errors.has_error()) {
		std::string message;

		for(std::uint32_t i = 0; i < errors.count(); i++) {
			const tg3_error_entry* entry = errors.entry(i);

			if(entry->severity != TG3_SEVERITY_ERROR) continue;
			if(!message.empty()) message += "; ";

			message += entry->message ? entry->message : "";
		}

		throw std::runtime_error("failed to load " + path + ": " + message);
	}

	py::dict out;
	py::dict asset;
	py::dict counts;
	py::dict intent;
	py::list warnings;
	py::list scenes;
	py::list nodes;
	py::list meshes;
	py::list materials;
	py::list skins;
	py::list animations;

	for(std::uint32_t i = 0; i < errors.count(); i++) {
		const tg3_error_entry* entry = errors.entry(i);

		if(entry->severity != TG3_SEVERITY_ERROR && entry->message) warnings.append(entry->message);
	}

	asset["version"] = tg3_to_string(model->asset.version);
	asset["minVersion"] = maybeString(model->asset.min_version);
	asset["generator"] = maybeString(model->asset.generator);
	asset["copyright"] = maybeString(model->asset.copyright);

	counts["scenes"] = model->scenes_count;
	counts["nodes"] = model->nodes_count;
	counts["meshes"] = model->meshes_count;
	counts["materials"] = model->materials_count;
	counts["textures"] = model->textures_count;
	counts["images"] = model->images_count;
	counts["skins"] = model->skins_count;
	counts["animations"] = model->animations_count;
	counts["accessors"] = model->accessors_count;
	counts["bufferViews"] = model->buffer_views_count;
	counts["buffers"] = model->buffers_count;

	for(std::uint32_t sceneIdx = 0; sceneIdx < model->scenes_count; sceneIdx++) {
		const tg3_scene& scene = model->scenes[sceneIdx];
		py::dict sceneDict;
		py::list sceneNodes;

		sceneDict["index"] = sceneIdx;
		sceneDict["name"] = maybeString(scene.name);

		for(std::uint32_t i = 0; i < scene.nodes_count; i++) sceneNodes.append(scene.nodes[i]);

		sceneDict["nodes"] = sceneNodes;

		scenes.append(sceneDict);
	}

	for(std::uint32_t nodeIdx = 0; nodeIdx < model->nodes_count; nodeIdx++) {
		const tg3_node& node = model->nodes[nodeIdx];
		py::dict nodeDict;
		py::list children;

		nodeDict["index"] = nodeIdx;
		nodeDict["name"] = maybeString(node.name);
		nodeDict["mesh"] = node.mesh;
		nodeDict["skin"] = node.skin;

		for(std::uint32_t i = 0; i < node.children_count; i++) children.append(node.children[i]);

		nodeDict["children"] = children;

		// v3 always populates translation/rotation/scale/matrix with spec defaults, plus an
		// explicit has_matrix flag - see Scene.cpp's _createNode for the same convention.
		nodeDict["hasMatrix"] = static_cast<bool>(node.has_matrix);
		nodeDict["hasTranslation"] = !node.has_matrix;
		nodeDict["hasRotation"] = !node.has_matrix;
		nodeDict["hasScale"] = !node.has_matrix;

		nodes.append(nodeDict);
	}

	for(std::uint32_t meshIdx = 0; meshIdx < model->meshes_count; meshIdx++) {
		meshes.append(meshInfo(*model.get(), static_cast<int>(meshIdx)));
	}

	for(std::uint32_t materialIdx = 0; materialIdx < model->materials_count; materialIdx++) {
		materials.append(materialInfo(*model.get(), static_cast<int>(materialIdx)));
	}

	for(std::uint32_t skinIdx = 0; skinIdx < model->skins_count; skinIdx++) {
		skins.append(skinInfo(*model.get(), static_cast<int>(skinIdx)));
	}

	for(std::uint32_t animationIdx = 0; animationIdx < model->animations_count; animationIdx++) {
		animations.append(animationInfo(*model.get(), static_cast<int>(animationIdx)));
	}

	bool hasSpecGloss = false;
	bool hasJoints0 = false;
	bool hasWeights0 = false;
	bool hasMorphTargets = false;
	bool hasPBRTextures = false;

	for(std::uint32_t i = 0; i < model->materials_count; i++) {
		const tg3_material& material = model->materials[i];

		hasSpecGloss = hasSpecGloss ||
			osgx::gltf::detail::tg3_find_extension(
				material.ext, "KHR_materials_pbrSpecularGlossiness"
			) != nullptr
		;

		hasPBRTextures = hasPBRTextures ||
			material.pbr_metallic_roughness.base_color_texture.index >= 0 ||
			material.pbr_metallic_roughness.metallic_roughness_texture.index >= 0 ||
			material.normal_texture.index >= 0 ||
			material.occlusion_texture.index >= 0 ||
			material.emissive_texture.index >= 0
		;
	}

	for(std::uint32_t meshIdx = 0; meshIdx < model->meshes_count; meshIdx++) {
		const tg3_mesh& mesh = model->meshes[meshIdx];

		for(std::uint32_t primIdx = 0; primIdx < mesh.primitives_count; primIdx++) {
			const tg3_primitive& primitive = mesh.primitives[primIdx];

			for(std::uint32_t attrIdx = 0; attrIdx < primitive.attributes_count; attrIdx++) {
				const tg3_str& name = primitive.attributes[attrIdx].key;

				hasJoints0 = hasJoints0 || tg3_str_equals_cstr(name, "JOINTS_0");
				hasWeights0 = hasWeights0 || tg3_str_equals_cstr(name, "WEIGHTS_0");
			}

			hasMorphTargets = hasMorphTargets || primitive.targets_count > 0;
		}
	}

	intent["hasSkinning"] = model->skins_count > 0 || hasJoints0 || hasWeights0;
	intent["hasAnimation"] = model->animations_count > 0;
	intent["hasMorphTargets"] = hasMorphTargets;
	intent["hasSpecGloss"] = hasSpecGloss;
	intent["hasPBRTextures"] = hasPBRTextures;
	intent["hasJoints0"] = hasJoints0;
	intent["hasWeights0"] = hasWeights0;

	out["path"] = path;
	out["asset"] = asset;
	out["counts"] = counts;
	out["intent"] = intent;
	out["warnings"] = warnings;
	out["defaultScene"] = model->default_scene;
	out["scenes"] = scenes;
	out["nodes"] = nodes;
	out["meshes"] = meshes;
	out["materials"] = materials;
	out["skins"] = skins;
	out["animations"] = animations;

	return out;
}

std::string inspectGLTFJson(const std::string& path, bool loadImages, int indent) {
	py::object json = py::module_::import("json");

	return py::str(json.attr("dumps")(
		inspectGLTF(path, loadImages),
		"indent"_a=indent,
		"sort_keys"_a=true
	));
}

// Owns both the pyx::PollableProgress<Stage> a background thread writes into and the poller's
// own "last seen generation" cursor. Kept as one Python-visible object (rather than exposing
// PollableProgress directly) so a caller never has to thread a generation variable through
// itself - construct one, hand it to readNodeFile(), and call .poll() from a loop with a real
// sleep between checks (see pyosg_async.run_with_progress()). The call itself is free of GIL
// contention (see pybind11x.hpp's PollableProgress docs), but the loop calling it still needs a
// real cadence - a zero-delay busy-loop caused a genuine, measured slowdown; see
// run_with_progress()'s docstring.
struct AsyncProgress {
	pyx::PollableProgress<osgx::gltf::Reader::Stage> progress;
	std::uint64_t seen = 0;

	// Returns None when nothing changed since the last poll() call, otherwise
	// (stage_name, current, total, section, overall) - the same shape readNodeFileAsync used to
	// push through a queue, just pulled instead of pushed. `overall` is a monotonic 0.0-1.0
	// estimate of progress across the whole load (see Reader::computeOverall()), reported
	// alongside the per-section (current, total) detail rather than in place of it.
	py::object poll() {
		osgx::gltf::Reader::Stage stage;
		std::uint64_t current = 0, total = 0;
		const char* section = nullptr;
		double overall = 0.0;

		if(!progress.poll(seen, stage, current, total, section, overall)) return py::none();

		return py::make_tuple(
			std::string(osgx::gltf::Reader::stageName(stage)),
			current,
			total,
			std::string(section ? section : ""),
			overall
		);
	}
};

// glTF load off the GIL: releases the GIL and calls osgx::gltf::Reader directly (bypassing the
// generic osgDB::readNodeFile plugin dispatch, which has no hook for a progress callback). This
// function is a plain blocking call - run it via asyncio.to_thread(...) from Python to get it
// off the render thread; see examples/pyosg_async.py's run_with_progress() for the awaiting
// side. Progress is written into `progress` (an AsyncProgress) purely through atomics - this
// function never touches Python once it starts, not even to report progress, which is what
// makes it safe to poll from the main thread as tightly as that thread likes without
// contending with it for the GIL (see aipython/25-async-loading.md).
//
// The final osg::ref_ptr<osg::Node> is returned normally, not pushed anywhere - when called via
// asyncio.to_thread(...), asyncio's own machinery delivers it back to the awaiting coroutine
// through its Future, touching the GIL exactly once, at completion. That single unavoidable
// touch was never the problem; the repeated per-tick progress touches were.
//
// Cancellation via `stop` is cooperative and can only take effect between osgx::gltf::Reader's own
// checkpoints (see osgx::gltf::Reader::ProgressCallback) - it cannot interrupt tinygltf's own file
// parse/decode, which is a single opaque blocking call. If a stop was requested by the time
// read() returns, nullptr is returned instead of the loaded node.
osg::ref_ptr<osg::Node> readNodeFile(
	std::string location,
	pyx::StopEvent* stop,
	AsyncProgress* progress,
	const osgDB::Options* readOptions
) {
	py::gil_scoped_release release;

	// Same texture-dedup cache ReaderWriterGLTF::readNode() wires up for the normal
	// osgDB::readNodeFile() path. Without this, the reader's three cache-checking call sites
	// (occlusion/metallic-roughness bake, base color, normal map) all skip their cache lookup and
	// silently reload+redecode any texture referenced by more than one material in the
	// model - measured 4.5x slower (13.5s vs 2.96s) on a real multi-material asset
	// before this was added.
	static osgx::gltf::Reader::TextureCache s_asyncTextureCache;

	const std::string ext = osgDB::getLowerCaseFileExtension(location);
	const bool isBinary = ext == "glb";

	osgx::gltf::Reader reader;

	reader.setTextureCache(&s_asyncTextureCache);

	osgx::gltf::Reader::ProgressCallback onProgress = [&](const osgx::gltf::Reader::Progress& p) {
		if(progress) {
			progress->progress.set(p.stage, p.current, p.total, p.section.data(), p.overall);
		}
	};

	auto result = reader.read(location, isBinary, readOptions, onProgress);

	if(stop && stop->stop.load(std::memory_order_relaxed)) return nullptr;

	return result.validNode() ? result.getNode() : nullptr;
}

}

namespace osgx_python {

// osgx::gltf - glTF 2.0 loader plus its optional osgx::gltf::pbribl PBR/IBL adapter, merged from
// the formerly-separate osgGLTF repo/Python module 2026-07-30. Nested exactly like the C++
// namespace (osgx::gltf::shader, osgx::gltf::pbribl).
void bind_gltf(py::module_& m_gltf) {
	osgx::gltf::pbribl::registerShaderLibs();

	auto m_gltf_shader = m_gltf.def_submodule(
		"shader",
		"Shader inputs and setup matching the scene state populated by the osgx::gltf loader"
	);

	m_gltf_shader.attr("TANGENT_ATTRIBUTE") = osgx::gltf::shader::TANGENT_ATTRIBUTE;
	m_gltf_shader.attr("JOINT_INDICES_ATTRIBUTE") = osgx::gltf::shader::JOINT_INDICES_ATTRIBUTE;
	m_gltf_shader.attr("JOINT_WEIGHTS_ATTRIBUTE") = osgx::gltf::shader::JOINT_WEIGHTS_ATTRIBUTE;
	m_gltf_shader.attr("TANGENT_ATTRIBUTE_NAME") = py::str(osgx::gltf::shader::TANGENT_ATTRIBUTE_NAME);
	m_gltf_shader.attr("JOINT_INDICES_ATTRIBUTE_NAME") =
		py::str(osgx::gltf::shader::JOINT_INDICES_ATTRIBUTE_NAME);
	m_gltf_shader.attr("JOINT_WEIGHTS_ATTRIBUTE_NAME") =
		py::str(osgx::gltf::shader::JOINT_WEIGHTS_ATTRIBUTE_NAME);
	m_gltf_shader.attr("BASE_COLOR_TEXTURE_UNIT") = osgx::gltf::shader::BASE_COLOR_TEXTURE_UNIT;
	m_gltf_shader.attr("NORMAL_TEXTURE_UNIT") = osgx::gltf::shader::NORMAL_TEXTURE_UNIT;
	m_gltf_shader.attr("ORM_TEXTURE_UNIT") = osgx::gltf::shader::ORM_TEXTURE_UNIT;
	m_gltf_shader.attr("EMISSIVE_TEXTURE_UNIT") = osgx::gltf::shader::EMISSIVE_TEXTURE_UNIT;
	m_gltf_shader.attr("BASE_COLOR_SAMPLER") = py::str(osgx::gltf::shader::BASE_COLOR_SAMPLER);
	m_gltf_shader.attr("NORMAL_SAMPLER") = py::str(osgx::gltf::shader::NORMAL_SAMPLER);
	m_gltf_shader.attr("ORM_SAMPLER") = py::str(osgx::gltf::shader::ORM_SAMPLER);
	m_gltf_shader.attr("EMISSIVE_SAMPLER") = py::str(osgx::gltf::shader::EMISSIVE_SAMPLER);
	m_gltf_shader.attr("ALPHA_MODE_UNIFORM") = py::str(osgx::gltf::shader::ALPHA_MODE_UNIFORM);
	m_gltf_shader.attr("ALPHA_CUTOFF_UNIFORM") = py::str(osgx::gltf::shader::ALPHA_CUTOFF_UNIFORM);
	m_gltf_shader.attr("ALPHA_MODE_OPAQUE") = osgx::gltf::shader::ALPHA_MODE_OPAQUE;
	m_gltf_shader.attr("ALPHA_MODE_MASK") = osgx::gltf::shader::ALPHA_MODE_MASK;
	m_gltf_shader.attr("ALPHA_MODE_BLEND") = osgx::gltf::shader::ALPHA_MODE_BLEND;
	m_gltf_shader.attr("MATERIAL_INPUTS") = py::str(osgx::gltf::shader::MATERIAL_INPUTS);
	m_gltf_shader.attr("SKINNING_HOOK_IDENTITY") =
		py::str(osgx::gltf::shader::SKINNING_HOOK_IDENTITY);
	m_gltf_shader.attr("SKINNING_HOOK_LINEAR_BLEND") =
		py::str(osgx::gltf::shader::SKINNING_HOOK_LINEAR_BLEND);

	m_gltf_shader
		.def(
			"configureProgram", &osgx::gltf::shader::configureProgram, "program"_a,
			"Binds the tangent/skinning generic vertex attribute locations (TANGENT_ATTRIBUTE, "
			"JOINT_INDICES_ATTRIBUTE, JOINT_WEIGHTS_ATTRIBUTE) on `program`."
		)
		.def(
			"configureStateSet", &osgx::gltf::shader::configureStateSet, "stateSet"_a,
			"Binds the four material sampler uniforms (base color/normal/ORM/emissive) on "
			"`stateSet` to their fixed texture units."
		)
	;

	auto m_gltf_pbribl = m_gltf.def_submodule(
		"pbribl",
		"Optional osgx::gltf rendering using the generic osgx PBR and IBL facilities"
	);

	m_gltf_pbribl.attr("GET_MATERIAL") = py::str(osgx::gltf::pbribl::GET_MATERIAL);
	m_gltf_pbribl.attr("SHADING_NORMAL") = py::str(osgx::gltf::pbribl::SHADING_NORMAL);
	m_gltf_pbribl.attr("EMISSIVE") = py::str(osgx::gltf::pbribl::EMISSIVE);
	m_gltf_pbribl.attr("ALPHA_COVERAGE") = py::str(osgx::gltf::pbribl::ALPHA_COVERAGE);
	m_gltf_pbribl.def(
		"registerShaderLibs", &osgx::gltf::pbribl::registerShaderLibs,
		"Registers the pbribl-specific `#pragma osgx::gltf ...` GLSL catalogs (GET_MATERIAL, "
		"SHADING_NORMAL, EMISSIVE, ALPHA_COVERAGE, DEFERRED_LIGHTING_INPUTS, GET_GBUFFER) so "
		"resolveShaderLibs() can expand them. Idempotent; called automatically on module import."
	);
	m_gltf_pbribl.def("resolveShaderLibs", [](const std::string& source) {
		return osgx::gltf::pbribl::resolveShaderLibs(source);
	}, "source"_a,
		"Expands `#pragma osgx::gltf ...` (plus the generic osgx::pbr/ibl/shadow catalogs) in "
		"`source`, returning the fully expanded GLSL. Required before wrapping a custom "
		"Hook shader that uses the pbribl-specific catalog in an osg.Shader."
	);

	py::class_<osgx::gltf::pbribl::PBRIBLEnvironment>(
		m_gltf_pbribl,
		"PBRIBLEnvironment",
		"Prepared IBL resources: a GGX-prefiltered specular cubemap, a Lambertian diffuse "
		"cubemap, a BRDF LUT, plus the KTX/OpenGL cubemap lookup basis relative to the loader's "
		"Z-up world. `root`, when present, holds PRE_RENDER bake passes still populating a "
		"texture - add it to a rendered scene graph before relying on that texture's contents; "
		"a fully pre-baked environment has no root and leaves it None."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRIBLEnvironment.")
		.def_readwrite(
			"root", &osgx::gltf::pbribl::PBRIBLEnvironment::root,
			"The Group holding this environment's PRE_RENDER bake passes, if any are still "
			"populating a texture; None for a fully pre-baked environment."
		)
		.def_readwrite(
			"envMap", &osgx::gltf::pbribl::PBRIBLEnvironment::envMap,
			"The GGX-prefiltered specular cubemap."
		)
		.def_readwrite(
			"brdfLUT", &osgx::gltf::pbribl::PBRIBLEnvironment::brdfLUT,
			"The split-sum BRDF lookup texture."
		)
		.def_readwrite(
			"diffuseEnv", &osgx::gltf::pbribl::PBRIBLEnvironment::diffuseEnv,
			"The Lambertian diffuse-irradiance cubemap."
		)
		.def_readwrite(
			"iblAxis", &osgx::gltf::pbribl::PBRIBLEnvironment::iblAxis,
			"The KTX/OpenGL cubemap lookup basis (3 orthonormal row vectors), expressed relative "
			"to the loader's Z-up world."
		)
		.def(
			"valid", &osgx::gltf::pbribl::PBRIBLEnvironment::valid,
			"True once envMap/brdfLUT/diffuseEnv are all set, regardless of whether any bake "
			"pass under `root` has actually finished populating them yet."
		)
		.def_static(
			"prepare",
			py::overload_cast<const std::string&, int>(
				&osgx::gltf::pbribl::PBRIBLEnvironment::prepare
			),
			"hdrPath"_a,
			"lutSize"_a=1024,
			"Bake diffuse irradiance, the BRDF LUT, and GGX-prefiltered specular all live from hdrPath "
			"alone - no pre-baked KTX2 required. Add root to the rendered scene graph so its "
			"PRE_RENDER passes can populate the generated textures."
		)
		.def_static(
			"prepareDiffuseOnly",
			py::overload_cast<const std::string&, int>(
				&osgx::gltf::pbribl::PBRIBLEnvironment::prepareDiffuseOnly
			),
			"hdrPath"_a,
			"lutSize"_a=1024,
			"Same as prepare(), but never GGX-prefilters a specular cubemap from hdrPath - for a "
			"caller whose specular reflection always comes from somewhere else (e.g. a live "
			"procedural rebake) and would otherwise pay for a real bake only to discard it before "
			"it's ever sampled. envMap comes back as a small, valid, unbaked placeholder; supply "
			"real content yourself by binding your own GGXPrefilterScene's prefilterTexture onto "
			"the same texture unit (5) PBRIBLScene.create() bound this placeholder to."
		)
		.def_static(
			"load",
			py::overload_cast<const std::string&>(&osgx::gltf::pbribl::PBRIBLEnvironment::load),
			"manifestPath"_a,
			"Load a fully pre-baked osgx_pbribl environment manifest."
		)
	;

	py::class_<osgx::gltf::pbribl::PBRIBLScene>(
		m_gltf_pbribl,
		"PBRIBLScene",
		"The result of applying osgx::gltf::pbribl's renderer to a node - the node itself plus "
		"live debug/intensity osg.Uniform refs a caller can tune after scene creation."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRIBLScene.")
		.def_readwrite(
			"node", &osgx::gltf::pbribl::PBRIBLScene::node,
			"The node the renderer was applied to."
		)
		.def_readwrite(
			"debugMode", &osgx::gltf::pbribl::PBRIBLScene::debugMode,
			"Debug-visualization mode uniform (see the shader's debugMode switch)."
		)
		.def_readwrite(
			"disableNormalMap", &osgx::gltf::pbribl::PBRIBLScene::disableNormalMap,
			"When set, forces the shading normal to the geometric normal, ignoring any normal map."
		)
		.def_readwrite(
			"disableRoughnessMap",
			&osgx::gltf::pbribl::PBRIBLScene::disableRoughnessMap,
			"When set, ignores the material's roughness texture, using a constant instead."
		)
		.def_readwrite(
			"disableSpecularAA",
			&osgx::gltf::pbribl::PBRIBLScene::disableSpecularAA,
			"When set, disables geometric specular anti-aliasing (roughness widening from normal "
			"map curvature)."
		)
		.def_readwrite(
			"iblDiffuseIntensity",
			&osgx::gltf::pbribl::PBRIBLScene::iblDiffuseIntensity,
			"Live multiplier on the IBL diffuse-irradiance contribution, independent of specular."
		)
		.def_readwrite(
			"iblSpecularIntensity",
			&osgx::gltf::pbribl::PBRIBLScene::iblSpecularIntensity,
			"Live multiplier on the IBL specular-reflection contribution, independent of diffuse."
		)
		.def(
			"valid", &osgx::gltf::pbribl::PBRIBLScene::valid,
			"True once node and every debug/intensity uniform are set."
		)
		.def_static(
			"create",
			&osgx::gltf::pbribl::PBRIBLScene::create,
			"node"_a,
			"environment"_a,
			"iblDiffuseIntensity"_a=1.0f,
			"iblSpecularIntensity"_a=1.0f,
			"diagnostics"_a=false,
			"shadowMap"_a=nullptr,
			"hooks"_a=osgx::HookList{},
			"Apply osgx::gltf's optional osgx-powered PBR/IBL renderer using prepared resources. Pass "
			"an osgx.shadow.ShadowMap to shadow the key/directional light (osgx::LightSet index "
			"shadowMap.casterIndex); omit it for today's unshadowed behavior.\n\n"
			"hooks: an osgx.HookList (a list of (osgx.Hook, osg.Shader) pairs) substituting this "
			"Program's built-in shader for a slot. This Program supports osgx.Hook.Skinning (a "
			"VERTEX osg.Shader defining osgx_gltf_ApplySkin(vec4, vec3, vec3), REPLACING the default "
			"identity passthrough - pass osgx.gltf.shader.SKINNING_HOOK_LINEAR_BLEND, wrapped in "
			"osgx.gltf.pbribl.resolveShaderLibs(), to enable standard glTF joint-matrix skinning) and "
			"osgx.Hook.Tonemap (a FRAGMENT osg.Shader defining osgx_Tonemap(vec3), REPLACING the "
			"built-in PBR Neutral curve). Each hook substitutes rather than adds - GLSL permits one "
			"body per function, so attaching a second definition alongside the built-in is a link "
			"error, not an override."
		)
	;

	py::class_<osgx::gltf::pbribl::PBRIBLGBuffer>(
		m_gltf_pbribl,
		"PBRIBLGBuffer",
		"Deferred-split geometry-pass output: material only (no lighting, not even emissive "
		"add), ready to feed PBRIBLLightingScene.create()."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRIBLGBuffer.")
		.def_readwrite(
			"gbuffer", &osgx::gltf::pbribl::PBRIBLGBuffer::gbuffer,
			"The underlying osgx.gbuffer.GBuffer this was built from."
		)
		.def_readwrite(
			"albedoTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::albedoTexture,
			"rgb = albedo, a = ambient occlusion."
		)
		.def_readwrite(
			"normalTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::normalTexture,
			"rgb = view-space shading normal (RGB16F)."
		)
		.def_readwrite(
			"materialTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::materialTexture,
			"r = roughness, g = metallic."
		)
		.def_readwrite(
			"emissiveTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::emissiveTexture,
			"rgb = emissive (HDR), a = alpha coverage."
		)
		.def_readwrite(
			"positionTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::positionTexture,
			"rgb = view-space position (RGBA32F)."
		)
		.def_readwrite(
			"depthTexture", &osgx::gltf::pbribl::PBRIBLGBuffer::depthTexture,
			"The geometry pass's depth attachment."
		)
		.def(
			"valid", &osgx::gltf::pbribl::PBRIBLGBuffer::valid,
			"True once every G-buffer texture is set."
		)
		.def_static(
			"create",
			&osgx::gltf::pbribl::PBRIBLGBuffer::create,
			"node"_a,
			"width"_a,
			"height"_a,
			"Deferred-split geometry pass: writes material only (albedo/view-space normal/ORM/"
			"emissive + depth) to a PBRIBLGBuffer, no lighting. Feed the result to "
			"PBRIBLLightingScene.create()."
		)
	;

	py::class_<osgx::gltf::pbribl::PBRIBLLightingPassOptions>(
		m_gltf_pbribl,
		"PBRIBLLightingPassOptions",
		"Extra PBRIBLLightingScene.create() inputs, each an independent, optional seam rather "
		"than one monolithic flag blob."
	)
		.def(py::init<>(), "Constructs the default options (tonemap on, everything else unset).")
		.def_readwrite(
			"tonemap", &osgx::gltf::pbribl::PBRIBLLightingPassOptions::tonemap,
			"True applies the built-in (or hooks[osgx.Hook.Tonemap]-substituted) tonemap curve; "
			"False leaves the result linear HDR, for a caller chaining further passes."
		)
		.def_readwrite(
			"hooks", &osgx::gltf::pbribl::PBRIBLLightingPassOptions::hooks,
			"An osgx.HookList substituting this pass's built-in shader for a slot - "
			"osgx.Hook.DeferredLighting (the whole fragment main()), osgx.Hook.DirectLighting, "
			"and osgx.Hook.Tonemap are supported. Each REPLACES its default, it does not add "
			"alongside it."
		)
		.def_readwrite(
			"shadowMap", &osgx::gltf::pbribl::PBRIBLLightingPassOptions::shadowMap,
			"Mirrors PBRIBLScene.create()'s own parameter exactly; None is unshadowed."
		)
		.def_readwrite(
			"aoTexture", &osgx::gltf::pbribl::PBRIBLLightingPassOptions::aoTexture,
			"Optional ambient-occlusion texture, multiplied into the ambient term. This pass does "
			"not bake SSAO itself - feed osgx.gbuffer.SSAO.create()'s result here, or any other "
			"occlusion source."
		)
		.def_readwrite(
			"diagnostics", &osgx::gltf::pbribl::PBRIBLLightingPassOptions::diagnostics,
			"Enables extra debug output on the lighting pass."
		)
	;

	py::class_<osgx::gltf::pbribl::PBRIBLLightingScene>(
		m_gltf_pbribl,
		"PBRIBLLightingScene",
		"The result of PBRIBLLightingScene.create(): a fullscreen-quad deferred lighting pass "
		"reading a PBRIBLGBuffer, plus live uniforms a caller must keep in sync via update()."
	)
		.def(py::init<>(), "Constructs an empty, invalid PBRIBLLightingScene.")
		.def_readwrite(
			"node", &osgx::gltf::pbribl::PBRIBLLightingScene::node,
			"The fullscreen-quad lighting-pass node (an ABSOLUTE_RF camera)."
		)
		.def_readwrite(
			"iblDiffuseIntensity",
			&osgx::gltf::pbribl::PBRIBLLightingScene::iblDiffuseIntensity,
			"Live multiplier on the IBL diffuse-irradiance contribution, independent of specular."
		)
		.def_readwrite(
			"iblSpecularIntensity",
			&osgx::gltf::pbribl::PBRIBLLightingScene::iblSpecularIntensity,
			"Live multiplier on the IBL specular-reflection contribution, independent of diffuse."
		)
		.def_readwrite(
			"mainViewMatrix",
			&osgx::gltf::pbribl::PBRIBLLightingScene::mainViewMatrix,
			"mainCamera's view matrix, refreshed by update() - the quad's own camera is "
			"ABSOLUTE_RF, so OSG's automatic osg_ViewMatrix resolves to identity, not mainCamera's "
			"real matrix."
		)
		.def_readwrite(
			"mainViewMatrixInverse",
			&osgx::gltf::pbribl::PBRIBLLightingScene::mainViewMatrixInverse,
			"mainCamera's inverse view matrix, refreshed by update() alongside mainViewMatrix."
		)
		.def(
			"valid", &osgx::gltf::pbribl::PBRIBLLightingScene::valid,
			"True once node and every uniform are set."
		)
		.def_static(
			"create",
			&osgx::gltf::pbribl::PBRIBLLightingScene::create,
			"gbuffer"_a,
			"environment"_a,
			"mainCamera"_a,
			"iblDiffuseIntensity"_a=1.0f,
			"iblSpecularIntensity"_a=1.0f,
			"options"_a=osgx::gltf::pbribl::PBRIBLLightingPassOptions{},
			"Deferred-split lighting pass: a fullscreen quad reading `gbuffer` (position included, "
			"not reconstructed from depth), rotating it into world space via `mainCamera`'s real "
			"view matrix, running the same osgx_EvaluateIBL()/osgx_DirectLighting() logic "
			"PBRIBLScene.create() does. options.hooks supports osgx.Hook.DeferredLighting (the "
			"entire fullscreen lighting shader), osgx.Hook.DirectLighting (the "
			"osgx_DirectLighting() definition), and osgx.Hook.Tonemap. Each replaces its default "
			"shader object. Call .update() from a preDrawCallback on the "
			"FIRST PRE_RENDER camera in the scene graph every frame to keep it in sync as mainCamera "
			"moves - NOT from mainCamera's own preDrawCallback or from application code after "
			"frame() returns, both of which hand this pass a stale matrix."
		)
		.def(
			"update",
			&osgx::gltf::pbribl::PBRIBLLightingScene::update,
			"mainCamera"_a,
			"Refreshes this scene's manually-maintained view-matrix uniforms from mainCamera's "
			"current matrices. Call from a preDrawCallback on the FIRST PRE_RENDER camera in the "
			"scene graph (by render order) every frame - every PRE_RENDER camera finishes drawing "
			"before mainCamera's own preDrawCallback fires, so calling this from mainCamera's "
			"callback (or from application code after viewer.frame() returns) hands the lighting "
			"pass a one-frame-stale matrix relative to what the geometry pass just rendered with, "
			"which shows up as artifacts that worsen while the camera is moving."
		)
	;

	py::class_<osgx::gltf::SimplePlayer>(
		m_gltf,
		"SimplePlayer",
		"A thin, optional wrapper that finds a loaded glTF model's animation-playback control "
		"(by walking its update callback chain) and exposes play/pause/restart. bool(player) "
		"reports whether a playable control was actually found; every accessor degrades to a "
		"harmless no-op/default otherwise."
	)
		.def(
			py::init<osg::Node*>(), "model"_a,
			"Wraps `model`, searching its update callback chain for animation playback control."
		)
		.def("__bool__", [](const osgx::gltf::SimplePlayer& player) {
			return static_cast<bool>(player);
		}, "True if a playable animation control was found on the wrapped model.")
		.def_property_readonly(
			"numAnimations",
			&osgx::gltf::SimplePlayer::getNumAnimations,
			"Number of animations available (0 if no control was found)."
		)
		.def(
			"getAnimationName", &osgx::gltf::SimplePlayer::getAnimationName, "index"_a,
			"Returns the name of animation `index` (empty string if no control was found)."
		)
		.def(
			"playAnimation",
			py::overload_cast<std::size_t>(&osgx::gltf::SimplePlayer::playAnimation),
			"index"_a,
			"Starts playing animation `index`. Returns False if no control was found."
		)
		.def(
			"playAnimation",
			py::overload_cast<const std::string&>(&osgx::gltf::SimplePlayer::playAnimation),
			"name"_a,
			"Starts playing the animation named `name`. Returns False if no control was found."
		)
		.def_property_readonly(
			"currentAnimationIndex",
			[](const osgx::gltf::SimplePlayer& player) -> py::object {
				const std::size_t index = player.getCurrentAnimationIndex();
				return index == osgx::gltf::SimplePlayer::NoAnimation
					? py::none()
					: py::cast(index);
			},
			"Index of the currently selected animation, or None if none is selected/no control was found."
		)
		.def_property_readonly(
			"currentAnimationName",
			&osgx::gltf::SimplePlayer::getCurrentAnimationName,
			"Name of the currently selected animation (empty string if none is selected)."
		)
		.def_property(
			"playing",
			&osgx::gltf::SimplePlayer::getPlaying,
			&osgx::gltf::SimplePlayer::setPlaying,
			"Whether the current animation is playing."
		)
		.def("togglePlaying", &osgx::gltf::SimplePlayer::togglePlaying, "Toggles between playing and paused.")
		.def("restart", &osgx::gltf::SimplePlayer::restart, "Restarts the current animation from the beginning.")
	;

	py::class_<AsyncProgress>(
		m_gltf,
		"AsyncProgress",
		"Owns the pollable progress state a background readNodeFile() call writes into, plus the "
		"poller's own generation cursor. Construct one, pass it to readNodeFile(), and call "
		".poll() from a loop with a real sleep between checks."
	)
		.def(py::init<>(), "Constructs a fresh progress tracker with nothing reported yet.")
		.def(
			"poll",
			&AsyncProgress::poll,
			"Returns None if nothing has changed since the last poll() call, otherwise "
			"(stage, current, total, section, overall). current/total/section are real, "
			"never-fabricated detail within the current section; overall is a monotonic 0.0-1.0 "
			"estimate of progress across the whole load, reported alongside that detail rather "
			"than in place of it - see osgx::gltf::Reader::computeOverall() for how it's "
			"weighted. The call itself is cheap (no GIL contention, since readNodeFile() never "
			"touches Python to report progress) - but still call it from a loop with a real "
			"sleep between checks (e.g. pyosg_async.run_with_progress()), never from a "
			"zero-delay busy-loop; see that function's docstring for the real slowdown a "
			"busy-loop caused."
		)
	;

	m_gltf
		.def(
			"readNodeFile",
			&readNodeFile,
			"location"_a,
			"stop_event"_a,
			"progress"_a,
			"options"_a=nullptr,
			"Load a glTF/GLB file off the GIL. A plain blocking call - run it via "
			"asyncio.to_thread(...) from Python; see examples/pyosg_async.py's "
			"run_with_progress() and examples/pyosg-async-gltf.py for the awaiting side. "
			"Progress is written into `progress` (an AsyncProgress) purely through atomics, "
			"polled rather than pushed - this function never touches Python once it starts. "
			"`options` is a plain osgDB.Options (its optionString reaches the same "
			"gltfSkipNormals/gltfZUp/gltfSkipSpecGlossBake/gltfSkipAnimation/"
			"gltfStripPNGColorMetadata flags osgDB.readNodeFile()'s options honor)."
		)
	;

	m_gltf
		.def(
			"inspect",
			&inspectGLTF,
			"path"_a,
			"load_images"_a=false,
			"Parse a glTF/GLB file and return a structured Python summary."
		)
		.def(
			"inspect_json",
			&inspectGLTFJson,
			"path"_a,
			"load_images"_a=false,
			"indent"_a=2,
			"Parse a glTF/GLB file and return a JSON summary string."
		)
	;
}

}

#endif // OSGX_GLTF
