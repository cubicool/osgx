#include "ReaderImpl.hpp"
#include "Texture.hpp"

#include "osgx/gltf/Reader.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace {

// Order tinygltf v3's single-pass parser visits these sections in (see the fixed sequence of
// TG3__STREAM_CB calls in tiny_gltf_v3.c) - fixed by the parser's own source, not by the glTF
// file's JSON key order. Each section gets an equal share of the Parsing stage's weight, credited
// by POSITION rather than by item count: reaching section N's first tick implies sections 0..N-1
// are already done, whether or not they had any items to tick. That's the only way an empty
// section (e.g. no skins) ever gets its share credited - it never fires a callback of its own.
constexpr std::string_view kParsingSectionOrder[] = {
	"meshes", "nodes", "materials", "textures", "images", "skins", "animations"
};

constexpr std::size_t kParsingSectionCount =
	sizeof(kParsingSectionOrder) / sizeof(kParsingSectionOrder[0]);

// Parsing (image decode included) is typically the slower half of a load; BuildingNodes is a
// comparatively quick tree walk over an already-parsed model. Fixed, not measured per-model --
// "predictable" was the explicit design goal, not item-count-accurate (which isn't knowable up
// front from a single-pass parser without reintroducing the pre-scan this plugin deliberately
// dropped).
constexpr double kParsingWeight = 0.8;
constexpr double kBuildingNodesWeight = 1.0 - kParsingWeight;

std::size_t sectionPosition(std::string_view section) {
	for(std::size_t i = 0; i < kParsingSectionCount; i++) {
		if(kParsingSectionOrder[i] == section) return i;
	}

	return 0;
}

}

namespace osgx::gltf {

struct Reader::TextureCache::Implementation {
	detail::TextureCache cache;
};

Reader::TextureCache::TextureCache():
_implementation(std::make_unique<Implementation>()) {}

Reader::TextureCache::~TextureCache() = default;
Reader::TextureCache::TextureCache(TextureCache&&) noexcept = default;
Reader::TextureCache& Reader::TextureCache::operator=(TextureCache&&) noexcept = default;

// BuildingNodes only ever starts once Parsing has fully finished, so Parsing's whole weight is
// credited unconditionally as soon as we're in that stage - current/total there IS a real,
// exact node count (unlike Parsing's per-section totals), so this reaches exactly 1.0 on the
// final node, regardless of how rough the Parsing-side estimate below was.
double Reader::computeOverall(
	Stage stage,
	std::uint64_t current,
	std::uint64_t total,
	std::string_view section
) {
	double fill = total ? static_cast<double>(current) / static_cast<double>(total) : 0.0;

	if(stage == Stage::Parsing) {
		double position = static_cast<double>(sectionPosition(section));

		return kParsingWeight * (position + fill) / static_cast<double>(kParsingSectionCount);
	}

	return kParsingWeight + kBuildingNodesWeight * fill;
}

osgDB::ReaderWriter::ReadResult Reader::read(
	const std::string& location,
	bool isBinary,
	const osgDB::Options* options,
	const ProgressCallback& progress
) const {
	detail::ReaderImpl reader;

	if(_textureCache) reader.setTextureCache(&_textureCache->_implementation->cache);

	return reader.read(location, isBinary, options, progress);
}

}
