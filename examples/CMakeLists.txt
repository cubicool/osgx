set(OSGX_EXAMPLES
	"array"
	"aura"
	"bc7"
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

# TEMPORARY: osgx-pbotexture.cpp is being tried against a real WebM decode via mbew, sourced
# directly from an unvendored ~/dev/mbew checkout (not ext/, not a submodule) to see whether it's
# worth vendoring for real. Links the static libs from mbew's OWN already-configured `BUILD` dir
# rather than add_subdirectory()'ing the whole mbew project in here, which would also pull in all
# of mbew's own SDL2/SDL3/cairo/libao examples. Remove this block (and revert
# osgx-pbotexture.cpp) once a decision is made either way.
set(OSGX_TEMP_MBEW_DIR "$ENV{HOME}/dev/mbew")
set(OSGX_TEMP_MBEW_BUILD_DIR "${OSGX_TEMP_MBEW_DIR}/BUILD")

if(EXISTS "${OSGX_TEMP_MBEW_DIR}/src/mbew.hpp"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/libmbewd.a"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-vpx/libmbew-vpxd.a"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-nestegg/libmbew-nesteggd.a"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-vorbis/libmbew-vorbisd.a"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-ogg/libmbew-oggd.a"
	AND EXISTS "${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-opus/libmbew-opusd.a")
	list(APPEND OSGX_EXAMPLES "pbotexture")
else()
	message(STATUS "mbew not found or not built; skipping osgx-pbotexture")
endif()

foreach(example IN LISTS OSGX_EXAMPLES)
	osgx_add_executable("osgx" "${example}")
endforeach()

if(TARGET osgx-pbotexture)
	target_include_directories(osgx-pbotexture PRIVATE "${OSGX_TEMP_MBEW_DIR}/src")
	target_link_libraries(osgx-pbotexture PRIVATE
		-Wl,--start-group
		"${OSGX_TEMP_MBEW_BUILD_DIR}/libmbewd.a"
		"${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-vpx/libmbew-vpxd.a"
		"${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-nestegg/libmbew-nesteggd.a"
		"${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-vorbis/libmbew-vorbisd.a"
		"${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-ogg/libmbew-oggd.a"
		"${OSGX_TEMP_MBEW_BUILD_DIR}/ext/mbew-opus/libmbew-opusd.a"
		-Wl,--end-group
		pthread
	)
endif()

# find_package(OpenGL REQUIRED)
# target_link_libraries(osgx-hover PRIVATE OpenGL::GL)
# target_link_libraries(osgx-hover-aura PRIVATE OpenGL::GL)
# target_link_libraries(osgx-picking PRIVATE OpenGL::GL)

if(OSGX_BUILD_GLTF)
	osgx_add_executable("osgx" "sdf")
	target_link_libraries(osgx-sdf PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "turntable")
	target_link_libraries(osgx-turntable PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "gbuffer")
	target_link_libraries(osgx-gbuffer PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "gbuffer-comic")
	target_link_libraries(osgx-gbuffer-comic PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "gbuffer-blueprint")
	target_link_libraries(osgx-gbuffer-blueprint PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "gbuffer-edgewear")
	target_link_libraries(osgx-gbuffer-edgewear PRIVATE osgx::gltf)

	osgx_add_executable("osgx" "library")
	target_link_libraries(osgx-library PRIVATE osgx::gltf)

else()
	message(STATUS "OSGX_BUILD_GLTF is OFF; skipping osgx-sdf, osgx-turntable, osgx-gbuffer and osgx-library")
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
