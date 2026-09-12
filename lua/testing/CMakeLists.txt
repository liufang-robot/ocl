cmake_dependent_option( BUILD_LUA_TESTCOMP "Build test-component used for testing lua bindings" OFF "BUILD_LUA_RTT" OFF )

if ( BUILD_LUA_TESTCOMP )

    set(SRCS testcomp.cpp)
    file( GLOB HPPS [^.]*.hpp )

    orocos_component(orocos-lua-testcomp ${SRCS} )
    target_link_libraries( orocos-lua-testcomp ${OROCOS_RTT_LIBS} )
    
endif ( BUILD_LUA_TESTCOMP )

if(BUILD_TESTING AND TARGET rttlua)
  add_test(NAME ocl_lua_cyclic_ports COMMAND rttlua ${CMAKE_CURRENT_SOURCE_DIR}/cyclic-ports.lua)
  set_tests_properties(ocl_lua_cyclic_ports PROPERTIES
    PASS_REGULAR_EXPRESSION "cyclic port image test passed")
endif()

if(BUILD_TESTING AND TARGET orocos-ocl-lua)
  add_executable(ocl_lua_cyclic_component_test cyclic_component_test.cpp)
  orocos_configure_executable(ocl_lua_cyclic_component_test)
  target_link_libraries(ocl_lua_cyclic_component_test orocos-ocl-lua ${OROCOS-RTT_TYPEKIT_LIBRARIES})
  add_test(NAME ocl_lua_cyclic_component COMMAND ocl_lua_cyclic_component_test)
endif()

if(BUILD_TESTING AND TARGET rttlua AND TARGET orocos-ocl-lua)
  set_tests_properties(ocl_lua_cyclic_ports ocl_lua_cyclic_component PROPERTIES
    ENVIRONMENT "RTT_COMPONENT_PATH=${CMAKE_INSTALL_PREFIX}/lib/orocos;OROCOS_TARGET=${OROCOS_TARGET}")
endif()
