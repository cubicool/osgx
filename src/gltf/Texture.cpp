#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Texture.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>

#include <osgDB/ConvertBase64>
#include <osgDB/FileNameUtils>
#include <osgDB/ReadFile>
#include <osgDB/Registry>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GL_SRGB8
# define GL_SRGB8 0x8C41
#endif
#ifndef GL_SRGB8_ALPHA8
# define GL_SRGB8_ALPHA8 0x8C43
#endif

namespace osgx::gltf::detail {
namespace {

// OSG's own image ReaderWriter plugins are this plugin's one and only image-decode path, by
// deliberate choice - osgDB::readImageFile() already uses them for external files, and this
// self-decodes embedded (bufferView-referenced) and base64 data-URI images the same way rather
// than depending on tinygltf's own decoder. (tinygltf v3.0.1's decoder is unimplemented anyway --
// confirmed by reading tg3__parse_image in tiny_gltf_v3.c, which never calls any decode callback
// and never populates tg3_image.image - but the choice to route everything through OSG stands
// regardless of whether/when a future tinygltf release wires one up.)

struct DataUriPayload {
	std::string mimeType;
	const char* data = nullptr;
	std::size_t length = 0;
};

// Mirrors tinygltf v3's own (non-exported) data URI convention: "data:<mime>;base64,<data>".
bool parseDataUri(const std::string& uri, DataUriPayload& out) {
	const std::string prefix = "data:";

	if(uri.compare(0, prefix.size(), prefix) != 0) return false;

	std::size_t semicolon = uri.find(';', prefix.size());

	if(semicolon == std::string::npos) return false;

	std::size_t comma = uri.find(',', semicolon);

	if(comma == std::string::npos) return false;

	if(uri.compare(semicolon + 1, comma - semicolon - 1, "base64") != 0) return false;

	out.mimeType = uri.substr(prefix.size(), semicolon - prefix.size());
	out.data = uri.data() + comma + 1;
	out.length = uri.size() - comma - 1;

	return true;
}

std::string extensionForMimeType(const std::string& mimeType) {
	if(mimeType == "image/jpeg") return "jpg";
	if(mimeType == "image/png") return "png";

	// Falls back to the substring after the last '/', close enough for osgDB::Registry to
	// pick the right plugin by extension for less common MIME types (image/webp, image/ktx2).
	std::size_t slash = mimeType.rfind('/');

	return slash == std::string::npos ? std::string() : mimeType.substr(slash + 1);
}

// Opt-in (not the default): stripping walks and copies every PNG's compressed bytes, and most
// assets don't carry gAMA/cHRM/sRGB/iCCP chunks in the first place. Enable only for sources known
// to ship them (e.g. the Khronos TextureEncodingTest emissive assets this was added for).
bool wantsPNGColorMetadataStripping(const osgDB::Options* readOptions) {
	return
		readOptions &&
		readOptions->getOptionString().find("gltfStripPNGColorMetadata") != std::string::npos
	;
}

// glTF defines each texture slot's color space itself, so PNG gAMA/cHRM/sRGB/iCCP chunks must
// not affect decoding. OSG's PNG plugin applies gAMA unconditionally; remove these metadata
// chunks before passing the original compressed pixels to that plugin. Chunk payloads and CRCs
// are copied verbatim, so this is not a PNG re-encode.
bool stripPNGColorMetadata(
	const unsigned char* bytes,
	std::size_t size,
	std::vector<unsigned char>& output
) {
	static constexpr unsigned char signature[] = {
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
	};

	if(size < sizeof(signature) || !std::equal(
		std::begin(signature), std::end(signature), bytes
	)) return false;

	auto isColorMetadata = [](const unsigned char* type) {
		return
			std::equal(type, type + 4, "gAMA") ||
			std::equal(type, type + 4, "cHRM") ||
			std::equal(type, type + 4, "sRGB") ||
			std::equal(type, type + 4, "iCCP")
		;
	};

	output.clear();
	output.insert(output.end(), bytes, bytes + sizeof(signature));

	for(std::size_t offset = sizeof(signature); offset < size;) {
		if(size - offset < 12) return false;

		const unsigned char* chunk = bytes + offset;
		const std::size_t length =
			(static_cast<std::size_t>(chunk[0]) << 24) |
			(static_cast<std::size_t>(chunk[1]) << 16) |
			(static_cast<std::size_t>(chunk[2]) << 8) |
			static_cast<std::size_t>(chunk[3])
		;
		const std::size_t chunkSize = length + 12;

		if(chunkSize < length || chunkSize > size - offset) return false;

		if(!isColorMetadata(chunk + 4)) output.insert(
			output.end(), chunk, chunk + chunkSize
		);

		offset += chunkSize;

		if(std::equal(chunk + 4, chunk + 8, "IEND")) return offset == size;
	}

	return false;
}

osg::Image* decodeCompressedImage(
	const unsigned char* bytes,
	std::size_t size,
	const std::string& extension,
	const osgDB::Options* readOptions
) {
	if(extension.empty()) return nullptr;

	std::vector<unsigned char> pngWithoutColorMetadata;

	if(
		extension == "png" &&
		wantsPNGColorMetadataStripping(readOptions) &&
		stripPNGColorMetadata(bytes, size, pngWithoutColorMetadata)
	) {
		bytes = pngWithoutColorMetadata.data();
		size = pngWithoutColorMetadata.size();
	}

	osgDB::ReaderWriter* readerWriter =
		osgDB::Registry::instance()->getReaderWriterForExtension(extension);

	if(!readerWriter) return nullptr;

	std::string data(reinterpret_cast<const char*>(bytes), size);
	std::istringstream stream(data);
	osg::ref_ptr<osg::Image> image;

	{
		// ReadResult holds its own internal ref_ptr<osg::Object> alongside the local `image`
		// ref_ptr below - two independent owners of the same refcount. Scoped so ReadResult's
		// destructor (a normal unref(), WITH delete-on-zero) runs here, while `image` still
		// holds a live reference keeping the count above zero. Without this block, `image`'s
		// release() (unref_nodelete(), no delete) at the bottom leaves the object relying
		// entirely on ReadResult's own reference to stay alive - and ReadResult's destructor
		// then runs during this function's unwind, deleting the object out from under the raw
		// pointer just handed to the caller.
		osgDB::ReaderWriter::ReadResult result = readerWriter->readImage(stream, readOptions);

		if(!result.success()) return nullptr;

		image = result.getImage();
	}

	// glTF's UV convention puts v=0 at the TOP of the image, opposite OpenGL's native texture
	// row order - a fixed, spec-wide convention (not per-image), so every glTF-in-GL loader
	// flips unconditionally after decode. Mirrors what the external-file path (loadRawImage's
	// osgDB::readImageFile branch) already does; this is the same requirement for bytes that
	// happen to come from a bufferView or data URI instead of a standalone file.
	if(image) image->flipVertical();

	return image.release();
}

}

osg::Texture2D* TextureCache::find(const std::string& key) const {
	std::lock_guard<std::mutex> lock(_mutex);
	auto match = _textures.find(key);

	if(match == _textures.end()) return nullptr;

	return match->second;
}

void TextureCache::store(const std::string& key, osg::Texture2D* texture) {
	std::lock_guard<std::mutex> lock(_mutex);

	_textures[key] = texture;
}

TextureLoader::TextureLoader(
	const tg3_model& model,
	const std::string& referrer,
	const osgDB::Options* readOptions,
	TextureCache* cache
):
_model(model),
_referrer(referrer),
_readOptions(readOptions),
_cache(cache) {}

osg::Image* TextureLoader::loadRawImage(int textureIndex) const {
	if(textureIndex < 0 || static_cast<std::uint32_t>(textureIndex) >= _model.textures_count) {
		return nullptr;
	}

	const tg3_texture& texture = _model.textures[static_cast<std::uint32_t>(textureIndex)];

	if(texture.source < 0 || static_cast<std::uint32_t>(texture.source) >= _model.images_count) {
		return nullptr;
	}

	const tg3_image& source = _model.images[static_cast<std::uint32_t>(texture.source)];
	osg::ref_ptr<osg::Image> image;
	bool imageAlreadyFlipped = false;

	// Deliberately never consumes source.image (tinygltf's own decoded-pixel-data field), even
	// though tg3_image declares it. OSG's own image ReaderWriter plugins are the one and only
	// decode path here, on purpose (user preference, not just working around v3.0.1's decoder
	// being unimplemented) - if a future tinygltf release starts populating source.image, that
	// must NOT silently start being used in place of OSG's own decode.
	if(
		source.buffer_view >= 0 &&
		static_cast<std::uint32_t>(source.buffer_view) < _model.buffer_views_count
	) {
		const tg3_buffer_view& bufferView =
			_model.buffer_views[static_cast<std::uint32_t>(source.buffer_view)];

		if(bufferView.buffer >= 0 && static_cast<std::uint32_t>(bufferView.buffer) < _model.buffers_count) {
			const tg3_buffer& buffer = _model.buffers[static_cast<std::uint32_t>(bufferView.buffer)];

			if(bufferView.byte_offset + bufferView.byte_length <= buffer.data.count) {
				image = decodeCompressedImage(
					buffer.data.data + bufferView.byte_offset,
					static_cast<std::size_t>(bufferView.byte_length),
					extensionForMimeType(tg3_to_string(source.mime_type)),
					_readOptions
				);
			}
		}
	}
	else if(source.uri.len > 0 && tg3_is_data_uri(source.uri.data, source.uri.len)) {
		DataUriPayload payload;

		if(parseDataUri(tg3_to_string(source.uri), payload)) {
			std::istringstream encoded(std::string(payload.data, payload.length));
			std::ostringstream decoded;
			osgDB::Base64decoder decoder;

			decoder.decode(encoded, decoded);

			std::string bytes = decoded.str();

			image = decodeCompressedImage(
				reinterpret_cast<const unsigned char*>(bytes.data()),
				bytes.size(),
				extensionForMimeType(payload.mimeType),
				_readOptions
			);
		}
	}
	else if(source.uri.len > 0) {
		const std::string path = osgDB::concatPaths(
			osgDB::getFilePath(_referrer),
			tg3_to_string(source.uri)
		);

		// The manual read+decode below only exists to route PNG bytes through
		// stripPNGColorMetadata() before OSG's plugin sees them - skip it (and the file-size
		// copy it implies) entirely unless that stripping was actually requested; the plugin's
		// own osgDB::readImageFile() is both simpler and faster for the common case.
		if(
			osgDB::getLowerCaseFileExtension(path) == "png" &&
			wantsPNGColorMetadataStripping(_readOptions)
		) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);

			if(file) {
				std::string bytes(static_cast<std::size_t>(file.tellg()), '\0');

				file.seekg(0);
				file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));

