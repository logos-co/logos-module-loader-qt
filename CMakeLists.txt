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
# SDK stack: logos-protocol (transports + lp_* C ABI), logos-qt-sdk (LogosAPI +
# provider glue), logos-cpp-sdk (Qt-free std headers). The logos_sdk alias
# points at logos-qt-sdk, whose link interface chains the rest.
# ---------------------------------------------------------------------------
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
  set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_SDK_ROOT AND DEFINED ENV{LOGOS_QT_SDK_ROOT})
  set(LOGOS_QT_SDK_ROOT "$ENV{LOGOS_QT_SDK_ROOT}")
endif()
if(NOT DEFINED LOGOS_CPP_SDK_ROOT AND DEFINED ENV{LOGOS_CPP_SDK_ROOT})
  set(LOGOS_CPP_SDK_ROOT "$ENV{LOGOS_CPP_SDK_ROOT}")
endif()
get_filename_component(LOGOS_CPP_SDK_ROOT "${LOGOS_CPP_SDK_ROOT}" ABSOLUTE)

if(EXISTS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol"
   AND EXISTS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk")
  message(STATUS "Using logos-protocol at ${LOGOS_PROTOCOL_ROOT}")
  message(STATUS "Using logos-qt-sdk at ${LOGOS_QT_SDK_ROOT}")
  find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
  find_package(logos-qt-sdk REQUIRED
    PATHS "${LOGOS_QT_SDK_ROOT}/lib/cmake/logos-qt-sdk" NO_DEFAULT_PATH)
  if(NOT TARGET logos_sdk)
    add_library(logos_sdk INTERFACE IMPORTED)
    target_link_libraries(logos_sdk INTERFACE logos-qt-sdk::logos_qt_sdk)
  endif()
else()
  message(FATAL_ERROR "logos-protocol / logos-qt-sdk not found. Set "
                      "LOGOS_PROTOCOL_ROOT and LOGOS_QT_SDK_ROOT to built packages.")
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

# Install rules: the parent-side loader lib + its header, and the host binary.
install(TARGETS logos_module_loader_qt
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES src/qt_plugin_format_loader.h
    DESTINATION include/logos_module_loader_qt
)
install(TARGETS logos_host_qt
    RUNTIME DESTINATION bin
)

# Compatibility symlink: logos_host -> logos_host_qt so downstream consumers
# (frontends that bundle the binary) keep working with either name.
install(CODE "
  execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
    logos_host_qt
    \${CMAKE_INSTALL_PREFIX}/bin/logos_host)
")
