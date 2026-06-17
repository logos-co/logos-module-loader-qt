# The Qt-plugin module loader, in two parts:
#
#   logos_module_loader_qt  parent-side QtPluginFormatLoader (resolves the
#                           logos_host_qt binary + builds its CLI args). Light:
#                           boost::dll + spdlog + the format-loader contract.
#                           Linked into logos_core.
#   logos_host_qt           the child-side host executable that loads a single
#                           Qt-plugin module in its own process. Links the full
#                           SDK stack + Qt + logos-module. Bundled by frontends.
#
# Both depend on the logos-module-loader contract (and through it logos-container
# for ModuleDescriptor). The loader/host pair is the matched unit a different
# format (WASM, native) would replace wholesale.

# ---------------------------------------------------------------------------
# Contracts (header-only, imported)
# ---------------------------------------------------------------------------
if(NOT DEFINED LOGOS_MODULE_LOADER_ROOT OR NOT EXISTS "${LOGOS_MODULE_LOADER_ROOT}/include")
  message(FATAL_ERROR "LOGOS_MODULE_LOADER_ROOT not set or ${LOGOS_MODULE_LOADER_ROOT}/include missing")
endif()
if(NOT DEFINED LOGOS_CONTAINER_ROOT OR NOT EXISTS "${LOGOS_CONTAINER_ROOT}/include")
  message(FATAL_ERROR "LOGOS_CONTAINER_ROOT not set or ${LOGOS_CONTAINER_ROOT}/include missing")
endif()

add_library(logos_container INTERFACE IMPORTED)
set_target_properties(logos_container PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_CONTAINER_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "nlohmann_json::nlohmann_json"
)

add_library(logos_module_loader INTERFACE IMPORTED)
set_target_properties(logos_module_loader PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_LOADER_ROOT}/include"
  INTERFACE_LINK_LIBRARIES "logos_container"
)

# ---------------------------------------------------------------------------
# Parent side — QtPluginFormatLoader (linked into logos_core)
# ---------------------------------------------------------------------------
add_library(logos_module_loader_qt STATIC
    qt_plugin_format_loader.cpp
    qt_plugin_format_loader.h
)

# boost::dll resolves the running executable's path (to find logos_host_qt next
# to it); program_location() pulls in the compiled Boost.Filesystem, so link it
# PUBLIC (it's needed at the final link of anything using this lib). Despite the
# name this lib is Qt-free — it only builds CLI strings.
target_link_libraries(logos_module_loader_qt PUBLIC
    logos_module_loader
    spdlog::spdlog
    Boost::filesystem
)

target_include_directories(logos_module_loader_qt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/logos_module_loader_qt>
)

# ---------------------------------------------------------------------------
# Child side — logos_host_qt binary
# ---------------------------------------------------------------------------
set(LOGOS_HOST_SOURCES
    host/logos_host.cpp
    host/command_line_parser.cpp
    host/command_line_parser.h
    host/module_initializer.cpp
    host/module_initializer.h
    host/token_source.cpp
    host/token_source.h
    host/qt/qt_app.cpp
    host/qt/qt_app.h
)

add_executable(logos_host_qt ${LOGOS_HOST_SOURCES})

# The host reads its auth token from an OS handle (TokenSource, default stdin)
# named by --token-source, so it depends on NO container package.
target_link_libraries(logos_host_qt PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::RemoteObjects
    logos_sdk
    logos_module
    CLI11::CLI11
    spdlog::spdlog
)

target_include_directories(logos_host_qt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${Qt${QT_VERSION_MAJOR}_INCLUDE_DIRS}
)

# logos_module include dirs
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/include")
    target_include_directories(logos_host_qt PRIVATE
        ${LOGOS_MODULE_ROOT}/include
        ${LOGOS_MODULE_ROOT}/include/module_lib
    )
endif()

# SDK include dirs
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib" AND EXISTS "${LOGOS_CPP_SDK_ROOT}/include")
    target_include_directories(logos_host_qt PRIVATE
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
else()
    target_include_directories(logos_host_qt PRIVATE ${LOGOS_CPP_SDK_ROOT}/cpp)
endif()

# RPATH so the wrapped binary finds Qt / SDK libs at runtime
if(APPLE)
    set(_qt_exec_runtime_paths "@loader_path/../lib")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_exec_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _qt_exec_runtime_paths "${_qt_exec_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _qt_exec_runtime_paths)

    set_target_properties(logos_host_qt PROPERTIES
        INSTALL_RPATH "${_qt_exec_runtime_paths}"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(logos_host_qt PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
