add_library(osgdb_dds MODULE
	ReaderWriterDDS.cpp
)

osgx_compile_options(osgdb_dds)
target_link_libraries(osgdb_dds PRIVATE osgx::core)

set_target_properties(osgdb_dds PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}"
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
)

if(OSGX_INSTALL)
	install(TARGETS osgdb_dds
		DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}"
	)
endif()
