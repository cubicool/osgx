#include "LibraryState.hpp"
#include "ShaderLibs.hpp"

#include "osgx/IBL.hpp"
#include "osgx/RTT.hpp"

namespace osgx {

void RunOnceCallback::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	if(_done) node->setNodeMask(0);
	// Logged at NOTICE (on by default) rather than INFO deliberately: a bake pass silently
	// re-running every frame instead of once is exactly the class of bug this callback exists
	// to prevent, and it stayed unnoticed for days precisely because nothing printed either
	// way. One line per pass at startup is worth the noise; drop to OSG_INFO if that changes.
	else OSG_NOTICE << "osgx::RunOnceCallback: baked \"" << node->getName() << "\"" << std::endl;

	_done = true;

	if(_traverseChildren) traverse(node, nv);
}

void RunOnceCallback::rebake(osg::Node* node) {
	node->setNodeMask(0xFFFFFFFF);
	_done = false;
}

void rearmRunOnceCallbacks(osg::Node* node) {
	if(!node) return;

	if(auto* callback = dynamic_cast<RunOnceCallback*>(node->getUpdateCallback())) callback->rebake(node);

	if(auto* group = node->asGroup()) {
		for(unsigned int child = 0; child < group->getNumChildren(); child++) {
			rearmRunOnceCallbacks(group->getChild(child));
		}
	}
}

osg::ref_ptr<osg::TextureCubeMap> loadPrefilterCubemap(const std::string& path) {
	osg::ref_ptr<osg::Object> obj = osgDB::readRefObjectFile(path);
	auto* cube = dynamic_cast<osg::TextureCubeMap*>(obj.get());

	if(!cube) {
		OSG_WARN <<
			"osgx::loadPrefilterCubemap: " << path <<
			" did not load as a TextureCubeMap" << std::endl
		;

		return nullptr;
	}

	cube->setUseHardwareMipMapGeneration(false);

	return cube;
}

osg::ref_ptr<osg::Camera> makeBRDFLUTCamera(int lutSize, osg::Texture2D* lut) {
	lut->setTextureSize(lutSize, lutSize);
	lut->setInternalFormat(GL_RGBA);
	lut->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
	lut->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
	lut->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
	lut->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

	auto cam = RTT::fullscreenQuad(lutSize, lutSize, BRDF_LUT_FRAG);

	cam->setName("osgx_ibl_BRDFLUTBake");
	cam->attach(osg::Camera::COLOR_BUFFER0, lut);
	cam->setUpdateCallback(new RunOnceCallback());

	return cam;
}

// The cache is owned by the live osgx::Library (LibraryState::brdfLUTs).
SharedBRDFLUT SharedBRDFLUT::create(int lutSize) {
	auto& cache = detail::libraryState().brdfLUTs;

	if(auto it = cache.find(lutSize); it != cache.end()) return {it->second, nullptr};

	auto texture = make_ref<osg::Texture2D>();
	auto camera = makeBRDFLUTCamera(lutSize, texture);

	cache.emplace(lutSize, texture);

	return {texture, camera};
}

void BRDFLUTReadback::operator()(osg::RenderInfo& ri) const {
	if(_done) return;

	_frameCount++;

	if(_frameCount < _triggerFrame) return;
	if(!_srcTex) return;

	auto* texObj = _srcTex->getTextureObject(ri.getContextID());

	if(!texObj) {
		OSG_WARN << "osgx::ibl: BRDF LUT not on GPU yet; retrying next frame" << std::endl;

		return;
	}

	if(_sync) glFinish();

#if OSG_MIN_VERSION_REQUIRED(3, 7, 0)
	texObj->bind(*ri.getState());
#else
	texObj->bind();
#endif

	_result = new osg::Image();

	// The LUT's FBO internal format is GL_RGBA (see makeBRDFLUTCamera()) - an unsized token
	// that resolves to 8-bit UNORM per channel, matching the [0,1]-ranged Fresnel scale/bias
	// values the shader writes. Read back as GL_UNSIGNED_BYTE to match; no mip chain exists.
	_result->readImageFromCurrentTexture(ri.getContextID(), false, GL_UNSIGNED_BYTE);

	_done = true;
}

