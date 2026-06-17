# Extracts the parent-side loader library + its header from the shared build.
{ pkgs, common, build }:

pkgs.runCommand "${common.pname}-lib-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    mkdir -p $out/lib
    if [ -d ${build}/lib ]; then
      cp -r ${build}/lib/* $out/lib/ 2>/dev/null || true
    fi

    # Public header for the parent-side QtPluginFormatLoader.
    mkdir -p $out/include/logos_module_loader_qt
    cp ${build.src}/src/qt_plugin_format_loader.h $out/include/logos_module_loader_qt/
  ''
