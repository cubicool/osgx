#include "osgx/Array.hpp"
#include "osgx/Environment.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/GL>
#include <osg/State>

#ifndef GL_RGB32F
#  define GL_RGB32F 0x8815
#endif

OSGX_ENABLE_WARNINGS

#include <stdexcept>

namespace osgx {

namespace {

float lastMipLevel(const osg::TextureCubeMap* texture) {
	if(!texture) return 0.0f;

	unsigned int levels = texture->getNumMipmapLevels();

	if(const osg::Image* image = texture->getImage(0); image && image->getNumMipmapLevels() > levels)
		levels = image->getNumMipmapLevels();

	return levels > 1 ? static_cast<float>(levels - 1) : 0.0f;
}

// Valid and bindable, never sampled meaningfully: stands in until setSpecularMap() replaces it.
osg::ref_ptr<osg::TextureCubeMap> makePlaceholderCubeMap() {
	auto texture = osgx::make_ref<osg::TextureCubeMap>();

	texture->setTextureSize(1, 1);
	texture->setInternalFormat(GL_RGB32F);
	texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
	texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
	texture->setWrap(osg::Texture::WRAP_R, osg::Texture::CLAMP_TO_EDGE);

	return texture;
}

}

Environment::Environment() {
	_initBuffer();
}

Environment::Environment(osg::Image* equirectangularHDR, const EnvironmentBakeOptions& options) {
	if(!equirectangularHDR) throw std::invalid_argument("osgx::Environment: null HDR image");

	_initLUT(options.lutSize);

	auto diffuseBake = LambertianBakeScene::create(equirectangularHDR, options.diffuse);

	_diffuseMap = diffuseBake.diffuseTexture;

	if(!_bakeRoot) _bakeRoot = osgx::make_nref<osg::Group>("osgx_EnvironmentBake");

	_bakeRoot->addChild(diffuseBake.root);

	if(options.bakeSpecular) {
		auto specularBake = GGXPrefilterScene::create(equirectangularHDR, options.specular);

		_specularMap = specularBake.prefilterTexture;

		_bakeRoot->addChild(specularBake.root);
	}

	else _specularMap = makePlaceholderCubeMap();

	_maxSpecularMip = lastMipLevel(_specularMap.get());

	_initBuffer();
}

Environment::Environment(
	osg::TextureCubeMap* specularMap,
	osg::TextureCubeMap* diffuseMap,
	float maxSpecularMip,
	int lutSize
):
_specularMap(specularMap),
_diffuseMap(diffuseMap) {
	_maxSpecularMip = maxSpecularMip < 0.0f ? lastMipLevel(specularMap) : maxSpecularMip;

	_initLUT(lutSize);
	_initBuffer();
}

Environment::Environment(
	osg::TextureCubeMap* specularMap,
	osg::TextureCubeMap* diffuseMap,
	osg::Texture2D* brdfLUT,
	float maxSpecularMip
):
_specularMap(specularMap),
_diffuseMap(diffuseMap),
_brdfLUT(brdfLUT) {
	_maxSpecularMip = maxSpecularMip < 0.0f ? lastMipLevel(specularMap) : maxSpecularMip;

	_initBuffer();
}

Environment::Environment(const Environment& environment, const osg::CopyOp& copyop):
osg::StateAttribute(environment, copyop),
_specularMap(environment._specularMap),
_diffuseMap(environment._diffuseMap),
_brdfLUT(environment._brdfLUT),
_bakeRoot(environment._bakeRoot),
_rotation(environment._rotation),
_maxSpecularMip(environment._maxSpecularMip),
_diffuseIntensity(environment._diffuseIntensity),
_specularIntensity(environment._specularIntensity) {
	_initBuffer();
}

Environment::~Environment() {}

// Built once (not per-write) so every setter can update it in place via dirty(). DYNAMIC because
// every setter mutates state the draw traversal reads, same as osgx::LightSet.
void Environment::_initBuffer() {
	setDataVariance(osg::Object::DYNAMIC);

	_inputs = osgx::make_ref<osgx::FloatArray>(ENVIRONMENT_FLOATS);
	_inputs->setBufferObject(new osg::UniformBufferObject());

	// Index 0 until apply() resolves the "osgx::environment" slot.
	_binding = new osg::UniformBufferBinding(
		0, _inputs, 0, static_cast<GLsizeiptr>(_inputs->getTotalDataSize())
	);

	_writeInputs();
}

// The shared LUT's camera is non-null only the first time a size is requested in this process;
// it then has to render once, so it joins the bake root.
void Environment::_initLUT(int lutSize) {
	auto lut = SharedBRDFLUT::create(lutSize);

	_brdfLUT = lut.texture;

	if(!lut.camera) return;

	if(!_bakeRoot) _bakeRoot = osgx::make_nref<osg::Group>("osgx_EnvironmentBake");

	_bakeRoot->addChild(lut.camera);
}

// Field order must match ENVIRONMENT_INPUTS' `osgx_EnvironmentInputs` std140 block exactly. Each
// axis row is the bake basis row rotated by _rotation: dot(R * b, W) == dot(b, R^-1 * W), i.e. the
// environment rotated by _rotation in world space.
void Environment::_writeInputs() {
	const osg::Vec3 axis[3] = {
		_rotation * ENVIRONMENT_BAKE_BASIS[0],
		_rotation * ENVIRONMENT_BAKE_BASIS[1],
		_rotation * ENVIRONMENT_BAKE_BASIS[2]
	};

	_inputs->set({
		axis[0].x(), axis[0].y(), axis[0].z(), 0.0f,
		axis[1].x(), axis[1].y(), axis[1].z(), 0.0f,
		axis[2].x(), axis[2].y(), axis[2].z(), 0.0f,
		_maxSpecularMip,
		_diffuseIntensity,
		_specularIntensity,
		0.0f
	});

	_inputs->dirty();
}

int Environment::compare(const osg::StateAttribute& sa) const {
	COMPARE_StateAttribute_Types(Environment, sa)

	COMPARE_StateAttribute_Parameter(_specularMap)
	COMPARE_StateAttribute_Parameter(_diffuseMap)
	COMPARE_StateAttribute_Parameter(_brdfLUT)
	COMPARE_StateAttribute_Parameter(_rotation)
	COMPARE_StateAttribute_Parameter(_maxSpecularMip)
	COMPARE_StateAttribute_Parameter(_diffuseIntensity)
	COMPARE_StateAttribute_Parameter(_specularIntensity)

	return 0;
}

// Read-only over this object's state, same as osgx::Material::apply().
//
// Textures are bound directly (active unit + Texture::apply()) rather than through
// State::applyTextureAttribute(). The latter records them in State's per-unit texture stacks as
// "changed"; on the next drawable whose StateSets don't list a texture on those units, State resets
// them to the global default (an empty texture), while this attribute - unchanged - is not
// re-applied. Binding outside that bookkeeping means the "osgx::environment.*" units belong to
// Environment: nothing else should bind textures there through a StateSet.
void Environment::apply(osg::State& state) const {
	resolveBinding(_bindingResolved, _binding.get(), "osgx::environment");

	const auto bind = [&state](unsigned int unit, const osg::Texture* texture) {
		if(!texture) return;

		state.setActiveTextureUnit(unit);

		texture->apply(state);
	};

	std::call_once(_unitsResolved, [this]() {
		auto& bindings = Library::instance().bindings();

		_units = {
			bindings.get("osgx::environment.specular"),
			bindings.get("osgx::environment.brdfLUT"),
			bindings.get("osgx::environment.diffuse")
		};
	});

	bind(_units[0], _specularMap.get());
	bind(_units[1], _brdfLUT.get());
	bind(_units[2], _diffuseMap.get());

	state.applyAttribute(_binding.get());
}

// The prefiltered specular map's small mips (1x1/2x2 per face) show hard face edges without
// seamless filtering; declaring the mode here lets setAttributeAndModes() enable it.
bool Environment::getModeUsage(osg::StateAttribute::ModeUsage& usage) const {
	usage.usesMode(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	return true;
}

void Environment::setRotation(const osg::Quat& rotation) {
	_rotation = rotation;

	_writeInputs();
}

void Environment::setSpecularMap(osg::TextureCubeMap* specularMap, float maxSpecularMip) {
	_specularMap = specularMap;
	_maxSpecularMip = maxSpecularMip < 0.0f ? lastMipLevel(specularMap) : maxSpecularMip;

	_writeInputs();
}

void Environment::setDiffuseMap(osg::TextureCubeMap* diffuseMap) {
	_diffuseMap = diffuseMap;
}

void Environment::setMaxSpecularMip(float maxSpecularMip) {
	_maxSpecularMip = maxSpecularMip;

	_writeInputs();
}

void Environment::setDiffuseIntensity(float intensity) {
	_diffuseIntensity = intensity;

	_writeInputs();
}

void Environment::setSpecularIntensity(float intensity) {
	_specularIntensity = intensity;

	_writeInputs();
}

void registerEnvironmentShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"ENVIRONMENT_INPUTS", "osgx_EnvironmentInputs", ENVIRONMENT_INPUTS},
		{"ENVIRONMENT_SAMPLE", "osgx_EnvironmentSpecular", ENVIRONMENT_SAMPLE},
		{"ENVIRONMENT_LIGHTING", "osgx_EvaluateEnvironment", ENVIRONMENT_LIGHTING}
	};
	::osgx::registerShaderLibs("osgx::environment", libs);
}

}
