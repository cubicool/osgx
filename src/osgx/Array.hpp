#pragma once

#include "Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/Array>
#include <osg/PrimitiveSet>

OSGX_ENABLE_WARNINGS

#include <initializer_list>
#include <ranges>
#include <span>

// Every osg::XxxArray typedef (IntArray, Vec3Array, FloatArray, ...) derives, via
// osg::TemplateArray/TemplateIndexArray, from an osg::MixinVector<T> specialization. Unlike
// TemplateArray/TemplateIndexArray's OTHER base (Array/IndexArray, both OSG_EXPORT), MixinVector
// itself carries no export annotation anywhere in OSG, and every one of its methods is defined
// inline inside its class body (see <osg/MixinVector>) - so any translation unit that touches
// one of these array types implicitly compiles its own private copy of MixinVector<T>'s methods
// for that T.
//
// GCC/Clang treat that as an ordinary weak/COMDAT symbol and silently fold duplicates, no matter
// how many translation units bring one, or whether OSG's own DLL happens to carry a dormant copy
// too. MSVC will ALSO fold it right up until something in the final link causes it to actually
// need a different symbol living in the same object file inside osg.lib as one of these dormant
// copies - at which point that whole object file gets pulled in, the duplicate is no longer
// dormant, and the link fails with LNK2005 "already defined". This bit osgx::gltf::Accessor's
// IntArray/UIntArray usage the moment osgx::PixelText was added elsewhere in the same module,
// with zero changes to Accessor.cpp itself - confirming this isn't a bug in any one file, but a
// standing gap in every osg::XxxArray specialization this project actually constructs.
//
// Declaring every such specialization `extern` here - matched by the one real instantiation of
// each in Array.cpp - stops every OTHER translation unit that includes this header from
// compiling its own copy. osgx then contributes exactly one copy of each, project-wide, instead
// of one per translation unit - closing off this whole class of bug instead of firefighting it
// one newly-exposed type at a time. The list below is every element type osgx::gltf::Accessor
// (the one file that constructs nearly the full glTF component-type space) actually instantiates;
// extend it if a new file starts constructing an osg::XxxArray specialization not already here.
extern template class osg::MixinVector<GLbyte>;
extern template class osg::MixinVector<GLubyte>;
extern template class osg::MixinVector<GLshort>;
extern template class osg::MixinVector<GLushort>;
extern template class osg::MixinVector<GLint>;
extern template class osg::MixinVector<GLuint>;
extern template class osg::MixinVector<GLfloat>;

extern template class osg::MixinVector<osg::Vec2>;
extern template class osg::MixinVector<osg::Vec3>;
extern template class osg::MixinVector<osg::Vec4>;

extern template class osg::MixinVector<osg::Vec2b>;
extern template class osg::MixinVector<osg::Vec3b>;
extern template class osg::MixinVector<osg::Vec4b>;

extern template class osg::MixinVector<osg::Vec2ub>;
extern template class osg::MixinVector<osg::Vec3ub>;
extern template class osg::MixinVector<osg::Vec4ub>;

extern template class osg::MixinVector<osg::Vec2s>;
extern template class osg::MixinVector<osg::Vec3s>;
extern template class osg::MixinVector<osg::Vec4s>;

extern template class osg::MixinVector<osg::Vec2us>;
extern template class osg::MixinVector<osg::Vec3us>;
extern template class osg::MixinVector<osg::Vec4us>;

extern template class osg::MixinVector<osg::Vec2i>;
extern template class osg::MixinVector<osg::Vec3i>;
extern template class osg::MixinVector<osg::Vec4i>;

extern template class osg::MixinVector<osg::Vec2ui>;
extern template class osg::MixinVector<osg::Vec3ui>;
extern template class osg::MixinVector<osg::Vec4ui>;

extern template class osg::MixinVector<osg::Matrixf>;

