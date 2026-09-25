#include "osgx/Array.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Mesh.hpp"
#include "Log.hpp"
#include "Material.hpp"
#include "Skin.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Geode>
#include <osg/Geometry>

#include <osgDB/Options>

#include <osgUtil/SmoothingVisitor>

OSGX_ENABLE_WARNINGS

#include "osgx/Skinning.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <typeinfo>

namespace osgx::gltf::detail {

MeshBuilder::MeshBuilder(
	const tg3_model& model,
	const osgDB::Options* readOptions,
	MaterialBuilder& materialBuilder,
	const std::vector<osg::ref_ptr<osg::Array>>& arrays,
	const std::vector<osg::ref_ptr<SkinData>>& skins
):
_model(model),
_readOptions(readOptions),
_materialBuilder(materialBuilder),
_arrays(arrays),
_skins(skins) {}

osg::Group* MeshBuilder::makeMesh(const tg3_mesh& mesh, int skinIdx) const {
	std::string meshName = tg3_to_string(mesh.name);

	GLTF_NOTIFY(1)
		<< "makeMesh '" << meshName
		<< "' skin=" << skinIdx
		<< " - " << mesh.primitives_count << " primitive(s)" << std::endl
	;

	osg::Group* group = new osg::Group();

	group->setName(meshName);

	for(std::uint32_t primIdx = 0; primIdx < mesh.primitives_count; primIdx++) {
		const tg3_primitive& primitive = mesh.primitives[primIdx];

		GLTF_NOTIFY(2)
			<< "primitive[" << primIdx << "]"
			<< " mode=" << primitive.mode
			<< " indices=" << primitive.indices
			<< " material=" << primitive.material
			<< " attrs=" << primitive.attributes_count << std::endl
		;

		auto geom = osgx::make_nref<osg::Geometry>(typeid(*this).name());

		geom->setUseVertexBufferObjects(true);

		osg::Vec4 baseColorFactor(1, 1, 1, 1);

		// Vertex attributes are parsed before material application since texture-unit
		// binding needs to know which UV set (TEXCOORD_n) each texture requests.
		GLTF_NOTIFY(3) << "attributes:" << std::endl;

		std::map<int, osg::Array*> texCoordSets;
		int jointsAccessor = -1;
		int weightsAccessor = -1;

		for(std::uint32_t attrIdx = 0; attrIdx < primitive.attributes_count; attrIdx++) {
			const tg3_str& attrName = primitive.attributes[attrIdx].key;
			const int accessorIdx = primitive.attributes[attrIdx].value;
			const bool nonnegative = accessorIdx >= 0;
			const std::size_t arrayIndex = nonnegative
				? static_cast<std::size_t>(accessorIdx)
				: 0
			;
			const bool valid =
				nonnegative && arrayIndex < _arrays.size() && _arrays[arrayIndex].valid();

			GLTF_NOTIFY(4)
				<< "" << tg3_to_string(attrName)
				<< " -> accessor[" << accessorIdx << "]"
				<< (valid ? " OK" : " NULL/INVALID") << std::endl
			;

			if(!valid) continue;

			if(tg3_str_equals_cstr(attrName, "POSITION")) geom->setVertexArray(_arrays[arrayIndex]);
			else if(tg3_str_equals_cstr(attrName, "NORMAL")) geom->setNormalArray(_arrays[arrayIndex]);
			else if(tg3_str_equals_cstr(attrName, "COLOR_0")) geom->setColorArray(_arrays[arrayIndex]);
			else if(tg3_str_equals_cstr(attrName, "TANGENT")) {
				_arrays[arrayIndex]->setBinding(osg::Array::BIND_PER_VERTEX);

				geom->setVertexAttribArray(
					osgx::TANGENT_ATTRIBUTE,
					_arrays[arrayIndex]
				);
			}
			else if(tg3_starts_with(attrName, "TEXCOORD_")) {
				int uvSet = tg3_texcoord_suffix(attrName);

				texCoordSets[uvSet] = _arrays[arrayIndex];
			}

			else if(tg3_str_equals_cstr(attrName, "JOINTS_0")) jointsAccessor = accessorIdx;
			else if(tg3_str_equals_cstr(attrName, "WEIGHTS_0")) weightsAccessor = accessorIdx;
		}

		if(jointsAccessor >= 0 || weightsAccessor >= 0) {
			GLTF_NOTIFY(3)
				<< "skinning attrs:"
				<< " JOINTS_0=" << jointsAccessor
				<< " WEIGHTS_0=" << weightsAccessor << std::endl
			;

			if(skinIdx >= 0 && static_cast<std::size_t>(skinIdx) < _skins.size()) {
				if(jointsAccessor >= 0) {
					const std::size_t jointsIndex = static_cast<std::size_t>(jointsAccessor);

					_arrays[jointsIndex]->setBinding(osg::Array::BIND_PER_VERTEX);
					_arrays[jointsIndex]->setPreserveDataType(true);
					geom->setVertexAttribArray(
						osgx::JOINT_INDICES_ATTRIBUTE,
						_arrays[jointsIndex]
					);
				}

				if(weightsAccessor >= 0) {
					const std::size_t weightsIndex = static_cast<std::size_t>(weightsAccessor);

					_arrays[weightsIndex]->setBinding(osg::Array::BIND_PER_VERTEX);
					geom->setVertexAttribArray(
						osgx::JOINT_WEIGHTS_ATTRIBUTE,
						_arrays[weightsIndex]
					);
				}
			}

			else {
				GLTF_NOTIFY(3)
					<< "skinning attrs present, but node has no valid skin; not binding them"
					<< std::endl
				;
			}
		}

		// A missing material means glTF's defined default material, not "leave
		// whatever render state happened to be inherited." MaterialBuilder also
		// validates positive indices before looking them up.
		GLTF_NOTIFY(3) << "applyMaterial " << primitive.material << std::endl;

		_materialBuilder.applyMaterial(
			primitive.material,
			baseColorFactor,
			geom,
			texCoordSets
		);

		// Fall back to a solid color if COLOR_0 is absent.
		if(!geom->getColorArray()) {
			auto* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
			const unsigned int count = verts
				? static_cast<unsigned int>(verts->size())
				: 1
			;
			auto* colors = new osgx::Vec4Array();

			colors->append_n(baseColorFactor, count);

			geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
		}

		// Index primitive set: uint8, uint16, or uint32.
		const bool haveIndex = primitive.indices >= 0;
		const std::size_t indexAccessor = haveIndex
			? static_cast<std::size_t>(primitive.indices)
			: 0
		;

		if(
			haveIndex &&
			indexAccessor < _arrays.size() &&
			indexAccessor < _model.accessors_count &&
			_arrays[indexAccessor].valid()
		) {
			const GLenum glMode = _primitiveMode(primitive.mode);
			const tg3_accessor& idxAcc = _model.accessors[indexAccessor];
			osg::Array* indexArray = _arrays[indexAccessor];

			switch(idxAcc.component_type) {
				case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: {
					auto* src = static_cast<osg::UByteArray*>(indexArray);
					auto* de = new osg::DrawElementsUByte(
						glMode,
						static_cast<unsigned int>(idxAcc.count)
					);

					std::copy(src->begin(), src->end(), de->begin());

					geom->addPrimitiveSet(de);

					break;
				}

				case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: {
					auto* src = static_cast<osg::UShortArray*>(indexArray);

					geom->addPrimitiveSet(new osg::DrawElementsUShort(
						glMode,
						src->begin(),
						src->end()
					));

					break;
				}

				case TG3_COMPONENT_TYPE_UNSIGNED_INT: {
					auto* src = static_cast<osg::UIntArray*>(indexArray);

					geom->addPrimitiveSet(new osg::DrawElementsUInt(
						glMode,
						src->begin(),
						src->end()
					));

					break;
				}

				default:
					OSG_WARN
						<< "unsupported index component type "
						<< idxAcc.component_type << std::endl
					;
			}
		}

		else {
			// Non-indexed: draw every vertex.
			auto* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());

			if(verts) geom->addPrimitiveSet(new osg::DrawArrays(
				_primitiveMode(primitive.mode),
				0,
				static_cast<GLsizei>(verts->size())
			));
		}

		// SmoothingVisitor assumes triangles; never call it for points or lines.
		bool isTriangles = (
			primitive.mode == TG3_MODE_TRIANGLES ||
			primitive.mode == TG3_MODE_TRIANGLE_STRIP ||
			primitive.mode == TG3_MODE_TRIANGLE_FAN
		);

		bool skipNormals =
			_readOptions &&
			_readOptions->getOptionString().find("gltfSkipNormals") != std::string::npos
		;

		osg::Geode* geode = new osg::Geode();

		geode->addDrawable(geom);

		if(isTriangles && !skipNormals && !geom->getNormalArray()) {
			GLTF_NOTIFY(3) << "generating normals via SmoothingVisitor" << std::endl;

			osgUtil::SmoothingVisitor sv;

			geode->accept(sv);
		}

		GLTF_NOTIFY(3) << "addChild geode to mesh group" << std::endl;

		group->addChild(geode);
	}

	return group;
}

GLenum MeshBuilder::_primitiveMode(int gltfMode) {
	switch(gltfMode) {
		case TG3_MODE_POINTS: return GL_POINTS;
		case TG3_MODE_LINE: return GL_LINES;
		case TG3_MODE_LINE_LOOP: return GL_LINE_LOOP;
		case TG3_MODE_LINE_STRIP: return GL_LINE_STRIP;
		case TG3_MODE_TRIANGLES: return GL_TRIANGLES;
		case TG3_MODE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
		case TG3_MODE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
		default: return GL_TRIANGLES;
	}
}

}
