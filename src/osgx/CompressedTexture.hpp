#pragma once

#include "Core.hpp"
#include "Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
# define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
# define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif

namespace osgx {

// True for GL_COMPRESSED_{RGBA,SRGB_ALPHA}_BPTC_UNORM (BC7). The one place this check lives -
// callers must not reimplement it against the raw 0x8e8c/0x8e8d values.
bool isBC7(GLenum format);

// isBC7() against both the image's pixel format and internal texture format, since different
// producers (the DDS reader, glTF loader) settle those fields differently.
bool isBPTC(const osg::Image& image);

// Builds an ordinary Texture2D except for BC7 BPTC images, whose block payload and authored mip
// chain need the custom compressed-upload callback because the local OSG core does not classify
// BPTC as a compressed internal format. Returns null for a null image.
osg::ref_ptr<osg::Texture2D> makeCompressedTexture2D(osg::Image* image);

}
