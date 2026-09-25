#include "osgx-python.hpp"
#include "osgx-library.hpp"

#include <memory>

namespace osgx_python {

Library::Library(const osgx::LibraryOptions& options): osgx::Library(nullptr, options) {}

void bind_library(py::module_& m) {
	auto bindingsClass = py::class_<osgx::Bindings>(
		m,
		"Bindings",
		"Named GL binding slots. A slot gets its index the first time it is used, so only the "
		"features a program uses consume indices. Configure with osgx.initialize(bindings=..., "
		"reserve=...); read with Library.binding(name)."
	);

	py::enum_<osgx::Bindings::Type>(bindingsClass, "Type", "The kind of GL binding point a slot names.")
		.value("UBO", osgx::Bindings::Type::UBO)
		.value("SSBO", osgx::Bindings::Type::SSBO)
		.value("TextureUnit", osgx::Bindings::Type::TextureUnit)
	;

	py::class_<Library, std::unique_ptr<Library>>(
		m,
		"Library",
		"Owns osgx's process-wide state (shader-lib catalogs, shader cache, BRDF LUTs, PixelText "
		"atlas, the osgx.gltf texture cache). Create exactly one with osgx.initialize() before using "
		"osgx, and keep a reference until the viewer is done; the state is released when the object "
		"is destroyed. osgx facilities that need it raise while no Library is alive."
	)
		.def_static("alive", &osgx::Library::alive, "True while an osgx.Library exists.")
		.def(
			"binding",
			[](Library& self, const std::string& name) { return self.bindings().get(name); },
			"name"_a,
			"The index of the named binding slot (e.g. \"osgx::environment\"). The first lookup "
			"freezes the slot table."
		)
	;

	m.def(
		"initialize",
		[](
			const std::map<std::string, unsigned int>& bindings,
			const std::map<osgx::Bindings::Type, std::vector<unsigned int>>& reserve
		) {
			return std::make_unique<Library>(osgx::LibraryOptions{bindings, reserve});
		},
		"bindings"_a = std::map<std::string, unsigned int>(),
		"reserve"_a = std::map<osgx::Bindings::Type, std::vector<unsigned int>>(),
		"Creates the osgx.Library. `bindings` pins slot indices by name, e.g. "
		"{\"osgx::environment\": 9}; `reserve` lists indices never assigned automatically, e.g. "
		"{osgx.Bindings.Type.SSBO: [0, 1, 2, 3]}. Raises if a Library is already alive, if two "
		"pinned slots of one type share an index, or if a pin names an undeclared slot."
	);
}

}