namespace osgx {

template<typename T>
concept OSGArray = requires {
	typename T::ElementDataType;

	requires std::derived_from<T, osg::Array>;
	// requires std::derived_from<T, osg::MixinVector<typename T::ElementDataType>>;
};

template<OSGArray BaseArray>
class Array: public BaseArray {
public:
	// using BaseArray = BaseArray;
	using ElementDataType = typename BaseArray::ElementDataType;

	using BaseArray::size;
	using BaseArray::resize;
	using BaseArray::begin;
	using BaseArray::end;
	using BaseArray::assign;

	Array() = default;

	Array(const Array& arr, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	BaseArray(arr, co) {}

	Array(std::initializer_list<ElementDataType> init) {
		assign(init.begin(), init.end());
	}

	// Preallocates `count` default-constructed elements - BaseArray's own sized constructor
	// (e.g. osg::Vec3Array(unsigned int)) isn't inherited, since Array declares its own
	// constructor set instead of `using BaseArray::BaseArray;`. For an arithmetic
	// ElementDataType (osgx::FloatArray), a bare int/unsigned literal prefers the variadic
	// single-element constructor below instead (its exact-match template deduction beats the
	// int->size_t conversion this overload needs) - pass an actual std::size_t (e.g. the `_sz`
	// UDL from Core.hpp) to select this overload unambiguously in that case. Non-arithmetic
	// element types (Vec2Array/Vec3Array/Vec4Array) have no such ambiguity: any integral argument
	// resolves here cleanly, since ElementDataType isn't constructible from it at all.
	explicit Array(std::size_t count) {
		resize(count);
	}

	template<std::ranges::input_range R>
	explicit Array(R&& r) {
		resize(std::ranges::size(r));

		std::ranges::copy(r, begin());
	}

	// std::vector-style "N default-constructed elements" constructor. Guarded off whenever
	// size_t is itself convertible to ElementDataType (e.g. FloatArray/IntArray) - there,
	// Array(5) already means "one element valued 5" via the variadic constructor below, and an
	// unconstrained sized constructor would make that call ambiguous. No such collision for
	// struct-like element types (Vec2/Vec3/Vec4Array), where this is unambiguous and safe.
	explicit Array(size_t n) requires(!std::convertible_to<size_t, ElementDataType>) {
		resize(n);
	}

	template<typename... Args>
	requires(std::convertible_to<Args, ElementDataType> && ...)
	explicit Array(Args&&... args) {
		constexpr size_t N = sizeof...(Args);

		resize(N);

		// I really, REALLY love "code folding", even though I need LLM/AI help at times to get
		// the syntax exactly right.
		size_t i = 0;

		(( (*this)[i++] = ElementDataType(std::forward<Args>(args)) ), ...);
	}

	// osg::Object* cloneType() const override { return new Array(); }
	// osg::Object* clone(const osg::CopyOp&) const override { return new Array(*this); }

	template<std::ranges::input_range R>
	void append_range(R&& r) {
		if constexpr(std::ranges::sized_range<R>) {
			auto _size = size();

			resize(_size + std::ranges::size(r));

			std::ranges::copy(r, begin() + static_cast<
				typename std::iterator_traits<decltype(begin())>::difference_type
			>(_size));
		}

		else {
			for(auto&& v : r) push_back(ElementDataType(v));
		}
	}

	void append_range(std::initializer_list<ElementDataType> il) {
		append_range(std::ranges::subrange(il.begin(), il.end()));
	}

	// TODO: In C++23, we could use `std::views::repeat`!
	// void append_n(const ElementDataType& value, size_t n) {
	// 	append_range(std::views::repeat(value, n));
	// }

	template<std::size_t N>
	constexpr void append_n(const ElementDataType& value) {
		append_range(std::views::iota(0_sz, N) | std::views::transform([&](auto) {
			return value;
		}));
	}

	// Runetime version of the above...
	void append_n(const ElementDataType& value, size_t n) {
		append_range(std::views::iota(0_sz, n) | std::views::transform([&](auto) {
			return value;
		}));
	}

	// --- Full-span access ----------------------------------------------------
	auto span() { return std::span<ElementDataType>(&(*this)[0], size()); }
	auto span() const { return std::span<const ElementDataType>(&(*this)[0], size()); }

	// --- Partial-span access -------------------------------------------------
	auto span(size_t start, size_t count) {
		return std::span<ElementDataType>(&(*this)[start], count);
	}

	auto span(size_t start, size_t count) const {
		return std::span<const ElementDataType>(&(*this)[start], count);
	}

	// --- Range-based views ---------------------------------------------------
	auto view() { return std::ranges::subrange(begin(), end()); }
	auto view() const { return std::ranges::subrange(begin(), end()); }

	// --- Partial-range view --------------------------------------------------
	auto view(size_t start, size_t count) {
		return std::ranges::subrange(begin() + start, begin() + start + count);
	}

	auto view(size_t start, size_t count) const {
		return std::ranges::subrange(begin() + start, begin() + start + count);
	}

	template<typename... Args>
	// static osg::ref_ptr<Array> create(Args&&... args) {
	static auto create(Args&&... args) {
		return osg::ref_ptr<Array>(new Array(std::forward<Args>(args)...));
	}

	// static osg::ref_ptr<Array> create(std::initializer_list<ElementDataType> init) {
	static auto create(std::initializer_list<ElementDataType> init) {
		return osg::ref_ptr<Array>(new Array(init));
	}
};

using Vec2Array = Array<osg::Vec2Array>;
using Vec3Array = Array<osg::Vec3Array>;
using Vec4Array = Array<osg::Vec4Array>;
using FloatArray = Array<osg::FloatArray>;

// ================================================================================================
// Concepts
// ================================================================================================

template<typename T>
concept OSGDrawElements =
requires {
	typename T::value_type;

	requires std::derived_from<T, osg::DrawElements>;
};

// ================================================================================================
// Generic wrapper
// ================================================================================================

template<OSGDrawElements BaseElements>
class DrawElements: public BaseElements {
public:
	using value_type = typename BaseElements::value_type;
	using index_type = value_type;

