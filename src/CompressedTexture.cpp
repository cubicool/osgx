#include "osgx/CompressedTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/GLExtensions>
#include <osg/GL>
#include <osg/Notify>
#include <osg/State>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <limits>

namespace osgx {

bool isBC7(GLenum format) {
	return format == GL_COMPRESSED_RGBA_BPTC_UNORM ||
		format == GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
}

bool isBPTC(const osg::Image& image) {
	return
		isBC7(static_cast<GLenum>(image.getPixelFormat())) ||
		isBC7(static_cast<GLenum>(image.getInternalTextureFormat()))
	;
}

namespace {

bool supportsBPTC(const osg::State& state) {
	return osg::isGLExtensionOrVersionSupported(
		state.getContextID(), "GL_ARB_texture_compression_bptc", 4.2f
	);
}

class BC7SubloadCallback: public osg::Texture2D::SubloadCallback {
public:
	void load(const osg::Texture2D& texture, osg::State& state) const override {
		upload(texture, state, false);
	}

	void subload(const osg::Texture2D& texture, osg::State& state) const override {
		const osg::Image* image = texture.getImage();

		if(!image || texture.getModifiedCount(state.getContextID()) == image->getModifiedCount()) return;

		if(upload(texture, state, true)) {
			texture.getModifiedCount(state.getContextID()) = image->getModifiedCount();
		}
	}

private:
	static bool upload(const osg::Texture2D& texture, osg::State& state, bool subload) {
		const osg::Image* image = texture.getImage();

		if(!image || !image->data()) return false;

		const GLenum format = static_cast<GLenum>(image->getInternalTextureFormat());

		if(!isBC7(format)) return false;

		const osg::GLExtensions* extensions = state.get<osg::GLExtensions>();

		if(
			!supportsBPTC(state) ||
			!extensions->isCompressedTexImage2DSupported() ||
			!extensions->isCompressedTexSubImage2DSupported()
		) {
			OSG_WARN << "osgx::makeCompressedTexture2D: BPTC is unavailable in the current GL context"
				<< std::endl;
			return false;
		}

		const unsigned int levels = image->getNumMipmapLevels();
		int width = image->s();
		int height = image->t();
		std::size_t expectedOffset = 0;

		if(width <= 0 || height <= 0) return false;

		for(unsigned int level = 0; level < levels; level++) {
			const std::size_t blockWidth = static_cast<std::size_t>((width + 3) / 4);
			const std::size_t blockHeight = static_cast<std::size_t>((height + 3) / 4);
			const std::size_t bytes = blockWidth * blockHeight * 16;
			const std::size_t offset = image->getMipmapOffset(level);

			if(bytes > static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()) || offset != expectedOffset) {
				OSG_WARN << "osgx::makeCompressedTexture2D: invalid BC7 mip chain" << std::endl;

				return false;
			}

			if(subload) extensions->glCompressedTexSubImage2D(
				GL_TEXTURE_2D,
				static_cast<GLint>(level),
				0,
				0,
				width,
				height,
				format,
				static_cast<GLsizei>(bytes),
				image->data() + offset
			);

			else extensions->glCompressedTexImage2D(
				GL_TEXTURE_2D,
				static_cast<GLint>(level),
				format,
				width,
				height,
				0,
				static_cast<GLsizei>(bytes),
				image->data() + offset
			);

			width = std::max(width >> 1, 1);
			height = std::max(height >> 1, 1);
			expectedOffset += bytes;
		}

		return true;
	}
};

}

osg::ref_ptr<osg::Texture2D> makeCompressedTexture2D(osg::Image* image) {
	if(!image) return nullptr;

	auto texture = make_ref<osg::Texture2D>(image);
	const GLenum format = static_cast<GLenum>(image->getInternalTextureFormat());

	if(isBC7(format)) {
		texture->setInternalFormat(static_cast<GLint>(format));
		texture->setTextureSize(image->s(), image->t());
		texture->setNumMipmapLevels(image->getNumMipmapLevels());
		texture->setUseHardwareMipMapGeneration(false);
		texture->setSubloadCallback(new BC7SubloadCallback);
	}

	return texture;
}

}
