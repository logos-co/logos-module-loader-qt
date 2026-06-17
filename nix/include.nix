# Installs the parent-side loader header.
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;

  inherit src;
  inherit (common) meta;

  dontBuild = true;
  dontConfigure = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/include/logos_module_loader_qt
    cp src/qt_plugin_format_loader.h $out/include/logos_module_loader_qt/

    runHook postInstall
  '';
}
