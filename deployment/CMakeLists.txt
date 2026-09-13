OPTION( BUILD_DEPLOYMENT "Build Deployment Component" ON )

IF ( BUILD_DEPLOYMENT )

  find_package(RTTPlugin REQUIRED rtt-marshalling rtt-scripting)
  
  find_package(Boost 1.36 REQUIRED filesystem system)

    # This gathers all the .cpp files into the variable 'SRCS'
    SET( HPPS DeploymentComponent.hpp DeploymentServiceLifecycle.hpp )
    SET( SRCS DeploymentComponent.cpp )

    # Add Lua support if BUILD_LUA_RTT is on
    if(BUILD_LUA_RTT)
        add_definitions(-DBUILD_LUA_RTT)
    endif()

    orocos_library( orocos-ocl-deployment ${SRCS})
    SET_TARGET_PROPERTIES( orocos-ocl-deployment PROPERTIES
      SOVERSION ${OCL_SOVERSION}
      DEFINE_SYMBOL OCL_DLL_EXPORT)
    orocos_install_headers( ${HPPS} INSTALL include/orocos/ocl )
    
    TARGET_LINK_LIBRARIES( orocos-ocl-deployment ${RTT_PLUGIN_rtt-marshalling_${OROCOS_TARGET}_LIBRARIES} ${RTT_PLUGIN_rtt-scripting_${OROCOS_TARGET}_LIBRARIES})
    
    IF (OROCOS-RTT_CORBA_FOUND)
        orocos_library(orocos-ocl-deployment-corba CorbaDeploymentComponent.cpp )
        SET_TARGET_PROPERTIES( orocos-ocl-deployment-corba PROPERTIES
          SOVERSION ${OCL_SOVERSION}
          DEFINE_SYMBOL OCL_DLL_EXPORT)
        orocos_install_headers( CorbaDeploymentComponent.hpp INSTALL include/orocos/ocl )
        TARGET_LINK_LIBRARIES( orocos-ocl-deployment-corba orocos-ocl-deployment)
        TARGET_LINK_LIBRARIES( orocos-ocl-deployment-corba ${OROCOS-RTT_CORBA_LIBRARIES})
    ENDIF(OROCOS-RTT_CORBA_FOUND)

    IF (OCL_OPCUA_FOUND)
        orocos_library(orocos-ocl-deployment-opcua
          OpcUaDeploymentComponent.cpp OpcUaDeploymentService.cpp)
        SET_TARGET_PROPERTIES(orocos-ocl-deployment-opcua PROPERTIES
          SOVERSION ${OCL_SOVERSION}
          DEFINE_SYMBOL OCL_DLL_EXPORT)
        orocos_install_headers(OpcUaDeploymentComponent.hpp OpcUaDeploymentService.hpp
          INSTALL include/orocos/ocl)
        TARGET_INCLUDE_DIRECTORIES(orocos-ocl-deployment-opcua SYSTEM BEFORE PRIVATE
          ${RTT_OPCUA_INCLUDE_DIRS})
        TARGET_COMPILE_OPTIONS(orocos-ocl-deployment-opcua PRIVATE
          ${RTT_OPCUA_CFLAGS_OTHER})
        IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
          TARGET_COMPILE_OPTIONS(orocos-ocl-deployment-opcua PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        ENDIF()
        TARGET_LINK_LIBRARIES(orocos-ocl-deployment-opcua
          orocos-ocl-deployment PkgConfig::RTT_OPCUA)

        orocos_plugin(opcua OpcUaServicePlugin.cpp)
        TARGET_INCLUDE_DIRECTORIES(opcua SYSTEM BEFORE PRIVATE ${RTT_OPCUA_INCLUDE_DIRS})
        TARGET_LINK_LIBRARIES(opcua orocos-ocl-deployment-opcua)
    ENDIF(OCL_OPCUA_FOUND)
    
    IF(OCL_HTTP_FOUND)
      orocos_library(orocos-ocl-deployment-http HttpDeploymentService.cpp)
      SET_TARGET_PROPERTIES(orocos-ocl-deployment-http PROPERTIES
        SOVERSION ${OCL_SOVERSION} DEFINE_SYMBOL OCL_DLL_EXPORT)
      orocos_install_headers(HttpDeploymentService.hpp INSTALL include/orocos/ocl)
      target_link_libraries(orocos-ocl-deployment-http orocos-ocl-deployment rtt_http::rtt_http)
      if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(orocos-ocl-deployment-http PRIVATE -Wall -Wextra -Wpedantic -Werror)
      endif()
      orocos_plugin(http HttpServicePlugin.cpp)
      target_link_libraries(http orocos-ocl-deployment-http)
    ENDIF()

    set(deployment_dependencies)
    if(OCL_OPCUA_FOUND)
      list(APPEND deployment_dependencies rtt_opcua-${OROCOS_TARGET})
    endif()
    if(OCL_HTTP_FOUND)
      list(APPEND deployment_dependencies rtt_http-${OROCOS_TARGET})
    endif()
    orocos_generate_package(DEPENDS ${deployment_dependencies})

    IF(BUILD_TESTING)
      # RTT identifies an explicitly loaded Windows DLL by its parent folder.
      # Use the installed plugins/ layout for both test service modules.
      foreach(service IN ITEMS http opcua)
        if(TARGET ${service})
          set_target_properties(${service} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins")
          foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_target_properties(${service} PROPERTIES
              LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}/plugins"
              RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}/plugins")
          endforeach()
        endif()
      endforeach()
      if(OCL_HTTP_FOUND)
        find_path(OCL_HTTP_TEST_HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
        add_executable(ocl_http_deployment_test tests/http_deployment_test.cpp)
        orocos_configure_executable(ocl_http_deployment_test)
        add_dependencies(ocl_http_deployment_test http)
        target_compile_definitions(ocl_http_deployment_test PRIVATE
          OCL_TEST_HTTP_PLUGIN="$<TARGET_FILE:http>"
          CPPHTTPLIB_NO_DEFAULT_SIGPIPE CPPHTTPLIB_OWNED_SERVER_SOCKETS)
        target_include_directories(ocl_http_deployment_test SYSTEM PRIVATE ${OCL_HTTP_TEST_HTTPLIB_INCLUDE_DIR})
        target_link_libraries(ocl_http_deployment_test orocos-ocl-deployment-http ${OROCOS-RTT_TYPEKIT_LIBRARIES})
        if(WIN32)
          target_link_libraries(ocl_http_deployment_test ws2_32 crypt32)
        endif()
        if(OCL_OPCUA_FOUND)
          find_package(open62541pp 0.21.2 CONFIG REQUIRED)
          target_compile_definitions(ocl_http_deployment_test PRIVATE OCL_HTTP_TEST_OPCUA)
          target_link_libraries(ocl_http_deployment_test orocos-ocl-deployment-opcua open62541pp::open62541pp PkgConfig::RTT_OPCUA)
        endif()
        add_test(NAME ocl_http_deployment COMMAND ocl_http_deployment_test)
        set_tests_properties(ocl_http_deployment PROPERTIES TIMEOUT 30)
      endif()
      # Compile-only API guard: Xenomai auto-init can abort before main on non-RT hosts.
      add_executable(ocl_deployment_api_check tests/deployment_api_test.cpp)
      orocos_configure_executable(ocl_deployment_api_check)
      target_link_libraries(ocl_deployment_api_check orocos-ocl-deployment)

      IF(OCL_OPCUA_FOUND)
        find_package(open62541pp 0.21.2 CONFIG REQUIRED)
        add_executable(ocl_opcua_deployment_test tests/opcua_deployment_test.cpp)
        add_dependencies(ocl_opcua_deployment_test opcua)
        target_compile_definitions(ocl_opcua_deployment_test PRIVATE
          OCL_TEST_OPCUA_PLUGIN="$<TARGET_FILE:opcua>")
        orocos_configure_executable(ocl_opcua_deployment_test)
        target_include_directories(ocl_opcua_deployment_test SYSTEM BEFORE PRIVATE
          ${RTT_OPCUA_INCLUDE_DIRS})
        target_compile_options(ocl_opcua_deployment_test PRIVATE
          ${RTT_OPCUA_CFLAGS_OTHER})
        IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
          target_compile_options(ocl_opcua_deployment_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        ENDIF()
        target_link_libraries(ocl_opcua_deployment_test
          orocos-ocl-deployment-opcua open62541pp::open62541pp PkgConfig::RTT_OPCUA
          ${OROCOS-RTT_TYPEKIT_LIBRARIES})
        if(WIN32)
          target_link_libraries(ocl_opcua_deployment_test ws2_32)
        endif()
        foreach(case IN ITEMS
            input_writing_is_explicit_and_separate_from_publication
            endpoint_start_publishes_no_components
            explicit_complete_deployer_publication
            explicit_selected_deployer_publication
            selected_peer_publication_is_sparse
            selected_peer_publication_reports_diagnostics_atomically
            selected_publication_is_idempotent_and_rejects_conflicts
            strict_publication_is_static_and_idempotent
            server_metadata_does_not_auto_publish
            failed_start_freezes_registry_and_can_retry
            remote_components_remain_aliased_client_peers
            published_component_unload_is_rejected
            plugin_is_component_owned_and_rejects_duplicate_loads
            ordinary_deployer_preserves_publication_and_unload_veto
            lifecycle_attachment_seeds_inventory_and_rolls_back_rejection
            shutdown_closes_all_services_before_draining_held_operation
            destructor_stops_service_when_auto_unload_is_disabled)
          add_test(
            NAME ocl_opcua_deployment_${case}
            COMMAND ocl_opcua_deployment_test --run_test=${case})
          set_tests_properties(ocl_opcua_deployment_${case} PROPERTIES TIMEOUT 30)
        endforeach()
      ENDIF(OCL_OPCUA_FOUND)
    ENDIF(BUILD_TESTING)

    ADD_SUBDIRECTORY( tests )

    # This requires the orocos-rtt library to be carefully tagged
    # with the necessary 'export' attributes, or unresolved symbols
    # will appear. Especially template classes and exceptions are 
    # sensitive.
    # SET_SOURCE_FILES_PROPERTIES( ${SRCS} PROPERTIES COMPILE_FLAGS "-fvisibility=hidden")

ENDIF ( BUILD_DEPLOYMENT )
