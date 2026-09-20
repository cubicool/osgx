set(OSGX_EXAMPLES
	"array"
	"aura"
	"callback"
	"culling"
	"eventhandler"
	"grid"
	"hover"
	# "hover-aura"
	"ibl"
	"ktx2-skybox"
	"lights"
	"manipulator"
	"notifyfilter"
	"picking"
	"pixel-text"
	"platform"
	"shadow"
	"shapes"
	# "temporal-edge"
	"viewer"
	"visitor"
)

foreach(example IN LISTS OSGX_EXAMPLES)
	osgx_add_executable("osgx" "${example}")
endforeach()

# find_package(OpenGL REQUIRED)
# target_link_libraries(osgx-hover PRIVATE OpenGL::GL)
# target_link_libraries(osgx-hover-aura PRIVATE OpenGL::GL)
# target_link_libraries(osgx-picking PRIVATE OpenGL::GL)

if(OSGX_BUILD_GLTF)
	osgx_add_executable("osgx" "sdf")
	target_link_libraries(osgx-sdf PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "turntable")
	target_link_libraries(osgx-turntable PRIVATE osgx::gltf_pbribl)

	osgx_add_executable("osgx" "gbuffer")
	target_link_libraries(osgx-gbuffer PRIVATE osgx::gltf_pbribl)

	osgx_add_executable("osgx" "gbuffer-comic")
	target_link_libraries(osgx-gbuffer-comic PRIVATE osgx::gltf_pbribl)

	osgx_add_executable("osgx" "gbuffer-blueprint")
	target_link_libraries(osgx-gbuffer-blueprint PRIVATE osgx::gltf_pbribl)

	osgx_add_executable("osgx" "gbuffer-edgewear")
	target_link_libraries(osgx-gbuffer-edgewear PRIVATE osgx::gltf_pbribl)

else()
	message(STATUS "OSGX_BUILD_GLTF is OFF; skipping osgx-sdf, osgx-turntable and osgx-gbuffer")
endif()

# OSGX_WITH_IMGUI + the Dear ImGui detection (IMGUI_FOUND, etc.) come from the root
# CMakeLists.txt -- already resolved by the time this subdirectory is processed.
if(OSGX_WITH_IMGUI)
	add_executable(osgx-imgui
		"osgx-imgui.cpp"
		"../src/osgx/osgx.hpp"
	)

	osgx_compile_options(osgx-imgui)
	target_link_libraries(osgx-imgui PRIVATE osgx::osgx OpenGL::GL)

	add_executable(osgx-imgui-external
		"osgx-imgui-external.cpp"
		"../src/osgx/osgx.hpp"
	)

	osgx_compile_options(osgx-imgui-external)
	target_link_libraries(osgx-imgui-external PRIVATE osgx::osgx OpenGL::GL)
else()
	message(STATUS "OSGX_WITH_IMGUI is OFF; skipping osgx-imgui")
endif()
