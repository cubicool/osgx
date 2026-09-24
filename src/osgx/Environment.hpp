#pragma once

#include "Array.hpp"
#include "GGXPrefilter.hpp"
#include "LambertianBake.hpp"
#include "Shader.hpp"
#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Group>
#include <osg/Image>
#include <osg/Quat>
#include <osg/StateAttribute>
#include <osg/Texture2D>
#include <osg/TextureCubeMap>

OSGX_ENABLE_WARNINGS

#include <array>

namespace osg {
	class ShaderStorageBufferBinding;
}

namespace osgx {

// ================================================================================================
// Distant image-based lighting
//
// osgx::Environment is the third lighting StateAttribute alongside osgx::Material (surface
// response, PBR.hpp) and osgx::LightSet (direct lights, Light.hpp): it owns a prefiltered specular
// cubemap, a diffuse irradiance cubemap, and the split-sum BRDF LUT, plus the small amount of data
// needed to sample them correctly (orientation, roughness-to-mip mapping, intensities). Attaching
// it with setAttributeAndModes() binds all of that and enables seamless cubemap filtering.
//
// The GLSL side mirrors Light.hpp's LIGHT_SAMPLE split: ENVIRONMENT_SAMPLE is Material-free (any
// shader can read environment radiance/irradiance), and ENVIRONMENT_LIGHTING combines it with an
// osgx_Material for PBR.
// ================================================================================================

// Binding point for ENVIRONMENT_INPUTS' osgx_EnvironmentInputs block below. Chosen to avoid both
// osgx's other buffers (Material 0, joints 2, LightSet 3, Grid 4, SDF 5) and osgSlug's (0, 1, 2),
// since an osgSlug hook shader can declare both in one program.
inline constexpr unsigned int ENVIRONMENT_BINDING = 6;

// Texture units Environment::apply() binds its three textures at, matching the `layout(binding)`
// values in ENVIRONMENT_INPUTS. Reserved: apply() binds them outside osg::State's per-unit texture
// tracking (see Environment::apply()), so nothing else should use these units via a StateSet.
inline constexpr int ENVIRONMENT_SPECULAR_TEXTURE_UNIT = 5;
inline constexpr int ENVIRONMENT_BRDF_LUT_TEXTURE_UNIT = 6;
inline constexpr int ENVIRONMENT_DIFFUSE_TEXTURE_UNIT = 7;

// The bake convention (GGXPrefilter.cpp/LambertianBake.cpp): world (Z-up) direction W is stored at
// cube direction (W.x, W.z, -W.y), i.e. lookup = (dot(W, row0), dot(W, row1), dot(W, row2)) with
// these rows. Environment's uploaded axis rows are these rows rotated by its rotation.
inline const std::array<osg::Vec3, 3> ENVIRONMENT_BAKE_BASIS{
	osg::Vec3(1.0f, 0.0f, 0.0f),
	osg::Vec3(0.0f, 0.0f, 1.0f),
	osg::Vec3(0.0f, -1.0f, 0.0f)
};

// Size, in 4-byte floats, of the osgx_EnvironmentInputs block below.
inline constexpr std::size_t ENVIRONMENT_FLOATS = 16;

// Every hardcoded binding here must match its C++ constant above.
//
// `axis` maps a world-space (Z-up) direction to the cubemap lookup direction:
// lookup = (dot(d, axis[0]), dot(d, axis[1]), dot(d, axis[2])). Environment computes the rows from
// the bake convention and its own rotation; shaders never build them.
//
// Packed layout (std430; 16 floats / 64 bytes - must match Environment::_writeInputs()):
//   vec4  axis[3]              floats  0-11  (xyz used, w = 0)
//   float maxSpecularMip              12    (mip level holding roughness 1.0)
//   float diffuseIntensity            13
//   float specularIntensity           14
//   float _pad0                       15
inline constexpr const char* ENVIRONMENT_INPUTS = R"GLSL(
layout(std430, binding = 6) readonly buffer osgx_EnvironmentInputs {
	vec4 axis[3];
	float maxSpecularMip;
	float diffuseIntensity;
	float specularIntensity;
	float _pad0;
} osgx_environment;

layout(binding = 5) uniform samplerCube osgx_environmentSpecularMap;
layout(binding = 6) uniform sampler2D osgx_environmentBRDFLUT;
layout(binding = 7) uniform samplerCube osgx_environmentDiffuseMap;
)GLSL";

