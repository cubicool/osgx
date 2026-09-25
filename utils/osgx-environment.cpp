// osgx-environment - bake a self-contained osgx_environment bundle.
//
// Usage:
//   osgx-environment <input.hdr> <output-basename>
//       [--prefilter-size N] [--samples N] [--diffuse-cube-size N]
//       [--diffuse-samples N] [--lut-size N] [--software]
//
// Produces <basename>-specular.ktx2, <basename>-diffuse.ktx2, and
// <basename>.gltf. The manifest identifies the matching built-in BRDF LUT,
// which the renderer caches and bakes once per process rather than serializing
// an HDR-independent artifact beside every environment.

#include "osgx/GGXPrefilter.hpp"
#include "osgx/LambertianBake.hpp"
#include "osgx/Library.hpp"
#include "osgx/Warnings.hpp"

#ifdef OSGX_ENVIRONMENT_SOFTWARE_AVAILABLE
# include "osgx/ktx2/KTX2.hpp"
#endif

OSGX_DISABLE_WARNINGS

#include <osg/GL>
#include <osg/Group>
#include <osg/Image>
#include <osg/Notify>
#include <osg/TextureCubeMap>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/Viewer>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#ifdef _OPENMP
OSGX_DISABLE_WARNINGS
# include <omp.h>
OSGX_ENABLE_WARNINGS
#endif

namespace {

struct OutputPaths {
	std::filesystem::path specular;
	std::filesystem::path diffuse;
	std::filesystem::path manifest;
};

OutputPaths makeOutputPaths(const std::filesystem::path& basename) {
	const std::string base = basename.string();

	return {
		base + "-specular.ktx2",
		base + "-diffuse.ktx2",
		base + ".gltf"
	};
}

bool writeManifest(
	const std::filesystem::path& path,
	const OutputPaths& outputs,
	int lutSize
) {
	std::ofstream file(path);

	if(!file) {
		OSG_WARN << "osgx-environment: failed to open " << path << std::endl;

		return false;
	}

	// Resource URIs are relative to this manifest, even when the requested basename includes a
	// directory.  The loader resolves them against manifest.parent_path().
	file << "{" << std::endl
		<< "  \"asset\": {\"version\": \"2.0\", \"generator\": \"osgx-environment\"}," << std::endl
		<< "  \"extensionsUsed\": [\"osgx_environment\"]," << std::endl
		<< "  \"extensions\": {" << std::endl
		<< "    \"osgx_environment\": {" << std::endl
		<< "      \"environments\": [{" << std::endl
		<< "        \"specular\": {\"uri\": \"" << outputs.specular.filename().string() << "\"}," << std::endl
		<< "        \"diffuse\": {\"uri\": \"" << outputs.diffuse.filename().string() << "\"}," << std::endl
		<< "        \"brdfLUT\": {\"builtin\": \"osgx:split-sum-ggx-v1\", \"size\": " << lutSize << "}" << std::endl
		<< "      }]" << std::endl
		<< "    }" << std::endl
		<< "  }" << std::endl
		<< "}" << std::endl;

	if(!file) {
		OSG_WARN << "osgx-environment: failed to write " << path << std::endl;

		return false;
	}

	OSG_NOTICE << "osgx-environment: wrote " << path << std::endl;

	return true;
}

void usage(const char* program) {
	std::cerr
		<< "Usage: " << program << " <input.hdr> <output-basename>"
		<< " [--prefilter-size N] [--samples N] [--diffuse-cube-size N]"
		<< " [--diffuse-samples N] [--lut-size N] [--software]" << std::endl;
}

}

#ifdef OSGX_ENVIRONMENT_SOFTWARE_AVAILABLE

