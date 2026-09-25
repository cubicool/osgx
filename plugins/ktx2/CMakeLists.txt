add_library(osgx_ktx2 STATIC
	KTX2.cpp
)
add_library(osgx::ktx2 ALIAS osgx_ktx2)

osgx_compile_options(osgx_ktx2)
target_link_libraries(osgx_ktx2
	PUBLIC osgx::core
	PRIVATE ktx
)
target_sources(osgx_ktx2 PUBLIC
	"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../src/osgx/ktx2/KTX2.hpp>"
)
set_target_properties(osgx_ktx2 PROPERTIES
	EXPORT_NAME ktx2
	POSITION_INDEPENDENT_CODE ON
)

add_library(osgdb_ktx2 MODULE
	ReaderWriterKTX2.cpp
)

osgx_compile_options(osgdb_ktx2)
target_link_libraries(osgdb_ktx2 PRIVATE osgx::ktx2)

set_target_properties(osgdb_ktx2 PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
)

if(OSGX_INSTALL)
	install(TARGETS osgx_ktx2 EXPORT osgxTargets
		ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
		LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	)
	install(TARGETS osgdb_ktx2
		DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}"
	)
endif()