void readCubeMapFaces(
	unsigned int contextID,
	GLenum type,
	bool copyMipMapsIfAvailable,
	osg::TextureCubeMap* result
) {
	for(int face = 0; face < 6; face++) {
		auto* img = new osg::Image();

		img->readImageFromCurrentTexture(contextID, copyMipMapsIfAvailable, type, static_cast<unsigned int>(face));

		result->setImage(static_cast<unsigned int>(face), img);
	}
}

SH9 computeSH(const osg::Image* img) {
	SH9 sh;

	const auto W = static_cast<std::size_t>(std::max(img->s(), 0));
	const auto H = static_cast<std::size_t>(std::max(img->t(), 0));

	for(std::size_t y = 0; y < H; y++) {
		const double theta = (double(y) + 0.5) / double(H) * osg::PI;
		const double sinTheta = std::sin(theta);
		const double cosTheta = std::cos(theta);
		const double dOmega = sinTheta * (osg::PI / double(H)) * (2.0 * osg::PI / double(W));

		for(std::size_t x = 0; x < W; x++) {
			const double phi = (double(x) + 0.5) / double(W) * 2.0 * osg::PI;
			const double sx = sinTheta * std::cos(phi);
			const double sy = sinTheta * std::sin(phi);
			const double sz = cosTheta;

			const double Y[9] = {
				0.282095,
				0.488603 * sy,
				0.488603 * sz,
				0.488603 * sx,
				1.092548 * sx * sy,
				1.092548 * sy * sz,
				0.315392 * (3.0 * sz * sz - 1.0),
				1.092548 * sx * sz,
				0.546274 * (sx * sx - sy * sy)
			};

			const double A[9] = {
				osg::PI,
				2.0 * osg::PI / 3.0,
				2.0 * osg::PI / 3.0,
				2.0 * osg::PI / 3.0,
				osg::PI / 4.0,
				osg::PI / 4.0,
				osg::PI / 4.0,
				osg::PI / 4.0,
				osg::PI / 4.0
			};

			const osg::Vec4f c = img->getColor(
				static_cast<unsigned int>(x),
				static_cast<unsigned int>(y)
			);

			for(int i = 0; i < 9; i++) {
				const double w = Y[i] * A[i] * dOmega;

				sh.coeffs[i] += osg::Vec3f(
					float(double(c.r()) * w),
					float(double(c.g()) * w),
					float(double(c.b()) * w)
				);
			}
		}
	}

	return sh;
}

namespace {

double radicalInverseVdC(std::uint32_t bits) {
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
	bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
	bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
	bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);

	return double(bits) * 2.3283064365386963e-10;
}

// Unnormalized direction to a texel center on a GL-convention cube face - face index/formula
// order matches osg::TextureCubeMap's POSITIVE_X/NEGATIVE_X/POSITIVE_Y/NEGATIVE_Y/POSITIVE_Z/
// NEGATIVE_Z, same as cube_directions() in the Python original.
osg::Vec3f cubeFaceDirection(int face, float s, float t) {
	switch(face) {
		case 0: return osg::Vec3f(1.0f, -t, -s);
		case 1: return osg::Vec3f(-1.0f, -t, s);
		case 2: return osg::Vec3f(s, 1.0f, t);
		case 3: return osg::Vec3f(s, -1.0f, -t);
		case 4: return osg::Vec3f(s, -t, 1.0f);
		default: return osg::Vec3f(-s, -t, -1.0f);
	}
}

