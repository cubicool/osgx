#include "osgx/Array.hpp"
#include "osgx/PBR.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/State>
#include <osg/Texture2D>

OSGX_ENABLE_WARNINGS

namespace osgx {

std::string snippets() {
	return std::string(D_GGX) + G_SCHLICK + G_SMITH + F_SCHLICK + F_SCHLICK_ROUGHNESS;
}

Material::Material() {
	_initBuffer();
}

Material::~Material() {}

Material::Material(const Material& material, const osg::CopyOp& copyop):
osg::StateAttribute(material, copyop),
_baseColor(material._baseColor),
_roughness(material._roughness),
_metallic(material._metallic),
_emissiveFactor(material._emissiveFactor),
_alphaMode(material._alphaMode),
_alphaCutoff(material._alphaCutoff),
_hasOcclusion(material._hasOcclusion),
_baseColorMap(static_cast<osg::Texture2D*>(copyop(material._baseColorMap.get()))),
_normalMap(static_cast<osg::Texture2D*>(copyop(material._normalMap.get()))),
_metallicRoughnessMap(static_cast<osg::Texture2D*>(copyop(material._metallicRoughnessMap.get()))),
_emissiveMap(static_cast<osg::Texture2D*>(copyop(material._emissiveMap.get()))) {
	_initBuffer();
}

// Field order must match MATERIAL_INPUTS' `osgx_MaterialInputs` std430 block (PBR.hpp) exactly.
// Built once here (not per-write) so every setter can mutate it in place via dirty()
// instead of standing up a new osg::ShaderStorageBufferObject/GL buffer on every call.
void Material::_initBuffer() {
	_buffer = osgx::make_ref<osgx::FloatArray>(static_cast<std::size_t>(16));
	_buffer->setBufferObject(new osg::ShaderStorageBufferObject());

	_binding = new osg::ShaderStorageBufferBinding(
		MATERIAL_BINDING, _buffer, 0, static_cast<GLsizeiptr>(_buffer->getTotalDataSize())
	);

	_writeFactors();
}

void Material::_writeFactors() {
	// 16 floats, no padding: vec3 emissiveFactor packs into the 16-byte slot after baseColorFactor
	// with roughnessFactor filling its fourth component (std430 vec3 alignment).
	_buffer->set({
		_baseColor.r(), _baseColor.g(), _baseColor.b(), _baseColor.a(),
		_emissiveFactor.x(), _emissiveFactor.y(), _emissiveFactor.z(),
		_roughness,
		_metallic,
		static_cast<float>(_alphaMode),
		_alphaCutoff,
		_baseColorMap.valid() ? 1.0f : 0.0f,
		_metallicRoughnessMap.valid() ? 1.0f : 0.0f,
		_hasOcclusion ? 1.0f : 0.0f,
		_normalMap.valid() ? 1.0f : 0.0f,
		_emissiveMap.valid() ? 1.0f : 0.0f
	});

	_buffer->dirty();
}

int Material::compare(const osg::StateAttribute& sa) const {
	COMPARE_StateAttribute_Types(Material, sa)

	COMPARE_StateAttribute_Parameter(_baseColorMap)
	COMPARE_StateAttribute_Parameter(_normalMap)
	COMPARE_StateAttribute_Parameter(_metallicRoughnessMap)
	COMPARE_StateAttribute_Parameter(_emissiveMap)
	COMPARE_StateAttribute_Parameter(_hasOcclusion)
	COMPARE_StateAttribute_Parameter(_baseColor)
	COMPARE_StateAttribute_Parameter(_roughness)
	COMPARE_StateAttribute_Parameter(_metallic)
	COMPARE_StateAttribute_Parameter(_emissiveFactor)
	COMPARE_StateAttribute_Parameter(_alphaMode)
	COMPARE_StateAttribute_Parameter(_alphaCutoff)

	return 0;
}

// Read-only over this object's state - see the class comment (PBR.hpp) for why that matters
// across multiple graphics contexts. Textures bind through osg::State's own per-unit tracking
// (applyTextureAttribute), so a texture already current at that unit from elsewhere is a no-op;
// the factor buffer binds through the usual osg::Array/BufferObject per-context sync.
void Material::apply(osg::State& state) const {
	if(_baseColorMap.valid())
		state.applyTextureAttribute(BASE_COLOR_TEXTURE_UNIT, _baseColorMap.get());

	if(_normalMap.valid())
		state.applyTextureAttribute(NORMAL_TEXTURE_UNIT, _normalMap.get());

	if(_metallicRoughnessMap.valid())
		state.applyTextureAttribute(ORM_TEXTURE_UNIT, _metallicRoughnessMap.get());

	if(_emissiveMap.valid())
		state.applyTextureAttribute(EMISSIVE_TEXTURE_UNIT, _emissiveMap.get());

	state.applyAttribute(_binding.get());
}

void Material::setBaseColor(const osg::Vec4& baseColor) {
	_baseColor = baseColor;

	_writeFactors();
}

void Material::setRoughness(float roughness) {
	_roughness = roughness;

	_writeFactors();
}

void Material::setMetallic(float metallic) {
	_metallic = metallic;

	_writeFactors();
}

void Material::setEmissiveFactor(const osg::Vec3& emissiveFactor) {
	_emissiveFactor = emissiveFactor;

	_writeFactors();
}

void Material::setAlphaMode(AlphaMode alphaMode) {
	_alphaMode = alphaMode;

	_writeFactors();
}

void Material::setAlphaCutoff(float alphaCutoff) {
	_alphaCutoff = alphaCutoff;

	_writeFactors();
}

void Material::setHasOcclusion(bool hasOcclusion) {
	_hasOcclusion = hasOcclusion;

	_writeFactors();
}

void Material::setBaseColorMap(osg::Texture2D* texture) {
	_baseColorMap = texture;

	_writeFactors();
}

void Material::setNormalMap(osg::Texture2D* texture) {
	_normalMap = texture;

	_writeFactors();
}

void Material::setMetallicRoughnessMap(osg::Texture2D* texture) {
	_metallicRoughnessMap = texture;

	_writeFactors();
}

void Material::setEmissiveMap(osg::Texture2D* texture) {
	_emissiveMap = texture;

	_writeFactors();
}

void registerPBRShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"D_GGX", "osgx_D_GGX", D_GGX},
		{"G_SCHLICK", "osgx_G_Schlick", G_SCHLICK},
		{"G_SMITH", "osgx_G_Smith", G_SMITH},
		{"F_SCHLICK", "osgx_F_Schlick", F_SCHLICK},
		{"F_SCHLICK_ROUGHNESS", "osgx_F_Schlick_roughness", F_SCHLICK_ROUGHNESS},
		{"MATERIAL_STRUCT", "osgx_Material", MATERIAL_STRUCT},
		{"MATERIAL_INPUTS", "osgx_MaterialInputs", MATERIAL_INPUTS},
		{"GET_MATERIAL", "osgx_GetMaterial", GET_MATERIAL},
		{"GET_EMISSIVE", "osgx_GetEmissive", GET_EMISSIVE},
		{"GET_ALPHA", "osgx_GetAlpha", GET_ALPHA},
		{"GET_SHADING_NORMAL", "osgx_GetShadingNormal", GET_SHADING_NORMAL},
		{"F_MULTISCATTER", "osgx_F_MultiScatter", F_MULTISCATTER},
		{"SPECULAR_AA", "osgx_SpecularAA", SPECULAR_AA},
		{"TONEMAP_PBR_NEUTRAL", "osgx_TonemapPBRNeutral", TONEMAP_PBR_NEUTRAL},
		{"TONEMAP_DECL", "osgx_Tonemap", TONEMAP_DECL}
	};
	::osgx::registerShaderLibs("osgx::pbr", libs);
}

}
