#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osgDB/ReaderWriter>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace osgx::gltf {

class Reader {
public:
	// Two phases for read(): Parsing (tinygltf's own single-pass parse of the glTF document,
	// including image decode - v3 has no separable "loading textures" phase, it happens
	// inline) and BuildingNodes (this plugin's own post-parse walk that converts the parsed
	// model into an osg::Node graph).
	enum class Stage { Parsing, BuildingNodes };

	static constexpr std::string_view stageName(Stage stage) {
		switch(stage) {
			case Stage::Parsing: return "parsing";
			case Stage::BuildingNodes: return "building_nodes";
		}

		return "";
	}

	// During Parsing, current/total are a real (never fabricated) item index/count within
	// `section` - tinygltf v3's per-section stream callbacks (on_mesh/on_node/on_image/etc)
	// fire once per item, only after that section's array is fully parsed, so `total` is read
	// directly off the already-built model rather than pre-counted. During BuildingNodes,
	// current/total are nodes built so far / model.nodes_count (also real, free denominators),
	// and `section` is empty.
	//
	// `overall` is a second, complementary facet of the same tick: a monotonic 0.0-1.0 estimate
	// of progress across the WHOLE load, always populated alongside current/total/section rather
	// than gated behind a separate mode - callers pick whichever facet suits them per tick. See
	// computeOverall()'s definition for why it's a predictable fixed-weight estimate rather than
	// an item-accurate one (that would need a pre-scan tinygltf v3's single-pass parser doesn't
	// do).
	struct Progress {
		Stage            stage;
		std::uint64_t    current;
		std::uint64_t    total;
		std::string_view section;
		double           overall;
	};

	using ProgressCallback = std::function<void(const Progress&)>;

	// Computes Progress::overall for a single tick. Exposed here (rather than kept file-local to
	// Reader.cpp) because both the Parsing ticks (ReaderImpl.cpp) and the BuildingNodes ticks
	// (Scene.cpp) need it, and they live in different translation units.
	static double computeOverall(
		Stage stage,
		std::uint64_t current,
		std::uint64_t total,
		std::string_view section
	);

	// Shared, thread-safe texture cache. The cache must outlive every Reader that refers to it.
	class TextureCache {
	public:
		TextureCache();
		~TextureCache();

		TextureCache(const TextureCache&) = delete;
		TextureCache& operator=(const TextureCache&) = delete;
		TextureCache(TextureCache&&) noexcept;
		TextureCache& operator=(TextureCache&&) noexcept;

	private:
		struct Implementation;
		std::unique_ptr<Implementation> _implementation;

		friend class Reader;
	};

	void setTextureCache(TextureCache* cache) const { _textureCache = cache; }

	osgDB::ReaderWriter::ReadResult read(
		const std::string& location,
		bool isBinary,
		const osgDB::Options* options,
		const ProgressCallback& progress = nullptr
	) const;

private:
	mutable TextureCache* _textureCache = nullptr;
};

}
