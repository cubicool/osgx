#pragma once

#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/Group>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <vector>

namespace osgDB { class Options; }
struct tg3_model;
struct tg3_mesh;

namespace osgx::gltf::detail {

class MaterialBuilder;
struct SkinData;

class MeshBuilder {
public:
	MeshBuilder(
		const tg3_model& model,
		const osgDB::Options* readOptions,
		MaterialBuilder& materialBuilder,
		const std::vector<osg::ref_ptr<osg::Array>>& arrays,
		const std::vector<osg::ref_ptr<SkinData>>& skins
	);

	osg::Group* makeMesh(const tg3_mesh& mesh, int skinIndex) const;

private:
	const tg3_model& _model;
	const osgDB::Options* _readOptions;
	MaterialBuilder& _materialBuilder;
	const std::vector<osg::ref_ptr<osg::Array>>& _arrays;
	const std::vector<osg::ref_ptr<SkinData>>& _skins;

	static GLenum _primitiveMode(int gltfMode);
};

}
