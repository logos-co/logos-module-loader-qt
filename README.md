# logos-module-loader-qt

Implementation of [`logos-module-loader`](https://github.com/logos-co/logos-module-loader) contract, using Qt Plugins for loading modules.

```
logos-module-loader   (ModuleFormatLoader interface)
   └─ logos-module-loader-qt  <-- this repo
         consumed by logos-liblogos
```

## What's here

The whole Qt-plugin mechanism, in two parts:

| Artifact | Side | Sources | Links |
|----------|------|---------|-------|
| `logos_module_loader_qt` (static lib) | parent | `qt_plugin_format_loader.{h,cpp}` | boost::dll, spdlog, the loader contract. **Qt-free / SDK-free.** Linked into `logos_core`. |
| `logos_host_qt` (binary) | child | `host/` (`logos_host`, `command_line_parser`, `module_initializer`, `qt_app`, `token_source`) | the full SDK stack + Qt + CLI11 + logos-module. Bundled by frontends. |

The **parent** (`QtPluginFormatLoader`) only resolves the `logos_host_qt` binary
path and builds its CLI arguments (`--name`, `--path`, `--instance-persistence-path`,
`--transport-set`) — it is deliberately light so `logos_core` doesn't pull Qt or
the SDK just to know *how* to launch a Qt-plugin module.

The **child** (`logos_host_qt`) loads a single Qt-plugin module in its own
process: it reads its auth token from the channel its container designated via
`--token-source` (default stdin — so the host depends on no container package),
then loads the plugin and brings up `LogosAPI`.

A `logos_host` → `logos_host_qt` compatibility symlink is installed so frontends
keep working with either name.

## Build & test

```bash
nix build .#logos-module-loader-qt              # lib + header + host binary
nix build .#checks.aarch64-linux.tests -L       # run the loader + token-source tests
```