// Material-free environment sampling. `dir`/`N`/`R` are world-space (Z-up), unnormalized is fine
// for the cubemap lookups. Intensities are already applied.
//
// - osgx_EnvironmentDirection(): world direction -> cubemap lookup direction.
// - osgx_EnvironmentSpecular(): prefiltered radiance along R for a GGX lobe of `roughness`.
// - osgx_EnvironmentIrradiance(): cosine-weighted irradiance around N, pre-divided by PI (multiply
//   by albedo for Lambertian diffuse).
// - osgx_EnvironmentBRDF(): split-sum (scale, bias) for NdotV/roughness.
//
// Requires ENVIRONMENT_INPUTS already in scope.
inline constexpr const char* ENVIRONMENT_SAMPLE = R"GLSL(
vec3 osgx_EnvironmentDirection(vec3 dir) {
	return vec3(
		dot(dir, osgx_environment.axis[0].xyz),
		dot(dir, osgx_environment.axis[1].xyz),
		dot(dir, osgx_environment.axis[2].xyz)
	);
}

vec3 osgx_EnvironmentSpecular(vec3 R, float roughness) {
	float lod = clamp(roughness, 0.0, 1.0) * osgx_environment.maxSpecularMip;

	return textureLod(osgx_environmentSpecularMap, osgx_EnvironmentDirection(R), lod).rgb
		* osgx_environment.specularIntensity
	;
}

vec3 osgx_EnvironmentIrradiance(vec3 N) {
	return texture(osgx_environmentDiffuseMap, osgx_EnvironmentDirection(N)).rgb
		* osgx_environment.diffuseIntensity
	;
}

vec2 osgx_EnvironmentBRDF(float NdotV, float roughness) {
	return texture(osgx_environmentBRDFLUT, clamp(vec2(NdotV, roughness), 0.0, 1.0)).rg;
}
)GLSL";

// Diffuse + specular environment lighting for an osgx_Material. `N`/`V` are world-space (Z-up).
//
// Fresnel is evaluated twice (dielectric F0 = 0.04, metal F0 = albedo) and mixed by metallic
// AFTER evaluation, matching the Khronos glTF-Sample-Viewer: osgx_F_MultiScatter() is nonlinear
// in F0, so evaluating it once on a pre-mixed F0 diverges on partially metallic materials at
// grazing angles/higher roughness.
//
// Requires MATERIAL_STRUCT and F_MULTISCATTER ("osgx::pbr") plus ENVIRONMENT_INPUTS and
// ENVIRONMENT_SAMPLE already in scope.
inline constexpr const char* ENVIRONMENT_LIGHTING = R"GLSL(
struct osgx_EnvironmentLight {
	vec3 diffuse;
	vec3 specular;
};

osgx_EnvironmentLight osgx_EvaluateEnvironment(osgx_Material mat, vec3 N, vec3 V) {
	osgx_EnvironmentLight result;

	vec3 R = reflect(-V, N);
	vec3 Fd = osgx_F_MultiScatter(N, V, mat.roughness, vec3(0.04), osgx_environmentBRDFLUT);
	vec3 Fm = osgx_F_MultiScatter(N, V, mat.roughness, mat.albedo, osgx_environmentBRDFLUT);
	vec3 kD = (1.0 - Fd) * (1.0 - mat.metallic);

	result.diffuse = osgx_EnvironmentIrradiance(N) * mat.albedo * kD * mat.ao;
	result.specular = osgx_EnvironmentSpecular(R, mat.roughness) * mix(Fd, Fm, mat.metallic) * mat.ao;

	return result;
}
)GLSL";

// Environment's bake-from-HDR constructor options. Namespace-scope rather than nested so it can be
// a default argument inside Environment's own class definition.
struct EnvironmentBakeOptions {
	EnvironmentBakeOptions() { specular.prefilterSize = 256; }

	GGXPrefilterOptions specular;
	LambertianBakeOptions diffuse;
	int lutSize = 1024;

	// False skips the GGX specular bake: the specular map is a 1x1 placeholder until
	// Environment::setSpecularMap() supplies one (e.g. a live-rebaked probe).
	bool bakeSpecular = true;
};

// Distant image-based lighting as one StateAttribute. See the file comment above.
//
// Construct from an equirectangular HDR image (live GPU bakes) or from already-made cubemaps
// (e.g. loaded with osgx::loadPrefilterCubemap()). File loading is deliberately not part of this
// class. The BRDF LUT is always osgx::SharedBRDFLUT: it depends only on `lutSize`, never on the
// environment.
//
// Bakes are frame-driven: textures are valid and bindable immediately, but their contents are only
// correct once getBakeRoot()'s passes have rendered (add it to a rendered scene graph). getBakeRoot()
// is null when there is nothing to bake.
//
// CAPABILITY/member 2 (Material is 0, LightSet is 1) - see Material's class comment (PBR.hpp) for
// why the (Type, member) pair must be unique per custom attribute.
class Environment: public osg::StateAttribute {
	public:
		static constexpr Type ENVIRONMENT_TYPE = CAPABILITY;
		static constexpr unsigned int ENVIRONMENT_MEMBER = 2;