namespace {

constexpr float PI = 3.14159265358979323846f;

struct Vec2 {
	float x;
	float y;
};

struct Vec3 {
	float x;
	float y;
	float z;
};

struct CubeMip {
	int size;
	std::vector<std::vector<float>> faces;
};

Vec3 operator+(Vec3 a, Vec3 b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator*(Vec3 value, float scale) {
	return {value.x * scale, value.y * scale, value.z * scale};
}

Vec3 operator-(Vec3 value) {
	return {-value.x, -value.y, -value.z};
}

float dot(Vec3 a, Vec3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

Vec3 normalize(Vec3 value) {
	const float length = std::sqrt(dot(value, value));

	if(length < std::numeric_limits<float>::epsilon()) return {0.0f, 0.0f, 1.0f};

	return value * (1.0f / length);
}

Vec3 reflect(Vec3 value, Vec3 normal) {
	return value + normal * (-2.0f * dot(value, normal));
}

float clamp(float value, float minimum, float maximum) {
	return std::max(minimum, std::min(value, maximum));
}

Vec3 clampFirefly(Vec3 color, float maximumLuminance) {
	if(maximumLuminance <= 0.0f) return color;

	const float luminance = dot(color, {0.2126f, 0.7152f, 0.0722f});

	return luminance > maximumLuminance ? color * (maximumLuminance / luminance) : color;
}

uint16_t floatToHalf(float value) {
	union {
		float value;
		uint32_t bits;
	} source = {value};
	const uint32_t sign = (source.bits >> 31) & 1u;
	const uint32_t exponent = (source.bits >> 23) & 0xffu;
	const uint32_t mantissa = source.bits & 0x7fffffu;

	if(exponent == 0xffu) return static_cast<uint16_t>((sign << 15) | 0x7c00u | (mantissa ? 0x0200u : 0u));

	const int halfExponent = static_cast<int>(exponent) - 127 + 15;

	if(halfExponent >= 31) return static_cast<uint16_t>((sign << 15) | 0x7c00u);
	if(halfExponent <= 0) {
		if(halfExponent < -10) return static_cast<uint16_t>(sign << 15);

		const uint32_t shiftedMantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(14 - halfExponent);

		return static_cast<uint16_t>((sign << 15) | shiftedMantissa);
	}

	return static_cast<uint16_t>((sign << 15) | (static_cast<uint32_t>(halfExponent) << 10) | (mantissa >> 13));
}

class EquirectangularImage {
public:
	explicit EquirectangularImage(const osg::Image& image):
	_image(image),
	_width(image.s()),
	_height(image.t()) {}

	bool valid() const {
		return _image.data() && _width > 0 && _height > 0 &&
			(_image.getPixelFormat() == GL_RGB || _image.getPixelFormat() == GL_RGBA) &&
			_image.getDataType() == GL_FLOAT;
	}

	Vec3 sample(Vec3 direction) const {
		const float phi = std::atan2(direction.z, direction.x);
		const float theta = std::acos(clamp(direction.y, -1.0f, 1.0f));
		float u = phi / (2.0f * PI) + 0.5f;
		const float v = 1.0f - theta / PI;

		u -= std::floor(u);

		const float pixelX = u * static_cast<float>(_width - 1);
		const float pixelY = clamp(v, 0.0f, 1.0f) * static_cast<float>(_height - 1);
		const int x0 = static_cast<int>(pixelX);
		const int y0 = static_cast<int>(pixelY);
		const int x1 = std::min(x0 + 1, _width - 1);
		const int y1 = std::min(y0 + 1, _height - 1);
		const float tx = pixelX - static_cast<float>(x0);
		const float ty = pixelY - static_cast<float>(y0);
		const Vec3 bottom = lerp(fetch(x0, y0), fetch(x1, y0), tx);
		const Vec3 top = lerp(fetch(x0, y1), fetch(x1, y1), tx);

		return lerp(bottom, top, ty);
	}

private:
	Vec3 fetch(int x, int y) const {
		const auto* pixel = reinterpret_cast<const float*>(_image.data(
			static_cast<unsigned int>(x),
			static_cast<unsigned int>(y)
		));

		return {pixel[0], pixel[1], pixel[2]};
	}

	static Vec3 lerp(Vec3 a, Vec3 b, float amount) {
		return a + (b + a * -1.0f) * amount;
	}

	const osg::Image& _image;
	int _width;
	int _height;
};

Vec3 faceDirection(int face, float u, float v) {
	switch(face) {
		case 0: return normalize({1.0f, -v, -u});
		case 1: return normalize({-1.0f, -v, u});
		case 2: return normalize({u, 1.0f, v});
		case 3: return normalize({u, -1.0f, -v});
		case 4: return normalize({u, -v, 1.0f});
		default: return normalize({-u, -v, -1.0f});
	}
}

Vec2 hammersley(uint32_t index, uint32_t count) {
	uint32_t bits = index;

	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
	bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0fu) >> 4u);
	bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);

