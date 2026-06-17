// Link-time provider for the logos-module-loader factory seam.
//
// Defines LogosCore::makeFormatLoader() (declared in the logos-module-loader
// contract) to return this repo's QtPluginFormatLoader, upcast to the
// ModuleFormatLoader interface. Linking this library makes the Qt-plugin loader
// the build's default; a different format-loader repo would provide its own
// definition of the same symbol. Consumers (logos-liblogos) call
// makeFormatLoader() and never name QtPluginFormatLoader.
#include <logos_module_loader/format_loader_factory.h>
#include "qt_plugin_format_loader.h"

namespace LogosCore {

std::shared_ptr<ModuleFormatLoader> makeFormatLoader() {
    return std::make_shared<::QtPluginFormatLoader>();
}

} // namespace LogosCore
