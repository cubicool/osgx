#pragma once

// Private (never installed) helpers over tinygltf's raw JSON tree (tinygltf_json_c.h), shared by
// the manifest decoders in this directory (osgx_environment, osgx_sdf).
//
// Every lookup guards for a null/wrong-typed value first - a missing/malformed field decodes to a
// default, it never crashes. This is a different value type from a real glTF asset's own embedded
// extensions (tg3_value, see tg3_util.hpp), so nothing here decodes an extension block embedded in
// a real asset yet - only a standalone manifest document.

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

// Declarations only - tiny_gltf_v3.c already compiles the implementation
// (TINYGLTF_JSON_C_IMPLEMENTATION) once into osgx_gltf.
#include "tinygltf_json_c.h"

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <fstream>
#include <string>

namespace osgx::gltf::detail {

inline std::string decodeString(const tg3json_value* value, const char* key) {
	if(!value || value->type != TG3JSON_OBJECT) return {};

	const tg3json_value* found = tg3json_object_get(value, key);

	return found && found->type == TG3JSON_STRING
		? std::string(found->u.string.ptr, found->u.string.len)
		: std::string()
	;
}

inline int decodeInt(const tg3json_value* value, const char* key, int fallback) {
	if(!value || value->type != TG3JSON_OBJECT) return fallback;

	const tg3json_value* found = tg3json_object_get(value, key);

	if(!found) return fallback;
	if(found->type == TG3JSON_INT) return static_cast<int>(found->u.integer);
	if(found->type == TG3JSON_REAL) return static_cast<int>(found->u.real);

	return fallback;
}

// A JSON number (integer or real) as a double; false for anything else.
inline bool asNumber(const tg3json_value* value, double& out) {
	if(!value) return false;

	if(value->type == TG3JSON_INT) {
		out = static_cast<double>(value->u.integer);

		return true;
	}

	if(value->type == TG3JSON_REAL) {
		out = value->u.real;

		return true;
	}

	return false;
}

inline double decodeReal(const tg3json_value* value, const char* key, double fallback) {
	if(!value || value->type != TG3JSON_OBJECT) return fallback;

	double result = fallback;

	return asNumber(tg3json_object_get(value, key), result) ? result : fallback;
}

// Owns a tg3json_value tree parsed by tg3json_parse_n(). tg3json_value_free() must never run
// twice on the same value (it doesn't reset itself after freeing, unlike tg3_model_free()'s
// whole-arena semantics) - parse() itself already frees on failure internally, so _owned only
// tracks the success case to avoid a double-free on that path.
class JsonDocument {
public:
	JsonDocument() { tg3json_value_init_null(&_value); }
	~JsonDocument() { if(_owned) tg3json_value_free(&_value); }

	JsonDocument(const JsonDocument&) = delete;
	JsonDocument& operator=(const JsonDocument&) = delete;

	bool parse(const std::string& text) {
		const char* errorPos = nullptr;

		_owned = tg3json_parse_n(text.data(), text.size(), 0, &_value, &errorPos) != 0;

		return _owned;
	}

	const tg3json_value& root() const { return _value; }

private:
	tg3json_value _value;
	bool _owned = false;
};

inline bool readWholeFile(const std::string& path, std::string& out) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);

	if(!file) return false;

	std::streamoff size = file.tellg();

	if(size < 0) return false;

	file.seekg(0, std::ios::beg);
	out.resize(static_cast<std::size_t>(size));

	return static_cast<bool>(file.read(out.data(), size));
}

}
