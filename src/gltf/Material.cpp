#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include "tiny_gltf_v3.h"

OSGX_ENABLE_WARNINGS

#include "Material.hpp"
#include "Log.hpp"
#include "tg3_util.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Math>
#include <osg/Uniform>

#include <osgDB/Options>

OSGX_ENABLE_WARNINGS

#include "osgx/PBR.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace osgx::gltf::detail {

namespace {

// KHR_materials_pbrSpecularGlossiness value-tree decode helpers - same shape as Json.hpp's
// decodeString/decodeInt for the manifest formats, just against tg3_value (glTF extension values)
// instead of tinygltf_json_c.h's tg3json_value.
double extNumber(const tg3_value& value, const char* key, double fallback) {
	const tg3_value* found = tg3_object_get(value, key);
	double result = fallback;

	return found && tg3_as_double(*found, result) ? result : fallback;
}

// Reads a fixed-length numeric array field (e.g. "diffuseFactor": [r,g,b,a]) into `out`.
// Leaves `out` untouched if the field is absent, not an array, or the wrong length.
void extVec(const tg3_value& value, const char* key, float* out, std::uint32_t count) {
	const tg3_value* found = tg3_object_get(value, key);

	if(!found || found->type != TG3_VALUE_ARRAY || found->array_count != count) return;

	for(std::uint32_t i = 0; i < count; i++) {
		double component;

		if(tg3_as_double(found->array_data[i], component)) out[i] = static_cast<float>(component);
	}
}

}

MaterialBuilder::MaterialBuilder(
	const tg3_model& model,
	const std::string& referrer,
	const osgDB::Options* readOptions,
	TextureLoader& textureLoader
):
_model(model),
_env{referrer, readOptions},
_textureLoader(textureLoader) {}

