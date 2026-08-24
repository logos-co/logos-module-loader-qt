# Common build configuration shared across all packages
{ pkgs, logosSdk, logosProtocolPkg, logosQtHost, logosModule, logosContainer, logosModuleLoader }:

{
  pname = "logos-module-loader-qt";
  version = "0.1.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ]
  ++ pkgs.lib.optional (!pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.wrapQtAppsNoGuiHook;

  # Qt6 listed explicitly (not propagated by the SDK — qtbase's setup hook must
  # be sourced after wrapQtAppsHook). Boost/OpenSSL/nlohmann arrive transitively
  # via logosSdk's propagatedBuildInputs.
  #
  # logosQtHost replaces what used to be logosQtSdk here. It is named DIRECTLY
  # rather than arriving through logos-qt-sdk's propagatedBuildInputs, because
  # that propagation carried logos-qt-sdk's OWN choice of qt-host onto this
  # build's CMAKE_PREFIX_PATH ahead of anything this build said. See flake.nix.
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    logosSdk
    logosProtocolPkg
    logosQtHost
    pkgs.gtest
    pkgs.cli11
    pkgs.spdlog
    logosModule
    logosContainer
    logosModuleLoader
  ];

  cmakeFlags = (pkgs.logosQtCrossCmakeFlags or [ ]) ++ [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
    "-DLOGOS_QT_HOST_ROOT=${logosQtHost}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DLOGOS_CONTAINER_ROOT=${logosContainer}"
    "-DLOGOS_MODULE_LOADER_ROOT=${logosModuleLoader}"
  ];

  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    LOGOS_QT_HOST_ROOT = "${logosQtHost}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
    LOGOS_MODULE_LOADER_ROOT = "${logosModuleLoader}";
  };

  meta = with pkgs.lib; {
    description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";
    platforms = platforms.unix ++ platforms.windows;
  };
}
