cmake_minimum_required(VERSION 3.14)
project(LogosModuleLoaderQt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt for the host binary (Core/Network/RemoteObjects).
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network RemoteObjects)

find_package(nlohmann_json REQUIRED)
# OpenSSL needed transitively by logos_sdk (plain-C++ TCP+SSL transport)
find_package(OpenSSL REQUIRED)
find_package(CLI11 REQUIRED)
find_package(spdlog REQUIRED)
# Boost: boost::dll (parent-side host-path resolution) pulls in compiled
# Boost.Filesystem via program_location().
find_package(Boost REQUIRED COMPONENTS filesystem)

# ---------------------------------------------------------------------------
# SDK stack: logos-protocol (transports + lp_* C ABI), logos-qt-host (the Qt
# HOST RUNTIME: LogosAPI, LogosAPIProvider, the provider glue and the Qt
# argument decoder), logos-cpp-sdk (Qt-free std headers). The logos_sdk alias
# points at logos-qt-host, whose link interface chains logos-protocol.
#
# This used to name logos-qt-sdk and take logos-qt-host THROUGH it. That was the
# defect. logos-qt-sdk re-exported the logos-qt-host it had itself been built
# against -- as a propagated build input, and as a store path baked into its
# generated Config as find_package HINTS -- so this build silently compiled and
# linked a qt-host it never chose, while the flake graph said nothing was wrong.
# When logos-qt-host gained Q_INVOKABLE currentCallerJson, the logos_host_qt
# built here could not see it: QMetaObject::invokeMethod(logosAPI,
# "currentCallerJson") failed at RUNTIME with "No such method" in EVERY module
# process, the glue pushed an empty string, and current_caller() collapsed to
# {"kind":"unknown"} fleet-wide. The build stayed green throughout.
#
# Naming logos-qt-host directly is the fix, and it is a REPLACEMENT rather than
# an addition: nothing here includes a header logos-qt-sdk owns
# (logos_qt_wire.h / logos_qt_lp_bridge.h / logos_qt_host_core.h). Every header
# this repo uses -- logos_api.h, logos_api_provider.h -- is logos-qt-host's, so
# logos-qt-sdk leaves this closure entirely and the dependency count is
# unchanged.
#
# ORDER IS LOAD-BEARING. find_package is FIRST-WINS, so logos-qt-host must be
# resolved here, before anything else can resolve one on this project's behalf.
# The assertion at the bottom of this file is what proves it actually was.
# ---------------------------------------------------------------------------
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_HOST_ROOT AND DEFINED ENV{LOGOS_QT_HOST_ROOT})
  set(LOGOS_QT_HOST_ROOT "$ENV{LOGOS_QT_HOST_ROOT}")
