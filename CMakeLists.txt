cmake_minimum_required(VERSION 3.25)

project(iocp_http_runtime LANGUAGES CXX)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

option(IOCP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# 빌드 아웃풋 정리: bin/ lib/ test/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

function(iocp_target_output_dirs target type)
    if(type STREQUAL "app")
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    elseif(type STREQUAL "lib")
        set_target_properties(${target} PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    elseif(type STREQUAL "test")
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    endif()
endfunction()

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de # v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

function(iocp_enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
        if(IOCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -finput-charset=UTF-8
                -fexec-charset=UTF-8
        )
        if(IOCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(iocp_server_core
    src/buffer/receive_buffer.cpp
    src/buffer/ring_receive_buffer.cpp
    src/core/logging.cpp
    src/core/config_utils.cpp
    src/core/json_utils.cpp
    src/execution/iocp_executor.cpp
    src/execution/manual_executor.cpp
    src/execution/serial_executor.cpp
    src/execution/thread_pool_executor.cpp
    src/runtime/io_context.cpp
    src/platform/windows/winsock_runtime.cpp
    src/protocol/http/http_message.cpp
    src/protocol/http/http_request_parser.cpp
    src/protocol/http/http_response_encoder.cpp
    src/protocol/http/http_router.cpp
    src/protocol/http/http_session.cpp
    src/protocol/http/simple_template.cpp
    src/protocol/http2/http2_frames.cpp
    src/protocol/http2/http2_hpack.cpp
    src/protocol/http2/http2_stream.cpp
    src/protocol/sample/length_prefixed_codec.cpp
    src/protocol/sample/length_prefixed_session.cpp
    src/protocol/sample/sample_dispatcher.cpp
    src/transport/connection_registry.cpp
    src/transport/send_queue.cpp
    src/transport/tcp_connection.cpp
    src/transport/tcp_connector.cpp
    src/transport/tcp_listener.cpp
)

target_compile_features(iocp_server_core PUBLIC cxx_std_17)
target_include_directories(iocp_server_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(iocp_server_core PUBLIC Threads::Threads ws2_32 tomlplusplus::tomlplusplus)

add_library(iocp_echo_application
    apps/echo_server/configuration.cpp
    apps/echo_server/echo_server.cpp
    apps/echo_server/toml_config_loader.cpp
)
target_link_libraries(
    iocp_echo_application
    PUBLIC iocp_server_core
    PRIVATE tomlplusplus::tomlplusplus
)
target_include_directories(
    iocp_echo_application
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/apps"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

add_executable(iocp_echo_server apps/echo_server/main.cpp)
target_link_libraries(iocp_echo_server PRIVATE iocp_echo_application)

add_executable(iocp_echo_client apps/echo_client/main.cpp)
target_link_libraries(iocp_echo_client PRIVATE iocp_echo_application)

add_library(iocp_http_application
    apps/http_server/configuration.cpp
    apps/http_server/http_server.cpp
)
target_link_libraries(
    iocp_http_application
    PUBLIC iocp_server_core
    PRIVATE tomlplusplus::tomlplusplus
)
target_include_directories(
    iocp_http_application
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/apps"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

add_executable(iocp_http_server apps/http_server/main.cpp)
target_link_libraries(iocp_http_server PRIVATE iocp_http_application)

add_library(iocp_webapp
    apps/webapp/webapp.cpp
    apps/webapp/board_handlers.cpp
    apps/webapp/configuration.cpp
)
target_link_libraries(
    iocp_webapp
    PUBLIC iocp_server_core
    PRIVATE tomlplusplus::tomlplusplus
)
target_include_directories(
    iocp_webapp
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/apps"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(
    iocp_webapp
    PRIVATE IOCP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(iocp_webapp_server apps/webapp/main.cpp)
target_link_libraries(iocp_webapp_server PRIVATE iocp_webapp)

iocp_enable_project_warnings(iocp_server_core)
iocp_enable_project_warnings(iocp_echo_application)
iocp_enable_project_warnings(iocp_echo_server)
iocp_enable_project_warnings(iocp_echo_client)
iocp_enable_project_warnings(iocp_http_application)
iocp_enable_project_warnings(iocp_http_server)
iocp_enable_project_warnings(iocp_webapp)
iocp_enable_project_warnings(iocp_webapp_server)

if(BUILD_TESTING)
    add_executable(buffer_tests tests/buffer_tests.cpp)
    target_link_libraries(buffer_tests PRIVATE iocp_server_core)

    add_executable(execution_tests tests/execution_tests.cpp)
    target_link_libraries(execution_tests PRIVATE iocp_server_core)

    add_executable(m2_transport_tests tests/m2_transport_tests.cpp)
    target_link_libraries(m2_transport_tests PRIVATE iocp_echo_application)

    add_executable(protocol_tests tests/protocol_tests.cpp)
    target_link_libraries(protocol_tests PRIVATE iocp_server_core)

    add_executable(http_tests tests/http_tests.cpp)
    target_link_libraries(http_tests PRIVATE iocp_server_core)

    add_executable(http_chunked_tests tests/http_chunked_tests.cpp)
    target_link_libraries(http_chunked_tests PRIVATE iocp_server_core)

    add_executable(http2_tests tests/http2_tests.cpp)
    target_link_libraries(http2_tests PRIVATE iocp_server_core)

    add_executable(webapp_handler_tests tests/webapp_handler_tests.cpp)
    target_link_libraries(webapp_handler_tests PRIVATE iocp_webapp)
    target_compile_definitions(
        webapp_handler_tests
        PRIVATE IOCP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(http_server_tests tests/http_server_tests.cpp)
    target_link_libraries(
        http_server_tests
        PRIVATE iocp_http_application
    )

    add_executable(
        http_configuration_tests
        tests/http_configuration_tests.cpp
    )
    target_link_libraries(
        http_configuration_tests
        PRIVATE iocp_http_application
    )
    target_compile_definitions(
        http_configuration_tests
        PRIVATE IOCP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(configuration_tests tests/configuration_tests.cpp)
    target_link_libraries(
        configuration_tests
        PRIVATE iocp_echo_application
    )

    iocp_enable_project_warnings(buffer_tests)
    iocp_enable_project_warnings(execution_tests)
    iocp_enable_project_warnings(m2_transport_tests)
    iocp_enable_project_warnings(protocol_tests)
    iocp_enable_project_warnings(http_tests)
    iocp_enable_project_warnings(http_chunked_tests)
    iocp_enable_project_warnings(http2_tests)
    iocp_enable_project_warnings(webapp_handler_tests)
    iocp_enable_project_warnings(http_server_tests)
    iocp_enable_project_warnings(http_configuration_tests)
    iocp_enable_project_warnings(configuration_tests)

    add_test(NAME buffer_tests COMMAND buffer_tests)
    add_test(NAME execution_tests COMMAND execution_tests)
    add_test(NAME m2_transport_tests COMMAND m2_transport_tests)
    add_test(NAME protocol_tests COMMAND protocol_tests)
    add_test(NAME http_tests COMMAND http_tests)
    add_test(NAME http_chunked_tests COMMAND http_chunked_tests)
    add_test(NAME http2_tests COMMAND http2_tests)
    add_test(NAME webapp_handler_tests COMMAND webapp_handler_tests)
    add_test(NAME http_server_tests COMMAND http_server_tests)
    add_test(
        NAME http_configuration_tests
        COMMAND http_configuration_tests
    )
    add_test(NAME configuration_tests COMMAND configuration_tests)

    # test 실행 파일은 test/ 디렉터리로
    set(TEST_TARGETS
        buffer_tests execution_tests m2_transport_tests
        protocol_tests http_tests http_chunked_tests
        http2_tests webapp_handler_tests http_server_tests
        http_configuration_tests configuration_tests)
    foreach(target ${TEST_TARGETS})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")
    endforeach()
endif()

# --- Install: 배포용 레이아웃 ---
# cmake --install build/windows-release --prefix dist

# 서버/클라 실행파일
install(TARGETS iocp_echo_server iocp_echo_client iocp_http_server iocp_webapp_server
    RUNTIME DESTINATION bin)

# config 파일
install(FILES config/echo_server.toml config/http_server.toml config/webapp.toml
    DESTINATION config)

# webapp templates
install(DIRECTORY apps/webapp/templates/
    DESTINATION share/webapp/templates)
