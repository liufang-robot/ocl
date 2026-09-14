CMAKE_DEPENDENT_OPTION( BUILD_REPORTING_TEST "Build Reporting Test" ON "BUILD_REPORTING;BUILD_TESTS" OFF)

IF ( BUILD_REPORTING_TEST )

    # This gathers all the .cpp files into the variable 'SRCS'

    GLOBAL_ADD_TEST( report main.cpp )
    # Use  TARGET_LINK_LIBRARIES( report libs... ) to add library deps.
    PROGRAM_ADD_DEPS( report orocos-ocl-taskbrowser orocos-ocl-reporting )

    GLOBAL_ADD_TEST( tcpreport tcpmain.cpp )
    # Use  TARGET_LINK_LIBRARIES( report libs... ) to add library deps.
    PROGRAM_ADD_DEPS( tcpreport orocos-ocl-taskbrowser orocos-ocl-reporting )

    # Copy this file to build dir.
    TEST_USES_FILE( reporter.cpf )

  IF ( BUILD_REPORTING_NETCDF AND NETCDF_FOUND )

    GLOBAL_ADD_TEST( ncreport ncmain.cpp )
    # Use  TARGET_LINK_LIBRARIES( report libs... ) to add library deps.
    PROGRAM_ADD_DEPS( ncreport orocos-ocl-taskbrowser orocos-ocl-reporting-netcdf )

  ENDIF( BUILD_REPORTING_NETCDF AND NETCDF_FOUND )

ENDIF ( BUILD_REPORTING_TEST )


if(BUILD_TESTING)
  add_executable(ocl_cyclic_reporting_test cyclic_reporting_test.cpp)
  orocos_configure_executable(ocl_cyclic_reporting_test)
  target_link_libraries(ocl_cyclic_reporting_test orocos-ocl-reporting ${OROCOS-RTT_TYPEKIT_LIBRARIES})
  add_test(NAME ocl_cyclic_reporting COMMAND ocl_cyclic_reporting_test)
endif()

if(BUILD_TESTING)
  set_tests_properties(ocl_cyclic_reporting PROPERTIES
    ENVIRONMENT "RTT_COMPONENT_PATH=${CMAKE_INSTALL_PREFIX}/lib/orocos;OROCOS_TARGET=${OROCOS_TARGET}")
endif()
