#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace osgx::gltf::detail {

// Plain "read this path, hand back owned bytes" fs callbacks for tg3_parse_options.fs --
// shared by ReaderImpl.cpp (via osgx::gltf::Reader) and osgx-gltf.cpp's inspect()/inspect_json()
// bindings, both of which need a real fs.read_file (tg3_parse_file has no fallback beyond
// TINYGLTF3_ENABLE_STB_IMAGE-style opt-in macros this project doesn't define). tg3_parse_file
// and tg3__load_external_file (tiny_gltf_v3.c) already resolve/concatenate base_dir + URI
// themselves before calling this, so it only needs to read whatever path it's given.
inline int32_t tg3_read_file(
	uint8_t** outData,
	uint64_t* outSize,
	const char* path,
	uint32_t pathLen,
	void*
) {
	std::ifstream file(std::string(path, pathLen), std::ios::binary | std::ios::ate);

	if(!file) return 0;

	std::streamoff size = file.tellg();

	if(size < 0) return 0;

	file.seekg(0, std::ios::beg);

	auto* data = new uint8_t[static_cast<std::size_t>(size)];

	if(!file.read(reinterpret_cast<char*>(data), size)) {
		delete[] data;

		return 0;
	}

	*outData = data;
	*outSize = static_cast<uint64_t>(size);

	return 1;
}

inline void tg3_free_file(uint8_t* data, uint64_t, void*) {
	delete[] data;
}

// tg3_str -> owned std::string. Only needed where data must outlive the model's arena
// (osg node names, texture cache keys) - everywhere else, read tg3_str fields directly.
inline std::string tg3_to_string(tg3_str s) {
	return s.data ? std::string(s.data, s.len) : std::string();
}

inline bool tg3_starts_with(tg3_str s, const char* prefix) {
	std::size_t prefixLen = std::strlen(prefix);

	return s.data && s.len >= prefixLen && std::memcmp(s.data, prefix, prefixLen) == 0;
}

// Parses the trailing digits of a "TEXCOORD_N" attribute name. Returns -1 if attrName
// doesn't start with "TEXCOORD_".
inline int tg3_texcoord_suffix(tg3_str attrName) {
	const char* prefix = "TEXCOORD_";
	std::size_t prefixLen = std::strlen(prefix);

	if(!tg3_starts_with(attrName, prefix)) return -1;

	int value = 0;

	for(std::size_t i = prefixLen; i < attrName.len; i++) {
		char c = attrName.data[i];

		if(c < '0' || c > '9') break;

		value = value * 10 + (c - '0');
	}

	return value;
}

// v3 ships no Value::Has/Get-style accessor - tg3_value/tg3_extension/tg3_kv_pair
// (Section 9 of tiny_gltf_v3.h) are raw structs only. These are the handful of tree-walk
// operations osgx actually needs (KHR_materials_pbrSpecularGlossiness in Material.cpp and
// the osgx_pbribl manifest decode in PBRIBL.cpp - though PBRIBL.cpp's manifest was never
// real glTF data and uses tinygltf_json_c.h's tg3json_value directly instead, a different
// type from tg3_value; do not conflate the two).
inline const tg3_extension* tg3_find_extension(const tg3_extras_ext& ext, const char* name) {
	for(std::uint32_t i = 0; i < ext.extensions_count; i++) {
		if(tg3_str_equals_cstr(ext.extensions[i].name, name)) return &ext.extensions[i];
	}

	return nullptr;
}

inline const tg3_value* tg3_object_get(const tg3_value& value, const char* key) {
	if(value.type != TG3_VALUE_OBJECT) return nullptr;

	for(std::uint32_t i = 0; i < value.object_count; i++) {
		if(tg3_str_equals_cstr(value.object_data[i].key, key)) return &value.object_data[i].value;
	}

	return nullptr;
}

inline bool tg3_as_double(const tg3_value& value, double& out) {
	switch(value.type) {
		case TG3_VALUE_REAL: out = value.real_val; return true;
		case TG3_VALUE_INT: out = static_cast<double>(value.int_val); return true;
		default: return false;
	}
}

inline bool tg3_as_string(const tg3_value& value, tg3_str& out) {
	if(value.type != TG3_VALUE_STRING) return false;

	out = value.string_val;

	return true;
}

// Only needed for MaterialBuilder's "primitive has no material index" fallback - every
// *parsed* material already gets these defaults from the parser itself (tg3__init_pbr /
// tg3__init_normal_texture_info / tg3__init_occlusion_texture_info / tg3__parse_material
// in tiny_gltf_v3.c), which this mirrors exactly.
inline const tg3_material& tg3_default_material() {
	static const tg3_material material = [] {
		tg3_material m{};

		m.alpha_mode = tg3_str{"OPAQUE", 6};
		m.alpha_cutoff = 0.5;

		m.pbr_metallic_roughness.base_color_factor[0] = 1.0;
		m.pbr_metallic_roughness.base_color_factor[1] = 1.0;
		m.pbr_metallic_roughness.base_color_factor[2] = 1.0;
		m.pbr_metallic_roughness.base_color_factor[3] = 1.0;
		m.pbr_metallic_roughness.metallic_factor = 1.0;
		m.pbr_metallic_roughness.roughness_factor = 1.0;
		m.pbr_metallic_roughness.base_color_texture.index = -1;
		m.pbr_metallic_roughness.metallic_roughness_texture.index = -1;

		m.normal_texture.index = -1;
		m.normal_texture.scale = 1.0;

		m.occlusion_texture.index = -1;
		m.occlusion_texture.strength = 1.0;

		m.emissive_texture.index = -1;

		return m;
	}();

	return material;
}

}