		Environment();

		// Bakes specular (GGXPrefilterScene, unless options.bakeSpecular is false) and diffuse
		// (LambertianBakeScene) cubemaps from `equirectangularHDR`. maxSpecularMip is the bake's
		// last mip level.
		explicit Environment(
			osg::Image* equirectangularHDR,
			const EnvironmentBakeOptions& options=EnvironmentBakeOptions()
		);

		// Wraps existing cubemaps. `maxSpecularMip` is the mip level of `specularMap` holding
		// roughness 1.0; a negative value means "its last level". Khronos-style KTX2 prefilters
		// carry one extra terminal level, so pass (levels - 2) for those.
		Environment(
			osg::TextureCubeMap* specularMap,
			osg::TextureCubeMap* diffuseMap,
			float maxSpecularMip=-1.0f,
			int lutSize=1024
		);

		// Same, with a caller-supplied BRDF LUT (e.g. one loaded from a file) instead of
		// osgx::SharedBRDFLUT. The caller owns making sure `brdfLUT`'s contents are ready.
		Environment(
			osg::TextureCubeMap* specularMap,
			osg::TextureCubeMap* diffuseMap,
			osg::Texture2D* brdfLUT,
			float maxSpecularMip=-1.0f
		);

		Environment(
			const Environment& environment,
			const osg::CopyOp& copyop=osg::CopyOp::SHALLOW_COPY
		);

		OSGX_META_StateAttribute(osgx, Environment, ENVIRONMENT_TYPE)

		unsigned int getMember() const override { return ENVIRONMENT_MEMBER; }
		int compare(const osg::StateAttribute& sa) const override;
		void apply(osg::State& state) const override;
		bool getModeUsage(osg::StateAttribute::ModeUsage& usage) const override;

		// Rotates the environment in world space. Identity is the equirect's own orientation
		// (the bake convention); e.g. osg::Quat(-PI/2, Z) matches the Khronos glTF-Sample-Viewer.
		void setRotation(const osg::Quat& rotation);
		const osg::Quat& getRotation() const { return _rotation; }

		void setMaxSpecularMip(float maxSpecularMip);
		float getMaxSpecularMip() const { return _maxSpecularMip; }

		void setDiffuseIntensity(float intensity);
		float getDiffuseIntensity() const { return _diffuseIntensity; }

		void setSpecularIntensity(float intensity);
		float getSpecularIntensity() const { return _specularIntensity; }

		// Replaces the specular map (e.g. with a freshly re-baked live probe). maxSpecularMip is
		// reset the same way the wrap constructor sets it: negative means the new map's last level.
		void setSpecularMap(osg::TextureCubeMap* specularMap, float maxSpecularMip=-1.0f);
		osg::TextureCubeMap* getSpecularMap() const { return _specularMap.get(); }

		void setDiffuseMap(osg::TextureCubeMap* diffuseMap);
		osg::TextureCubeMap* getDiffuseMap() const { return _diffuseMap.get(); }
		osg::Texture2D* getBRDFLUT() const { return _brdfLUT.get(); }

		// PRE_RENDER passes still populating a texture (live bakes and/or the first use of a LUT
		// size); null when nothing needs baking.
		osg::Group* getBakeRoot() const { return _bakeRoot.get(); }

	protected:
		virtual ~Environment();

	private:
		void _initBuffer();
		void _initLUT(int lutSize);
		void _writeInputs();

		osg::ref_ptr<osg::TextureCubeMap> _specularMap;
		osg::ref_ptr<osg::TextureCubeMap> _diffuseMap;
		osg::ref_ptr<osg::Texture2D> _brdfLUT;
		osg::ref_ptr<osg::Group> _bakeRoot;

		osg::Quat _rotation;
		float _maxSpecularMip = 0.0f;
		float _diffuseIntensity = 1.0f;
		float _specularIntensity = 1.0f;

		osg::ref_ptr<osgx::FloatArray> _inputs;
		osg::ref_ptr<osg::ShaderStorageBufferBinding> _binding;
};

// GLSL `#pragma osgx::environment` catalog registration (ENVIRONMENT_INPUTS, ENVIRONMENT_SAMPLE,
// ENVIRONMENT_LIGHTING) - see registerShaderLibs()/resolveShaderLibs() in Shader.hpp.
void registerEnvironmentShaderLibs();

}