	return {static_cast<float>(index) / static_cast<float>(count), static_cast<float>(bits) * 2.3283064365386963e-10f};
}

Vec3 importanceSampleGGX(Vec2 sample, Vec3 normal, float roughness) {
	const float alpha = roughness * roughness;
	const float phi = 2.0f * PI * sample.x;
	const float cosTheta = std::sqrt((1.0f - sample.y) / (1.0f + (alpha * alpha - 1.0f) * sample.y));
	const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
	const Vec3 halfVector = {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
	const Vec3 up = std::abs(normal.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
	const Vec3 tangent = normalize(cross(up, normal));
	const Vec3 bitangent = cross(normal, tangent);

	return normalize(tangent * halfVector.x + bitangent * halfVector.y + normal * halfVector.z);
}

Vec3 cosineSampleHemisphere(Vec2 sample, Vec3 normal) {
	const float sinTheta = std::sqrt(sample.y);
	const float cosTheta = std::sqrt(1.0f - sample.y);
	const float phi = 2.0f * PI * sample.x;
	const Vec3 local = {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
	const Vec3 up = std::abs(normal.z) > 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
	const Vec3 tangent = normalize(cross(up, normal));
	const Vec3 bitangent = cross(normal, tangent);

	return tangent * local.x + bitangent * local.y + normal * local.z;
}

int mipCountForSize(int size) {
	int count = 0;

	for(int currentSize = size; currentSize >= 1; currentSize >>= 1) count++;

	return std::max(1, count);
}

void bakeSpecularFace(
	const EquirectangularImage& input,
	std::vector<float>& output,
	int size,
	int face,
	float roughness,
	uint32_t sampleCount,
	float fireflyClamp
) {
	const float inverseSize = 1.0f / static_cast<float>(size);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 4)
#endif
	for(int y = 0; y < size; y++) {
		for(int x = 0; x < size; x++) {
			const float u = (static_cast<float>(x) + 0.5f) * inverseSize * 2.0f - 1.0f;
			const float v = (static_cast<float>(y) + 0.5f) * inverseSize * 2.0f - 1.0f;
			const Vec3 normal = faceDirection(face, u, v);
			Vec3 color = {0.0f, 0.0f, 0.0f};
			float weight = 0.0f;

			for(uint32_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
				const Vec3 halfVector = importanceSampleGGX(hammersley(sampleIndex, sampleCount), normal, roughness);
				const Vec3 light = normalize(reflect(-normal, halfVector));
				const float normalDotLight = std::max(0.0f, dot(normal, light));

				if(normalDotLight > 0.0f) {
					color = color + clampFirefly(input.sample(light), fireflyClamp) * normalDotLight;
					weight += normalDotLight;
				}
			}

			const Vec3 result = color * (1.0f / std::max(weight, 0.001f));
			const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 3;

			output[index] = result.x;
			output[index + 1] = result.y;
			output[index + 2] = result.z;
		}
	}
}

void bakeDiffuseFace(
	const EquirectangularImage& input,
	std::vector<float>& output,
	int size,
	int face,
	uint32_t sampleCount,
	float fireflyClamp
) {
	const float inverseSize = 1.0f / static_cast<float>(size);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 4)
#endif
	for(int y = 0; y < size; y++) {
		for(int x = 0; x < size; x++) {
			const float u = (static_cast<float>(x) + 0.5f) * inverseSize * 2.0f - 1.0f;
			const float v = (static_cast<float>(y) + 0.5f) * inverseSize * 2.0f - 1.0f;
			const Vec3 normal = faceDirection(face, u, v);
			Vec3 irradiance = {0.0f, 0.0f, 0.0f};

			for(uint32_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
				const Vec3 direction = cosineSampleHemisphere(hammersley(sampleIndex, sampleCount), normal);

				irradiance = irradiance + clampFirefly(input.sample(direction), fireflyClamp);
			}

			const Vec3 result = irradiance * (1.0f / static_cast<float>(sampleCount));
			const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 3;

			output[index] = result.x;
			output[index + 1] = result.y;
			output[index + 2] = result.z;
		}
	}
}

bool writeCubeMap(
	const std::vector<CubeMip>& data,
	int baseSize,
	const std::filesystem::path& path
) {
	const auto result = osgx::ktx2::write(
		static_cast<uint32_t>(baseSize),
		static_cast<uint32_t>(baseSize),
		static_cast<uint32_t>(data.size()),
		6,
		osgx::ktx2::Format::R16G16B16_SFLOAT,
		[&data](uint32_t mip, uint32_t face) -> osgx::ktx2::ImageSpan {
			static thread_local std::vector<uint16_t> halfFloats;
			const auto& source = data[mip].faces[face];

			halfFloats.resize(source.size());

			for(std::size_t index = 0; index < source.size(); index++) halfFloats[index] = floatToHalf(source[index]);

			return {halfFloats.data(), halfFloats.size() * sizeof(uint16_t)};
		},
		path.string()
	);

	return result.success();
}

bool bakeSoftwareIBL(
	osg::Image* equirectangularHDR,
	const osgx::GGXPrefilterOptions& specularOptions,
	const osgx::LambertianBakeOptions& diffuseOptions,
	const std::filesystem::path& specularPath,
	const std::filesystem::path& diffusePath
) {
	if(!equirectangularHDR) return false;

	const EquirectangularImage input(*equirectangularHDR);

	if(!input.valid()) {
		OSG_WARN << "osgx-environment: --software requires an RGB/RGBA float HDR image" << std::endl;

		return false;
	}

	const int numMips = mipCountForSize(specularOptions.prefilterSize);
	std::vector<CubeMip> specular(static_cast<std::size_t>(numMips));

	OSG_NOTICE << "osgx-environment: CPU software bake" << std::endl;
#ifdef _OPENMP
	OSG_NOTICE << "osgx-environment: OpenMP threads: " << omp_get_max_threads() << std::endl;
#endif

	for(int mip = 0; mip < numMips; mip++) {
		const int size = std::max(1, specularOptions.prefilterSize >> mip);
		const float roughness = numMips > 1 ? static_cast<float>(mip) / static_cast<float>(numMips - 1) : 0.0f;
		auto& output = specular[static_cast<std::size_t>(mip)];

		output.size = size;
		output.faces.resize(6);

		for(int face = 0; face < 6; face++) {
			auto& pixels = output.faces[static_cast<std::size_t>(face)];

			pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 3);
			bakeSpecularFace(
				input,
				pixels,
				size,
				face,
				roughness,
				static_cast<uint32_t>(specularOptions.sampleCount),
				specularOptions.fireflyClamp
			);
		}
	}

	std::vector<CubeMip> diffuse(1);

	diffuse.front().size = diffuseOptions.cubeSize;
	diffuse.front().faces.resize(6);

	for(int face = 0; face < 6; face++) {
		auto& pixels = diffuse.front().faces[static_cast<std::size_t>(face)];

		pixels.resize(
			static_cast<std::size_t>(diffuseOptions.cubeSize) *
			static_cast<std::size_t>(diffuseOptions.cubeSize) * 3
		);
		bakeDiffuseFace(
			input,
			pixels,
			diffuseOptions.cubeSize,
			face,
			static_cast<uint32_t>(diffuseOptions.sampleCount),
			diffuseOptions.fireflyClamp
		);
	}

	if(!writeCubeMap(specular, specularOptions.prefilterSize, specularPath)) {
		OSG_WARN << "osgx-environment: failed to write " << specularPath << std::endl;

		return false;
	}

	if(!writeCubeMap(diffuse, diffuseOptions.cubeSize, diffusePath)) {
		OSG_WARN << "osgx-environment: failed to write " << diffusePath << std::endl;

		return false;
	}

	OSG_NOTICE << "osgx-environment: wrote " << specularPath << std::endl;
	OSG_NOTICE << "osgx-environment: wrote " << diffusePath << std::endl;

	return true;
}

}

