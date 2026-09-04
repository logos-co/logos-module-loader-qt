#include "module_path.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace ModulePath {

std::string resolve(const std::string& path)
{
    if (path.empty())
        return path;

    const fs::path given(path);
    if (given.is_absolute())
        return path;

    // fs::absolute rather than current_path() / given: on Windows a
    // drive-relative path ("C:modules\x.dll") is NOT absolute and has to be
    // resolved against the current directory OF THAT DRIVE, which only
    // fs::absolute knows how to do. Composing by hand would produce
    // "D:\cwd\C:modules\x.dll".
    std::error_code ec;
    const fs::path absolute = fs::absolute(given, ec);
    if (ec)
        return path;  // no working directory to resolve against; say so later

    // Tidy up "." components, and only those: the resolved path is what the
    // host echoes back when it cannot load the file, and "/work/./modules/x.so"
    // is a needless thing to make someone compare against `ls` output. Dropping
    // "." cannot change which file is named. ".." can -- collapsing it
    // lexically names a different file whenever a symlink sits above it -- so a
    // path containing one is left exactly as composed.
    for (const fs::path& part : absolute) {
        if (part == "..")
            return absolute.string();
    }
    return absolute.lexically_normal().string();
}

std::string fileProblem(const std::string& path)
{
    if (path.empty())
        return "no plugin path was given";

    std::error_code ec;
    // status(), not symlink_status(): a plugin reached through a symlink is a
    // plugin, and a dangling symlink is as good as missing.
    const fs::file_status st = fs::status(fs::path(path), ec);

    // "Not there" comes back with `ec` SET -- the OS really did answer ENOENT
    // -- so ask about it before treating a set `ec` as a failure to look. The
    // other way round, the commonest case of all reports as the vaguest
    // message, which is the failure mode this whole function exists to end.
    if (st.type() == fs::file_type::not_found)
        return "no plugin file at " + path;
    if (ec)
        return "cannot examine the plugin path " + path + ": " + ec.message();
    if (fs::is_directory(st))
        return "the plugin path is a directory, not a file: " + path;
    if (!fs::is_regular_file(st))
        return "the plugin path is not a regular file: " + path;

    return {};
}

}  // namespace ModulePath
