#ifndef MODULE_PATH_H
#define MODULE_PATH_H

#include <string>

// What the host does with the `--path` it was given, before Qt sees it.
//
// Qt will not resolve a relative plugin path the way a shell user expects.
// QPluginLoader::setFileName() sends a relative file name through
// locatePlugin(), which searches QCoreApplication::libraryPaths() — the Qt
// PLUGIN search path (QT_PLUGIN_PATH, the Qt installation's plugins dir, the
// application dir) — and never the process working directory. When that search
// misses, the error it reports is "The shared library was not found.", which
// reads as a missing transitive dependency of a plugin that WAS found. The file
// is right there and the host says it is not: two wrong diagnoses in one
// session came out of that sentence.
//
// So the host resolves the path itself, and checks the file itself, so that
// "there is no such file" and "the file is there and the loader refused it"
// are different sentences.
//
// Deliberately Qt-free and dependency-free (std::filesystem only) so it is
// compiled into both logos_host_qt and the test binary, the same way
// token_source is.
namespace ModulePath {

// `path` made absolute, resolving a relative one against the PROCESS working
// directory (the host never chdir()s, so that is the directory the user ran it
// from). An absolute path is returned byte-identical — the daemon always passes
// one (out of ModuleRegistry::modulePath) and nothing here touches it. An empty
// path, or a working directory that cannot be read, is returned unchanged and
// left for the loader to complain about.
std::string resolve(const std::string& path);

// Why `path` cannot be a plugin file at all — it is not there, or it is not a
// regular file — or an empty string when the file IS there, in which case the
// loader's own verdict is the one worth reporting.
//
// This is only ever the cheap half of the answer: it says nothing about whether
// the file is a valid plugin or whether its dependencies resolve. That is
// exactly the point — those are the loader's to answer, and this exists so its
// answer is not made to carry "no such file" as well.
std::string fileProblem(const std::string& path);

}  // namespace ModulePath

#endif  // MODULE_PATH_H
