
CMAKE_DEPENDENT_OPTION( BUILD_TIMER_TEST "Build Timer Test" ON "BUILD_TIMER;BUILD_TESTS" OFF)

IF ( BUILD_TIMER_TEST )

    # This gathers all the .cpp files into the variable 'SRCS'

    GLOBAL_ADD_TEST( timer main.cpp )
    PROGRAM_ADD_DEPS( timer orocos-ocl-taskbrowser orocos-ocl-timer )

    GLOBAL_ADD_TEST( testWithStateMachine testWithStateMachine.cpp )
    
    find_package(RTTPlugin REQUIRED rtt-scripting)
    
    PROGRAM_ADD_DEPS( testWithStateMachine ${OROCOS-RTT_LIBRARIES} orocos-ocl-taskbrowser orocos-ocl-timer ${OROCOS-RTT_RTT-SCRIPTING_LIBRARY} )
    # Copy over state machine file
    CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/testWithStateMachine.osd
                   ${CMAKE_CURRENT_BINARY_DIR}/testWithStateMachine.osd COPYONLY)

ENDIF ( BUILD_TIMER_TEST )


if(BUILD_TESTING)
  add_executable(ocl_cyclic_timer_test cyclic_timer_test.cpp)
  orocos_configure_executable(ocl_cyclic_timer_test)
  target_link_libraries(ocl_cyclic_timer_test orocos-ocl-timer ${OROCOS-RTT_TYPEKIT_LIBRARIES})
  add_test(NAME ocl_cyclic_timer COMMAND ocl_cyclic_timer_test)
endif()

if(BUILD_TESTING)
  set_tests_properties(ocl_cyclic_timer PROPERTIES
    ENVIRONMENT "RTT_COMPONENT_PATH=${CMAKE_INSTALL_PREFIX}/lib/orocos;OROCOS_TARGET=${OROCOS_TARGET}")
endif()
