# Common build configuration shared across all packages
{ pkgs, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, logosContainer, logosModuleLoader }:

{
  pname = "logos-module-loader-qt";
  version = "0.1.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];

  # Qt6 listed explicitly (not propagated by the SDK — qtbase's setup hook must
  # be sourced after wrapQtAppsHook). Boost/OpenSSL/nlohmann arrive transitively
  # via logosSdk's propagatedBuildInputs.
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    logosSdk
    logosProtocolPkg
    logosQtSdk
    pkgs.gtest
    pkgs.cli11
    pkgs.spdlog
    logosModule
    logosContainer
    logosModuleLoader
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
    "-DLOGOS_QT_SDK_ROOT=${logosQtSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DLOGOS_CONTAINER_ROOT=${logosContainer}"
    "-DLOGOS_MODULE_LOADER_ROOT=${logosModuleLoader}"
  ];

  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    LOGOS_QT_SDK_ROOT = "${logosQtSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
    LOGOS_MODULE_LOADER_ROOT = "${logosModuleLoader}";
  };

  meta = with pkgs.lib; {
    description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";
    platforms = platforms.unix;
  };
}