// Read-only view of a GL_RGB/GL_FLOAT osg::Image, with the row stride pre-derived once. Passing
// this instead of an osg::Image* is the actual fix for the hot-loop cost sampleEquirect() below
// used to pay: osg::Image::data(x,y) LOOKS like a cheap header-inlined pointer computation, but it
// internally calls getPixelSizeInBits()/getRowStepInBytes(), which call
// computePixelSizeInBits()/computeRowWidthInBytes() - both declared `static` in the header but
// DEFINED out-of-line in libosg's Image.cpp, each running its own multi-case switch on
// pixelFormat/dataType/packing. Those values never change for a given image, so deriving them
// once here (instead of ~25 million times, once per bilinear tap per sample) is the difference
// that actually matters - not just swapping which OSG accessor gets called.
struct EquirectView {
	const float* data;
	int width, height;
	int rowStrideFloats; // == getRowStepInBytes() / sizeof(float); may exceed width*3 if padded

	explicit EquirectView(const osg::Image* img):
		data(reinterpret_cast<const float*>(img->data())),
		width(img->s()),
		height(img->t()),
		rowStrideFloats(static_cast<int>(img->getRowStepInBytes() / sizeof(float)))
	{
	}
};

// Bilinear sample of an equirectangular HDR image along a world-space (Z-up) direction - same
// theta/phi convention computeSH() uses per-texel, but continuous, since Monte Carlo sample
// directions don't land on exact source pixels. Ported from sample_hdr() in the Python original.
// Requires `view` to wrap a real HDR-loaded (GL_RGB/GL_FLOAT) osg::Image, same requirement
// computeSH() already documents for its own input - exactly what osgDB's own HDR (Radiance)
// reader always produces (see ReaderWriterHDR.cpp).
osg::Vec3f sampleEquirect(const EquirectView& view, const osg::Vec3f& dir) {
	const int W = view.width;
	const int H = view.height;

	double phi = std::atan2(double(dir.y()), double(dir.x())) / (2.0 * osg::PI);

	phi -= std::floor(phi); // wrap to [0, 1)

	const double theta = std::acos(std::clamp(double(dir.z()), -1.0, 1.0)) / osg::PI;
	const double x = phi * double(W) - 0.5;
	const double y = std::clamp(theta * double(H) - 0.5, 0.0, double(H - 1));
	const double fx = x - std::floor(x);
	const double fy = y - std::floor(y);

	int x0 = int(std::floor(x)) % W;

	if(x0 < 0) x0 += W;

	const int x1 = (x0 + 1) % W;
	const int y0 = int(std::floor(y));
	const int y1 = std::min(y0 + 1, H - 1);

	// OpenCV (the Python reference) addresses the Radiance panorama top-to-bottom. OSG images
	// address rows bottom-to-top; ReaderWriterHDR preserves that GL/OSG orientation when it loads
	// the file. Convert the sampled top-origin y coordinates back to OSG's row order here.
	const std::size_t row0Index = std::size_t(H - 1 - y0);
	const std::size_t row1Index = std::size_t(H - 1 - y1);
	const float* row0 = view.data + row0Index * std::size_t(view.rowStrideFloats);
	const float* row1 = view.data + row1Index * std::size_t(view.rowStrideFloats);
	const float* p00 = row0 + std::size_t(x0) * 3;
	const float* p10 = row0 + std::size_t(x1) * 3;
	const float* p01 = row1 + std::size_t(x0) * 3;
	const float* p11 = row1 + std::size_t(x1) * 3;
	const osg::Vec3f c00(p00[0], p00[1], p00[2]);
	const osg::Vec3f c10(p10[0], p10[1], p10[2]);
	const osg::Vec3f c01(p01[0], p01[1], p01[2]);
	const osg::Vec3f c11(p11[0], p11[1], p11[2]);
	const osg::Vec3f a = c00 * float(1.0 - fx) + c10 * float(fx);
	const osg::Vec3f b = c01 * float(1.0 - fx) + c11 * float(fx);

	return a * float(1.0 - fy) + b * float(fy);
}

}

