#include "LibraryState.hpp"
#include "ShaderLibs.hpp"

#include "osgx/Environment.hpp"
#include "osgx/Grid.hpp"
#include "osgx/IBL.hpp"
#include "osgx/Light.hpp"
#include "osgx/PBR.hpp"
#include "osgx/Picking.hpp"
#include "osgx/Projection.hpp"
#include "osgx/SDF.hpp"
#include "osgx/Shadow.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/BufferIndexBinding>

OSGX_ENABLE_WARNINGS

#include <algorithm>

namespace osgx {

namespace {

Library* liveLibrary = nullptr;

const char* typeName(Bindings::Type type) {
	switch(type) {
		case Bindings::Type::UBO: return "UBO";
		case Bindings::Type::SSBO: return "SSBO";
		case Bindings::Type::TextureUnit: return "texture unit";
	}

	return "unknown";
}

}

void Bindings::declare(Type type, std::string_view name, std::optional<unsigned int> preferred) {
	std::lock_guard<std::mutex> lock(_mutex);

	if(_closed) throw std::logic_error(
		"osgx::Bindings::declare(): '" + std::string(name) + "' declared after the first get()"
	);

	const auto sameName = [name](const Slot& slot) { return slot.name == name; };

	if(std::any_of(_slots.begin(), _slots.end(), sameName)) throw std::logic_error(
		"osgx::Bindings::declare(): '" + std::string(name) + "' is already declared"
	);

	_slots.push_back({type, std::string(name), preferred, std::nullopt});
}

unsigned int Bindings::get(std::string_view name) {
	std::lock_guard<std::mutex> lock(_mutex);

	_close();

	const auto found = std::find_if(_slots.begin(), _slots.end(), [name](const Slot& slot) {
		return slot.name == name;
	});

	if(found == _slots.end()) throw std::logic_error(
		"osgx::Bindings::get(): no binding named '" + std::string(name) + "'"
	);

	if(found->index) return *found->index;

	if(const auto pinned = _overrides.find(name); pinned != _overrides.end()) {
		found->index = pinned->second;

		return *found->index;
	}

	const auto& reserved = _reserved[found->type];

	// Reserved, assigned, or an override's; with `preferred`, also another unassigned slot's
	// preferred index.
	const auto taken = [&](unsigned int index, bool preferred) {
		if(std::find(reserved.begin(), reserved.end(), index) != reserved.end()) return true;

		for(const auto& slot : _slots) {
			if(slot.type != found->type) continue;
			if(slot.index == index) return true;

			const auto pinned = _overrides.find(slot.name);

			if(pinned != _overrides.end() && pinned->second == index) return true;
			if(preferred && &slot != &*found && !slot.index && slot.preferred == index) return true;
		}

		return false;
	};

	if(found->preferred && !taken(*found->preferred, false)) {
		found->index = found->preferred;

		return *found->index;
	}

	unsigned int index = 0;

	while(taken(index, true)) index++;

	found->index = index;

	return index;
}

LibraryOptions& LibraryOptions::reserveBelow(Bindings::Type type, unsigned int count) {
	auto& indices = reserve[type];

	for(unsigned int i = 0; i < count; i++) indices.push_back(i);

	return *this;
}

std::vector<Bindings::SlotInfo> Bindings::slots() {
	std::lock_guard<std::mutex> lock(_mutex);
	std::vector<SlotInfo> infos;

	infos.reserve(_slots.size());

	for(const auto& slot : _slots) infos.push_back({slot.type, slot.name, slot.index});

	return infos;
}

void Bindings::_close() {
	if(_closed) return;

	for(const auto& [name, index] : _overrides) {
		const auto slot = std::find_if(_slots.begin(), _slots.end(), [&name](const Slot& s) {
			return s.name == name;
		});

		if(slot == _slots.end()) throw std::logic_error(
			"osgx::Bindings: override for '" + name + "', which no library declared"
		);

		for(const auto& [otherName, otherIndex] : _overrides) {
			if(otherName <= name || otherIndex != index) continue;

			const auto other = std::find_if(_slots.begin(), _slots.end(), [&otherName](const Slot& s) {
				return s.name == otherName;
			});

			if(other != _slots.end() && other->type == slot->type) throw std::logic_error(
				"osgx::Bindings: overrides give '" + name + "' and '" + otherName + "' the same " +
				typeName(slot->type) + " binding " + std::to_string(index)
			);
		}
	}

	_closed = true;
}

void resolveBinding(std::once_flag& flag, osg::BufferIndexBinding* binding, const char* name) {
	std::call_once(flag, [binding, name]() {
		binding->setIndex(Library::instance().bindings().get(name));
	});
}

Library::Library(osg::ArgumentParser* arguments, const LibraryOptions& options) {
	if(liveLibrary) throw std::logic_error("osgx::Library: an osgx::Library is already alive");

	_state = new detail::LibraryState();
	_state->bindings._overrides.insert(options.bindings.begin(), options.bindings.end());
	_state->bindings._reserved = options.reserve;
	liveLibrary = this;

	try {
		auto& slots = _state->bindings;

		slots.declare(Bindings::Type::UBO, "osgx::environment");
		slots.declare(Bindings::Type::UBO, "osgx::grid");
		slots.declare(Bindings::Type::UBO, "osgx::light");
		slots.declare(Bindings::Type::UBO, "osgx::material");
		slots.declare(Bindings::Type::UBO, "osgx::sdf");

		slots.declare(Bindings::Type::SSBO, "osgx::joints");
		slots.declare(Bindings::Type::SSBO, "osgx::pixelText");

		// Preferred indices are the units these textures used before they were slots, which
		// application code written against those fixed numbers still avoids.
		slots.declare(Bindings::Type::TextureUnit, "osgx::material.baseColor", 0);
		slots.declare(Bindings::Type::TextureUnit, "osgx::material.normal", 1);
		slots.declare(Bindings::Type::TextureUnit, "osgx::material.orm", 2);
		slots.declare(Bindings::Type::TextureUnit, "osgx::material.emissive", 3);
		slots.declare(Bindings::Type::TextureUnit, "osgx::environment.specular", 5);
		slots.declare(Bindings::Type::TextureUnit, "osgx::environment.brdfLUT", 6);
		slots.declare(Bindings::Type::TextureUnit, "osgx::environment.diffuse", 7);
		slots.declare(Bindings::Type::TextureUnit, "osgx::sdf.texture", 10);
		slots.declare(Bindings::Type::TextureUnit, "osgx::shadowMap", 9);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.albedo", 0);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.normal", 1);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.material", 2);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.emissive", 3);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.position", 4);
		slots.declare(Bindings::Type::TextureUnit, "osgx::gbuffer.ao", 8);

		registerEnvironmentShaderLibs();
		registerGBufferShaderLibs();
		registerGridShaderLibs();
		registerIBLShaderLibs();
		registerLightShaderLibs();
		registerPBRShaderLibs();
		registerPickShaderLibs();
		registerProjectionShaderLibs();
		registerSDFShaderLibs();
		registerShadowShaderLibs();
		registerSkinningShaderLibs();
	}

	catch(...) {
		liveLibrary = nullptr;

		throw;
	}
}

Library::~Library() {
	_state = nullptr;

	liveLibrary = nullptr;
}

Library& Library::instance() {
	if(!liveLibrary) throw std::logic_error(
		"osgx::Library::instance(): no osgx::Library is alive; call osgx::initialize() first"
	);

	return *liveLibrary;
}

bool Library::alive() {
	return liveLibrary != nullptr;
}

Bindings& Library::bindings() {
	return _state->bindings;
}

detail::LibraryState& detail::libraryState() {
	return *Library::instance()._state;
}

Library initialize(osg::ArgumentParser& arguments, const LibraryOptions& options) {
	return Library(&arguments, options);
}

Library initialize(const LibraryOptions& options) {
	return Library(nullptr, options);
}

}
