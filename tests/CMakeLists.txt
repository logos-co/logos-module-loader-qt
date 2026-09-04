# Logos Module Loader Qt tests

add_executable(logos_module_loader_qt_tests
    test_qt_plugin_format_loader.cpp
    test_token_source.cpp
    test_module_path.cpp
    # These live in the logos_host_qt executable (not a linkable library), so
    # compile their implementations directly into the test binary. All three are
    # Qt-free, which is what keeps this binary free of Qt and the SDK stack.
    ${CMAKE_SOURCE_DIR}/src/host/token_source.cpp
    ${CMAKE_SOURCE_DIR}/src/host/module_path.cpp
    ${CMAKE_SOURCE_DIR}/src/host/command_line_parser.cpp
)

target_link_libraries(logos_module_loader_qt_tests PRIVATE
    logos_module_loader_qt
    GTest::gtest
    GTest::gtest_main
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
