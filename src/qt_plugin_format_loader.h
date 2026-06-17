#ifndef QT_PLUGIN_FORMAT_LOADER_H
#define QT_PLUGIN_FORMAT_LOADER_H

#include <logos_module_loader/module_format_loader.h>
#include <string>
#include <vector>

// Parent-side logic for the Qt-plugin module format: knows how to locate the
// logos_host_qt binary and build the CLI arguments it expects.
class QtPluginFormatLoader : public LogosCore::ModuleFormatLoader {
public:
    std::string id() const override { return "qt-plugin"; }

    bool canHandle(const LogosCore::ModuleDescriptor& desc) const override;

    std::string resolveHostBinary(const LogosCore::ModuleDescriptor& desc) const override;

    std::vector<std::string> buildArguments(const LogosCore::ModuleDescriptor& desc) const override;
};

#endif // QT_PLUGIN_FORMAT_LOADER_H