				image = decodeCompressedImage(
					reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), "png", _readOptions
				);
				imageAlreadyFlipped = image.valid();

				// decodeCompressedImage() reads via the PNG plugin's istream overload, which
				// (unlike its filename overload - see ReaderWriterPNG::readImage(const
				// std::string&, ...)) never calls Image::setFileName() itself. Set it here, the
				// one call site in this function that actually has a real path, so this branch
				// stays indistinguishable from the osgDB::readImageFile() branch below to any
				// caller that inspects Image::getFileName() (cache keys, diagnostics, etc.).
				if(image) image->setFileName(path);
			}
		}

		else image = osgDB::readImageFile(path, _readOptions);

		if(image && !imageAlreadyFlipped) image->flipVertical();
	}

	return image.release();
}

void TextureLoader::applyFormatAndSampler(
	osg::Texture2D* texture,
	osg::Image* image,
	bool sRGB,
	int samplerIndex
) const {
	if(image->getPixelFormat() == GL_RGB) {
		image->setInternalTextureFormat(sRGB ? GL_SRGB8 : GL_RGB8);
	}
	if(image->getPixelFormat() == GL_RGBA) {
		image->setInternalTextureFormat(sRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8);
	}

	texture->setResizeNonPowerOfTwoHint(false);
	texture->setDataVariance(osg::Object::STATIC);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setMaxAnisotropy(16.0f);

	if(samplerIndex >= 0 && static_cast<std::uint32_t>(samplerIndex) < _model.samplers_count) {
		const tg3_sampler& sampler =
			_model.samplers[static_cast<std::uint32_t>(samplerIndex)];

		texture->setWrap(
			osg::Texture::WRAP_S,
			static_cast<osg::Texture::WrapMode>(sampler.wrap_s)
		);
		texture->setWrap(
			osg::Texture::WRAP_T,
			static_cast<osg::Texture::WrapMode>(sampler.wrap_t)
		);
	}
	else {
		texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
		texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	}
}

