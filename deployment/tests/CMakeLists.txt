
CMAKE_DEPENDENT_OPTION( BUILD_DEPLOYMENT_TEST "Build Deployment Test" ON "BUILD_DEPLOYMENT;BUILD_TASKBROWSER;BUILD_TESTS" OFF)

IF ( BUILD_DEPLOYMENT_TEST )

    GLOBAL_ADD_TEST( deploy main.cpp )
    PROGRAM_ADD_DEPS( deploy orocos-ocl-taskbrowser orocos-ocl-deployment )

    # Copy this file to build dir.
    TEST_USES_FILE( ComponentA.cpf )
    TEST_USES_FILE( ComponentB.cpf )
    TEST_USES_FILE( deployment.cpf )

ENDIF ( BUILD_DEPLOYMENT_TEST )

if(BUILD_TESTING)
  add_executable(ocl_cyclic_deployment_test cyclic_deployment_test.cpp)
  orocos_configure_executable(ocl_cyclic_deployment_test)
  target_compile_definitions(ocl_cyclic_deployment_test PRIVATE
    OCL_CYCLIC_CONNECTION_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/cyclic-connections.ops")
  target_link_libraries(ocl_cyclic_deployment_test orocos-ocl-deployment ${OROCOS-RTT_TYPEKIT_LIBRARIES})
  add_test(NAME ocl_cyclic_deployment COMMAND ocl_cyclic_deployment_test)
  set_tests_properties(ocl_cyclic_deployment PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
  set_tests_properties(ocl_cyclic_deployment PROPERTIES
    ENVIRONMENT "RTT_COMPONENT_PATH=${CMAKE_INSTALL_PREFIX}/lib/orocos;OROCOS_TARGET=${OROCOS_TARGET}")
endif()
