# Logos Module Loader Qt tests

add_executable(logos_module_loader_qt_tests
    test_qt_plugin_format_loader.cpp
    test_token_source.cpp
    test_module_path.cpp
    test_transport_set_arg.cpp
    test_host_resolution.cpp
    test_native_module_host.cpp
    test_export_link.cpp
    # These live in the host executables (not a linkable library), so compile
    # their implementations directly into the test binary. Both are Qt-free,
    # which is what keeps this binary free of Qt and the SDK stack.
    ${CMAKE_SOURCE_DIR}/src/host/module_path.cpp
    ${CMAKE_SOURCE_DIR}/src/host/command_line_parser.cpp
)

target_link_libraries(logos_module_loader_qt_tests PRIVATE
    logos_module_loader_qt
    # The native host runs the fixture in this process, on the static runtime.
    logos_native_module_host
    logos_host_process
    logos-protocol::logos_protocol_plain
    GTest::gtest
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    # command_line_parser.cpp's only dependency (header-only).
    CLI11::CLI11
)

target_include_directories(logos_module_loader_qt_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/host
)

# DISCOVERY_MODE PRE_TEST: the default (POST_BUILD) RUNS the test binary at
# build time to enumerate cases, which a Linux builder cannot do with a PE.
gtest_discover_tests(logos_module_loader_qt_tests DISCOVERY_MODE PRE_TEST)

# The native host regression fixture is a real module-impl C ABI library. The
# Python check (POSIX) and the Windows stop test run the packaged
# logos_host_plain with it.
add_library(plain_host_fixture SHARED plain_host_fixture.cpp)
target_include_directories(plain_host_fixture PRIVATE
    ${LOGOS_PROTOCOL_ROOT}/include)
if(WIN32)
    set_target_properties(plain_host_fixture PROPERTIES
        PREFIX ""
        AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/windows-tests")
endif()

add_dependencies(logos_module_loader_qt_tests plain_host_fixture)

if(WIN32)
    add_subdirectory(windows)
endif()
