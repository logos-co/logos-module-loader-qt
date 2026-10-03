{
  description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk/feat/peering";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    # On protocol 0.14 (tls_tcp, logos-protocol#99) and the branches stacked on it until they merge.
    logos-protocol.url = "github:logos-co/logos-protocol/feat/peering";
    logos-plugin-qt.url = "github:logos-co/logos-plugin-qt/feat/peering";
    logos-plugin-qt.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-plugin-qt.follows = "logos-plugin-qt";
    logos-qt-sdk.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    logos-module.url = "github:logos-co/logos-module";
    logos-container.url = "github:logos-co/logos-container";
    logos-module-loader.url = "github:logos-co/logos-module-loader";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-plugin-qt, logos-qt-sdk, logos-module, logos-container, logos-module-loader }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        logosContainer = logos-container.packages.${system}.default;
        logosModuleLoader = logos-module-loader.packages.${system}.default;
      });

      # Same, plus "x86_64-windows". Every dependency here is a TARGET-side
      # library (headers/archives compiled into this one), so they all follow
      # ${system}; there is no build-time code generator to keep native.
      forAllTargets = f:
        nixpkgs.lib.genAttrs (systems ++ [ "x86_64-windows" ]) (system: f {
          inherit system;
          pkgs =
            if system == "x86_64-windows"
            then logos-nix.lib.mkWindowsPkgs { buildSystem = "x86_64-linux"; }
            else import nixpkgs { inherit system; };
          logosSdk = logos-cpp-sdk.packages.${system}.default;
          logosProtocolPkg = logos-protocol.packages.${system}.default;
          logosQtSdk = logos-qt-sdk.packages.${system}.default;
          logosModule = logos-module.packages.${system}.default;
          logosContainer = logos-container.packages.${system}.default;
          logosModuleLoader = logos-module-loader.packages.${system}.default;
        });
    in
    {
      packages = forAllTargets ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, logosContainer, logosModuleLoader, ... }:
        let
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosModule logosContainer logosModuleLoader;
          };
          src = ./.;

          build = import ./nix/build.nix { inherit pkgs common src; };

          lib = import ./nix/lib.nix { inherit pkgs common build; };
          include = import ./nix/include.nix { inherit pkgs common src; };
          bin = import ./nix/bin.nix { inherit pkgs common build; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };

          # Combined: the parent-side loader lib + header, and the host binary.
          logos-module-loader-qt = pkgs.symlinkJoin {
            name = "logos-module-loader-qt";
            paths = [ lib include bin ];
          };
        in
        {
          logos-module-loader-qt-lib = lib;
          logos-module-loader-qt-include = include;
          logos-module-loader-qt-bin = bin;
          logos-module-loader-qt-tests = tests;

          logos-module-loader-qt = logos-module-loader-qt;

          default = logos-module-loader-qt;
        }
      );

      checks = forAllSystems ({ pkgs, system, logosProtocolPkg, ... }:
        let
          testsPkg = self.packages.${system}.logos-module-loader-qt-tests;
          hostPkg = self.packages.${system}.logos-module-loader-qt-bin;
          # The shared plain runtime ships only in this package, not the Qt one.
          plainProtocolPkg = logos-protocol.packages.${system}.logos-protocol-plain;
        in
        {
          tests = pkgs.runCommand "logos-module-loader-qt-tests"
            {
              nativeBuildInputs = [ testsPkg pkgs.python3 ];
            } ''
            echo "Running logos-module-loader-qt tests..."
            ${testsPkg}/bin/logos_module_loader_qt_tests
            ${pkgs.python3}/bin/python3 ${./tests/test_plain_host.py} \
              ${hostPkg}/bin/logos_host_plain \
              ${testsPkg}/lib \
              ${plainProtocolPkg}/lib
            mkdir -p $out
            touch $out/.tests-passed
          '';
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.cli11
            pkgs.spdlog
          ];
        };
      });
    };
}
