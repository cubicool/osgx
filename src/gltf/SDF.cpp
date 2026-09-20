#include "osgx/gltf/SDF.hpp"
#include "Json.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osg/Notify>
#include <osgDB/ReadFile>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <filesystem>

namespace osgx::gltf::sdf {

namespace {

using detail::asNumber;
using detail::decodeReal;
using detail::decodeString;

// A JSON array of exactly `count` numbers; false for anything else.
bool decodeNumbers(const tg3json_value* value, std::size_t count, double* out) {
	if(!value || value->type != TG3JSON_ARRAY || value->u.array.count != count) return false;

	for(std::size_t i = 0; i < count; i++) {
		if(!asNumber(&value->u.array.items[i], out[i])) return false;
	}

	return true;
}

// False if the entry lacks what a tile needs to render (a well-formed rect and a positive pixelRange).
bool decodeTile(const tg3json_value* value, Tile& tile) {
	if(!value || value->type != TG3JSON_OBJECT) return false;

	double rect[4] = {};

	if(!decodeNumbers(tg3json_object_get(value, "rect"), 4, rect)) return false;

	tile.x = static_cast<int>(rect[0]);
	tile.y = static_cast<int>(rect[1]);
	tile.w = static_cast<int>(rect[2]);
	tile.h = static_cast<int>(rect[3]);
	tile.pixelRange = static_cast<float>(decodeReal(value, "pixelRange", 0.0));

	// The em-space frame is all-or-nothing: a lone `range` without the rest is unusable.
	double origin[2] = {};

	if(
		tg3json_object_get(value, "range") &&
		tg3json_object_get(value, "texelsPerEm") &&
		decodeNumbers(tg3json_object_get(value, "emOrigin"), 2, origin)
	) {
		tile.hasEmFrame = true;
		tile.range = static_cast<float>(decodeReal(value, "range", 0.0));
		tile.texelsPerEm = static_cast<float>(decodeReal(value, "texelsPerEm", 0.0));
		tile.emOrigin = osg::Vec2(static_cast<float>(origin[0]), static_cast<float>(origin[1]));
	}

	return tile.w > 0 && tile.h > 0 && tile.pixelRange > 0.0f;
}

Manifest decodeManifest(const tg3json_value* entry) {
	Manifest manifest;

	if(!entry || entry->type != TG3JSON_OBJECT) return manifest;

	const std::string type = decodeString(entry, "type");

	if(type == "SDF") manifest.sdfType = ::osgx::SDF::SDFType::SDF;
	else if(type == "MSDF") manifest.sdfType = ::osgx::SDF::SDFType::MSDF;
	else {
		OSG_WARN << "osgx::gltf::sdf: unknown type \"" << type << "\" (want \"SDF\" or \"MSDF\")" << std::endl;

		return manifest;
	}

	manifest.uri = decodeString(tg3json_object_get(entry, "texture"), "uri");

	const tg3json_value* tiles = tg3json_object_get(entry, "tiles");

	if(!tiles || tiles->type != TG3JSON_OBJECT) return manifest;

	for(std::size_t i = 0; i < tiles->u.object.count; i++) {
		const auto& item = tiles->u.object.items[i];
		const std::string name(item.key, item.key_len);

		Tile tile;

		if(decodeTile(item.value, tile)) manifest.tiles[name] = tile;
		else OSG_WARN << "osgx::gltf::sdf: skipping malformed tile \"" << name << "\"" << std::endl;
	}

	return manifest;
}

std::vector<Manifest> decodeManifests(const tg3json_value* extension) {
	std::vector<Manifest> result;

	if(!extension || extension->type != TG3JSON_OBJECT) return result;

	const tg3json_value* data = tg3json_object_get(extension, "data");

	if(!data || data->type != TG3JSON_ARRAY) return result;

	for(std::size_t i = 0; i < data->u.array.count; i++) {
		result.push_back(decodeManifest(&data->u.array.items[i]));
	}

	return result;
}

}

TileSet TileSet::load(const std::string& manifestPath, std::size_t index) {
	std::string text;

	if(!detail::readWholeFile(manifestPath, text)) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: failed to read " << manifestPath << std::endl;

		return {};
	}

