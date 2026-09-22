# logos-module-loader-qt

Implementation of [`logos-module-loader`](https://github.com/logos-co/logos-module-loader) contract, using Qt Plugins for loading modules.

```
logos-module-loader   (ModuleFormatLoader interface)
   └─ logos-module-loader-qt  <-- this repo
         consumed by logos-liblogos
```

## What's here

The compatibility loader and its two child hosts:

| Artifact | Side | Sources | Links |
|----------|------|---------|-------|
| `logos_module_loader_qt` (static lib) | parent | `qt_plugin_format_loader.{h,cpp}`, `qt_plugin_format_loader_factory.cpp` | boost::dll, spdlog, the loader contract. **Qt-free / SDK-free.** Linked into `logos_core`. |
| `logos_host_qt` (binary) | child | `host/` (`logos_host`, `command_line_parser`, `module_initializer`, `qt_app`, `token_source`) | the full SDK stack + Qt + CLI11 + logos-module. Bundled by frontends. |
| `logos_host_plain` (binary) | child | `host/logos_host_plain.cpp` | logos-protocol plain C ABI + native dynamic loading. **Qt-free / SDK-free.** |

The **parent** (`QtPluginFormatLoader`) selects a host from the trusted module
format: `qt-plugin` resolves `logos_host_qt`, while `native-cdylib` resolves
`logos_host_plain`. It resolves the selected path and builds its CLI arguments
(`--name`, `--path`, `--instance-persistence-path`,
`--transport-set`) — it is deliberately light so `logos_core` doesn't pull Qt or
the SDK just to know *how* to launch a Qt-plugin module. The factory TU defines
`LogosCore::makeFormatLoader()` (the logos-module-loader factory seam) → a
`QtPluginFormatLoader`, so `logos_core` selects this loader at link time without
naming it.

The **child** (`logos_host_qt`) loads a single Qt-plugin module in its own
process: it reads its auth token from the channel its container designated via
`--token-source` (default stdin — so the host depends on no container package),
then loads the plugin and brings up `LogosAPI`.

`logos_host_plain` loads the module-impl C ABI (`logos_module_*` exports),
checks its protocol version and declared name, gives it the host context, and
publishes it through the Qt-compatible plain wire. Both hosts emit the same
`@logos-load-status` verdict consumed by liblogos.

`logos_host_qt --inspect <plugin>` prints a current Qt plugin's embedded Logos
metadata and exits. The Qt-free parent uses this only as the compatibility path
for existing binaries that do not yet carry the adjacent metadata sidecar.

A `logos_host` → `logos_host_qt` compatibility symlink is installed so frontends
keep working with either name.

## Windows DLL dependencies

Before loading its plugin, the dedicated `logos_host_qt` process enables
`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` and registers the plugin's absolute directory
with `AddDllDirectory`. The registration lasts until process exit, so libraries
loaded later by bare name (for example `libpq.dll`) and their dependencies can
be bundled beside the plugin without module-specific search-path code.

The host executable's directory and System32 remain searchable. The current
working directory and `PATH` are excluded from DLL searching; package runtime
libraries beside the plugin or host instead of relying on either. This does not
change executable lookup through `PATH`, nor resolve the host's own startup
imports before `main()` runs. Setup failures abort module loading and use the
normal host load-failure report, including the failing API and Windows error.

This policy belongs only to the child hosting one module. It does not add all
installed module directories to the parent, change shared-process plugin
loaders, or replace logos-module's per-load handling of immediate DLL imports.
Linux and macOS continue to use their packaged runtime paths.

## Consuming it

This package installs a generic CMake config so a consumer selects "the
format-loader implementation" without naming this one:

```cmake
find_package(LogosFormatLoaderImpl REQUIRED)   # provided by this package
target_link_libraries(your_target PRIVATE LogosFormatLoaderImpl::impl)
```

`LogosFormatLoaderImpl::impl` carries the `logos_module_loader_qt` static library
plus its own deps (Boost.Filesystem, spdlog, nlohmann_json). A different
format-loader implementation that ships the same `LogosFormatLoaderImpl` config
is a drop-in replacement. (The host binaries are consumed separately —
frontends bundle them; liblogos re-exports them.)

## Build & test

```bash
nix build .#logos-module-loader-qt              # lib + header + host binary
nix build .#checks.aarch64-linux.tests -L       # run the loader + token-source tests
```

The Windows workflow cross-builds `logos-module-loader-qt-tests`, then runs it
and the DLL-search regression on native Windows. The regression first proves
the initial plugin load alone cannot find a later dependency, then enables the
host policy in a fresh process and checks that dependency and its transitive
import. It also checks Unicode/spaced paths, host/System32 lookup, and exclusion
of libraries available only through the current directory or `PATH`.
