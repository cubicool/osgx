// osgx-cubemap - convert an equirectangular panorama into a raw visual cubemap KTX2.
//
// Usage:
//   osgx-cubemap <input-image> <output.ktx2> [--size N]
//
// This intentionally performs no GGX prefiltering, irradiance convolution, tone mapping, or
// firefly clamping. It is for visible skyboxes, not IBL. The output contains one 32-bit float RGB
// mip level, with conventional OpenGL/KTX cubemap face orientation.

#include "osgx/Core.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osg/Notify>
#include <osg/TextureCubeMap>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage(const char* program) {
	std::cerr << "Usage: " << program << " <input-image> <output.ktx2> [--size N]" << std::endl;
}

int wrap(int value, int extent) {
	value %= extent;

	return value < 0 ? value + extent : value;
}

osg::Vec3f cubeFaceDirection(int face, float s, float t) {
	switch(face) {
		case 0: return osg::Vec3f(1.0f, -t, -s);
		case 1: return osg::Vec3f(-1.0f, -t, s);
		case 2: return osg::Vec3f(s, 1.0f, t);
		case 3: return osg::Vec3f(s, -1.0f, -t);
		case 4: return osg::Vec3f(s, -t, 1.0f);
		default: return osg::Vec3f(-s, -t, -1.0f);
	}
}

osg::Vec3f sampleEquirect(const osg::Image& image, const osg::Vec3f& direction) {
	const int width = image.s();
	const int height = image.t();
	const osg::Vec3f d = direction / direction.length();
	const double u = std::atan2(double(d.z()), double(d.x())) / (2.0 * osg::PI) + 0.5;
	const double v = 1.0 - std::acos(std::clamp(double(d.y()), -1.0, 1.0)) / osg::PI;
	const double x = u * double(width) - 0.5;
	const double y = v * double(height) - 0.5;
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	const float fx = static_cast<float>(x - double(x0));
	const float fy = static_cast<float>(y - double(y0));
	const int x1 = wrap(x0 + 1, width);
	const int y1 = std::min(y0 + 1, height - 1);
	const unsigned int sx0 = static_cast<unsigned int>(wrap(x0, width));
	const unsigned int sx1 = static_cast<unsigned int>(x1);
	const unsigned int sy0 = static_cast<unsigned int>(std::clamp(y0, 0, height - 1));
	const unsigned int sy1 = static_cast<unsigned int>(y1);

	const osg::Vec4f c00 = image.getColor(sx0, sy0);
	const osg::Vec4f c10 = image.getColor(sx1, sy0);
	const osg::Vec4f c01 = image.getColor(sx0, sy1);
	const osg::Vec4f c11 = image.getColor(sx1, sy1);
	const osg::Vec4f a = c00 * (1.0f - fx) + c10 * fx;
	const osg::Vec4f b = c01 * (1.0f - fx) + c11 * fx;
	const osg::Vec4f color = a * (1.0f - fy) + b * fy;

	return osg::Vec3f(color.x(), color.y(), color.z());
}

osg::ref_ptr<osg::TextureCubeMap> makeCubeMap(const osg::Image& panorama, int size) {
	auto cubeMap = osgx::make_ref<osg::TextureCubeMap>();

	cubeMap->setInternalFormat(GL_RGB32F);
	cubeMap->setSourceFormat(GL_RGB);
	cubeMap->setSourceType(GL_FLOAT);
	cubeMap->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	cubeMap->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	cubeMap->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	cubeMap->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	cubeMap->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);
	cubeMap->setUseHardwareMipMapGeneration(false);

	for(int face = 0; face < 6; face++) {
		auto image = osgx::make_ref<osg::Image>();

		image->allocateImage(size, size, 1, GL_RGB, GL_FLOAT);
		image->setInternalTextureFormat(GL_RGB32F);

		auto* pixels = reinterpret_cast<float*>(image->data());

		for(int y = 0; y < size; y++) {
			const float t = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;

			for(int x = 0; x < size; x++) {
				const float s = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
				const osg::Vec3f color = sampleEquirect(panorama, cubeFaceDirection(face, s, t));
				const std::size_t index = (
					static_cast<std::size_t>(y) * static_cast<std::size_t>(size)
					+ static_cast<std::size_t>(x)
				) * 3;

				pixels[index] = color.x();
				pixels[index + 1] = color.y();
				pixels[index + 2] = color.z();
			}
		}

		cubeMap->setImage(static_cast<unsigned int>(face), image);
	}

	return cubeMap;
}

}

int main(int argc, char* argv[]) {
	if(argc < 3) {
		usage(argv[0]);

		return 1;
	}

	const std::string inputPath(argv[1]);
	const std::string outputPath(argv[2]);
	int size = 512;

	for(int i = 3; i < argc; i++) {
		const std::string argument(argv[i]);

		if(argument == "--size" && i + 1 < argc) size = std::atoi(argv[++i]);
		else {
			OSG_WARN << "osgx-cubemap: unknown or incomplete option " << argument << std::endl;
			usage(argv[0]);

			return 1;
		}
	}

	if(size < 1) {
		OSG_WARN << "osgx-cubemap: --size must be positive" << std::endl;

		return 1;
	}

	osg::setNotifyLevel(osg::NOTICE);
	auto panorama = osgDB::readRefImageFile(inputPath);

	if(!panorama) {
		OSG_WARN << "osgx-cubemap: failed to load " << inputPath << std::endl;

		return 1;
	}

	if(panorama->s() < 1 || panorama->t() < 1) {
		OSG_WARN << "osgx-cubemap: source image has invalid dimensions" << std::endl;

		return 1;
	}

	OSG_NOTICE << "osgx-cubemap: converting " << panorama->s() << "x" << panorama->t()
		<< " panorama to " << size << "x" << size << " cubemap" << std::endl;

	auto cubeMap = makeCubeMap(*panorama, size);

	if(!osgDB::writeObjectFile(*cubeMap, outputPath)) {
		OSG_WARN << "osgx-cubemap: failed to write " << outputPath << std::endl;

		return 1;
	}

	OSG_NOTICE << "osgx-cubemap: wrote " << outputPath << std::endl;

	return 0;
}
