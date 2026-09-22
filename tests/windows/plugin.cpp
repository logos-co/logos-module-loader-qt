#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// No import-table reference to the late dependency: model a module calling
// LoadLibrary only after its own initial LoadLibraryEx has returned.
extern "C" __declspec(dllexport) int loadLateDependency()
{
    const HMODULE library = LoadLibraryW(L"logos_dll_search_late.dll");
    if (!library)
        return 0;
    const auto value = reinterpret_cast<int (*)()>(GetProcAddress(library, "lateValue"));
    const int result = value ? value() : -1;
    FreeLibrary(library);
    return result;
}
