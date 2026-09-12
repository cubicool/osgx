#pragma once

#include "osgx/PBR.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Program>
#include <osg/StateSet>
#include <osg/Uniform>

OSGX_ENABLE_WARNINGS

namespace osgx::gltf::shader {

// Position, normal, color, and texture coordinates use OSG's conventional arrays. These are the
// additional generic vertex attributes populated by the glTF loader.
inline constexpr unsigned int TANGENT_ATTRIBUTE = 7;
inline constexpr unsigned int JOINT_INDICES_ATTRIBUTE = 8;
inline constexpr unsigned int JOINT_WEIGHTS_ATTRIBUTE = 9;

inline constexpr char TANGENT_ATTRIBUTE_NAME[] = "osg_Tangent";
inline constexpr char JOINT_INDICES_ATTRIBUTE_NAME[] = "osgx_gltf_JointIndices";
inline constexpr char JOINT_WEIGHTS_ATTRIBUTE_NAME[] = "osgx_gltf_JointWeights";

// Alias, not a separate constant - osgx::MATERIAL_BINDING (PBR.hpp) is now the canonical value,
// since the buffer it names (MATERIAL_INPUTS' `osgx_gltf_Material` block) has a glTF-independent
// C++-side builder too (osgx::Material). Keeping the name here means existing code under
// osgx::gltf::shader:: doesn't need to change, just what it points at.
inline constexpr unsigned int MATERIAL_BINDING = osgx::MATERIAL_BINDING;
inline constexpr unsigned int JOINT_MATRICES_BINDING = 2;

// Aliases, not separate constants - osgx::{BASE_COLOR,NORMAL,ORM,EMISSIVE}_TEXTURE_UNIT (PBR.hpp)
// are now the canonical values, since osgx::Material (also PBR.hpp) binds its four maps at these
// same units independent of glTF. Same reasoning as MATERIAL_BINDING just above.
inline constexpr int BASE_COLOR_TEXTURE_UNIT = osgx::BASE_COLOR_TEXTURE_UNIT;
inline constexpr int NORMAL_TEXTURE_UNIT = osgx::NORMAL_TEXTURE_UNIT;
inline constexpr int ORM_TEXTURE_UNIT = osgx::ORM_TEXTURE_UNIT;
inline constexpr int EMISSIVE_TEXTURE_UNIT = osgx::EMISSIVE_TEXTURE_UNIT;

inline constexpr char BASE_COLOR_SAMPLER[] = "osgx_gltf_textures.baseColor";
inline constexpr char NORMAL_SAMPLER[] = "osgx_gltf_textures.normal";
inline constexpr char ORM_SAMPLER[] = "osgx_gltf_textures.orm";
inline constexpr char EMISSIVE_SAMPLER[] = "osgx_gltf_textures.emissive";

inline constexpr char ALPHA_MODE_UNIFORM[] = "osgx_gltf_alphaMode";
inline constexpr char ALPHA_CUTOFF_UNIFORM[] = "osgx_gltf_alphaCutoff";

inline constexpr float ALPHA_MODE_OPAQUE = 0.0f;
inline constexpr float ALPHA_MODE_MASK = 1.0f;
inline constexpr float ALPHA_MODE_BLEND = 2.0f;

// Canonical GLSL declaration matching the data uploaded by the loader. osgx::gltf defines
// this interface but deliberately does not impose a particular material or lighting shader.
inline constexpr char MATERIAL_INPUTS[] = R"GLSL(
layout(std430, binding = 0) readonly buffer osgx_gltf_Material {
	vec4 baseColorFactor;
	float roughnessFactor;
	float metallicFactor;
	float hasBaseColorMap;
	float hasMetallicRoughnessMap;
	float hasOcclusion;
	float hasNormalMap;
} osgx_gltf_material;

struct GLTFTextures {
	sampler2D baseColor;
	sampler2D normal;
	sampler2D orm;
	sampler2D emissive;
};

uniform GLTFTextures osgx_gltf_textures;
uniform float osgx_gltf_alphaMode;
uniform float osgx_gltf_alphaCutoff;
uniform vec3 osgx_gltf_emissiveFactor;
uniform int osgx_gltf_hasEmissiveMap;
)GLSL";