	using BaseElements::size;
	using BaseElements::resize;
	using BaseElements::begin;
	using BaseElements::end;
	using BaseElements::assign;
	using BaseElements::push_back;
	using BaseElements::reserve;

	// --------------------------------------------------------------------------------------------
	// Constructors
	// --------------------------------------------------------------------------------------------

	DrawElements():
	BaseElements(osg::PrimitiveSet::TRIANGLES) {}

	explicit DrawElements(GLenum mode):
	BaseElements(mode) {}

	DrawElements(const DrawElements& rhs, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	BaseElements(rhs, co) {}

	DrawElements(GLenum mode, std::initializer_list<value_type> init):
	BaseElements(mode) {
		assign(init.begin(), init.end());
	}

	template<std::ranges::input_range R>
	DrawElements(GLenum mode, R&& r):
	BaseElements(mode) {
		append_range(std::forward<R>(r));
	}

	template<typename... Args>
	requires(std::convertible_to<Args, value_type> && ...)
	DrawElements(GLenum mode, Args&&... args):
	BaseElements(mode) {
		append(std::forward<Args>(args)...);
	}

	// --------------------------------------------------------------------------------------------
	// Append helpers
	// --------------------------------------------------------------------------------------------

	template<typename... Args>
	requires(std::convertible_to<Args, value_type> && ...)
	void append(Args&&... args) {
		(push_back(static_cast<value_type>(args)), ...);
	}

	template<std::ranges::input_range R>
	void append_range(R&& r) {
		if constexpr(std::ranges::sized_range<R>) {
			reserve(size() + std::ranges::size(r));
		}

		for(auto&& v : r) push_back(static_cast<value_type>(v));
	}

	void append_range(std::initializer_list<value_type> il) {
		append_range(std::views::all(il));
	}

	// --------------------------------------------------------------------------------------------
	// Triangle helpers
	// --------------------------------------------------------------------------------------------

	template<typename... Args>
	requires(sizeof...(Args) % 3 == 0)
	static auto triangles(Args&&... args) {
		return osg::ref_ptr<DrawElements>(new DrawElements(
			osg::PrimitiveSet::TRIANGLES,
			std::forward<Args>(args)...)
		);
	}

	template<typename... Args>
	requires(sizeof...(Args) % 2 == 0)
	static auto lines(Args&&... args) {
		return osg::ref_ptr<DrawElements>(new DrawElements(
			osg::PrimitiveSet::LINES,
			std::forward<Args>(args)...)
		);
	}

	template<typename... Args>
	static auto strip(Args&&... args) {
		return osg::ref_ptr<DrawElements>(new DrawElements(
			osg::PrimitiveSet::TRIANGLE_STRIP,
			std::forward<Args>(args)...)
		);
	}

	template<typename... Args>
	static auto fan(Args&&... args) {
		return osg::ref_ptr<DrawElements>(new DrawElements(
			osg::PrimitiveSet::TRIANGLE_FAN,
			std::forward<Args>(args)...)
		);
	}

	// --------------------------------------------------------------------------------------------
	// Safe push helpers
	// --------------------------------------------------------------------------------------------

	void checked_push(unsigned int v) {
		if(v > std::numeric_limits<value_type>::max()) {
			throw std::out_of_range("DrawElements index overflow");
		}

		push_back(static_cast<value_type>(v));
	}

	// --------------------------------------------------------------------------------------------
	// Views
	// --------------------------------------------------------------------------------------------

	auto span() {
		return std::span<value_type>(&(*this)[0], size());
	}

	auto span() const {
		return std::span<const value_type>(&(*this)[0], size());
	}

	auto view() {
		return std::ranges::subrange(begin(), end());
	}

	auto view() const {
		return std::ranges::subrange(begin(), end());
	}

	// --------------------------------------------------------------------------------------------
	// Factories
	// --------------------------------------------------------------------------------------------

	template<typename... Args>
	static auto create(Args&&... args) {
		return osg::ref_ptr<DrawElements>(new DrawElements(std::forward<Args>(args)...));
	}
};

// ================================================================================================
// Aliases
// ================================================================================================

using DrawElementsUByte = DrawElements<osg::DrawElementsUByte>;
using DrawElementsUShort = DrawElements<osg::DrawElementsUShort>;
using DrawElementsUInt = DrawElements<osg::DrawElementsUInt>;

// ================================================================================================
// DrawArrays wrapper
// ================================================================================================

// osg::DrawArrays isn't a template family like DrawElements (one concrete class, no index-type
// variants), so this is a direct subclass rather than another OSGArray-style template. Its real
// job: osg::DrawArrays' first/count/numInstances are GLint/GLsizei/int, but real call sites almost
// always compute them from size_t (container.size(), size() - 1, etc.), which trips
// -Wconversion under -Werror at every call site. Smooth that over here once instead.
class DrawArrays: public osg::DrawArrays {
public:
	DrawArrays(GLenum mode=0): osg::DrawArrays(mode) {}

	DrawArrays(GLenum mode, size_t first, size_t count, size_t numInstances=0):
	osg::DrawArrays(
		mode,
		static_cast<GLint>(first),
		static_cast<GLsizei>(count),
		static_cast<int>(numInstances)
	) {}

	DrawArrays(const DrawArrays& da, const osg::CopyOp& co=osg::CopyOp::SHALLOW_COPY):
	osg::DrawArrays(da, co) {}

	template<typename... Args>
	static auto create(Args&&... args) {
		return osg::ref_ptr<DrawArrays>(new DrawArrays(std::forward<Args>(args)...));
	}
};

}
