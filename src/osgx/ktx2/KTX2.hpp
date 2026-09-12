#pragma once

#include "../Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osgDB/ReaderWriter>

OSGX_ENABLE_WARNINGS

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace osg {
class Image;
class TextureCubeMap;
}

namespace osgx::ktx2 {

osgDB::ReaderWriter::ReadResult read(const std::string& file);

osgDB::ReaderWriter::WriteResult write(
	const osg::TextureCubeMap& texture,
	const std::string& file
);

osgDB::ReaderWriter::WriteResult write(
	const osg::Image& image,
	const std::string& file
);

// Numeric values are frozen by the Vulkan spec (already hardcoded as fallback #defines in
// KTX2.cpp) - callers never need <vulkan/vulkan_core.h> or <ktx.h> to pick one.
enum class Format: uint32_t {
	R8G8B8_UNORM = 23,
	R8G8B8A8_UNORM = 37,
	R8G8B8A8_SRGB = 43,
	R16G16B16_SFLOAT = 90,
	R16G16B16A16_SFLOAT = 97,
	R32G32B32_SFLOAT = 106,
	R32G32B32A32_SFLOAT = 109,
};

// One (mip, face) image's raw, tightly-packed bytes - borrowed, not owned; only needs to stay
// valid for the duration of a single accessor call.
struct ImageSpan {
	const void* data;
	std::size_t size;
};

// Called exactly once per (mip, face) pair, in mip-major/face-minor order (mip 0 face 0 ..
// mip 0 face N-1, mip 1 face 0, ...), immediately before that image's bytes are handed to
// libktx - so callers may fill a small reused scratch buffer per call (e.g. a float32->float16
// conversion) instead of pre-materializing every mip/face up front.
using ImageAccessor = std::function<ImageSpan(uint32_t mip, uint32_t face)>;

// Low-level write path: builds a KTX2 file directly from raw pixel buffers, with no osg::Image/
// osg::TextureCubeMap involved at all. numFaces must be 1 (2D image) or 6 (cubemap).
osgDB::ReaderWriter::WriteResult write(
	uint32_t width,
	uint32_t height,
	uint32_t numMips,
	uint32_t numFaces,
	Format format,
	const ImageAccessor& accessor,
	const std::string& file
);

}
