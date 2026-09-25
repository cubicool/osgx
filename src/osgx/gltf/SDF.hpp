#pragma once

#include "osgx/SDF.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Texture2D>
#include <osg/Vec2>
#include <osg/Vec4>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace osgx::gltf::sdf {

// ================================================================================================
// `osgx_sdf` - baked distance-field tiles as a glTF extension
//
// The loader half of osgx's SDF support: it only ever CONSUMES baked SDF/MSDF data (osgx never
// generates it) and knows nothing about whoever produced it - slughorn's `bin/slughorn sdf` writes
// exactly this schema, but so could an msdfgen/msdf-atlas-gen post-process. Like
// `osgx_environment`, the same schema is meant to work both as a STANDALONE manifest (a glTF-shaped
// file with no real scene data - what load() reads today) and embedded in a real asset (not
// decoded yet; see Json.hpp).
//
//   {
//     "asset": {"version": "2.0", "generator": "slughorn"},
//     "extensionsUsed": ["osgx_sdf"],
//     "extensions": {
//       "osgx_sdf": {
//         "data": [{
//           "type": "MSDF",                          // "SDF" (read .r) | "MSDF" (median of .rgb)
//           "texture": {"uri": "atlas.png"},         // relative to the manifest
//           "tiles": {
//             "circle": {
//               "rect": [100, 10, 96, 88],           // [x, y, width, height] in PIXELS, origin TOP-left
//               "pixelRange": 16.0,                     // total distance range in TEXELS (msdfgen -pxrange)
//               "range": 0.1,                        // optional, from here down: the shape's em-space frame,
//               "texelsPerEm": 80.0,                 //   for effects that work in shape units; a generic
//               "emOrigin": [-0.1, -0.1]             //   renderer ignores them
//             }
//           }
//         }]
//       }
//     }
//   }
//
// - `data` is an array (like `osgx_environment`'s `environments`), so an asset can carry an SDF
//   sheet and an MSDF sheet together.
// - `rect` follows glTF's own convention - origin at the TOP-left of the image as it is stored/
//   displayed, y growing downward - and the image must be stored UPRIGHT (content as displayed).
//   The loader turns it into OSG's texture space (V = 0 at the bottom, since OSG flips images on
//   load); a tile's uvRect() is therefore never something the file has to get right by hand.
// - `texture` is `{"uri": ...}` today. `{"index": n}` (a glTF `textures[]` entry) is reserved for
//   embedding in a real asset and is NOT implemented yet.
// - Required to render: `type`, `texture`, and per tile `rect` and `pixelRange`.
// ================================================================================================

// One tile as declared in the manifest. Pure data.
struct Tile {
	int x = 0, y = 0, w = 0, h = 0; // pixels, origin TOP-left
	float pixelRange = 0.0f;

	// The optional em-space frame (all zero unless hasEmFrame).
	bool hasEmFrame = false;
	float range = 0.0f;
	float texelsPerEm = 0.0f;
	osg::Vec2 emOrigin;
};

// One `data[]` entry decoded from an `osgx_sdf` extension block. Pure data: no textures, no I/O.
// `uri` is exactly what the manifest declared - relative to the manifest.
struct Manifest {
	::osgx::SDF::SDFType sdfType = ::osgx::SDF::SDFType::SDF;
	std::string uri;
	std::map<std::string, Tile> tiles;

	bool valid() const { return !uri.empty() && !tiles.empty(); }
};

// A loaded `data[]` entry: ONE shared distance-field texture plus its named tiles. attribute() hands
// out an osgx::SDF per tile (all sharing the texture), which is what you attach to a StateSet.
//
// A default-constructed TileSet is invalid; load() returns one (after an OSG_WARN) on any failure -
// a missing file, no `osgx_sdf` block, an unreadable image, an MSDF entry whose image has fewer
// than three channels, or no usable tile. Individual bad tiles (malformed, or a rect outside the
// image) are skipped with a warning rather than failing the whole set.
class TileSet {
public:
	// Loads the `index`th `data[]` entry of the `osgx_sdf` extension in the manifest at
	// `manifestPath`; its image is resolved relative to that file.
	static TileSet load(const std::string& manifestPath, std::size_t index=0);
	static TileSet load(const Manifest& manifest, const std::string& baseDir);

	bool valid() const { return _texture.valid(); }

	::osgx::SDF::SDFType sdfType() const { return _sdfType; }
	osg::Texture2D* texture() const { return _texture.get(); }

	// Tile names in sorted order.
	std::vector<std::string> names() const;
	bool has(const std::string& name) const { return _tiles.count(name) > 0; }

	// Throws std::out_of_range for an unknown name.
	const Tile& tile(const std::string& name) const { return _tiles.at(name); }

	// The tile's rect in OSG texture space, (u0, v0, u1, v1) with V = 0 at the bottom - exactly what
	// osgx::SDF::setUVRect() takes. Throws std::out_of_range for an unknown name.
	osg::Vec4 uvRect(const std::string& name) const;

	// A ready-to-attach osgx::SDF for one tile (shared texture, sdfType, pixelRange, uvRect); null for an
	// unknown name.
	osg::ref_ptr<::osgx::SDF> attribute(const std::string& name) const;

private:
	osg::ref_ptr<osg::Texture2D> _texture;
	::osgx::SDF::SDFType _sdfType = ::osgx::SDF::SDFType::SDF;
	osg::Vec2 _imageSize;
	std::map<std::string, Tile> _tiles;
};

}