#endif

int main(int argc, char* argv[]) {
	auto lib = osgx::initialize();

	if(argc < 3) {
		usage(argv[0]);

		return 1;
	}

	const std::string inputPath = argv[1];
	const OutputPaths outputs = makeOutputPaths(argv[2]);
	osgx::GGXPrefilterOptions specularOptions;
	osgx::LambertianBakeOptions diffuseOptions;
	int lutSize = 1024;
	bool software = false;

	for(int i = 3; i < argc; ++i) {
		const std::string argument = argv[i];

		if(argument == "--prefilter-size" && i + 1 < argc) specularOptions.prefilterSize = std::atoi(argv[++i]);
		else if(argument == "--samples" && i + 1 < argc) specularOptions.sampleCount = std::atoi(argv[++i]);
		else if(argument == "--diffuse-cube-size" && i + 1 < argc) diffuseOptions.cubeSize = std::atoi(argv[++i]);
		else if(argument == "--diffuse-samples" && i + 1 < argc) diffuseOptions.sampleCount = std::atoi(argv[++i]);
		else if(argument == "--lut-size" && i + 1 < argc) lutSize = std::atoi(argv[++i]);
		else if(argument == "--software") software = true;
		else {
			OSG_WARN << "osgx-environment: unknown or incomplete option " << argument << std::endl;
			usage(argv[0]);

			return 1;
		}
	}

	if(specularOptions.prefilterSize < 1 || specularOptions.sampleCount < 1 ||
		diffuseOptions.cubeSize < 1 || diffuseOptions.sampleCount < 1 || lutSize < 1) {
		OSG_WARN << "osgx-environment: all bake sizes and sample counts must be positive" << std::endl;

		return 1;
	}

	osg::setNotifyLevel(osg::NOTICE);
	auto image = osgDB::readRefImageFile(inputPath);

	if(!image) {
		OSG_WARN << "osgx-environment: failed to load HDR image " << inputPath << std::endl;

		return 1;
	}

	if(software) {
#ifdef OSGX_ENVIRONMENT_SOFTWARE_AVAILABLE
		if(!bakeSoftwareIBL(
			image,
			specularOptions,
			diffuseOptions,
			outputs.specular,
			outputs.diffuse
		)) return 1;

		return writeManifest(outputs.manifest, outputs, lutSize) ? 0 : 1;
#else
		OSG_WARN << "osgx-environment: --software requires OSGX_BUILD_KTX2=ON" << std::endl;

		return 1;
#endif
	}

	auto specular = osgx::GGXPrefilterScene::create(image, specularOptions);
	auto diffuse = osgx::LambertianBakeScene::create(image, diffuseOptions);

	if(!specular.root || !diffuse.root) {
		OSG_WARN << "osgx-environment: failed to create cubemap bake passes" << std::endl;

		return 1;
	}

	auto diffuseReadback = new osgx::LambertianCubeReadback(
		diffuse.diffuseTexture,
		diffuse.completion
	);
	auto root = new osg::Group();

	root->addChild(specular.root);
	root->addChild(diffuse.root);

	osgViewer::Viewer viewer;

	viewer.setUpViewInWindow(0, 0, 128, 128);
	viewer.setSceneData(root);
	// Both cube readbacks live on the outer camera so every PRE_RENDER face pass has completed.
	viewer.getCamera()->addPostDrawCallback(specular.readback);
	viewer.getCamera()->addPostDrawCallback(diffuseReadback);

	const int maxFrames = std::max(1, specularOptions.maxFrames);

	for(int frame = 0; frame < maxFrames; ++frame) {
		if(specular.readback->isDone() && diffuseReadback->isDone()) break;

		viewer.frame();
	}

	if(!specular.readback->isDone() || !diffuseReadback->isDone()) {
		OSG_WARN << "osgx-environment: one or more cubemap readbacks did not complete" << std::endl;

		return 1;
	}

	auto specularResult = specular.readback->finish();
	auto diffuseResult = diffuseReadback->finish();

	if(!specularResult || !osgDB::writeObjectFile(*specularResult, outputs.specular.string())) {
		OSG_WARN << "osgx-environment: failed to write " << outputs.specular << std::endl;

		return 1;
	}

	if(!diffuseResult || !osgDB::writeObjectFile(*diffuseResult, outputs.diffuse.string())) {
		OSG_WARN << "osgx-environment: failed to write " << outputs.diffuse << std::endl;

		return 1;
	}

	OSG_NOTICE << "osgx-environment: wrote " << outputs.specular << std::endl;
	OSG_NOTICE << "osgx-environment: wrote " << outputs.diffuse << std::endl;

	return writeManifest(outputs.manifest, outputs, lutSize) ? 0 : 1;
}
