add_library(osgdb_gltf MODULE
	ReaderWriterGLTF.cpp
)

osgx_compile_options(osgdb_gltf)
target_link_libraries(osgdb_gltf PRIVATE osgx::gltf)

set_target_properties(osgdb_gltf PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
	RUNTIME_OUTPUT_DIRECTORY "${OSGX_PLUGIN_OUTPUT_DIR}"
)

if(OSGX_INSTALL)
	install(TARGETS osgdb_gltf
		DESTINATION "${CMAKE_INSTALL_PREFIX}/lib64/osgPlugins-${OPENSCENEGRAPH_VERSION}"
	)
endif()
