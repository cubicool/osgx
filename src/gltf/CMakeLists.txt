add_library(osgx_gltf STATIC
	Accessor.cpp
	Animation.cpp
	Environment.cpp
	Material.cpp
	Mesh.cpp
	Reader.cpp
	ReaderImpl.cpp
	Scene.cpp
	SDF.cpp
	Skin.cpp
	Texture.cpp
	"${PROJECT_SOURCE_DIR}/ext/tinygltf/tiny_gltf_v3.c"
)
add_library(osgx::gltf ALIAS osgx_gltf)

osgx_compile_options(osgx_gltf)
target_include_directories(osgx_gltf PRIVATE "${PROJECT_SOURCE_DIR}/ext/tinygltf")
target_link_libraries(osgx_gltf PUBLIC osgx::osgx)
target_sources(osgx_gltf PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osgx/gltf/Environment.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osgx/gltf/Reader.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osgx/gltf/SDF.hpp>"
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../osgx/gltf/SimplePlayer.hpp>"
)
set_target_properties(osgx_gltf PROPERTIES
	EXPORT_NAME gltf
	POSITION_INDEPENDENT_CODE ON
	C_STANDARD 11
	C_STANDARD_REQUIRED ON
)

# tiny_gltf_v3.c lives in ext/tinygltf (a .c file among otherwise-.cpp sources) and must be
# compiled as C, not C++ -- CMake infers language from file extension by default, which is
# already correct here, but this is explicit since the extension lives outside src/gltf/.
# TINYGLTF3_ENABLE_STB_IMAGE turns on tiny_gltf_v3.c's built-in stb_image-based decoder, same
# intent as the old STB_IMAGE_IMPLEMENTATION/STB_IMAGE_WRITE_IMPLEMENTATION defines it replaces.
set_source_files_properties("${PROJECT_SOURCE_DIR}/ext/tinygltf/tiny_gltf_v3.c" PROPERTIES
	LANGUAGE C
	COMPILE_DEFINITIONS TINYGLTF3_ENABLE_STB_IMAGE
)

if(OSGX_INSTALL)
	install(TARGETS osgx_gltf EXPORT osgxTargets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	)
endif()
