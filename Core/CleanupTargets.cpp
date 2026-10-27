// CleanupTargets.cpp
#include "CleanupTargets.h"

#include <shlobj.h>
#include <algorithm>
#include <cwctype>

namespace
{
    // Default minimum ages. Temp folders keep anything touched in the last day so running
    // installers and apps are not disturbed; logs are kept for a week for troubleshooting.
    const unsigned kTempMinAgeHours   = 24;
    const unsigned kCacheMinAgeHours  = 0;
    const unsigned kUpdateMinAgeHours = 24;
    const unsigned kLogMinAgeHours    = 7 * 24;

    std::wstring Expand(const wchar_t* text)
    {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ExpandEnvironmentStringsW(text, buf, _countof(buf));
        return (n > 0 && n <= _countof(buf)) ? std::wstring(buf) : std::wstring();
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        std::wstring result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path)
            result = path;
        CoTaskMemFree(path);
        return result;
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    // A 32-bit process on 64-bit Windows is redirected from System32 to SysWOW64;
    // "Sysnative" reaches the real System32.
    std::wstring System32Dir()
    {
        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);
        return Expand(wow64 ? L"%WINDIR%\\Sysnative" : L"%WINDIR%\\System32");
    }

    struct Profile
    {
        std::wstring userName;   // folder name of the profile
        std::wstring localAppData;
    };

    // Local profiles of real users (S-1-5-21-...), from the ProfileList registry key.
    std::vector<Profile> LocalUserProfiles()
    {
        std::vector<Profile> profiles;
        HKEY hList = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                          0, KEY_READ, &hList) != ERROR_SUCCESS)
            return profiles;

        wchar_t sid[256];
        for (DWORD i = 0;; ++i)
        {
            DWORD cch = _countof(sid);
            if (RegEnumKeyExW(hList, i, sid, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            if (wcsncmp(sid, L"S-1-5-21-", 9) != 0)
                continue;

            wchar_t raw[MAX_PATH] = {};
            DWORD cb = sizeof(raw) - sizeof(wchar_t);
            if (RegGetValueW(hList, sid, L"ProfileImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                             nullptr, raw, &cb) != ERROR_SUCCESS)
                continue;

            const std::wstring profilePath = Expand(raw);
            if (profilePath.empty())
                continue;

            Profile p;
            const size_t slash = profilePath.find_last_of(L'\\');
            p.userName = slash == std::wstring::npos ? profilePath : profilePath.substr(slash + 1);
            p.localAppData = profilePath + L"\\AppData\\Local";
            profiles.push_back(p);
        }
        RegCloseKey(hList);
        return profiles;
    }

    void Add(std::vector<CleanupTarget>& targets, CleanupTarget t, const TargetOptions& options)
    {
        if (t.path.empty())
            return;
        if (options.minAgeHoursOverride >= 0)
            t.minAgeHours = static_cast<unsigned>(options.minAgeHoursOverride);
        // The same folder can come from two sources (e.g. the caller's profile and ProfileList).
        for (const auto& existing : targets)
            if (_wcsicmp(existing.path.c_str(), t.path.c_str()) == 0)
                return;
        targets.push_back(std::move(t));
    }
}

bool IsProcessElevated()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD cb = 0;
    const bool elevated = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cb) &&
                          elevation.TokenIsElevated;
    CloseHandle(hToken);
    return elevated;
}

std::vector<CleanupTarget> BuildTargets(const TargetOptions& options)
{
    std::vector<CleanupTarget> targets;

    // --- Standard (safe) targets -------------------------------------------------------
    {
        CleanupTarget t;
        t.id = L"SystemTemp";
        t.name = L"System temporary files";
        t.path = Expand(L"%WINDIR%\\Temp");
        t.requiresAdmin = true;
        t.minAgeHours = kTempMinAgeHours;
        Add(targets, t, options);
    }

    // Per-user folders: the caller's own profile first, then (with allUsers) every other profile.
    struct UserFolders { std::wstring label; std::wstring localAppData; bool other; };
    std::vector<UserFolders> users;
    const std::wstring ownLocal = KnownFolder(FOLDERID_LocalAppData);
    if (!ownLocal.empty())
        users.push_back({ L"", ownLocal, false });
    if (options.allUsers)
        for (const auto& p : LocalUserProfiles())
            if (_wcsicmp(p.localAppData.c_str(), ownLocal.c_str()) != 0)
                users.push_back({ p.userName, p.localAppData, true });

    for (const auto& u : users)
    {
        const std::wstring suffix = u.other ? L" (" + u.label + L")" : L"";
        const std::wstring idSuffix = u.other ? L":" + u.label : L"";

        CleanupTarget temp;
        temp.id = L"UserTemp" + idSuffix;
        temp.name = L"User temporary files" + suffix;
        temp.path = u.localAppData + L"\\Temp";
        temp.requiresAdmin = u.other;
        temp.minAgeHours = kTempMinAgeHours;
        Add(targets, temp, options);

        CleanupTarget cache;
        cache.id = L"INetCache" + idSuffix;
        cache.name = L"Internet cache" + suffix;
        cache.path = u.localAppData + L"\\Microsoft\\Windows\\INetCache";
        cache.requiresAdmin = u.other;
        cache.minAgeHours = kCacheMinAgeHours;
        Add(targets, cache, options);
    }

    if (!options.deep)
        return targets;

    // --- Deep targets ------------------------------------------------------------------
    {
        CleanupTarget t;
        t.id = L"WindowsUpdate";
        t.name = L"Windows Update download cache";
        t.path = Expand(L"%WINDIR%\\SoftwareDistribution\\Download");
        t.deep = true;
        t.requiresAdmin = true;
        t.minAgeHours = kUpdateMinAgeHours;
        Add(targets, t, options);
    }

    // Logs: only recognised log/trace files, and never folders other components depend on
    // (MeasuredBoot is used for TPM attestation, Sum is the User Access Logging database,
    // WMI holds live autologger sessions).
    const std::vector<std::wstring> logExtensions = { L".log", L".lo_", L".etl", L".old", L".bak", L".cab", L".txt" };
    {
        CleanupTarget t;
        t.id = L"WindowsLogs";
        t.name = L"Windows log files";
        t.path = Expand(L"%WINDIR%\\Logs");
        t.deep = true;
        t.requiresAdmin = true;
        t.minAgeHours = kLogMinAgeHours;
        t.extensions = logExtensions;
        t.excludedDirs = { L"measuredboot" };
        Add(targets, t, options);
    }
    {
        CleanupTarget t;
        t.id = L"System32LogFiles";
        t.name = L"System log files (System32\\LogFiles)";
        t.path = System32Dir() + L"\\LogFiles";
        t.deep = true;
        t.requiresAdmin = true;
        t.minAgeHours = kLogMinAgeHours;
        t.extensions = logExtensions;
        t.excludedDirs = { L"sum", L"wmi" };
        Add(targets, t, options);
    }

    for (auto& t : targets)
        for (auto& d : t.excludedDirs)
            d = Lower(d);
    return targets;
}