endif()
if(NOT DEFINED LOGOS_CPP_SDK_ROOT AND DEFINED ENV{LOGOS_CPP_SDK_ROOT})
  set(LOGOS_CPP_SDK_ROOT "$ENV{LOGOS_CPP_SDK_ROOT}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol"
   AND EXISTS "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  message(STATUS "Using logos-qt-host at ${LOGOS_QT_HOST_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
  find_package(logos-qt-host REQUIRED
    PATHS "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host" NO_DEFAULT_PATH)
  if(NOT TARGET logos_sdk)
    add_library(logos_sdk INTERFACE IMPORTED)
    # The static archive, which is what logos-qt-sdk::logos_qt_sdk resolved to
    # before -- logos_host_qt is a standalone process, not an in-process
    # consumer sharing TokenManager with liblogos_core, so it keeps linking
    # qt-host statically. Same link interface as before; different, and now
    # DECLARED, provenance.
    target_link_libraries(logos_sdk INTERFACE logos-qt-host::logos_qt_host)
  endif()
else()
  message(FATAL_ERROR "logos-protocol / logos-qt-host not found. Set "
                      "LOGOS_PROTOCOL_ROOT and LOGOS_QT_HOST_ROOT to built "
                      "packages. LOGOS_QT_SDK_ROOT is NOT a substitute: "
                      "logos-qt-sdk no longer exports a qt-host identity, "
                      "which is deliberate (nix build "
                      "'github:logos-co/logos-plugin-qt#logos-qt-host').")
endif()

if(EXISTS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk")
  find_package(logos-cpp-sdk REQUIRED
    PATHS "${LOGOS_CPP_SDK_ROOT}/lib/cmake/logos-cpp-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_headers)
    add_library(logos_headers INTERFACE IMPORTED)
    target_link_libraries(logos_headers INTERFACE logos-cpp-sdk::logos_headers)
  endif()
  target_link_libraries(logos_sdk INTERFACE logos-cpp-sdk::logos_headers)
endif()

# ---------------------------------------------------------------------------
# logos-module library (used by the host to load plugins)
# ---------------------------------------------------------------------------
if(DEFINED LOGOS_MODULE_ROOT AND EXISTS "${LOGOS_MODULE_ROOT}/lib")
  message(STATUS "Using logos-module at ${LOGOS_MODULE_ROOT}")
  find_library(LOGOS_MODULE_LIBRARY
    NAMES logos_module
    PATHS "${LOGOS_MODULE_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  if(NOT LOGOS_MODULE_LIBRARY)
    message(FATAL_ERROR "logos_module library not found in ${LOGOS_MODULE_ROOT}/lib")
  endif()
  add_library(logos_module STATIC IMPORTED)
  set_target_properties(logos_module PROPERTIES
    IMPORTED_LOCATION "${LOGOS_MODULE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_MODULE_ROOT}/include"
  )
else()
  message(FATAL_ERROR "LOGOS_MODULE_ROOT not set or ${LOGOS_MODULE_ROOT}/lib does not exist")
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(src)

# GoogleTest setup
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GoogleTest")
endif()

enable_testing()
include(GoogleTest)

add_subdirectory(tests)

# ---------------------------------------------------------------------------
# IDENTITY ASSERTION — the logos-qt-host CMake actually resolved must be the one
# this build declared.
#
# Deliberately at the END, after every find_package in the project (including
# the ones inside add_subdirectory), because find_package is FIRST-WINS and the
# losing call is SILENT. logos-qt-hostTargets.cmake opens with a "Protect
# against multiple inclusion" block that return()s, so a SECOND
# find_package(logos-qt-host) -- even REQUIRED, with explicit PATHS and
# NO_DEFAULT_PATH -- succeeds, sets logos-qt-host_FOUND=TRUE, and changes
# nothing. Measured: with a foreign qt-host resolved first, an explicit
# REQUIRED/PATHS/NO_DEFAULT_PATH call for the right one left logos-qt-host_DIR
# and the imported IMPORTED_LOCATION pointing at the foreign prefix.
#
# That is the mechanism by which a consumer that believes it chose its qt-host
# links somebody else's, with a green build and a runtime "No such method".
# Nothing else in the build system will say a word about it, so this is the only
# place it can be caught at configure time.
# ---------------------------------------------------------------------------
set(_expected_qt_host_dir "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host")
get_filename_component(_expected_qt_host_dir "${_expected_qt_host_dir}" REALPATH)
get_filename_component(_actual_qt_host_dir "${logos-qt-host_DIR}" REALPATH)
if(NOT _actual_qt_host_dir STREQUAL _expected_qt_host_dir)
  message(FATAL_ERROR
    "logos-qt-host IDENTITY MISMATCH.\n"
    "  declared (LOGOS_QT_HOST_ROOT): ${_expected_qt_host_dir}\n"
    "  resolved (logos-qt-host_DIR):  ${_actual_qt_host_dir}\n"
    "Something resolved logos-qt-host before this project did. find_package is "
    "first-wins, so this build's own call was a silent no-op and logos_host_qt "
    "would link a qt-host it did not choose. The symptom of shipping that is a "
    "GREEN build whose QMetaObject::invokeMethod fails at runtime with "
    "\"No such method\" in every module process.")
endif()
unset(_expected_qt_host_dir)
unset(_actual_qt_host_dir)

# Install rules: the parent-side loader lib + its header, and the host binary.
install(TARGETS logos_module_loader_qt
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES src/qt_plugin_format_loader.h
    DESTINATION include/logos_module_loader_qt
)
# Install the generic format-loader-implementation CMake config so consumers can
# find_package(LogosFormatLoaderImpl) and get this loader (with its own deps)
# without naming it. See cmake/LogosFormatLoaderImplConfig.cmake.
install(FILES cmake/LogosFormatLoaderImplConfig.cmake
    DESTINATION lib/cmake/LogosFormatLoaderImpl
)
install(TARGETS logos_host_qt
    RUNTIME DESTINATION bin
)

# Compatibility symlink: logos_host -> logos_host_qt so downstream consumers
# (frontends that bundle the binary) keep working with either name.
#
# CMAKE_EXECUTABLE_SUFFIX matters on Windows: the installed file is
# logos_host_qt.exe, so linking to the bare name produces a DANGLING symlink --
# which nixpkgs' noBrokenSymlinks check then fails the build over. The link
# itself also carries the suffix so that resolving "logos_host.exe" works.
install(CODE "
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
    logos_host_qt${CMAKE_EXECUTABLE_SUFFIX}
    \${CMAKE_INSTALL_PREFIX}/bin/logos_host${CMAKE_EXECUTABLE_SUFFIX})
")