osg::ref_ptr<osg::TextureCubeMap> computeLambertianCubeMap(
	const osg::Image* hdrImg,
	int size,
	int samples
) {
	auto cube = make_ref<osg::TextureCubeMap>();

	static constexpr osg::TextureCubeMap::Face FACES[6] = {
		osg::TextureCubeMap::POSITIVE_X, osg::TextureCubeMap::NEGATIVE_X,
		osg::TextureCubeMap::POSITIVE_Y, osg::TextureCubeMap::NEGATIVE_Y,
		osg::TextureCubeMap::POSITIVE_Z, osg::TextureCubeMap::NEGATIVE_Z
	};

	// The tangent-space sample direction for sample `i` depends only on `i`/`samples` - not on the
	// texel or face it'll be transformed into - so it's computed exactly `samples` times here,
	// once, instead of redone from scratch (radicalInverseVdC() + sin/cos/sqrt) inside the
	// per-texel loop below (which would repeat identical work 6*size*size times over, unused
	// libm/bit-twiddling work that dwarfed the actual per-texel HDR sampling). Matches
	// make_lambertian_environment()'s own `sequence`/`phi`/`local` precomputed once up top.
	std::vector<osg::Vec3f> localSamples(static_cast<std::size_t>(samples));
	const EquirectView hdrView(hdrImg);

	for(int i = 0; i < samples; i++) {
		const double xi2 = radicalInverseVdC(static_cast<std::uint32_t>(i));
		const double phi = 2.0 * osg::PI * double(i) / double(samples);
		const double r = std::sqrt(xi2);

		localSamples[static_cast<std::size_t>(i)] = osg::Vec3f(
			float(r * std::cos(phi)),
			float(r * std::sin(phi)),
			float(std::sqrt(std::max(0.0, 1.0 - xi2)))
		);
	}

	for(int face = 0; face < 6; face++) {
		auto image = make_ref<osg::Image>();

		image->allocateImage(size, size, 1, GL_RGB, GL_FLOAT);

		for(int y = 0; y < size; y++) {
			const float t = (float(y) + 0.5f) * (2.0f / float(size)) - 1.0f;

			for(int x = 0; x < size; x++) {
				const float s = (float(x) + 0.5f) * (2.0f / float(size)) - 1.0f;

				osg::Vec3f g = cubeFaceDirection(face, s, t);

				g.normalize();

				// GL-cube-face local direction -> Z-up world direction (the inverse of
				// osgx::ENVIRONMENT_BAKE_BASIS).
				const osg::Vec3f n(g.x(), -g.z(), g.y());
				const osg::Vec3f up = std::abs(n.z()) > 0.99f
					? osg::Vec3f(0.0f, 1.0f, 0.0f)
					: osg::Vec3f(0.0f, 0.0f, 1.0f)
				;

				osg::Vec3f T = up ^ n;

				T.normalize();

				const osg::Vec3f B = n ^ T;

				osg::Vec3d accum(0.0, 0.0, 0.0);

				for(int i = 0; i < samples; i++) {
					const osg::Vec3f& local = localSamples[static_cast<std::size_t>(i)];
					const osg::Vec3f dir = T * local.x() + B * local.y() + n * local.z();
					const osg::Vec3f c = sampleEquirect(hdrView, dir);

					accum += osg::Vec3d(double(c.x()), double(c.y()), double(c.z()));
				}

				accum /= double(samples);

				auto* px = reinterpret_cast<float*>(image->data(
					static_cast<unsigned int>(x),
					static_cast<unsigned int>(y)
				));

				px[0] = float(accum.x());
				px[1] = float(accum.y());
				px[2] = float(accum.z());
			}
		}

		cube->setImage(FACES[face], image);
	}

	cube->setUseHardwareMipMapGeneration(false);

	return cube;
}

void registerIBLShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"SH_IRRADIANCE", "osgx_SHIrradiance", SH_IRRADIANCE},
		{"HEMISPHERE_AMBIENT", "osgx_HemisphereAmbient", HEMISPHERE_AMBIENT}
	};
	::osgx::registerShaderLibs("osgx::ibl", libs);
}

}