void MaterialBuilder::applyMaterial(
	int matIdx,
	osg::Vec4& baseColorFactor,
	osg::Geometry* geom,
	const std::map<int, osg::Array*>& texCoordSets
) const {
	// A primitive without a material uses glTF's defined default material;
	// it must still receive this loader's material buffer and alpha state.
	// Returning early here previously left it with inherited/undefined state.
	const tg3_material* material = &tg3_default_material();

	if(matIdx >= 0) {
		const std::uint32_t materialIndex = static_cast<std::uint32_t>(matIdx);

		if(materialIndex >= _model.materials_count) return;

		material = &_model.materials[materialIndex];
	}

	const tg3_material& mat = *material;
	const auto& pbr = mat.pbr_metallic_roughness;

	// One osgx::Material StateAttribute collects every map/factor this function decides on below;
	// it's applied to the StateSet exactly once, at the very end, replacing what used to be up to
	// four separate setTextureAttributeAndModes() calls plus a trailing osgx::attachMaterialFactors()
	// - and, unlike that split, has*Map can no longer disagree with what's actually bound: it's
	// osgx::Material's own texture ref_ptrs, checked at apply() time, not a separately-tracked bool.
	osg::ref_ptr<osgx::Material> materialAttr = new osgx::Material();

	// sRGB: per the glTF spec, baseColor/diffuse and emissive textures
	// are authored in sRGB gamma space; normal and ORM (occlusion/
	// roughness/metallic) textures are linear data, not color, and
	// must never be gamma-decoded.
	// `channel` names the map (its osgx::*_UV_CHANNEL) and is where its UV set goes.
	auto bindTexture = [&](unsigned int channel, int texIdx, int texCoord, bool sRGB) {
		osg::Texture2D* tex = _textureLoader.getOrCreateTexture(texIdx, sRGB);

		if(!tex) return;

		if(channel == osgx::BASE_COLOR_UV_CHANNEL) materialAttr->setBaseColorMap(tex);
		else if(channel == osgx::NORMAL_UV_CHANNEL) materialAttr->setNormalMap(tex);
		else if(channel == osgx::ORM_UV_CHANNEL) materialAttr->setMetallicRoughnessMap(tex);
		else if(channel == osgx::EMISSIVE_UV_CHANNEL) materialAttr->setEmissiveMap(tex);

		auto it = texCoordSets.find(texCoord);

		if(it != texCoordSets.end()) geom->setTexCoordArray(channel, it->second);
	};

	// base_color_factor is now a fixed-size array, always populated with the spec default
	// ({1,1,1,1}) by the parser (or by tg3_default_material() above) - no presence check needed.
	baseColorFactor.set(
		static_cast<float>(pbr.base_color_factor[0]),
		static_cast<float>(pbr.base_color_factor[1]),
		static_cast<float>(pbr.base_color_factor[2]),
		static_cast<float>(pbr.base_color_factor[3])
	);

	bool haveCoreBaseColor = pbr.base_color_texture.index >= 0;

	if(haveCoreBaseColor) bindTexture(
		osgx::BASE_COLOR_UV_CHANNEL,
		pbr.base_color_texture.index,
		pbr.base_color_texture.tex_coord,
		true
	);

	bool haveNormalMap = mat.normal_texture.index >= 0;

	if(haveNormalMap) bindTexture(
		osgx::NORMAL_UV_CHANNEL,
		mat.normal_texture.index,
		mat.normal_texture.tex_coord,
		false
	);

	// metallicRoughnessTexture and occlusionTexture are often the same
	// image (R=occlusion, G=roughness, B=metallic) - when they share
	// the same texture index, the plain bind below already carries
	// correct AO in R. When occlusionTexture is a genuinely separate
	// image (e.g. SciFiHelmet), bake the two together so real
	// per-pixel AO isn't silently dropped - downstream shaders
	// (09-ibl.py) gate their AO read on the hasOcclusion uniform
	// (exported below) rather than trusting an "unused" R channel
	// when no occlusionTexture is present at all.
	bool haveOcclusion = mat.occlusion_texture.index >= 0;
	bool sameOcclusionImage =
		haveOcclusion &&
		mat.occlusion_texture.index == pbr.metallic_roughness_texture.index
	;

	if(haveOcclusion && !sameOcclusionImage) {
		std::string bakeKey = _env._referrer + "|orm-occlusion|" + std::to_string(matIdx);
		osg::ref_ptr<osg::Texture2D> ormTex = _textureLoader.findCached(bakeKey);

		if(!ormTex.valid()) {
			osg::ref_ptr<osg::Image> occImg =
				_textureLoader.loadRawImage(mat.occlusion_texture.index);
			osg::ref_ptr<osg::Image> mrImg =
				_textureLoader.loadRawImage(pbr.metallic_roughness_texture.index);

			if(mat.occlusion_texture.tex_coord != pbr.metallic_roughness_texture.tex_coord) {
				GLTF_NOTIFY(3)
					<< "material " << matIdx
					<< ": occlusionTexture and metallicRoughnessTexture use"
					<< " different UV sets - occlusion bake assumes they"
					<< " share the same UV space; result may be UV-mismatched" << std::endl
				;
			}

			osg::ref_ptr<osg::Image> bakedOrm;

			_bakeOcclusionIntoOrm(
				occImg,
				static_cast<float>(mat.occlusion_texture.strength),
				mrImg,
				bakedOrm
			);

			int samplerIdx = -1;

			if(
				pbr.metallic_roughness_texture.index >= 0 &&
				static_cast<std::uint32_t>(pbr.metallic_roughness_texture.index) < _model.textures_count
			) samplerIdx = _model.textures[
				static_cast<std::uint32_t>(pbr.metallic_roughness_texture.index)
			].sampler;

			else if(static_cast<std::uint32_t>(mat.occlusion_texture.index) < _model.textures_count)
				samplerIdx = _model.textures[
					static_cast<std::uint32_t>(mat.occlusion_texture.index)
				].sampler;

			ormTex = new osg::Texture2D(bakedOrm);

			_textureLoader.applyFormatAndSampler(ormTex, bakedOrm, false, samplerIdx);

			ormTex->setUnRefImageDataAfterApply(true);

			_textureLoader.cache(bakeKey, ormTex);
		}

		materialAttr->setMetallicRoughnessMap(ormTex);

		auto occTexCoordIt = texCoordSets.find(mat.occlusion_texture.tex_coord);

		if(occTexCoordIt != texCoordSets.end()) geom->setTexCoordArray(
			osgx::ORM_UV_CHANNEL,
			occTexCoordIt->second
		);
	}

	else if(pbr.metallic_roughness_texture.index >= 0) bindTexture(
		osgx::ORM_UV_CHANNEL,
		pbr.metallic_roughness_texture.index,
		pbr.metallic_roughness_texture.tex_coord,
		false
	);

	if(mat.emissive_texture.index >= 0) bindTexture(
		osgx::EMISSIVE_UV_CHANNEL,
		mat.emissive_texture.index,
		mat.emissive_texture.tex_coord,
		true
	);

	// KHR_materials_pbrSpecularGlossiness - legacy but still valid,
	// real Sketchfab-era content uses it, sometimes extension-only
	// with no core pbrMetallicRoughness fallback. Converted to the
	// core metallic-roughness workflow at load time (see
	// _bakeSpecGlossToMetalRough) rather than binding
	// specularGlossinessTexture straight into the ORM slot - that
	// texture's channels (RGB=specular color/F0, A=glossiness) don't
	// mean the same thing as ORM's (R=AO, G=roughness, B=metallic),
	// so a direct bind previously fed the shader's Cook-Torrance path
	// garbage roughness/metallic values.
	if(!haveCoreBaseColor) {
		const tg3_extension* ext =
			tg3_find_extension(mat.ext, "KHR_materials_pbrSpecularGlossiness");

		if(ext) {
			const tg3_value& sg = ext->value;

			osg::Vec4 diffuseFactor(1, 1, 1, 1);

			extVec(sg, "diffuseFactor", diffuseFactor.ptr(), 4);

			baseColorFactor = diffuseFactor;

			osg::Vec3 specularFactor(1, 1, 1);

			extVec(sg, "specularFactor", specularFactor.ptr(), 3);

			float glossinessFactor = static_cast<float>(extNumber(sg, "glossinessFactor", 1.0));

			int diffuseIdx = -1, diffuseTexCoord = 0;

			if(const tg3_value* dt = tg3_object_get(sg, "diffuseTexture")) {
				diffuseIdx = static_cast<int>(extNumber(*dt, "index", -1.0));
				diffuseTexCoord = static_cast<int>(extNumber(*dt, "texCoord", 0.0));
			}

			int specGlossIdx = -1, specGlossTexCoord = 0;

			if(const tg3_value* sgt = tg3_object_get(sg, "specularGlossinessTexture")) {
				specGlossIdx = static_cast<int>(extNumber(*sgt, "index", -1.0));
				specGlossTexCoord = static_cast<int>(extNumber(*sgt, "texCoord", 0.0));
			}

			// gltfSkipSpecGlossBake opts out of the metal-rough bake
			// below and falls back to a raw pass-through of
			// diffuseTexture/specularGlossinessTexture, for callers
			// that implement their own spec-gloss BRDF shader branch
			// (Sketchfab's own viewer takes this approach - its
			// Model Inspector panel lists Albedo/Specular/Glossiness
			// as native channels, not a converted metallic-roughness
			// pair) and would rather skip the bake's load-time cost
			// and lossy conversion entirely.
			bool skipSpecGlossBake =
				_env._readOptions &&
				_env._readOptions->getOptionString().find("gltfSkipSpecGlossBake") != std::string::npos
			;

			if(skipSpecGlossBake) {
				if(diffuseIdx >= 0) bindTexture(
					osgx::BASE_COLOR_UV_CHANNEL,
					diffuseIdx,
					diffuseTexCoord,
					true
				);
				if(specGlossIdx >= 0) bindTexture(
					osgx::ORM_UV_CHANNEL,
					specGlossIdx,
					specGlossTexCoord,
					true
				);
			}

			else {
				// Every primitive that references this material would
				// otherwise redo the full per-pixel bake from
				// scratch - for a mesh whose parts all share one
				// material (the common case), that's an N-way
				// redundant multi-second cost for identical output.
				// Cache by referrer+matIdx, same TextureCache the
				// texIdx-keyed path already uses.
				std::string bakeKey = _env._referrer + "|specgloss|" + std::to_string(matIdx);
				osg::ref_ptr<osg::Texture2D> bcTex =
					_textureLoader.findCached(bakeKey + "|bc");
				osg::ref_ptr<osg::Texture2D> ormTex =
					_textureLoader.findCached(bakeKey + "|orm");

				if(!bcTex.valid() || !ormTex.valid()) {
					// The bake combines diffuse and specGloss pixel-
					// for-pixel, which only makes sense if both
					// textures share the same UV layout. texCoord
					// index alone doesn't prove that, but it's the
					// cheapest signal we have without comparing UV
					// accessor data - warn instead of silently
					// mis-rendering.
					if(
						diffuseIdx >= 0 &&
						specGlossIdx >= 0 &&
						diffuseTexCoord != specGlossTexCoord
					) {
						GLTF_NOTIFY(3)
							<< "material " << matIdx
							<< ": diffuseTexture and specularGlossinessTexture use"
							" different UV sets (" << diffuseTexCoord << " vs "
							<< specGlossTexCoord << ") - spec-gloss bake assumes they"
							" share the same UV space; result may be UV-mismatched"
							<< std::endl
						;
					}

					osg::ref_ptr<osg::Image> diffuseImg =
						_textureLoader.loadRawImage(diffuseIdx);
					osg::ref_ptr<osg::Image> specGlossImg =
						_textureLoader.loadRawImage(specGlossIdx);
					osg::ref_ptr<osg::Image> bakedBaseColor, bakedOrm;

					// Hint for next time someone sees a multi-second stall between
					// "building nodes" ticks: this CPU per-pixel bake (single-threaded,
					// no SIMD) is the culprit, not I/O or a hang. Cost scales with the
					// larger of diffuseImg/specGlossImg - a 2048x2048 pair alone runs
					// ~2s. TODO: row-parallelize (std::thread/OpenMP) once this becomes
					// a real bottleneck for more assets.
					GLTF_NOTIFY(3)
						<< "material " << matIdx << ": baking specGloss -> metal-rough ("
						<< (diffuseImg.valid() ? diffuseImg->s() : 0) << "x"
						<< (diffuseImg.valid() ? diffuseImg->t() : 0) << " diffuse, "
						<< (specGlossImg.valid() ? specGlossImg->s() : 0) << "x"
						<< (specGlossImg.valid() ? specGlossImg->t() : 0) << " specGloss)"
						" - single-threaded CPU bake, may take a moment" << std::endl
					;

					_bakeSpecGlossToMetalRough(
						diffuseImg,
						diffuseFactor,
						specGlossImg,
						specularFactor,
						glossinessFactor,
						bakedBaseColor,
						bakedOrm
					);

					int samplerIdx = -1;

					if(diffuseIdx >= 0 && static_cast<std::uint32_t>(diffuseIdx) < _model.textures_count)
						samplerIdx = _model.textures[static_cast<std::uint32_t>(diffuseIdx)].sampler;

					else if(
						specGlossIdx >= 0 &&
						static_cast<std::uint32_t>(specGlossIdx) < _model.textures_count
					)
						samplerIdx = _model.textures[static_cast<std::uint32_t>(specGlossIdx)].sampler;

					// Baked images are already linear (converted, not
					// merely re-encoded), so bind them with
					// sRGB=false - GPU sRGB decode must not run twice.
					bcTex = new osg::Texture2D(bakedBaseColor);

					_textureLoader.applyFormatAndSampler(
						bcTex,
						bakedBaseColor,
						false,
						samplerIdx
					);
					bcTex->setUnRefImageDataAfterApply(true);

					ormTex = new osg::Texture2D(bakedOrm);

					_textureLoader.applyFormatAndSampler(
						ormTex,
						bakedOrm,
						false,
						samplerIdx
					);
					ormTex->setUnRefImageDataAfterApply(true);

					_textureLoader.cache(bakeKey + "|bc", bcTex);
					_textureLoader.cache(bakeKey + "|orm", ormTex);
				}

				// The bake always produces a real (at-least-1x1) baseColor + ORM image even for
				// factor-only spec-gloss materials (see _bakeSpecGlossToMetalRough's comment), so
				// both setters above always receive a genuine texture, regardless of what the core
				// pbrMetallicRoughness JSON block did or didn't declare - materialAttr's has*Map
				// flags (derived from these same ref_ptrs at apply() time) come along for free.
				materialAttr->setBaseColorMap(bcTex);
				materialAttr->setMetallicRoughnessMap(ormTex);

				int bakeTexCoord = (diffuseIdx >= 0) ? diffuseTexCoord : specGlossTexCoord;
				auto texCoordIt = texCoordSets.find(bakeTexCoord);

				if(texCoordIt != texCoordSets.end()) {
					geom->setTexCoordArray(
						osgx::BASE_COLOR_UV_CHANNEL,
						texCoordIt->second
					);
					geom->setTexCoordArray(
						osgx::ORM_UV_CHANNEL,
						texCoordIt->second
					);
				}

				// metallicFactor/roughnessFactor uniforms (added
				// below) default to 1.0 for extension-only materials
				// - tinygltf always populates pbrMetallicRoughness
				// with spec defaults even without a core JSON block
				// present, so the baked-in per-pixel metallic/
				// roughness above won't get double-multiplied.
			}
		}
	}

	// Finish and apply materialAttr as a single osgx_gltf_Material buffer for downstream PBR
	// shaders (e.g. pyosg-lighting/09-ibl.py) instead of one osg::Uniform per field - this is
	// this plugin's own extension to the material interface, not part of OSG's osg_*
	// built-in uniform set, so it's namespaced (block name + binding) to avoid colliding
	// with an unrelated shader's own material uniforms.
	//
	// hasBaseColorMap/hasMetallicRoughnessMap/hasNormalMap are no longer set here at all - they're
	// osgx::Material's own derived state, gating a factor-only material (no texture at all - e.g.
	// Fox's roughnessFactor=0.58 with no metallicRoughnessTexture) from having its authored factor
	// silently discarded by an unconditional texture() read of an unbound unit, same as before,
	// just computed from whichever setXMap() calls above actually landed a real texture rather than
	// from a separately-tracked bool that could disagree with that. hasOcclusion has no texture of
	// its own to derive from (see osgx::Material's class comment, PBR.hpp), so it's still explicit.
	materialAttr->setBaseColor(
		osg::Vec4(baseColorFactor.x(), baseColorFactor.y(), baseColorFactor.z(), baseColorFactor.w())
	);
	materialAttr->setRoughness(static_cast<float>(pbr.roughness_factor));
	materialAttr->setMetallic(static_cast<float>(pbr.metallic_factor));
	materialAttr->setHasOcclusion(haveOcclusion);

	// Alpha mode/cutoff and emissive factor are stored in osgx::Material's buffer
	// (MATERIAL_INPUTS, PBR.hpp) with every other factor. hasEmissiveMap is derived from
	// setEmissiveMap() above.
	auto alphaMode = osgx::Material::AlphaMode::Opaque;

	if(tg3_str_equals_cstr(mat.alpha_mode, "MASK")) alphaMode = osgx::Material::AlphaMode::Mask;
	else if(tg3_str_equals_cstr(mat.alpha_mode, "BLEND")) alphaMode = osgx::Material::AlphaMode::Blend;

	materialAttr->setAlphaMode(alphaMode);
	materialAttr->setAlphaCutoff(static_cast<float>(mat.alpha_cutoff));
	materialAttr->setEmissiveFactor(osg::Vec3(
		static_cast<float>(mat.emissive_factor[0]),
		static_cast<float>(mat.emissive_factor[1]),
		static_cast<float>(mat.emissive_factor[2])
	));

	auto* stateSet = geom->getOrCreateStateSet();

	stateSet->setAttributeAndModes(materialAttr.get());

	if(tg3_str_equals_cstr(mat.alpha_mode, "BLEND")) {
		// glTF BLEND uses conventional non-premultiplied source-over alpha.
		// Render it after opaque geometry and leave depth testing enabled while
		// preventing a transparent surface from occluding later transparent draws.
		stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
		stateSet->setAttributeAndModes(
			new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
			osg::StateAttribute::ON
		);
		stateSet->setAttributeAndModes(
			new osg::Depth(osg::Depth::LEQUAL, 0.0, 1.0, false),
			osg::StateAttribute::ON
		);
		stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
	}

	// Per the glTF spec, doubleSided disables backface culling for THIS
	// material specifically (thin single-sided sheets like capes/cloth/leaves
	// with no back geometry, meant to be visible and lit from both sides).
	// readTopNode() enables GL_CULL_FACE unconditionally at the root; this
	// per-geometry override on the child StateSet takes precedence over that
	// ancestor mode (no OVERRIDE flag was used at the root, so normal OSG
	// StateSet inheritance already lets a more-specific child win). The
	// matching back-face normal flip belongs in whatever fragment shader
	// consumes this geometry (gl_FrontFacing-based) - not this loader's
	// concern, since the loader doesn't ship its own PBR shader.
	if(mat.double_sided) {
		geom->getOrCreateStateSet()->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
	}
}

