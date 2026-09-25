# Builds the loader/host tests; flake checks and Windows CI run them.
{ pkgs, common, build }:

let
  # What Windows CI runs. The Python host check needs /proc, so Windows runs
  # these host executables instead; each reports by its exit code.
  manifest = builtins.toFile "loader-tests.json" (builtins.toJSON {
    suites = [
      { name = "loader"; exe = "bin/logos_module_loader_qt_tests.exe"; }
      { name = "dll_search_control"; exe = "bin/logos_host_dll_search_tests.exe";
        kind = "exe"; args = [ "control" ]; timeout = 120; }
      { name = "dll_search_configured"; exe = "bin/logos_host_dll_search_tests.exe";
        kind = "exe"; args = [ "configured" ]; timeout = 120; }
      { name = "plain_host_stop"; exe = "bin/logos_host_plain_stop_tests.exe";
        kind = "exe"; timeout = 120; }
      { name = "plain_host_dlls"; exe = "bin/logos_host_plain_dll_tests.exe";
        kind = "exe"; timeout = 120; }
    ];
  });
in
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
      mkdir -p $out/share/logos-tests
      cp ${manifest} $out/share/logos-tests/loader.json
    ''}

    mkdir -p $out/lib
    cp -r lib/* $out/lib/ 2>/dev/null || true

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      # OpenSSL: the in-process host tests link the static plain runtime.
      patchelf --set-rpath "$out/lib:${pkgs.boost}/lib:${pkgs.gtest}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.lib.getLib pkgs.openssl}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/logos_module_loader_qt_tests || true
    ''}

    runHook postInstall
  '';
}
