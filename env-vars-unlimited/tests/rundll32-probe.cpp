// Run the same compatibility checks from a real rundll32.exe host.
// This helper only loads sysdm.cpl and reports metadata; it never opens the UI
// or reads or writes environment variables.
#define WH_MOD
#define WH_EDITING
#include "../env-vars-unlimited.wh.cpp"

#include <cstdio>

static void ReportVersion(FILE* output, PCWSTR path, bool neutral) {
    DWORD ignored = 0;
    DWORD size = neutral ? GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path, &ignored)
                         : GetFileVersionInfoSizeW(path, &ignored);
    if (!size) {
        fprintf(output, "%s.size=0 error=%lu\n", neutral ? "neutral" : "default", GetLastError());
        return;
    }
    std::vector<BYTE> data(size);
    BOOL ok = neutral ? GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path, 0, size, data.data())
                      : GetFileVersionInfoW(path, 0, size, data.data());
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!ok || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        infoSize < sizeof(*info)) {
        fprintf(output, "%s.query=failed error=%lu\n", neutral ? "neutral" : "default", GetLastError());
        return;
    }
    fprintf(output, "%s.file_version=%u.%u.%u.%u\n", neutral ? "neutral" : "default",
            static_cast<unsigned>(HIWORD(info->dwFileVersionMS)),
            static_cast<unsigned>(LOWORD(info->dwFileVersionMS)),
            static_cast<unsigned>(HIWORD(info->dwFileVersionLS)),
            static_cast<unsigned>(LOWORD(info->dwFileVersionLS)));
}

extern "C" __declspec(dllexport) void CALLBACK Probe(HWND, HINSTANCE, LPSTR, int) {
    WCHAR outputPath[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ENV_VARS_TEST_OUTPUT", outputPath, ARRAYSIZE(outputPath))) return;
    FILE* output = _wfopen(outputPath, L"w");
    if (!output) return;
    HMODULE module = LoadLibraryExW(L"sysdm.cpl", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        fprintf(output, "load=failed error=%lu\n", GetLastError());
        fclose(output);
        return;
    }
    WCHAR path[MAX_PATH]{};
    GetModuleFileNameW(module, path, ARRAYSIZE(path));
    HMODULE unrelated = GetModuleHandleW(L"kernel32.dll");
    GUID guid{};
    DWORD age = 0;
    BYTE digest[32]{};
    const bool pdbRead = ReadPdbIdentity(module, guid, age);
    const bool hashRead = HashFile(path, digest);
    BYTE changed[32];
    memcpy(changed, digest, sizeof(changed));
    changed[0] ^= 1;
    const bool wrongHashRejected = hashRead && !FindBuildProfile(guid, age, changed);
    const bool wrongAgeRejected = hashRead && !FindBuildProfile(guid, age + 1, digest);
    const bool crossPairRejected = !FindBuildProfile(kBuildProfiles[0].pdb, 1, kBuildProfiles[1].sha256);
    fprintf(output, "pdb=%d sha256=%d supported=%d wrong_module_rejected=%d "
                    "wrong_hash_rejected=%d wrong_age_rejected=%d cross_pair_rejected=%d\n",
            pdbRead, hashRead, IsSupportedBuild(module), unrelated && !IsSupportedBuild(unrelated),
            wrongHashRejected, wrongAgeRejected, crossPairRejected);
    ReportVersion(output, path, false);
    ReportVersion(output, path, true);
    fclose(output);
}
