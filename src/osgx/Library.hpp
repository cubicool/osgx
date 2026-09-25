#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/ArgumentParser>
#include <osg/ref_ptr>

OSGX_ENABLE_WARNINGS

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace osg {
	class BufferIndexBinding;
}

namespace osgx {

namespace detail {
	struct LibraryState;

	// The live Library's state; throws std::logic_error when no Library is alive. For libosgx's own
	// translation units (shader-lib catalogs, shader cache, BRDF LUT cache, PixelText atlas, debug
	// accumulators); not part of the public API.
	LibraryState& libraryState();
}

// ================================================================================================
// Binding slots
//
// Named, process-wide GL binding points. Each library declares its slot names from its Library
// constructor, optionally with a preferred index. A slot gets an index the first time it is used
// (get()), so only the features a program uses consume indices: the index LibraryOptions::bindings
// gives it; else its preferred index if that is not reserved (LibraryOptions::reserve), assigned,
// or claimed by an override; else the lowest index of its type that is none of those and not
// another slot's preferred index. GLSL refers to a slot as `@<name>@`
// (e.g. `layout(std140, binding = @osgx::environment@)`), which resolveShaderLibs() replaces with
// its index; C++ looks it up with get().
//
// Declarations close on the first get(); declaring after that throws. So do an override naming a
// slot nobody declared and two overrides giving slots of one type the same index. get() is
// thread-safe (attributes resolve their slots from apply(), on any draw thread).
// ================================================================================================
class Bindings {
	public:
		// Each type is its own GL index space. UBO and SSBO name buffer binding points
		// (GL_UNIFORM_BUFFER / GL_SHADER_STORAGE_BUFFER); docs/CORE.md's "Buffer blocks and
		// samplers" says which one a block should use.
		enum class Type {
			UBO,
			SSBO,
			TextureUnit
		};

		void declare(
			Type type,
			std::string_view name,
			std::optional<unsigned int> preferred=std::nullopt
		);

		// The slot's index, assigned on its first call; throws std::logic_error for an undeclared
		// name.
		unsigned int get(std::string_view name);

	private:
		friend class Library;

		struct Slot {
			Type type;
			std::string name;
			std::optional<unsigned int> preferred;
			std::optional<unsigned int> index;
		};

		void _close();

		std::vector<Slot> _slots;
		std::map<std::string, unsigned int, std::less<>> _overrides;
		std::map<Type, std::vector<unsigned int>> _reserved;
		bool _closed = false;
		std::mutex _mutex;
};

struct LibraryOptions {
	// Binding index overrides by slot name, e.g. {{"osgx::environment", 9}}.
	std::map<std::string, unsigned int> bindings;

	// Indices never assigned automatically, per type - e.g. {{Bindings::Type::SSBO, {0, 1, 2, 3}}}
	// for an application whose own shaders use SSBO bindings 0-3. An override may still name one.
	std::map<Bindings::Type, std::vector<unsigned int>> reserve;
};

// Sets `binding`'s index to the named slot's, the first time it is called for `flag`. For
// StateAttributes, which may be constructed before any Library exists (the glTF loader builds
// Materials and SDFs): call it from apply(), which only runs once a Library does.
void resolveBinding(std::once_flag& flag, osg::BufferIndexBinding* binding, const char* name);

// ================================================================================================
// Library lifetime
//
// osgx::Library owns all of libosgx's process-wide state: the shader-lib catalogs, the shader
// cache, the shared BRDF LUTs, the PixelText atlas, and the debug profiler's per-context
// accumulators. That state is created by the constructor and released by the destructor, never by
// a function-local static, so its construction and destruction are explicit.
//
// Create exactly one, before building any osgx shader or scene, and declare it before the viewer:
//
//   auto lib = osgx::initialize(arguments);
//   osgViewer::Viewer viewer(arguments);
//
// The viewer (and its graphics contexts) is then destroyed first, and the Library releases its
// state while OSG is still fully alive. Using osgx state with no live Library throws, as does
// constructing a second Library while one is alive.
//
// Libraries built on osgx subclass it. Base-before-derived construction initializes osgx first;
// derived-before-base destruction releases the subclass's own state first:
//
//   class Library: public osgx::Library { ... }; // e.g. osgSlug::Library
//
//   auto lib = osgSlug::initialize(arguments);
//
// The state lives in libosgx's single compiled definition (Library.cpp), not in a header or a class
// template, so every module loaded into the process (executables, osgdb_* plugins, the Python
// module) sees the same instance.
// ================================================================================================
class Library {
	public:
		// `arguments` is reserved for command-line configuration; none is read yet.
		explicit Library(
			osg::ArgumentParser* arguments=nullptr,
			const LibraryOptions& options=LibraryOptions()
		);

		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;

		virtual ~Library();

		// The live Library; throws std::logic_error when there is none.
		static Library& instance();

		// The live Library as a subclass; throws std::logic_error when there is none or it is not
		// a T.
		template<typename T>
		static T& instance() {
			auto* library = dynamic_cast<T*>(&instance());

			if(!library) throw std::logic_error(
				std::string("osgx::Library::instance<T>(): the live Library is not a ") + typeid(T).name()
			);

			return *library;
		}

		static bool alive();

		Bindings& bindings();

	private:
		friend detail::LibraryState& detail::libraryState();

		osg::ref_ptr<detail::LibraryState> _state;
};

// Constructs the Library. Returned by value (guaranteed copy elision), so `auto lib = ...` works
// with the non-copyable, non-movable Library.
Library initialize(osg::ArgumentParser& arguments, const LibraryOptions& options=LibraryOptions());
Library initialize(const LibraryOptions& options=LibraryOptions());

}