osg::Texture2D* TextureLoader::getOrCreateTexture(int textureIndex, bool sRGB) const {
	if(textureIndex < 0 || static_cast<std::uint32_t>(textureIndex) >= _model.textures_count) {
		return nullptr;
	}

	const tg3_texture& texture = _model.textures[static_cast<std::uint32_t>(textureIndex)];

	if(texture.source < 0 || static_cast<std::uint32_t>(texture.source) >= _model.images_count) {
		return nullptr;
	}

	const tg3_image& image = _model.images[static_cast<std::uint32_t>(texture.source)];
	const bool dataURI = image.uri.len > 0 && tg3_is_data_uri(image.uri.data, image.uri.len);
	const bool externalImage = image.uri.len > 0 && !dataURI;
	// Embedded (bufferView) and data-URI images are decoded fresh by loadRawImage() every
	// call (self-decoded via OSG's plugins, see the anonymous-namespace comment above) --
	// there's no shared/cached raw-byte identity to preserve, unlike an external file path,
	// so it's always safe to drop the CPU copy once the GPU upload has happened.
	const bool unrefImageDataAfterApply = dataURI || image.buffer_view >= 0;
	std::string cacheKey;

	if(externalImage) {
		cacheKey = osgDB::getRealPath(osgDB::concatPaths(
			osgDB::getFilePath(_referrer),
			tg3_to_string(image.uri)
		)) + (sRGB ? "|sRGB" : "|linear");
	}
	else cacheKey = _referrer + "|image:" + std::to_string(texture.source) +
		(sRGB ? "|sRGB" : "|linear");

	if(osg::Texture2D* cached = findCached(cacheKey)) return cached;

	osg::ref_ptr<osg::Image> loadedImage = loadRawImage(textureIndex);

	if(!loadedImage) return nullptr;

	osg::ref_ptr<osg::Texture2D> osgTexture = new osg::Texture2D(loadedImage);

	applyFormatAndSampler(osgTexture, loadedImage, sRGB, texture.sampler);
	osgTexture->setUnRefImageDataAfterApply(unrefImageDataAfterApply);
	cache(cacheKey, osgTexture);

	return osgTexture.release();
}

osg::Texture2D* TextureLoader::findCached(const std::string& key) const {
	return _cache && !key.empty() ? _cache->find(key) : nullptr;
}

void TextureLoader::cache(const std::string& key, osg::Texture2D* texture) const {
	if(_cache && !key.empty()) _cache->store(key, texture);
}

}