	detail::JsonDocument document;

	if(!document.parse(text)) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: failed to parse " << manifestPath << std::endl;

		return {};
	}

	const tg3json_value* extensions = tg3json_object_get(&document.root(), "extensions");
	const tg3json_value* extension = extensions ? tg3json_object_get(extensions, "osgx_sdf") : nullptr;

	if(!extension) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: " << manifestPath << " has no osgx_sdf extension" << std::endl;

		return {};
	}

	const auto manifests = decodeManifests(extension);

	if(index >= manifests.size()) {
		OSG_WARN
			<< "osgx::gltf::sdf::TileSet::load: " << manifestPath << " has " << manifests.size()
			<< " data entries; no entry " << index << std::endl
		;

		return {};
	}

	return load(manifests[index], std::filesystem::path(manifestPath).parent_path().string());
}

TileSet TileSet::load(const Manifest& manifest, const std::string& baseDir) {
	if(!manifest.valid()) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: manifest needs a texture uri and at least one tile" << std::endl;

		return {};
	}

	const auto path = (std::filesystem::path(baseDir) / manifest.uri).string();
	auto image = osgDB::readRefImageFile(path);

	if(!image) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: failed to load " << path << std::endl;

		return {};
	}

	if(
		manifest.sdfType == ::osgx::SDF::SDFType::MSDF &&
		osg::Image::computeNumComponents(image->getPixelFormat()) < 3
	) {
		OSG_WARN << "osgx::gltf::sdf::TileSet::load: " << path << " is declared MSDF but has fewer than 3 channels" << std::endl;

		return {};
	}

	TileSet set;

	set._sdfType = manifest.sdfType;
	set._imageSize = osg::Vec2(static_cast<float>(image->s()), static_cast<float>(image->t()));

	for(const auto& [name, tile] : manifest.tiles) {
		if(tile.x < 0 || tile.y < 0 || tile.x + tile.w > image->s() || tile.y + tile.h > image->t()) {
			OSG_WARN << "osgx::gltf::sdf::TileSet::load: skipping tile \"" << name << "\" - its rect lies outside " << path << std::endl;

			continue;
		}

		set._tiles[name] = tile;
	}

	if(set._tiles.empty()) return {};

	set._texture = ::osgx::SDF::makeTexture(image);

	return set;
}

std::vector<std::string> TileSet::names() const {
	std::vector<std::string> result;

	result.reserve(_tiles.size());

	for(const auto& [name, tile] : _tiles) result.push_back(name);

	return result;
}

osg::Vec4 TileSet::uvRect(const std::string& name) const {
	const Tile& t = _tiles.at(name);

	// rect is measured from the TOP-left of the stored image; OSG flips images on load, so texture
	// space has V = 0 at the BOTTOM.
	return osg::Vec4(
		static_cast<float>(t.x) / _imageSize.x(),
		1.0f - static_cast<float>(t.y + t.h) / _imageSize.y(),
		static_cast<float>(t.x + t.w) / _imageSize.x(),
		1.0f - static_cast<float>(t.y) / _imageSize.y()
	);
}

osg::ref_ptr<::osgx::SDF> TileSet::attribute(const std::string& name) const {
	const auto it = _tiles.find(name);

	if(it == _tiles.end()) return nullptr;

	osg::ref_ptr<::osgx::SDF> attribute = new ::osgx::SDF();

	attribute->setTexture(_texture.get());
	attribute->setSDFType(_sdfType);
	attribute->setPixelRange(it->second.pixelRange);
	attribute->setUVRect(uvRect(name));

	return attribute;
}

}
