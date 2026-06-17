# Extracts the logos_host_qt module-host binary from the shared build, Qt-wrapped
# and patched so it runs standalone. Frontends bundle this binary (it is what
# QtPluginFormatLoader::resolveHostBinary locates at runtime).
{ pkgs, common, build }:

pkgs.stdenvNoCC.mkDerivation {
  pname = "${common.pname}-bin";
  version = common.version;

  dontUnpack = true;

  nativeBuildInputs =
    [ pkgs.qt6.wrapQtAppsNoGuiHook ] ++
    pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ] ++
    pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];

  # Clear LD_LIBRARY_PATH so an external Qt can't interfere with the wrapped host.
  qtWrapperArgs = [
    "--unset LD_LIBRARY_PATH"
  ];

  # Required for autoPatchelfHook (Linux) and wrapQtAppsNoGuiHook (both).
  buildInputs = common.buildInputs;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    if [ -d ${build}/bin ]; then
      cp -r ${build}/bin/* $out/bin/
      chmod -R +w $out/bin
    fi

    # logos_host -> logos_host_qt compatibility symlink (the CMake install adds
    # one too, but bin.nix copies only the build/ tree, so recreate it here).
    if [ -e $out/bin/logos_host_qt ] && [ ! -e $out/bin/logos_host ]; then
      ln -s logos_host_qt $out/bin/logos_host
    fi

    runHook postInstall
  '';

  inherit (common) meta;
}
