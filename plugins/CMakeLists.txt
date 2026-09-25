# osgdb_debug intentionally has no build option and is not add_subdirectory()'d here yet -- it
# doesn't have a valid design (see TODO.md), and fixing that is out of scope for the KTX2
# migration. Its CMakeLists.txt exists so the directory shape is ready, not because it builds.

# Built plugins go to <build>/osgPlugins-<version>/, the path osgDB::Registry searches under each
# library path entry, so a build dir on OSG_LIBRARY_PATH is found before an installed copy. The
# generator expression keeps multi-config generators from appending a per-config subdirectory.
set(OSGX_PLUGIN_OUTPUT_DIR "$<1:${PROJECT_BINARY_DIR}/osgPlugins-${OPENSCENEGRAPH_VERSION}>")

if(OSGX_BUILD_KTX2)
	add_subdirectory("ktx2")
endif()

if(OSGX_BUILD_GLTF)
	add_subdirectory("gltf")
endif()
