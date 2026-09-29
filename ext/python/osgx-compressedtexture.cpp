#include "osgx-python.hpp"
#include "osgx/CompressedTexture.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Image>
#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

namespace osgx_python {

void bind_compressedtexture(py::module_& m) {
	m.def(
		"isBC7",
		&osgx::isBC7,
		"format"_a,
		"True for GL_COMPRESSED_{RGBA,SRGB_ALPHA}_BPTC_UNORM (BC7)."
	);

	m.def(
		"isBPTC",
		[](const osg::Image& image) { return osgx::isBPTC(image); },
		"image"_a,
		"isBC7() against both image.getPixelFormat() and image.getInternalTextureFormat(), since "
		"different producers (the DDS reader, the glTF loader) settle those fields differently."
	);

	m.def(
		"makeCompressedTexture2D",
		&osgx::makeCompressedTexture2D,
		"image"_a,
		"Builds an ordinary Texture2D except for BC7 BPTC images, whose block payload and authored "
		"mip chain need a custom compressed-upload callback because the local OSG core does not "
		"classify BPTC as a compressed internal format. Returns None for a null image."
	);
}

}