// KHR_materials_pbrSpecularGlossiness is legacy, but real Sketchfab-era content still uses
// it, sometimes extension-only with no core pbrMetallicRoughness fallback. Rather than give
// a shader a second BRDF path to maintain, convert to the core metallic-roughness workflow
// at load time using the standard reference formula from the (archived) extension spec /
// glTF-Sample-Viewer / three.js - the same approach those engines use when they don't
// carry a native spec-gloss BRDF. Must be per-pixel, not per-factor: real content (e.g.
// Sketchfab's "Dead Space" suit) carries per-part variation in the specular/glossiness
// texture, not just flat material factors.
float MaterialBuilder::_srgbToLinear(float c) {
	return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float MaterialBuilder::_solveMetallic(
	float diffuse,
	float specular,
	float oneMinusSpecularStrength
) {
	const float dielectricSpecular = 0.04f;

	if(specular < dielectricSpecular) return 0.0f;

	float a = dielectricSpecular;
	float b = diffuse * oneMinusSpecularStrength / (1.0f - dielectricSpecular) + specular - 2.0f * dielectricSpecular;
	float c = dielectricSpecular - specular;
	float D = b * b - 4.0f * a * c;

	if(D < 0.0f) return 0.0f;

	return osg::clampBetween((-b + std::sqrt(D)) / (2.0f * a), 0.0f, 1.0f);
}

// Bakes new baseColor (linear, for unit 0) + ORM-style (unit 2:
// R=AO placeholder - spec-gloss has no occlusion channel, G=roughness,
// B=metallic) images. Either source image may be null (factor-only
// material); output is always at least 1x1 so callers can bind
// unconditionally.
void MaterialBuilder::_bakeSpecGlossToMetalRough(
	osg::Image* diffuseImg,
	const osg::Vec4& diffuseFactor,
	osg::Image* specGlossImg,
	const osg::Vec3& specularFactor,
	float glossinessFactor,
	osg::ref_ptr<osg::Image>& outBaseColor,
	osg::ref_ptr<osg::Image>& outOrm
) const {
	const float epsilon = 1e-6f;
	const float dielectricSpecular = 0.04f;

	int w = 1, h = 1;

	if(diffuseImg) { w = std::max(w, diffuseImg->s()); h = std::max(h, diffuseImg->t()); }
	if(specGlossImg) { w = std::max(w, specGlossImg->s()); h = std::max(h, specGlossImg->t()); }

	osg::ref_ptr<osg::Image> diffuseR = diffuseImg;

	if(diffuseImg && (diffuseImg->s() != w || diffuseImg->t() != h)) {
		diffuseR = new osg::Image(*diffuseImg);

		diffuseR->scaleImage(w, h, 1);
	}

	osg::ref_ptr<osg::Image> specGlossR = specGlossImg;

	if(specGlossImg && (specGlossImg->s() != w || specGlossImg->t() != h)) {
		specGlossR = new osg::Image(*specGlossImg);

		specGlossR->scaleImage(w, h, 1);
	}

	const unsigned int width = static_cast<unsigned int>(w);
	const unsigned int height = static_cast<unsigned int>(h);
	auto* baseColorData = new unsigned char[static_cast<size_t>(width) * height * 4];
	auto* ormData = new unsigned char[static_cast<size_t>(width) * height * 3];

	for(unsigned int y = 0; y < height; y++) {
		for(unsigned int x = 0; x < width; x++) {
			osg::Vec4 dTex = diffuseR ? diffuseR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
			osg::Vec4 sgTex = specGlossR ? specGlossR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);

			// diffuse.rgb and specular.rgb are sRGB-encoded color;
			// glossiness (specGloss alpha) and the factors are linear.
			float dR = _srgbToLinear(dTex.x()) * diffuseFactor.x();
			float dG = _srgbToLinear(dTex.y()) * diffuseFactor.y();
			float dB = _srgbToLinear(dTex.z()) * diffuseFactor.z();
			float sR = _srgbToLinear(sgTex.x()) * specularFactor.x();
			float sG = _srgbToLinear(sgTex.y()) * specularFactor.y();
			float sB = _srgbToLinear(sgTex.z()) * specularFactor.z();
			float glossiness = sgTex.w() * glossinessFactor;

			float specularStrength = std::max(sR, std::max(sG, sB));
			float oneMinusSpecularStrength = 1.0f - specularStrength;
			float maxDiffuse = std::max(dR, std::max(dG, dB));
			float metallic = _solveMetallic(
				maxDiffuse,
				specularStrength,
				oneMinusSpecularStrength
			);

			float invOneMinusMetallic = 1.0f / std::max(1.0f - metallic, epsilon);
			float invMetallic = 1.0f / std::max(metallic, epsilon);
			float diffuseScale = oneMinusSpecularStrength / (1.0f - dielectricSpecular);

			float bcFromDiffuseR = dR * diffuseScale * invOneMinusMetallic;
			float bcFromDiffuseG = dG * diffuseScale * invOneMinusMetallic;
			float bcFromDiffuseB = dB * diffuseScale * invOneMinusMetallic;

			float bcFromSpecR = (sR - dielectricSpecular * (1.0f - metallic)) * invMetallic;
			float bcFromSpecG = (sG - dielectricSpecular * (1.0f - metallic)) * invMetallic;
			float bcFromSpecB = (sB - dielectricSpecular * (1.0f - metallic)) * invMetallic;

			float t = metallic * metallic;
			float baseR = osg::clampBetween(bcFromDiffuseR * (1.0f - t) + bcFromSpecR * t, 0.0f, 1.0f);
			float baseG = osg::clampBetween(bcFromDiffuseG * (1.0f - t) + bcFromSpecG * t, 0.0f, 1.0f);
			float baseB = osg::clampBetween(bcFromDiffuseB * (1.0f - t) + bcFromSpecB * t, 0.0f, 1.0f);
			float roughness = osg::clampBetween(1.0f - glossiness, 0.0f, 1.0f);

			size_t bi = (static_cast<size_t>(y) * width + x) * 4;
			baseColorData[bi + 0] = static_cast<unsigned char>(baseR * 255.0f + 0.5f);
			baseColorData[bi + 1] = static_cast<unsigned char>(baseG * 255.0f + 0.5f);
			baseColorData[bi + 2] = static_cast<unsigned char>(baseB * 255.0f + 0.5f);
			baseColorData[bi + 3] = static_cast<unsigned char>(dTex.w() * diffuseFactor.w() * 255.0f + 0.5f);

			size_t oi = (static_cast<size_t>(y) * width + x) * 3;
			ormData[oi + 0] = 255; // AO: spec-gloss carries no occlusion channel
			ormData[oi + 1] = static_cast<unsigned char>(roughness * 255.0f + 0.5f);
			ormData[oi + 2] = static_cast<unsigned char>(metallic * 255.0f + 0.5f);
		}
	}

	outBaseColor = new osg::Image();
	outBaseColor->setImage(
		w,
		h,
		1,
		GL_RGBA8,
		GL_RGBA,
		GL_UNSIGNED_BYTE,
		baseColorData,
		osg::Image::USE_NEW_DELETE
	);

	outOrm = new osg::Image();
	outOrm->setImage(
		w,
		h,
		1,
		GL_RGB8,
		GL_RGB,
		GL_UNSIGNED_BYTE,
		ormData,
		osg::Image::USE_NEW_DELETE
	);
}

