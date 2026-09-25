#pragma once

#include "osgx/Environment.hpp"
#include "osgx/Warnings.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Math>
#include <osg/Quat>
#include <osg/Vec3>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <string>
#include <vector>

// Forward declaration only - keeps tinygltf_json_c.h out of this public header. The manifest is a
// glTF-shaped JSON document, parsed with tinygltf's standalone JSON backend.
struct tg3json_value;

namespace osgx::gltf {

// The osgx::Environment rotation that matches the Khronos glTF-Sample-Viewer for glTF content (the
// loader converts glTF's Y-up to Z-up). osgx::Environment's own default is the equirect's natural
// orientation; loadEnvironment() applies this, and HDR-built environments for glTF content set it
// themselves: `environment->setRotation(KHRONOS_ENVIRONMENT_ROTATION)`.
inline const osg::Quat KHRONOS_ENVIRONMENT_ROTATION(-osg::PI_2, osg::Vec3(0.0f, 0.0f, 1.0f));

// One `environments[]` entry of an `osgx_environment` manifest extension: a pre-baked
// osgx::Environment's resources. Pure data: no textures, no I/O. Each `uri` is exactly what the
// manifest declared (relative to the manifest).
struct EnvironmentManifest {
	struct Resource {
		std::string uri;

		bool valid() const { return !uri.empty(); }
	};

	// Either a URI to a serialized LUT, or the name of a built-in one ("osgx:split-sum-ggx-v1",
	// osgx::SharedBRDFLUT::create(size)).
	struct BRDFLUTResource: Resource {
		std::string builtin;
		int size = 1024;

		bool valid() const { return !uri.empty() || !builtin.empty(); }
	};

	Resource specular;
	Resource diffuse;
	BRDFLUTResource brdfLUT;
};

// Decodes every `environments[]` entry of an `osgx_environment` extension object (the value of the
// manifest root's `extensions.osgx_environment`).
std::vector<EnvironmentManifest> decodeEnvironments(const tg3json_value* extensionValue);

// Loads a pre-baked environment: the manifest's specular and diffuse KTX2 cubemaps plus either a
// serialized BRDF LUT or the shared built-in one, rotated by KHRONOS_ENVIRONMENT_ROTATION.
// `manifest`'s relative URIs resolve against `baseDir`. Returns null (and logs) on failure.
// A built-in LUT used for the first time in this process leaves a pass in getBakeRoot().
osg::ref_ptr<osgx::Environment> loadEnvironment(
	const EnvironmentManifest& manifest,
	const std::string& baseDir
);

// Loads the first environment declared by the `osgx_environment` manifest at `manifestPath`,
// resolving URIs against the manifest's directory.
osg::ref_ptr<osgx::Environment> loadEnvironment(const std::string& manifestPath);

}
