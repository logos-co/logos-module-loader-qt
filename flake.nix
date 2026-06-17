{
  description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-protocol.url = "github:logos-co/logos-protocol";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    logos-module.url = "github:logos-co/logos-module";
    logos-container.url = "github:logos-co/logos-container/abstract_container";
    logos-module-loader.url = "github:logos-co/logos-module-loader/abstract_module_loader";
    logos-module-loader.inputs.logos-container.follows = "logos-container";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-module, logos-container, logos-module-loader }:
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
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, logosContainer, logosModuleLoader }:
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

      checks = forAllSystems ({ pkgs, system, ... }:
        let
          testsPkg = self.packages.${system}.logos-module-loader-qt-tests;
        in
        {
          tests = pkgs.runCommand "logos-module-loader-qt-tests"
            {
              nativeBuildInputs = [ testsPkg ];
            } ''
            echo "Running logos-module-loader-qt tests..."
            ${testsPkg}/bin/logos_module_loader_qt_tests
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