// ---- separate occlusionTexture merge --------------------------- //
// Only needed when occlusionTexture is a genuinely distinct image
// from metallicRoughnessTexture (e.g. SciFiHelmet) - the common
// "packed ORM" convention (same texture index for both) already
// carries correct AO in R via the plain metallicRoughnessTexture
// bind and never reaches this function. `strength` is baked in
// directly per the spec formula (occludedColor = lerp(1, r,
// strength)) so no extra uniform is needed downstream.
void MaterialBuilder::_bakeOcclusionIntoOrm(
	osg::Image* occlusionImg,
	float strength,
	osg::Image* metalRoughImg,
	osg::ref_ptr<osg::Image>& outOrm
) const {
	int w = 1, h = 1;

	if(occlusionImg) {
		w = std::max(w, occlusionImg->s());
		h = std::max(h, occlusionImg->t());
	}

	if(metalRoughImg) {
		w = std::max(w, metalRoughImg->s());
		h = std::max(h, metalRoughImg->t());
	}

	osg::ref_ptr<osg::Image> occR = occlusionImg;

	if(occlusionImg && (occlusionImg->s() != w || occlusionImg->t() != h)) {
		occR = new osg::Image(*occlusionImg);

		occR->scaleImage(w, h, 1);
	}

	osg::ref_ptr<osg::Image> mrR = metalRoughImg;

	if(metalRoughImg && (metalRoughImg->s() != w || metalRoughImg->t() != h)) {
		mrR = new osg::Image(*metalRoughImg);

		mrR->scaleImage(w, h, 1);
	}

	const unsigned int width = static_cast<unsigned int>(w);
	const unsigned int height = static_cast<unsigned int>(h);
	auto* ormData = new unsigned char[static_cast<size_t>(width) * height * 3];

	for(unsigned int y = 0; y < height; y++) {
		for(unsigned int x = 0; x < width; x++) {
			osg::Vec4 occTex = occR ? occR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
			osg::Vec4 mrTex = mrR ? mrR->getColor(x, y) : osg::Vec4(1, 1, 1, 1);
			float ao = 1.0f + strength * (occTex.x() - 1.0f);
			size_t oi = (static_cast<size_t>(y) * width + x) * 3;

			ormData[oi + 0] = static_cast<unsigned char>(osg::clampBetween(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
			ormData[oi + 1] = static_cast<unsigned char>(mrTex.y() * 255.0f + 0.5f);
			ormData[oi + 2] = static_cast<unsigned char>(mrTex.z() * 255.0f + 0.5f);
		}
	}

	outOrm = new osg::Image();
	outOrm->setImage(
		w,
		h,
		1,
		GL_RGB8,
		GL_RGB,
		GL_UNSIGNED_BYTE,
		ormData,
		osg::Image::USE_NEW_DELETE
	);
}

}