// Vertex-side hook: FULL_PBR_VERTEX_SHADER (PBRIBL.cpp) forward-declares osgx_gltf_ApplySkin() and
// calls it on the raw glTF-authored vertex/normal/tangent before applying any camera transform --
// the same "forward-declare + call site in the main shader, DEFINITION in a separate, separately
// compiled hook shader object" pattern PBR.hpp's DIRECT_LIGHTING_DECL/DIRECT_LIGHTING_HOOK_DEFAULT
// already uses (see that comment for the full mechanism: GLSL permits exactly one body per
// function, so a hook SUBSTITUTES the built-in, never adds alongside it).
//
// SKINNING_HOOK_IDENTITY is what PBRIBLScene::create() attaches when no `hooks` entry names
// osgx::Hook::Skinning - a pure passthrough, so an unskinned model (or a caller not yet
// exercising this) pays for nothing beyond one extra function call. SKINNING_HOOK_LINEAR_BLEND is
// the real joint-matrix
// linear blend skin (LBS), reading the exact JOINT_INDICES_ATTRIBUTE/JOINT_WEIGHTS_ATTRIBUTE/
// JOINT_MATRICES_BINDING wiring the loader (Skin.cpp's SkinPaletteCallback) already populates
// UNCONDITIONALLY for every skinned primitive, whether or not anything ever reads it - this hook
// is what proves that data path was already sound; only a consuming vertex shader was missing.
// Deliberately whole-Program scope, not per-primitive selection - see TODO.md's still-open
// "select skinning per primitive" item for the real, more general version of this.
inline constexpr char SKINNING_HOOK_IDENTITY[] = R"GLSL(
#version 460 core

struct osgx_gltf_SkinnedVertex {
	vec4 position;
	vec3 normal;
	vec3 tangent;
};

osgx_gltf_SkinnedVertex osgx_gltf_ApplySkin(vec4 position, vec3 normal, vec3 tangent) {
	return osgx_gltf_SkinnedVertex(position, normal, tangent);
}
)GLSL";

inline constexpr char SKINNING_HOOK_LINEAR_BLEND[] = R"GLSL(
#version 460 core

layout(location = 8) in uvec4 osgx_gltf_JointIndices;
layout(location = 9) in vec4 osgx_gltf_JointWeights;

layout(std430, binding = 2) readonly buffer osgx_gltf_JointMatrixBuffer {
	mat4 osgx_gltf_jointMatrices[];
};

struct osgx_gltf_SkinnedVertex {
	vec4 position;
	vec3 normal;
	vec3 tangent;
};

osgx_gltf_SkinnedVertex osgx_gltf_ApplySkin(vec4 position, vec3 normal, vec3 tangent) {
	mat4 skin =
		osgx_gltf_JointWeights.x * osgx_gltf_jointMatrices[osgx_gltf_JointIndices.x] +
		osgx_gltf_JointWeights.y * osgx_gltf_jointMatrices[osgx_gltf_JointIndices.y] +
		osgx_gltf_JointWeights.z * osgx_gltf_jointMatrices[osgx_gltf_JointIndices.z] +
		osgx_gltf_JointWeights.w * osgx_gltf_jointMatrices[osgx_gltf_JointIndices.w];

	return osgx_gltf_SkinnedVertex(
		skin * position,
		mat3(skin) * normal,
		mat3(skin) * tangent
	);
}
)GLSL";

inline void configureProgram(osg::Program& program) {
	program.addBindAttribLocation(TANGENT_ATTRIBUTE_NAME, TANGENT_ATTRIBUTE);
	program.addBindAttribLocation(JOINT_INDICES_ATTRIBUTE_NAME, JOINT_INDICES_ATTRIBUTE);
	program.addBindAttribLocation(JOINT_WEIGHTS_ATTRIBUTE_NAME, JOINT_WEIGHTS_ATTRIBUTE);
}

inline void configureStateSet(osg::StateSet& stateSet) {
	stateSet.addUniform(new osg::Uniform(BASE_COLOR_SAMPLER, BASE_COLOR_TEXTURE_UNIT));
	stateSet.addUniform(new osg::Uniform(NORMAL_SAMPLER, NORMAL_TEXTURE_UNIT));
	stateSet.addUniform(new osg::Uniform(ORM_SAMPLER, ORM_TEXTURE_UNIT));
	stateSet.addUniform(new osg::Uniform(EMISSIVE_SAMPLER, EMISSIVE_TEXTURE_UNIT));
}

}
