# Native Windows loader behavior, using the production host policy and real
# DLLs. Keep artifacts out of bin/ so the host package does not ship fixtures.
set(test_dir "${CMAKE_BINARY_DIR}/windows-tests")
foreach(name leaf late plugin host)
    if(name STREQUAL "host")
        set(source leaf.cpp)
    elseif(name STREQUAL "late")
        set(source late_dependency.cpp)
    else()
        set(source ${name}.cpp)
    endif()
    add_library(logos_dll_search_${name} SHARED ${source})
    set_target_properties(logos_dll_search_${name} PROPERTIES
        PREFIX ""
        AUTOMOC OFF
        RUNTIME_OUTPUT_DIRECTORY "${test_dir}/dll-search-fixtures")
endforeach()
target_link_libraries(logos_dll_search_late PRIVATE logos_dll_search_leaf)
set_target_properties(logos_dll_search_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${test_dir}")

add_executable(logos_host_dll_search_tests
    test_dll_search.cpp
    ${PROJECT_SOURCE_DIR}/src/host/module_dll_search.cpp)
target_include_directories(logos_host_dll_search_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/host)
set_target_properties(logos_host_dll_search_tests PROPERTIES
    AUTOMOC OFF
    RUNTIME_OUTPUT_DIRECTORY "${test_dir}")
add_dependencies(logos_host_dll_search_tests
    logos_dll_search_plugin logos_dll_search_late logos_dll_search_host)
foreach(mode control configured)
    add_test(NAME windows_dll_search_${mode} COMMAND logos_host_dll_search_tests ${mode})
endforeach()
