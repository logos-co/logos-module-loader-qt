# Common build configuration shared across all packages
{ pkgs, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, logosContainer, logosModuleLoader
# false: the Qt-free parts only (no logos_host_qt, no tests), as for Android.
, qtHost ? true }:

let
  inherit (pkgs.lib) optional optionals;
in
{
  pname = "logos-module-loader-qt";
  version = "0.1.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ]
  ++ optional (qtHost && !pkgs.stdenv.hostPlatform.isWindows) pkgs.qt6.wrapQtAppsNoGuiHook;

  # Qt6 listed explicitly (not propagated by the SDK — qtbase's setup hook must
  # be sourced after wrapQtAppsHook). Boost/OpenSSL/nlohmann arrive transitively
  # via logosSdk's propagatedBuildInputs.
  # Without the Qt host, logosProtocolPkg brings them along.
  buildInputs = [
    logosProtocolPkg
    pkgs.cli11
    pkgs.spdlog
    logosContainer
    logosModuleLoader
  ] ++ optionals qtHost [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    logosSdk
    logosQtSdk
    pkgs.gtest
    logosModule
  ];

  cmakeFlags = (pkgs.logosQtCrossCmakeFlags or [ ]) ++ [
    "-GNinja"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
    "-DLOGOS_CONTAINER_ROOT=${logosContainer}"
    "-DLOGOS_MODULE_LOADER_ROOT=${logosModuleLoader}"
  ] ++ (if qtHost then [
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_QT_SDK_ROOT=${logosQtSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
  ] else [
    "-DLOGOS_BUILD_QT_HOST=OFF"
    "-DLOGOS_MODULE_LOADER_QT_BUILD_TESTS=OFF"
  ]);

  env = {
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
    LOGOS_MODULE_LOADER_ROOT = "${logosModuleLoader}";
  } // pkgs.lib.optionalAttrs qtHost {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_QT_SDK_ROOT = "${logosQtSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
  };

  meta = with pkgs.lib; {
    description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";
    platforms = platforms.unix ++ platforms.windows;
  };
}
