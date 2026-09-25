#include "ShaderLibs.hpp"

#include "osgx/Array.hpp"
#include "osgx/Light.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>
#include <osg/BufferObject>
#include <osg/State>

OSGX_ENABLE_WARNINGS

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace osgx {

void OrbitLightRig::operator()(osg::Node* node, osg::NodeVisitor* nv) {
	float t = nv->getFrameStamp() ? float(nv->getFrameStamp()->getSimulationTime()) : 0.0f;

	if(orbits.size() > static_cast<std::size_t>(MAX_LIGHTS)) {
		throw std::out_of_range("OrbitLightRig has more orbits than LightSet supports");
	}
	if(!lights) throw std::logic_error("OrbitLightRig has no LightSet");

	for(std::size_t i = 0; i < orbits.size(); i++) {
		const auto& o = orbits[i];
		float a = t * o.speed + o.phase;

		lights->setPosition(
			i,
			osg::Vec3(
				center.x() + std::cos(a) * o.radius,
				center.y() + std::sin(a) * o.radius,
				center.z() + o.height
			),
			o.intensity * intensity
		);
	}

	traverse(node, nv);
}

namespace detail {

// Float offsets into one packed osgx_Light struct (LIGHT_STRUCT_FLOATS=16 floats/64 bytes) --
// must match LIGHT_UNIFORMS' GLSL layout comment in Light.hpp exactly.
constexpr std::size_t POS_INTENSITY_OFFSET = 0; // vec4
constexpr std::size_t COLOR_OFFSET = 4; // vec3
constexpr std::size_t TYPE_OFFSET = 7; // int
constexpr std::size_t DIR_OFFSET = 8; // vec3
constexpr std::size_t SOURCE_RADIUS_OFFSET = 11; // float
constexpr std::size_t SPOT_ANGLES_OFFSET = 12; // vec2
constexpr std::size_t ENABLED_OFFSET = 14; // int

// The `type` field is declared `int` on the GLSL side (LIGHT_UNIFORMS) but stored in this
// float-typed backing array - std::bit_cast reinterprets the bit pattern without UB (unlike a
// union-based type pun), which is all that's needed since the GPU reads the same raw bytes back
// as int regardless of how the CPU side labeled the storage.
float intBitsToFloat(int value) { return std::bit_cast<float>(value); }
int floatBitsToInt(float value) { return std::bit_cast<int>(value); }

}

LightSet::LightSet() {
	_lights = new osgx::FloatArray(static_cast<std::size_t>(MAX_LIGHTS * LIGHT_STRUCT_FLOATS));

	std::fill(_lights->begin(), _lights->end(), 0.0f);
	_lights->setBufferObject(new osg::UniformBufferObject());

	// Index 0 until apply() resolves the "osgx::light" slot.
	_binding = new osg::UniformBufferBinding(
		0, _lights, 0, static_cast<GLsizeiptr>(_lights->getTotalDataSize())
	);
	_lightCount = new osg::Uniform("osgx_lightCount", 0);
	setDataVariance(osg::Object::DYNAMIC);
}

LightSet::LightSet(const LightSet& lights, const osg::CopyOp& copyop):
osg::StateAttribute(lights, copyop),
_lights(static_cast<osgx::FloatArray*>(copyop(lights._lights.get()))),
_lightCount(static_cast<osg::Uniform*>(copyop(lights._lightCount.get()))) {
	// Index 0 until apply() resolves the "osgx::light" slot.
	_binding = new osg::UniformBufferBinding(
		0, _lights, 0, static_cast<GLsizeiptr>(_lights->getTotalDataSize())
	);
	setDataVariance(osg::Object::DYNAMIC);
}

LightSet::~LightSet() {}

int LightSet::compare(const osg::StateAttribute& sa) const {
	COMPARE_StateAttribute_Types(LightSet, sa)

	COMPARE_StateAttribute_Parameter(_lights)
	COMPARE_StateAttribute_Parameter(_lightCount)

	return 0;
}

void LightSet::apply(osg::State& state) const {
	resolveBinding(_bindingResolved, _binding.get(), "osgx::light");

	// osgx_lightCount is NOT pushed as a uniform at all - see LIGHT_UNIFORMS/DIRECT_LIGHTING_HOOK_
	// DEFAULT's own history comment (Light.hpp) for why two different ways of doing that (OSG's
	// deprecated applyShaderCompositionUniform() stash, then a direct getLastAppliedProgramObject()
	// push) both turned out unreliable - the second broke the moment ANY Program elsewhere in the
	// same frame used StateAttribute::OVERRIDE (osgx::PBRScene::create() included),
	// confirmed via a live repro 2026-09-03. The shader loop now reads a compile-time OSGX_MAX_LIGHTS
	// bound instead, gated per-light by `enabled` - data that already lives in the uniform block this single
	// applyAttribute() call binds, so it needs no separate, Program-targeted push at all.
	state.applyAttribute(_binding.get());
}

bool LightSet::valid() const {
	return _lights.valid() && _binding.valid() && _lightCount.valid();
}

std::size_t LightSet::lightOffset(std::size_t index) const {
	if(!valid()) throw std::logic_error("LightSet is invalid");
	if(index >= static_cast<std::size_t>(MAX_LIGHTS)) throw std::out_of_range("LightSet index out of range");

	return index * LIGHT_STRUCT_FLOATS;
}

float* LightSet::lightFloats(std::size_t index, std::size_t offset) const {
	return &(*_lights)[lightOffset(index) + offset];
}

void LightSet::setPoint(
	std::size_t index,
	const osg::Vec3& position,
	const osg::Vec3& color,
	float intensity,
	float sourceRadius
) const {
	const auto base = lightOffset(index);
	const float typeBits = detail::intBitsToFloat(static_cast<int>(LightType::Point));

	_lights->set(
		{position.x(), position.y(), position.z(), intensity},
		base + detail::POS_INTENSITY_OFFSET
	);
	_lights->set({color.x(), color.y(), color.z()}, base + detail::COLOR_OFFSET);
	_lights->set({typeBits}, base + detail::TYPE_OFFSET);
	_lights->set({sourceRadius}, base + detail::SOURCE_RADIUS_OFFSET);
	_lights->set({detail::intBitsToFloat(1)}, base + detail::ENABLED_OFFSET);

	_lights->dirty();
}

void LightSet::setDirectional(
	std::size_t index,
	const osg::Vec3& direction,
	const osg::Vec3& color,
	float intensity
) const {
	const auto base = lightOffset(index);
	const float typeBits = detail::intBitsToFloat(static_cast<int>(LightType::Directional));

	_lights->set({0.0f, 0.0f, 0.0f, intensity}, base + detail::POS_INTENSITY_OFFSET);
	_lights->set({color.x(), color.y(), color.z()}, base + detail::COLOR_OFFSET);
	_lights->set({typeBits}, base + detail::TYPE_OFFSET);
	_lights->set({direction.x(), direction.y(), direction.z()}, base + detail::DIR_OFFSET);
	_lights->set({0.0f}, base + detail::SOURCE_RADIUS_OFFSET);
	_lights->set({detail::intBitsToFloat(1)}, base + detail::ENABLED_OFFSET);

	_lights->dirty();
}

void LightSet::setSpot(
	std::size_t index,
	const osg::Vec3& position,
	const osg::Vec3& direction,
	const osg::Vec3& color,
	float intensity,
	float innerConeAngle,
	float outerConeAngle,
	float sourceRadius
) const {
	const auto base = lightOffset(index);
	const float typeBits = detail::intBitsToFloat(static_cast<int>(LightType::Spot));

	_lights->set(
		{position.x(), position.y(), position.z(), intensity},
		base + detail::POS_INTENSITY_OFFSET
	);
	_lights->set({color.x(), color.y(), color.z()}, base + detail::COLOR_OFFSET);
	_lights->set({typeBits}, base + detail::TYPE_OFFSET);
	_lights->set({direction.x(), direction.y(), direction.z()}, base + detail::DIR_OFFSET);
	_lights->set({sourceRadius}, base + detail::SOURCE_RADIUS_OFFSET);
	_lights->set(
		{std::cos(innerConeAngle), std::cos(outerConeAngle)},
		base + detail::SPOT_ANGLES_OFFSET
	);
	_lights->set({detail::intBitsToFloat(1)}, base + detail::ENABLED_OFFSET);

	_lights->dirty();
}

void LightSet::setCount(std::size_t count) const {
	if(!valid()) throw std::logic_error("LightSet is invalid");
	if(count > static_cast<std::size_t>(MAX_LIGHTS)) throw std::out_of_range("LightSet count out of range");

	// _lightCount itself is no longer read by the shader (see DIRECT_LIGHTING_HOOK_DEFAULT's own
	// history comment, Light.hpp) - kept only as this object's own CPU-side bookkeeping for
	// getCount(). What the shader actually honors is each light's `enabled` flag, so setCount()
	// disables every slot this new count no longer covers - preserving the existing
	// setCount(0)-disables-everything behavior every caller relies on (e.g. a --no-lights CLI
	// flag) - without touching slots still in range, which stay however setPoint()/
	// setDirectional()/setSpot()/setEnabled() last left them; setCount() only ever narrows what's
	// active, it never (re-)enables anything on its own.
	_lightCount->set(static_cast<int>(count));

	for(auto i = count; i < static_cast<std::size_t>(MAX_LIGHTS); i++) setEnabled(i, false);
}

void LightSet::setEnabled(std::size_t index, bool enabled) const {
	_lights->set(
		{detail::intBitsToFloat(enabled ? 1 : 0)},
		lightOffset(index) + detail::ENABLED_OFFSET
	);

	_lights->dirty();
}

void LightSet::setPosition(std::size_t index, const osg::Vec3& position, float intensity) const {
	_lights->set(
		{position.x(), position.y(), position.z(), intensity},
		lightOffset(index) + detail::POS_INTENSITY_OFFSET
	);

	_lights->dirty();
}

int LightSet::getCount() const {
	if(!valid()) throw std::logic_error("LightSet is invalid");

	int count = 0;

	_lightCount->get(count);

	return count;
}

osg::Vec4 LightSet::getPosIntensity(std::size_t index) const {
	auto* f = lightFloats(index, detail::POS_INTENSITY_OFFSET);

	return osg::Vec4(f[0], f[1], f[2], f[3]);
}

osg::Vec3 LightSet::getColor(std::size_t index) const {
	auto* f = lightFloats(index, detail::COLOR_OFFSET);

	return osg::Vec3(f[0], f[1], f[2]);
}

LightType LightSet::getType(std::size_t index) const {
	auto* f = lightFloats(index, detail::TYPE_OFFSET);

	return static_cast<LightType>(detail::floatBitsToInt(f[0]));
}

bool LightSet::getEnabled(std::size_t index) const {
	auto* f = lightFloats(index, detail::ENABLED_OFFSET);

	return detail::floatBitsToInt(f[0]) != 0;
}

osg::Vec3 LightSet::getDirection(std::size_t index) const {
	auto* f = lightFloats(index, detail::DIR_OFFSET);

	return osg::Vec3(f[0], f[1], f[2]);
}

osg::Vec2 LightSet::getSpotAngles(std::size_t index) const {
	auto* f = lightFloats(index, detail::SPOT_ANGLES_OFFSET);

	return osg::Vec2(f[0], f[1]);
}

float LightSet::getSourceRadius(std::size_t index) const {
	auto* f = lightFloats(index, detail::SOURCE_RADIUS_OFFSET);

	return f[0];
}

void registerLightShaderLibs() {
	static constexpr ShaderLib libs[] = {
		{"DIRECT_SPECULAR", "osgx_DirectSpecular", DIRECT_SPECULAR},
		{"DIRECT_DIFFUSE", "osgx_DirectDiffuse", DIRECT_DIFFUSE},
		{"POINT_LIGHT_RADIANCE", "osgx_PointLightRadiance", POINT_LIGHT_RADIANCE},
		{"LIGHT_UNIFORMS", "osgx_LightUniforms", LIGHT_UNIFORMS},
		{"DIRECT_LIGHT", "osgx_DirectLight", DIRECT_LIGHT},
		{"DIRECTIONAL_LIGHT_RADIANCE", "osgx_DirectionalLightRadiance", DIRECTIONAL_LIGHT_RADIANCE},
		{"SPOT_LIGHT_RADIANCE", "osgx_SpotLightRadiance", SPOT_LIGHT_RADIANCE},
		{"LIGHT_SAMPLE", "osgx_SampleLight", LIGHT_SAMPLE},
		{"SPHERE_LIGHT_SPECULAR", "osgx_SphereLightDir", SPHERE_LIGHT_SPECULAR},
		{"DIRECT_LIGHT_SPHERE", "osgx_DirectLightSphere", DIRECT_LIGHT_SPHERE},
		{"DIRECT_LIGHTING_DECL", "osgx_DirectLighting", DIRECT_LIGHTING_DECL}
	};
	::osgx::registerShaderLibs("osgx::light", libs);
}

}
