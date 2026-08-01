#include <Windows.h>

#include <iostream>

struct PluginVersion {
    int major;
    int minor;
    int patch;
};

using RequiredVersionFn = void (*)(PluginVersion*);

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "expected plugin DLL path\n";
        return 2;
    }
    const auto module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibraryW failed: " << GetLastError() << '\n';
        return 3;
    }
    const auto function = reinterpret_cast<RequiredVersionFn>(
        GetProcAddress(module, "uevr_plugin_required_version"));
    if (function == nullptr) {
        std::cerr << "required-version export is missing\n";
        FreeLibrary(module);
        return 4;
    }
    PluginVersion version{};
    function(&version);
    FreeLibrary(module);
    if (version.major != 2 || version.minor != 34 || version.patch != 0) {
        std::cerr << "expected UEVR API 2.34.0, got " << version.major << '.'
                  << version.minor << '.' << version.patch << '\n';
        return 5;
    }
    std::cout << "official UEVR plugin ABI baseline verified: 2.34.0\n";
    return 0;
}
