#include "LibraryState.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace osgx {

namespace {

bool shaderLibCatalogNameMatches(std::string_view lhs, std::string_view rhs) {
	return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](
		const char a, const char b
	) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
	return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(), [](
		const char a, const char b
	) {
		return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
	});
}

std::string_view trimShaderText(std::string_view text) {
	while(!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
	while(!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
	return text;
}

std::string normalizeShaderLibName(std::string_view name) {
	name = trimShaderText(name);
	if(name.starts_with("osgx_")) name.remove_prefix(5);
	std::string result;
	for(const char c : name) {
		if(std::isalnum(static_cast<unsigned char>(c))) result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return result;
}

bool shaderLibNameMatches(std::string_view requested, const ShaderLib& lib) {
	const auto normalized = normalizeShaderLibName(requested);
	return normalized == normalizeShaderLibName(lib.name) || normalized == normalizeShaderLibName(lib.glslName);
}

std::vector<std::string_view> splitShaderLibNames(std::string_view names) {
	if(const auto comment = names.find("//"); comment != std::string_view::npos) names = names.substr(0, comment);
	std::vector<std::string_view> result;
	for(size_t pos = 0; pos <= names.size();) {
		const auto comma = names.find(',', pos);
		const auto end = comma == std::string_view::npos ? names.size() : comma;
		if(const auto name = trimShaderText(names.substr(pos, end - pos)); !name.empty()) result.push_back(name);
		if(comma == std::string_view::npos) break;
		pos = comma + 1;
	}
	return result;
}

}

void registerShaderLibs(std::string_view namespaceName, std::span<const ShaderLib> libs) {
	if(namespaceName.empty() || libs.empty()) throw std::runtime_error("osgx::registerShaderLibs: namespace and catalog must not be empty");
	auto& catalogs = detail::libraryState().shaderLibCatalogs;
	auto catalog = std::find_if(catalogs.begin(), catalogs.end(), [namespaceName](const auto& candidate) {
		return shaderLibCatalogNameMatches(namespaceName, candidate.namespaceName);
	});
	if(catalog == catalogs.end()) catalog = catalogs.insert(catalogs.end(), {std::string(namespaceName), {}});
	for(const auto& lib : libs) {
		auto existing = std::find_if(catalog->libs.begin(), catalog->libs.end(), [&lib](const auto& candidate) {
			return shaderLibCatalogNameMatches(lib.name, candidate.name);
		});
		if(existing == catalog->libs.end()) catalog->libs.push_back(lib);
		else if(existing->name != lib.name || existing->glslName != lib.glslName || existing->source != lib.source) {
			throw std::runtime_error("osgx::registerShaderLibs: conflicting library '" + std::string(lib.name) + "'");
		}
	}
}

osg::Shader* cachedShader(osg::Shader::Type type, std::string src) {
	auto& cache = detail::libraryState().shaderCache;
	auto key = std::make_pair(type, std::move(src));
	const auto found = cache.find(key);

	if(found != cache.end()) return found->second.get();

	auto* shader = new osg::Shader(type, key.second);

	return cache.emplace(std::move(key), shader).first->second.get();
}

namespace {

// "<programName>.<hookName>Hook" is applyHooks()'s own half of the "<programName>.<role>" shader-
// naming convention every osgx Program-builder follows (see PBRScene.cpp/IBL.cpp's plain
// setName() calls for the other half, on shaders that aren't hook slots) - so a shader shows up
// meaningfully in introspection/debug UIs (e.g. pyside6-glsl.py's tree/tab view) instead of a
// bare "VERTEX"/"FRAGMENT". Kept here, next to the enum, so a new Hook value can't be added
// without this switch failing to compile silently-wrong (no default: case).
std::string hookName(Hook hook) {
	switch(hook) {
		case Hook::Tonemap: return "tonemapHook";
		case Hook::Skinning: return "skinningHook";
		case Hook::DeferredLighting: return "deferredLightingHook";
		case Hook::DirectLighting: return "directLightingHook";
	}

	return "hook";
}

}

bool hasHook(const HookList& hooks, Hook hook) {
	return std::any_of(hooks.begin(), hooks.end(), [hook](const auto& entry) { return entry.first == hook; });
}

void applyHooks(osg::Program* program, const HookList& hooks, const HookList& defaults) {
	for(const auto& [hook, defaultShader] : defaults) {
		const auto found = std::find_if(hooks.begin(), hooks.end(), [hook](const auto& entry) {
			return entry.first == hook;
		});

		osg::Shader* chosen = (found != hooks.end() ? found->second : defaultShader).get();

		// Only when unnamed - never clobber a name a caller deliberately gave their own
		// substituted (hooks-supplied) shader.
		if(chosen->getName().empty()) chosen->setName(program->getName() + "." + hookName(hook));

		program->addShader(chosen);
	}

	for(const auto& [hook, shader] : hooks) {
		if(!hasHook(defaults, hook)) {
			throw std::runtime_error(
				"osgx::applyHooks: hook " + std::to_string(static_cast<int>(hook)) +
				" is not supported by this Program"
			);
		}
	}
}

namespace {

// Expands `src`, then (recursively) each library it pulls in. `expanding` holds the
// "<namespace> <lib>" keys of the libraries currently being expanded, so a library that pulls
// itself in (directly or through others) throws instead of recursing forever.
std::string expandShaderLibs(
	std::string_view src,
	const std::vector<detail::ShaderLibCatalog>& catalogs,
	std::vector<std::string>& expanding
) {
	// Only pragmas whose namespace matches a registered osgx catalog are expanded.
	// Other pragmas are intentionally preserved. In particular, this lets callers
	// compose snippet expansion with OSG's state-driven shader variants, such as
	// #pragma import_defines(...), #pragma import_modes(...), and #pragma requires(...).
	static constexpr char CARRIAGE_RETURN = 13;
	static constexpr char LINE_FEED = 10;
	static constexpr char LINE_ENDINGS[] = {CARRIAGE_RETURN, LINE_FEED, 0};
	static constexpr char SHADER_WHITESPACE[] = {' ', 9, CARRIAGE_RETURN, LINE_FEED, 0};

	std::string resolved;
	resolved.reserve(src.size());
	for(size_t pos = 0; pos < src.size();) {
		const auto lineEnd = src.find_first_of(LINE_ENDINGS, pos);
		const auto lineSize = lineEnd == std::string::npos ? src.size() - pos : lineEnd - pos;
		const auto line = std::string_view(src.data() + pos, lineSize);
		auto rest = trimShaderText(line);
		std::optional<std::string> replacement;
		if(startsWithIgnoringCase(rest, "#pragma")) {
			rest = trimShaderText(rest.substr(7));
			const auto nsEnd = rest.find_first_of(SHADER_WHITESPACE);
			const auto namespaceName = rest.substr(0, nsEnd);
			const auto names = nsEnd == std::string_view::npos ? std::string_view{} : rest.substr(nsEnd);
			for(const auto& catalog : catalogs) {
				if(!shaderLibCatalogNameMatches(namespaceName, catalog.namespaceName)) continue;
				const auto requested = splitShaderLibNames(names);
				if(requested.empty()) throw std::runtime_error("osgx::resolveShaderLibs: empty #pragma " + std::string(namespaceName));
				const bool all = requested.size() == 1 && requested.front() == "*";
				std::string expanded;
				for(const auto& lib : catalog.libs) {
					if(!all && !std::any_of(requested.begin(), requested.end(), [&lib](auto name) { return shaderLibNameMatches(name, lib); })) continue;
					auto key = catalog.namespaceName + " " + std::string(lib.name);
					if(std::find(expanding.begin(), expanding.end(), key) != expanding.end()) throw std::runtime_error("osgx::resolveShaderLibs: #pragma " + key + " pulls itself in");
					expanding.push_back(std::move(key));
					expanded += expandShaderLibs(lib.source, catalogs, expanding);
					expanding.pop_back();
				}
				if(!all) for(const auto name : requested) if(!std::any_of(catalog.libs.begin(), catalog.libs.end(), [name](const auto& lib) { return shaderLibNameMatches(name, lib); })) throw std::runtime_error("osgx::resolveShaderLibs: unknown #pragma " + std::string(namespaceName) + " lib '" + std::string(name) + "'");
				replacement = std::move(expanded);
				break;
			}
		}
		if(replacement) resolved += *replacement; else resolved.append(line);
		if(lineEnd == std::string::npos) break;
		if(src[lineEnd] == CARRIAGE_RETURN && lineEnd + 1 < src.size() && src[lineEnd + 1] == LINE_FEED) {
			resolved += CARRIAGE_RETURN;
			resolved += LINE_FEED;
			pos = lineEnd + 2;
		}
		else { resolved += src[lineEnd]; pos = lineEnd + 1; }
	}
	return resolved;
}

}

namespace {

bool isBindingNameChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.';
}

// Replaces every `@<name>@` token (name characters: alphanumerics, '_', ':', '.') with that
// binding's index. An '@' not starting such a token is copied unchanged.
std::string substituteBindings(const std::string& src, Bindings& bindings) {
	std::string out;

	out.reserve(src.size());

	for(size_t pos = 0; pos < src.size();) {
		const auto open = src.find('@', pos);

		if(open == std::string::npos) {
			out.append(src, pos, std::string::npos);

			break;
		}

		out.append(src, pos, open - pos);

		auto close = open + 1;

		while(close < src.size() && isBindingNameChar(src[close])) close++;

		if(close == open + 1 || close >= src.size() || src[close] != '@') {
			out += '@';
			pos = open + 1;

			continue;
		}

		out += std::to_string(bindings.get(std::string_view(src).substr(open + 1, close - open - 1)));
		pos = close + 1;
	}

	return out;
}

}

std::string resolveShaderLibs(std::string src) {
	auto& state = detail::libraryState();

	std::vector<std::string> expanding;

	return substituteBindings(expandShaderLibs(src, state.shaderLibCatalogs, expanding), state.bindings);
}

}
