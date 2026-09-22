//vimrun! ./examples/osgx-array

#include "osgx/Array.hpp"
#include "osgx/Core.hpp"

OSGX_DISABLE_WARNINGS

#include <osg/CopyOp>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Vec2>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

OSGX_ENABLE_WARNINGS

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

using namespace osgx::literals;
// using osgx::literals::operator""_v;

namespace {

bool check(bool condition, std::string_view description) {
	std::cout << (condition ? "PASS: " : "FAIL: ") << description << std::endl;

	return condition;
}

}

int main(int argc, char** argv) {
	auto av3_3 = osgx::Vec3Array({
		{1_v, 2_v, 3_v},
		{10_v, 20_v, 30_v},
		{100_v, 200_v, 300_v}
	});

	assert(av3_3.size() == 3);
	assert(av3_3[0] == osg::Vec3(1_v, 2_v, 3_v));

	av3_3.append_range({
		{1000_v, 2000_v, 3000_v},
		{10000_v, 20000_v, 30000_v}
	});

	assert(av3_3.size() == 5);
	assert(av3_3[3] == osg::Vec3(1000_v, 2000_v, 3000_v));

	auto nav3_2 = osgx::make_ref<osgx::Vec3Array>(
		osg::Vec3(1_v, 2_v, 3_v),
		osg::Vec3(4_v, 5_v, 6_v)
	);

	assert(nav3_2->size() == 2);
	assert((*nav3_2)[1] == osg::Vec3(4_v, 5_v, 6_v));

	nav3_2->append_n<1>({7_v, 8_v, 9_v});

	assert(nav3_2->size() == 3);
	assert((*nav3_2)[2] == osg::Vec3(7_v, 8_v, 9_v));

	auto nav2_3 = osgx::make_ref<osgx::Vec2Array>(nullptr);

	// nav2_3 = osgx::Vec2Array::create({
	nav2_3 = new osgx::Vec2Array({
		{11_v, 22_v},
		{33_v, 44_v},
		{55_v, 66_v},
	});

	assert(nav2_3->size() == 3);
	assert((*nav2_3)[2] == osg::Vec2(55_v, 66_v));

	// set(): in-place overwrite at an offset - no resize, and elements outside the written span are
	// left alone.
	av3_3.set({{-1_v, -2_v, -3_v}, {-4_v, -5_v, -6_v}}, 1);

	assert(av3_3.size() == 5);
	assert(av3_3[0] == osg::Vec3(1_v, 2_v, 3_v));
	assert(av3_3[1] == osg::Vec3(-1_v, -2_v, -3_v));
	assert(av3_3[2] == osg::Vec3(-4_v, -5_v, -6_v));
	assert(av3_3[3] == osg::Vec3(1000_v, 2000_v, 3000_v));

	// A span that would run past the end throws, and a rejected set() writes nothing.
	bool setThrew = false;

	try {
		nav2_3->set({{0_v, 0_v}, {1_v, 1_v}}, 2); // needs 4 elements, has 3
	}

	catch(const std::out_of_range&) {
		setThrew = true;
	}

	bool setWorks = check(setThrew, "set() throws std::out_of_range when the span does not fit");

	setWorks &= check(
		nav2_3->size() == 3 && (*nav2_3)[2] == osg::Vec2(55_v, 66_v),
		"a rejected set() leaves the array untouched"
	);

	// Interchangeability checks: osgx::Array should be usable anywhere its native OSG base is used,
	// including OSG's runtime type system, cloning, and serialization.
	static_assert(std::derived_from<osgx::Vec3Array, osg::Vec3Array>);

	bool interchangeable = true;
	auto native = osgx::make_ref<osg::Vec3Array>();
	osg::ref_ptr<osg::Vec3Array> nativeView = nav3_2.get();

	interchangeable &= check(nativeView.get() == nav3_2.get(), "converts to osg::Vec3Array without copying");
	interchangeable &= check(
		std::string_view(nav3_2->libraryName()) == native->libraryName(),
		"reports the native OSG library name"
	);
	interchangeable &= check(
		std::string_view(nav3_2->className()) == native->className(),
		"reports the native OSG class name"
	);

	osg::ref_ptr<osg::Object> clone = nav3_2->clone(osg::CopyOp::DEEP_COPY_ALL);
	auto* clonedArray = dynamic_cast<osg::Vec3Array*>(clone.get());

	interchangeable &= check(clonedArray != nullptr, "clones as an osg::Vec3Array-compatible object");
	interchangeable &= check(
		clonedArray && clonedArray->size() == nav3_2->size() && (*clonedArray)[2] == (*nav3_2)[2],
		"preserves values through a deep clone"
	);

	// A clone (deep or via a CopyOp) is still an osgx::Array, so old code that static_casts
	// `copyop(array)` back to the wrapper type is safe - and cloneType() agrees.
	interchangeable &= check(
		dynamic_cast<osgx::Vec3Array*>(clone.get()) != nullptr,
		"a deep clone is still an osgx::Vec3Array"
	);

	osg::ref_ptr<osg::Object> clonedType = nav3_2->cloneType();

	interchangeable &= check(
		dynamic_cast<osgx::Vec3Array*>(clonedType.get()) != nullptr &&
		std::string_view(clonedType->className()) == native->className(),
		"cloneType() is an osgx::Vec3Array with the native class name"
	);

	osg::CopyOp shallowCopy(osg::CopyOp::SHALLOW_COPY);

	interchangeable &= check(
		shallowCopy(static_cast<const osg::Vec3Array*>(nav3_2.get())) == nav3_2.get(),
		"a shallow CopyOp still shares the same array"
	);

	auto geometry = osgx::make_ref<osg::Geometry>();
	geometry->setVertexArray(nav3_2.get());
	auto elements = osgx::DrawElementsUShort::triangles(0, 1, 2);
	auto nativeElements = osgx::make_ref<osg::DrawElementsUShort>();

	interchangeable &= check(
		std::string_view(elements->libraryName()) == nativeElements->libraryName() &&
		std::string_view(elements->className()) == nativeElements->className(),
		"DrawElements reports the native OSG identity"
	);
	osg::ref_ptr<osg::Object> clonedElements = elements->clone(osg::CopyOp::DEEP_COPY_ALL);

	interchangeable &= check(
		dynamic_cast<osg::DrawElementsUShort*>(clonedElements.get()) != nullptr,
		"DrawElements clones as a native-compatible object"
	);

	// Same guarantee as the Array clones above: the clone is still an osgx::DrawElements, so
	// static_casting `copyop(elements)` back to the wrapper type is safe - and cloneType() agrees.
	auto* clonedWrapper = dynamic_cast<osgx::DrawElementsUShort*>(clonedElements.get());

	interchangeable &= check(
		clonedWrapper != nullptr && clonedWrapper != elements.get() &&
		clonedWrapper->size() == elements->size() && (*clonedWrapper)[2] == (*elements)[2],
		"a deep clone is still an osgx::DrawElementsUShort with the same indices"
	);

	osg::ref_ptr<osg::Object> clonedElementsType = elements->cloneType();

	interchangeable &= check(
		dynamic_cast<osgx::DrawElementsUShort*>(clonedElementsType.get()) != nullptr,
		"DrawElements::cloneType() is an osgx::DrawElementsUShort"
	);

	geometry->addPrimitiveSet(elements.get());

	interchangeable &= check(
		geometry->getVertexArray() == nav3_2.get(),
		"works directly as an osg::Geometry vertex array"
	);

	auto geode = osgx::make_ref<osg::Geode>();
	geode->addDrawable(geometry.get());

	// Establish that the active OSG serializer can write the equivalent native array before using
	// serialization as an interchangeability verdict.
	static constexpr GLushort nativeIndices[] = {0, 1, 2};
	auto nativeGeometry = osgx::make_ref<osg::Geometry>();
	nativeGeometry->setVertexArray(new osg::Vec3Array(nav3_2->begin(), nav3_2->end()));
	nativeGeometry->addPrimitiveSet(new osg::DrawElementsUShort(
		GL_TRIANGLES,
		std::begin(nativeIndices),
		std::end(nativeIndices)
	));
	auto nativeGeode = osgx::make_ref<osg::Geode>();
	nativeGeode->addDrawable(nativeGeometry.get());

	const auto nativePath = std::filesystem::temp_directory_path() / "osg-array-native-control.osgt";
	const bool nativeWrote = osgDB::writeNodeFile(*nativeGeode, nativePath.string());
	check(nativeWrote, "native osg::Vec3Array serialization control");

	const auto path = std::filesystem::temp_directory_path() / "osgx-array-interchange.osgt";
	const bool wrote = osgDB::writeNodeFile(*geode, path.string());
	osg::ref_ptr<osg::Node> restored = wrote ? osgDB::readRefNodeFile(path.string()) : nullptr;
	auto* restoredGeode = restored ? restored->asGeode() : nullptr;
	auto* restoredGeometry = restoredGeode && restoredGeode->getNumDrawables() > 0
		? restoredGeode->getDrawable(0)->asGeometry()
		: nullptr
	;
	auto* restoredArray = restoredGeometry
		? dynamic_cast<osg::Vec3Array*>(restoredGeometry->getVertexArray())
		: nullptr
	;

	interchangeable &= check(wrote, "serializes inside an OSG Geometry");
	interchangeable &= check(
		restoredArray && restoredArray->size() == nav3_2->size() && (*restoredArray)[2] == (*nav3_2)[2],
		"round-trips through OSG serialization as a native-compatible array"
	);

	std::error_code ec;
	std::filesystem::remove(path, ec);
	std::filesystem::remove(nativePath, ec);

	return interchangeable && setWorks ? 0 : 1;
}
