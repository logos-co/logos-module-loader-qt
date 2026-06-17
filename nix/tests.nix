# Builds and runs the loader + token-source tests.
{ pkgs, common, build }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit (build) src;
  inherit (common) buildInputs meta env;

  nativeBuildInputs = common.nativeBuildInputs
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];

  cmakeFlags = common.cmakeFlags;

  configurePhase = ''
    runHook preConfigure

    cp -r ${build}/* .
    chmod -R u+w .

    cmake -B build -S ${build.src} \
      -GNinja \
      -DLOGOS_CPP_SDK_ROOT=${common.env.LOGOS_CPP_SDK_ROOT} \
      -DLOGOS_PROTOCOL_ROOT=${common.env.LOGOS_PROTOCOL_ROOT} \
      -DLOGOS_QT_SDK_ROOT=${common.env.LOGOS_QT_SDK_ROOT} \
      -DLOGOS_MODULE_ROOT=${common.env.LOGOS_MODULE_ROOT} \
      -DLOGOS_CONTAINER_ROOT=${common.env.LOGOS_CONTAINER_ROOT} \
      -DLOGOS_MODULE_LOADER_ROOT=${common.env.LOGOS_MODULE_LOADER_ROOT} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild

    cd build
    ninja logos_module_loader_qt_tests

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp bin/logos_module_loader_qt_tests $out/bin/

    mkdir -p $out/lib
    cp -r lib/* $out/lib/ 2>/dev/null || true

    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      patchelf --set-rpath "$out/lib:${pkgs.boost}/lib:${pkgs.gtest}/lib:${pkgs.spdlog}/lib:${pkgs.fmt}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/logos_module_loader_qt_tests || true
    ''}

    runHook postInstall
  '';
}
