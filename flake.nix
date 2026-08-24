{
  description = "Qt-plugin module loader: QtPluginFormatLoader + the logos_host_qt module-host binary";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-protocol.url = "github:logos-co/logos-protocol";
    # The Qt HOST RUNTIME this repo's logos_host_qt links: LogosAPI (the object
    # handed to initLogos), LogosAPIProvider, the provider glue and the Qt
    # argument decoder.
    #
    # It is taken from logos-plugin-qt DIRECTLY, and that is the whole point of
    # this input. Until now this repo took logos-qt-sdk and let it supply
    # logos-qt-host transitively -- but logos-qt-sdk re-exported the qt-host IT
    # was built against (a propagated build input plus a store path baked into
    # its Config as find_package HINTS), so this build linked whatever qt-host
    # logos-qt-sdk's own lock happened to resolve. Two logos-qt-host prefixes
    # then coexisted in a single `nix build` with consumers split between them,
    # and nothing in the flake graph described it: the stale identity travelled
    # in a generated CMake file and a propagated-build-inputs file, so no
    # --override-input reading could reveal it. Measured consequence:
    # logos_host_qt had no Q_INVOKABLE currentCallerJson, invokeMethod failed
    # with "No such method" on every dispatch in every module process, and
    # current_caller() returned {"kind":"unknown"} fleet-wide -- on a green
    # build.
    #
    # logos-qt-sdk is GONE from this flake rather than kept alongside: nothing
    # here includes a header it owns (logos_qt_wire.h / logos_qt_lp_bridge.h /
    # logos_qt_host_core.h), so it was only ever an alias for logos-qt-host.
    #
    # The follows are not decoration. logos_host_qt and liblogos_core share
    # TokenManager and the transport ABI across the host<->plugin boundary, so
    # two logos-protocol revs there is the same split-brain in a different
    # layer; and logos-nix/nixpkgs must match or the Qt this is compiled against
    # is not the Qt it runs against.
    logos-plugin-qt.url = "github:logos-co/logos-plugin-qt";
    logos-plugin-qt.inputs.logos-nix.follows = "logos-nix";
    logos-plugin-qt.inputs.nixpkgs.follows = "nixpkgs";
    logos-plugin-qt.inputs.logos-protocol.follows = "logos-protocol";
    logos-module.url = "github:logos-co/logos-module";
    logos-container.url = "github:logos-co/logos-container";
    logos-module-loader.url = "github:logos-co/logos-module-loader";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-plugin-qt, logos-module, logos-container, logos-module-loader }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
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
          logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
          logosModule = logos-module.packages.${system}.default;
          logosContainer = logos-container.packages.${system}.default;
          logosModuleLoader = logos-module-loader.packages.${system}.default;
        });
    in
    {
      packages = forAllTargets ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtHost, logosModule, logosContainer, logosModuleLoader, ... }:
        let
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtHost logosModule logosContainer logosModuleLoader;
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

      checks = forAllSystems ({ pkgs, system, logosQtHost, ... }:
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

          # The logos-qt-host logos_host_qt actually LINKS must be the one this
          # flake declared. This repo is where the fleet-wide defect surfaced,
          # so this is the assertion that matters most: logos_host_qt links
          # liblogos_qt_host.a STATICALLY, which means its runtime closure keeps
          # ZERO "-logos-qt-host-" store paths and no path-based check can see
          # what it linked. The check compares moc metaobject blobs instead --
          # see nix/checks/qt-host-identity.nix for why that is the only
          # DCE-proof predicate, and why closure cardinality alone is GREEN on
          # exactly this bug.
          #
          # Exposing it here is not enough. Wire it into CI: a nix check that no
          # workflow runs is dead code.
          qt-host-identity = import ./nix/checks/qt-host-identity.nix {
            inherit pkgs;
            declaredQtHost = logosQtHost;
            subjects = [ self.packages.${system}.logos-module-loader-qt-bin ];
          };
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
