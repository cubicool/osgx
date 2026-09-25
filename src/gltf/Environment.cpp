#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

OSGX_ENABLE_WARNINGS

// Also brings in tinygltf_json_c.h (declarations only - tiny_gltf_v3.c already compiles the
// implementation once into osgx_gltf).
#include "Json.hpp"

#include "osgx/gltf/Environment.hpp"

#include "osgx/GGXPrefilter.hpp"
#include "osgx/IBL.hpp"
#include "osgx/PBR.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Image>
#include <osg/Notify>
#include <osg/Program>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/TextureCubeMap>
#include <osgDB/ReadFile>

OSGX_ENABLE_WARNINGS

#include <filesystem>
#include <fstream>

// ================================================================================================
// The osgx_environment manifest loader: a glTF-shaped JSON document declaring pre-baked
// osgx::Environment resources, loaded with the Khronos glTF-Sample-Viewer environment rotation.
// ================================================================================================

namespace osgx::gltf {

namespace {

// decodeString/decodeInt/JsonDocument/readWholeFile live in Json.hpp (shared with the other extension
// decoders in this directory).
using ::osgx::gltf::detail::decodeInt;
using ::osgx::gltf::detail::decodeString;
using ::osgx::gltf::detail::JsonDocument;
using ::osgx::gltf::detail::readWholeFile;

EnvironmentManifest decodeEnvironment(const tg3json_value* entry) {
	EnvironmentManifest manifest;

	if(!entry || entry->type != TG3JSON_OBJECT) return manifest;

	const tg3json_value* specular = tg3json_object_get(entry, "specular");

	manifest.specular.uri = decodeString(specular, "uri");
	manifest.diffuse.uri = decodeString(tg3json_object_get(entry, "diffuse"), "uri");

	const tg3json_value* brdfLUT = tg3json_object_get(entry, "brdfLUT");

	manifest.brdfLUT.uri = decodeString(brdfLUT, "uri");
	manifest.brdfLUT.builtin = decodeString(brdfLUT, "builtin");
	manifest.brdfLUT.size = decodeInt(brdfLUT, "size", 1024);

	return manifest;
}

}

std::vector<EnvironmentManifest> decodeEnvironments(const tg3json_value* extensionValue) {
	std::vector<EnvironmentManifest> result;

	if(!extensionValue || extensionValue->type != TG3JSON_OBJECT) return result;

	const tg3json_value* environments = tg3json_object_get(extensionValue, "environments");

	if(!environments || environments->type != TG3JSON_ARRAY) return result;

	for(std::size_t i = 0; i < tg3json_array_size(environments); i++) result.push_back(
		decodeEnvironment(tg3json_array_get(environments, i))
	);

	return result;
}

osg::ref_ptr<osgx::Environment> loadEnvironment(
	const EnvironmentManifest& manifest,
	const std::string& baseDir
) {
	if(!manifest.specular.valid() || !manifest.diffuse.valid() || !manifest.brdfLUT.valid()) {
		OSG_WARN << "osgx::gltf::loadEnvironment: manifest is missing a required resource" << std::endl;

		return nullptr;
	}

	const std::filesystem::path base(baseDir);

	// loadPrefilterCubemap() just loads a KTX2 as a TextureCubeMap, equally correct for the
	// Lambertian diffuse cube as for GGX specular.
	auto specularMap = osgx::loadPrefilterCubemap((base / manifest.specular.uri).string());

	if(!specularMap) return nullptr;

	auto diffuseMap = osgx::loadPrefilterCubemap((base / manifest.diffuse.uri).string());

	if(!diffuseMap) return nullptr;

	osg::ref_ptr<osgx::Environment> environment;

	if(!manifest.brdfLUT.builtin.empty()) {
		if(manifest.brdfLUT.builtin != "osgx:split-sum-ggx-v1" || manifest.brdfLUT.size < 1) {
			OSG_WARN
				<< "osgx::gltf::loadEnvironment: unsupported built-in BRDF LUT "
				<< manifest.brdfLUT.builtin << std::endl
			;

			return nullptr;
		}

		environment = osgx::make_ref<osgx::Environment>(
			specularMap.get(), diffuseMap.get(), -1.0f, manifest.brdfLUT.size
		);
	}

	else {
		auto lutImage = osgDB::readRefImageFile((base / manifest.brdfLUT.uri).string());

		if(!lutImage) {
			OSG_WARN << "osgx::gltf::loadEnvironment: failed to load " << manifest.brdfLUT.uri << std::endl;

			return nullptr;
		}

		auto lut = osgx::make_ref<osg::Texture2D>(lutImage.get());

		lut->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
		lut->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
		lut->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
		lut->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

		environment = osgx::make_ref<osgx::Environment>(specularMap.get(), diffuseMap.get(), lut.get());
	}

	environment->setRotation(KHRONOS_ENVIRONMENT_ROTATION);

	return environment;
}

osg::ref_ptr<osgx::Environment> loadEnvironment(const std::string& manifestPath) {
	std::string text;

	if(!readWholeFile(manifestPath, text)) {
		OSG_WARN << "osgx::gltf::loadEnvironment: failed to read " << manifestPath << std::endl;

		return nullptr;
	}

	JsonDocument document;

	if(!document.parse(text)) {
		OSG_WARN << "osgx::gltf::loadEnvironment: failed to parse " << manifestPath << std::endl;

		return nullptr;
	}

	const tg3json_value* extensions = tg3json_object_get(&document.root(), "extensions");
	const tg3json_value* environmentExtension = extensions
		? tg3json_object_get(extensions, "osgx_environment")
		: nullptr
	;

	if(!environmentExtension) {
		OSG_WARN << "osgx::gltf::loadEnvironment: " << manifestPath << " has no osgx_environment extension" << std::endl;

		return nullptr;
	}

	auto environments = decodeEnvironments(environmentExtension);

	if(environments.empty()) {
		OSG_WARN << "osgx::gltf::loadEnvironment: " << manifestPath << " declares no environments" << std::endl;

		return nullptr;
	}

	const std::string baseDir = std::filesystem::path(manifestPath).parent_path().string();
	const auto& manifest = environments.front();
	auto environment = loadEnvironment(manifest, baseDir);
	const std::string brdfLUT = !manifest.brdfLUT.uri.empty() ? manifest.brdfLUT.uri : manifest.brdfLUT.builtin;

	if(environment) {
		OSG_NOTICE
			<< "osgx::gltf::loadEnvironment: loaded pre-baked environment manifest \"" << manifestPath
			<< "\" (specular=\"" << manifest.specular.uri
			<< "\", diffuse=\"" << manifest.diffuse.uri
			<< "\", brdfLUT=\"" << brdfLUT << "\")"
			<< std::endl
		;
	}

	return environment;
}

}
