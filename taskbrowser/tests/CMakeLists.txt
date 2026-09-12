
CMAKE_DEPENDENT_OPTION(
  BUILD_TASKBROWSER_TEST
  "Build TaskBrowser Test"
  ON
  "BUILD_TASKBROWSER;BUILD_TESTS"
  OFF
)

IF(BUILD_TASKBROWSER_TEST)
  GLOBAL_ADD_TEST(taskb main.cpp)
  program_add_deps(taskb ${OROCOS-RTT_LIBRARIES} orocos-ocl-taskbrowser)
ENDIF()

IF(BUILD_TESTING)
  GLOBAL_ADD_TEST(
    taskbrowser_value_renderer_test
    structured_value_renderer_test.cpp
  )
  program_add_deps(
    taskbrowser_value_renderer_test
    ${OROCOS-RTT_LIBRARIES}
    ${OROCOS-RTT_TYPEKIT_LIBRARIES}
    orocos-ocl-taskbrowser
  )
  if(WIN32)
    # Boost.Test includes windows.h before RTT includes Winsock2.
    target_compile_definitions(taskbrowser_value_renderer_test PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
  set_tests_properties(
    taskbrowser_value_renderer_test
    PROPERTIES
      ENVIRONMENT
        "ORO_TB_HISTFILE=${CMAKE_CURRENT_BINARY_DIR}/taskbrowser-value-renderer.history"
  )
ENDIF()
