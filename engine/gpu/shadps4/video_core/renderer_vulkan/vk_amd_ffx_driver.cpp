// SPDX-License-Identifier: GPL-3.0-or-later
#include "vk_amd_ffx_driver.h"
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cwchar>
#endif

namespace Vulkan::AmdFfxDriver {
#ifdef _WIN32
namespace {
const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t kProvider[] = L"amdvkffx64.dll";
constexpr const wchar_t* kSdkNames[] = {L"amd_fidelityfx_loader_vk.dll", L"amd_fidelityfx_upscaler_vk.dll",
                                       L"amd_fidelityfx_framegeneration_vk.dll"};

struct ProviderTable {
    std::uint64_t version_id;
    const char* version_name;
    bool (*can_provide)(std::uint64_t);
    void* create;
    void* destroy;
    void* configure;
    void* query;
    void* dispatch;
};

struct Request {
    std::uint32_t struct_version;
    std::uint64_t context_type;
    ProviderTable provider;
    bool (*supported)(const void*);
    void* reserved;
};
static_assert(offsetof(Request, provider) == 0x10 && offsetof(Request, supported) == 0x50 && sizeof(Request) == 0x60);

using UpdateProvider = HRESULT(__stdcall*)(void*, std::uint32_t);
using ModuleLookup = HMODULE(WINAPI*)(LPCWSTR);

HMODULE g_provider = nullptr;
ModuleLookup g_lookup = nullptr;

HMODULE WINAPI FindModule(LPCWSTR name) {
    HMODULE found = g_lookup ? g_lookup(name) : GetModuleHandleW(name);
    if (found || !name) return found;
    for (const wchar_t* sdk : kSdkNames)
        if (_wcsicmp(name, sdk) == 0) return g_provider;
    return nullptr;
}

bool AnswerSdkLookup(HMODULE module) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "GetModuleHandleW") != 0) continue;
            if (reinterpret_cast<ModuleLookup>(slots->u1.Function) == &FindModule) return true;
            DWORD before = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &before)) return false;
            g_lookup = reinterpret_cast<ModuleLookup>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&FindModule);
            DWORD ignored = 0;
            VirtualProtect(&slots->u1.Function, sizeof(void*), before, &ignored);
            return true;
        }
    }
    return false;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FindProvider() {
    HDEVINFO devices = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof info;
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInfo(devices, i, &info); ++i) {
        wchar_t ids[2048]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, nullptr,
                                               reinterpret_cast<PBYTE>(ids), sizeof ids - sizeof(wchar_t), nullptr))
            continue;
        bool amd = false;
        for (const wchar_t* id = ids; *id; id += std::wcslen(id) + 1)
            if (std::wcsstr(id, L"VEN_1002")) amd = true;
        if (!amd) continue;
        HKEY key = SetupDiOpenDevRegKey(devices, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t inf[MAX_PATH]{};
        DWORD size = sizeof inf - sizeof(wchar_t);
        const bool have = RegQueryValueExW(key, L"InfPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(inf), &size) ==
                          ERROR_SUCCESS;
        RegCloseKey(key);
        if (!have) continue;
        wchar_t windows[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windows, MAX_PATH)) continue;
        const std::filesystem::path published = std::filesystem::path(windows) / L"INF" / inf;
        using StoreLocation = BOOL(WINAPI*)(PCWSTR, void*, PCWSTR, PWSTR, DWORD, PDWORD);
        static const auto store_location = reinterpret_cast<StoreLocation>(
            GetProcAddress(GetModuleHandleW(L"setupapi.dll"), "SetupGetInfDriverStoreLocationW"));
        wchar_t stored[MAX_PATH]{};
        if (!store_location || !store_location(published.c_str(), nullptr, nullptr, stored, MAX_PATH, nullptr)) continue;
        const std::filesystem::path dll = std::filesystem::path(stored).parent_path() / kProvider;
        std::error_code error;
        if (std::filesystem::is_regular_file(dll, error)) found = dll.wstring();
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}
}

std::string ProviderPath() {
    return Narrow(FindProvider());
}

bool Load(std::uint64_t context_type, const void* backend, Functions& out, std::string& why) {
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    const std::wstring path = FindProvider();
    if (path.empty()) {
        why = "no AMD display driver with an FSR provider (amdvkffx64.dll)";
        return false;
    }
    if (!g_provider) {
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            why = "cannot load " + Narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        g_provider = module;
        if (!AnswerSdkLookup(module)) {
            why = "the AMD provider's module lookup was not found";
            g_provider = nullptr;
            FreeLibrary(module);
            return false;
        }
    }
    const auto update = reinterpret_cast<UpdateProvider>(GetProcAddress(g_provider, "UpdateFfxApiProvider"));
    if (!update) {
        why = "the AMD driver has no UpdateFfxApiProvider";
        return false;
    }
    Request request{};
    request.struct_version = 3;
    request.context_type = context_type;
    const HRESULT result = update(&request, sizeof request);
    const ProviderTable& p = request.provider;
    if (FAILED(result) || !p.create || !p.destroy || !p.query || !p.dispatch || !p.configure) {
        why = "the AMD driver gave no FSR provider (HRESULT " + std::to_string(long(result)) + ")";
        return false;
    }
    if (!request.supported || !request.supported(backend)) {
        why = "the AMD driver's FSR provider does not support this graphics card";
        return false;
    }
    out = {p.create, p.destroy, p.configure, p.query, p.dispatch, p.version_id,
           p.version_name ? p.version_name : "", Narrow(path)};
    return true;
}
#else
std::string ProviderPath() {
    return {};
}

bool Load(std::uint64_t, const void*, Functions&, std::string& why) {
    why = "only on Windows";
    return false;
}
#endif
}
