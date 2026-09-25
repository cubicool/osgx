#pragma once

// The Python module's osgx::Library subclass: libosgx's state plus the Python bindings' own
// process-wide state, all released when the Python object returned by osgx.initialize() is
// destroyed. Included only by the binding translation units that need that state.

#include "osgx/Library.hpp"

#ifdef OSGX_GLTF
#include "osgx/gltf/Reader.hpp"
#endif

namespace osgx_python {

class Library: public osgx::Library {
	public:
		explicit Library(const osgx::LibraryOptions& options=osgx::LibraryOptions());

#ifdef OSGX_GLTF
		// Texture dedup cache for osgx.gltf's async readNodeFile(), the same role ReaderWriterGLTF's
		// own cache plays for osgDB::readNodeFile().
		osgx::gltf::Reader::TextureCache gltfTextureCache;
#endif
};

}
