#pragma once

// libosgx-internal: the state osgx::Library (osgx/Library.hpp) owns. Not installed; included only by
// libosgx's own translation units.

#include "osgx/Library.hpp"
#include "osgx/Shader.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Referenced>
#include <osg/Shader>
#include <osg/Texture2D>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace osgx::detail {

struct ShaderLibCatalog {
	std::string namespaceName;
	std::vector<ShaderLib> libs;
};

struct LibraryState: public osg::Referenced {
	// Named binding slots; see osgx::Bindings (Library.hpp).
	Bindings bindings;

	// registerShaderLibs() writes, resolveShaderLibs() reads.
	std::vector<ShaderLibCatalog> shaderLibCatalogs;

	// cachedShader(), keyed by (type, source text).
	std::map<std::pair<osg::Shader::Type, std::string>, osg::ref_ptr<osg::Shader>> shaderCache;

	// SharedBRDFLUT::create(), keyed by LUT size.
	std::map<int, osg::ref_ptr<osg::Texture2D>> brdfLUTs;

	// PixelText's charset atlas, created on first use.
	osg::ref_ptr<osg::Texture2D> pixelTextAtlas;

	protected:
		~LibraryState() override = default;
};

}
