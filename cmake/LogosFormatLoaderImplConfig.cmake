# CMake package config for "the format-loader implementation installed in this prefix".
#
# Deliberately generic-named (LogosFormatLoaderImpl) so a consumer
# (logos-liblogos) selects *a* format-loader implementation via
# find_package(LogosFormatLoaderImpl) without naming this specific one. It
# defines the imported target LogosFormatLoaderImpl::impl, carrying this
# implementation's static library, its public include dir, and its own
# transitive dependencies, so the consumer needs to know none of them.

include(CMakeFindDependencyMacro)
find_dependency(Boost COMPONENTS filesystem)
find_dependency(spdlog)
find_dependency(nlohmann_json)

# This file installs at <prefix>/lib/cmake/LogosFormatLoaderImpl/ — three levels
# down from the package prefix.
get_filename_component(_logos_format_loader_impl_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET LogosFormatLoaderImpl::impl)
  add_library(LogosFormatLoaderImpl::impl STATIC IMPORTED)
  set_target_properties(LogosFormatLoaderImpl::impl PROPERTIES
    IMPORTED_LOCATION "${_logos_format_loader_impl_prefix}/lib/liblogos_module_loader_qt.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_logos_format_loader_impl_prefix}/include"
    INTERFACE_LINK_LIBRARIES "Boost::filesystem;spdlog::spdlog;nlohmann_json::nlohmann_json"
  )
endif()

unset(_logos_format_loader_impl_prefix)
