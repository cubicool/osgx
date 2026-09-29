#include "osgx/CompressedTexture.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>

#include <osgDB/ReadFile>

OSGX_ENABLE_WARNINGS

#include <cstdlib>
#include <iomanip>
#include <iostream>

namespace {

std::size_t bptcMipChainBytes(const osg::Image& image) {
	std::size_t bytes = 0;
	int width = image.s();
	int height = image.t();
	int depth = image.r();

	for(unsigned int level = 0; level < image.getNumMipmapLevels(); level++) {
		const std::size_t blocksWide = static_cast<std::size_t>((width + 3) / 4);
		const std::size_t blocksHigh = static_cast<std::size_t>((height + 3) / 4);

		bytes += blocksWide * blocksHigh * static_cast<std::size_t>(depth) * 16;
		width = osg::maximum(width >> 1, 1);
		height = osg::maximum(height >> 1, 1);
		depth = osg::maximum(depth >> 1, 1);
	}

	return bytes;
}

}

int main(int argc, char* argv[]) {
	if(argc != 2) {
		std::cerr << "Usage: " << argv[0] << " file.dds" << std::endl;

		return EXIT_FAILURE;
	}

	osg::ref_ptr<osg::Image> image = osgDB::readRefImageFile(argv[1]);

	if(!image) {
		std::cerr << "Failed to load '" << argv[1] << "'" << std::endl;

		return EXIT_FAILURE;
	}

	const unsigned int pixelFormat = image->getPixelFormat();
	const std::size_t payloadBytes = osgx::isBC7(pixelFormat) ?
		bptcMipChainBytes(*image) : image->getTotalSizeInBytesIncludingMipmaps()
	;

	std::cout
		<< image->s() << "x" << image->t() << "x" << image->r()
		<< " levels=" << image->getNumMipmapLevels()
		<< " pixelFormat=0x" << std::hex << pixelFormat
		<< " internalFormat=0x" << image->getInternalTextureFormat()
		<< std::dec
		<< " bytes=" << payloadBytes
		<< std::endl
	;

	return EXIT_SUCCESS;
}
