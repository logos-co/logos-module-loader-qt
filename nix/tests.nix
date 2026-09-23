# Builds the loader/host tests; flake checks and Windows CI run them.
{ pkgs, common, build }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;
  dontWrapQtApps = true;

  inherit (build) src;
  inherit (common) buildInputs meta env;

  nativeBuildInputs = common.nativeBuildInputs
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];

  cmakeFlags = common.cmakeFlags;

  # Use the standard CMake/Ninja phases so the cross toolchain and
  # logosQtCrossCmakeFlags reach configuration as well as the native flags.

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp bin/logos_module_loader_qt_tests${pkgs.stdenv.hostPlatform.extensions.executable} $out/bin/
    ${pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isWindows ''
      cp -r windows-tests/* $out/bin/
      cp bin/logos_host_plain.exe $out/bin/
    ''}

    mkdir -p $out/lib
    cp -r lib/* $out/lib/ 2>/dev/null || true

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      patchelf --set-rpath "$out/lib:${pkgs.boost}/lib:${pkgs.gtest}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/logos_module_loader_qt_tests || true
    ''}

    runHook postInstall
  '';
}
