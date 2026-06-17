# Logos Module Loader Qt tests

add_executable(logos_module_loader_qt_tests
    test_qt_plugin_format_loader.cpp
    test_token_source.cpp
    # token_source.cpp lives in the logos_host_qt executable (not a linkable
    # library), so compile its implementation directly into the test binary.
    ${CMAKE_SOURCE_DIR}/src/host/token_source.cpp
)

target_link_libraries(logos_module_loader_qt_tests PRIVATE
    logos_module_loader_qt
    GTest::gtest
    GTest::gtest_main
    spdlog::spdlog
)

target_include_directories(logos_module_loader_qt_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/host
)

gtest_discover_tests(logos_module_loader_qt_tests)
