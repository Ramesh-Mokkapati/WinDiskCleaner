// CleanerTests.cpp : tests for the cleanup engine, target list, command line and report.
// Every test works inside its own folder under %TEMP% and removes it afterwards.
#include "..\Core\CleanupEngine.h"
#include "..\Core\CleanupTargets.h"
#include "..\Core\CommandLine.h"
#include "..\Core\Report.h"

#include <cstdio>
#include <functional>
#include <string>

namespace
{
    int g_failures = 0;
    int g_checks = 0;
    std::string g_current;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            std::printf("  FAILED: %s (line %d): %s\n", g_current.c_str(), __LINE__, #cond); \
        }                                                                                    \
    } while (0)

    // ---- file system helpers --------------------------------------------------------

    std::wstring NewRoot(const wchar_t* name)
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%sWdcTest_%lu_%llu_%s", tmp, GetCurrentProcessId(), GetTickCount64(), name);
        CreateDirectoryW(dir, nullptr);
        return dir;
    }

    void MakeDir(const std::wstring& path) { CreateDirectoryW(path.c_str(), nullptr); }

    void MakeFile(const std::wstring& path, size_t bytes = 1000)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string data(bytes, 'x');
        DWORD written = 0;
        WriteFile(h, data.data(), DWORD(data.size()), &written, nullptr);
        CloseHandle(h);
    }

    // Back-dates creation and last-write time (works for files and folders).
    void SetAgeHours(const std::wstring& path, unsigned hours)
    {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER t;
        t.LowPart = now.dwLowDateTime;
        t.HighPart = now.dwHighDateTime;
        t.QuadPart -= static_cast<unsigned long long>(hours) * 36000000000ULL;
        FILETIME ft = { t.LowPart, t.HighPart };
        SetFileTime(h, &ft, nullptr, &ft);
        CloseHandle(h);
    }

    bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    void RemoveTree(const std::wstring& path)
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            RemoveDirectoryW(path.c_str());   // removes only the junction itself
            return;
        }
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                const std::wstring child = path + L"\\" + name;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    RemoveTree(child);
                else
                {
                    SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child.c_str());
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryW(path.c_str());
    }

    bool MakeJunction(const std::wstring& link, const std::wstring& target)
    {
        // mklink /J needs no privileges.
        std::wstring cmd = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return false;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (GetFileAttributesW(link.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    CleanupTarget Target(const std::wstring& root, unsigned minAgeHours = 24)
    {
        CleanupTarget t;
        t.id = L"Test";
        t.name = L"Test target";
        t.path = root;
        t.minAgeHours = minAgeHours;
        return t;
    }

    TargetResult RunOne(CCleanupEngine::Mode mode, const CleanupTarget& t)
    {
        CCleanupEngine engine;
        std::vector<TargetResult> results;
        engine.Run(mode, { t }, results);
        return results.at(0);
    }

    const auto Clean = CCleanupEngine::Mode::Clean;
    const auto Analyze = CCleanupEngine::Mode::Analyze;

    // ---- engine tests -----------------------------------------------------------------

    void OldFilesDeletedRecentFilesKept()
    {
        const auto root = NewRoot(L"age");
        MakeFile(root + L"\\old.tmp", 4096);
        SetAgeHours(root + L"\\old.tmp", 48);
        MakeFile(root + L"\\new.tmp", 100);

        const auto r = RunOne(Clean, Target(root, 24));
        CHECK(r.rootState == TargetResult::RootState::Ok);
        CHECK(!Exists(root + L"\\old.tmp"));
        CHECK(Exists(root + L"\\new.tmp"));
        CHECK(r.filesDeleted == 1);
        CHECK(r.bytesDeleted == 4096);
        CHECK(r.filesTooNew == 1);
        CHECK(Exists(root));   // the target folder itself is never removed
        RemoveTree(root);
    }

    void AnalyzeDeletesNothing()
    {
        const auto root = NewRoot(L"analyze");
        MakeFile(root + L"\\a.tmp", 10);
        MakeFile(root + L"\\b.tmp", 20);
        SetAgeHours(root + L"\\a.tmp", 100);
        SetAgeHours(root + L"\\b.tmp", 100);

        const auto r = RunOne(Analyze, Target(root, 24));
        CHECK(r.filesFound == 2);
        CHECK(r.bytesFound == 30);
        CHECK(r.filesDeleted == 0);
        CHECK(Exists(root + L"\\a.tmp") && Exists(root + L"\\b.tmp"));
        RemoveTree(root);
    }

    void ReadOnlyFilesDeleted()
    {
        const auto root = NewRoot(L"readonly");
        MakeFile(root + L"\\ro.tmp");
        SetAgeHours(root + L"\\ro.tmp", 48);
        SetFileAttributesW((root + L"\\ro.tmp").c_str(), FILE_ATTRIBUTE_READONLY);

        const auto r = RunOne(Clean, Target(root));
        CHECK(r.filesDeleted == 1);
        CHECK(!Exists(root + L"\\ro.tmp"));
        RemoveTree(root);
    }

    void LockedFilesSkipped()
    {
        const auto root = NewRoot(L"locked");
        const auto path = root + L"\\locked.tmp";
        MakeFile(path);
        SetAgeHours(path, 48);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);

        const auto r = RunOne(Clean, Target(root));
        CHECK(r.filesLocked == 1);
        CHECK(r.filesDeleted == 0);
        CHECK(r.errors.empty());   // in-use files are expected, not errors
        CHECK(Exists(path));

        CloseHandle(h);
        RemoveTree(root);
    }

    void JunctionsNeverFollowed()
    {
        const auto outside = NewRoot(L"outside");
        MakeFile(outside + L"\\precious.doc");
        SetAgeHours(outside + L"\\precious.doc", 1000);

        const auto root = NewRoot(L"junction");
        const auto link = root + L"\\link";
        if (!MakeJunction(link, outside))
        {
            std::printf("  (skipped: could not create a junction)\n");
            RemoveTree(root);
            RemoveTree(outside);
            return;
        }

        const auto r = RunOne(Clean, Target(root, 0));
        CHECK(Exists(outside + L"\\precious.doc"));
        CHECK(Exists(link));
        CHECK(r.linksSkipped == 1);
        CHECK(r.filesDeleted == 0);

        RemoveTree(root);
        RemoveTree(outside);
    }

    void JunctionRootRejected()
    {
        const auto outside = NewRoot(L"outside2");
        MakeFile(outside + L"\\precious.doc");
        SetAgeHours(outside + L"\\precious.doc", 1000);
        const auto holder = NewRoot(L"holder");
        const auto link = holder + L"\\Temp";
        if (!MakeJunction(link, outside))
        {
            std::printf("  (skipped: could not create a junction)\n");
            RemoveTree(holder);
            RemoveTree(outside);
            return;
        }

        const auto r = RunOne(Clean, Target(link, 0));
        CHECK(r.rootState == TargetResult::RootState::Rejected);
        CHECK(Exists(outside + L"\\precious.doc"));

        RemoveTree(holder);
        RemoveTree(outside);
    }

    void ExtensionFilterApplied()
    {
        const auto root = NewRoot(L"ext");
        MakeFile(root + L"\\setup.LOG");
        MakeFile(root + L"\\trace.etl");
        MakeFile(root + L"\\state.dat");
        for (const wchar_t* f : { L"\\setup.LOG", L"\\trace.etl", L"\\state.dat" })
            SetAgeHours(root + f, 500);

        auto t = Target(root, 24);
        t.extensions = { L".log", L".etl" };
        const auto r = RunOne(Clean, t);
        CHECK(!Exists(root + L"\\setup.LOG"));   // extension match is case-insensitive
        CHECK(!Exists(root + L"\\trace.etl"));
        CHECK(Exists(root + L"\\state.dat"));
        CHECK(r.filesDeleted == 2);
        RemoveTree(root);
    }

    void ExcludedFoldersUntouched()
    {
        const auto root = NewRoot(L"exclude");
        MakeDir(root + L"\\MeasuredBoot");
        MakeDir(root + L"\\Other");
        MakeFile(root + L"\\MeasuredBoot\\boot.log");
        MakeFile(root + L"\\Other\\old.log");
        SetAgeHours(root + L"\\MeasuredBoot\\boot.log", 500);
        SetAgeHours(root + L"\\Other\\old.log", 500);

        auto t = Target(root, 24);
        t.excludedDirs = { L"measuredboot" };
        RunOne(Clean, t);
        CHECK(Exists(root + L"\\MeasuredBoot\\boot.log"));
        CHECK(!Exists(root + L"\\Other\\old.log"));
        RemoveTree(root);
    }

    void EmptyOldFoldersRemoved()
    {
        const auto root = NewRoot(L"dirs");
        MakeDir(root + L"\\old");
        MakeDir(root + L"\\old\\deeper");
        MakeFile(root + L"\\old\\deeper\\x.tmp");
        MakeDir(root + L"\\fresh");                 // empty but just created: kept
        SetAgeHours(root + L"\\old\\deeper\\x.tmp", 48);
        SetAgeHours(root + L"\\old\\deeper", 48);
        SetAgeHours(root + L"\\old", 48);

        const auto r = RunOne(Clean, Target(root, 24));
        CHECK(!Exists(root + L"\\old"));
        CHECK(Exists(root + L"\\fresh"));
        CHECK(r.dirsRemoved == 2);
        CHECK(Exists(root));
        RemoveTree(root);
    }

    void FolderWithKeptFileNotRemoved()
    {
        const auto root = NewRoot(L"keepdir");
        MakeDir(root + L"\\app");
        MakeFile(root + L"\\app\\old.tmp");
        MakeFile(root + L"\\app\\new.tmp");
        SetAgeHours(root + L"\\app\\old.tmp", 48);
        SetAgeHours(root + L"\\app", 48);

        RunOne(Clean, Target(root, 24));
        CHECK(!Exists(root + L"\\app\\old.tmp"));
        CHECK(Exists(root + L"\\app\\new.tmp"));
        RemoveTree(root);
    }

    void UnsafeRootsRejected()
    {
        // Analyze mode only: even if a check were wrong, nothing is deleted.
        for (const wchar_t* bad : { L"C:\\", L"C:", L"", L"Temp", L"\\\\server\\share\\temp", L"C:\\Windows\\..\\" })
        {
            const auto r = RunOne(Analyze, Target(bad, 0));
            CHECK(r.rootState == TargetResult::RootState::Rejected);
            CHECK(r.filesFound == 0);
        }

        unsigned long error = 0;
        CHECK(!CCleanupEngine::IsSafeRoot(L"C:\\", error));
        const auto file = NewRoot(L"isfile") + L".txt";
        MakeFile(file);
        CHECK(!CCleanupEngine::IsSafeRoot(file, error));
        DeleteFileW(file.c_str());
    }

    void MissingRootReported()
    {
        const auto r = RunOne(Clean, Target(L"C:\\WdcTest_does_not_exist_9f1c", 0));
        CHECK(r.rootState == TargetResult::RootState::Missing);
    }

    void RunResetsCancelFlag()
    {
        const auto root = NewRoot(L"cancel");
        MakeFile(root + L"\\a.tmp");
        SetAgeHours(root + L"\\a.tmp", 48);

        // A Stop from a previous run must not cancel the next one.
        CCleanupEngine engine;
        std::vector<TargetResult> results;
        engine.Cancel();
        engine.Run(Analyze, { Target(root) }, results);
        CHECK(results.size() == 1 && results[0].filesFound == 1);
        CHECK(results[0].rootState == TargetResult::RootState::Ok);
        RemoveTree(root);
    }

    // ---- targets ------------------------------------------------------------------------

    void StandardAndDeepTargets()
    {
        TargetOptions standard;
        const auto s = BuildTargets(standard);
        auto has = [](const std::vector<CleanupTarget>& v, const wchar_t* id) {
            for (const auto& t : v)
                if (t.id == id)
                    return true;
            return false;
        };
        CHECK(has(s, L"SystemTemp"));
        CHECK(has(s, L"UserTemp"));
        CHECK(has(s, L"INetCache"));
        CHECK(!has(s, L"WindowsUpdate"));
        for (const auto& t : s)
            CHECK(!t.deep);

        TargetOptions deepOpt;
        deepOpt.deep = true;
        const auto d = BuildTargets(deepOpt);
        CHECK(has(d, L"WindowsUpdate"));
        CHECK(has(d, L"WindowsLogs"));
        CHECK(has(d, L"System32LogFiles"));
        for (const auto& t : d)
        {
            unsigned long error = 0;
            // Every built-in target must pass the root safety rules' shape check.
            CHECK(t.path.size() > 3 && t.path[1] == L':' && t.path[2] == L'\\');
            if (t.id == L"WindowsLogs" || t.id == L"System32LogFiles")
            {
                CHECK(!t.extensions.empty());
                CHECK(t.minAgeHours >= 24 * 7);
            }
            (void)error;
        }

        TargetOptions zero;
        zero.deep = true;
        zero.minAgeHoursOverride = 0;
        for (const auto& t : BuildTargets(zero))
            CHECK(t.minAgeHours == 0);
    }

    // ---- command line -------------------------------------------------------------------

    void CommandLineParsing()
    {
        CommandLineOptions o;
        std::wstring err;

        CHECK(ParseCommandLine({}, o, err));
        CHECK(!o.silent && !o.autoRun && !o.deep);

        CHECK(ParseCommandLine({ L"/SILENT", L"/AUTO" }, o, err));
        CHECK(o.silent && o.autoRun && !o.deep);

        CHECK(ParseCommandLine({ L"/silent" }, o, err));
        CHECK(o.silent && o.autoRun);   // silent alone means /AUTO

        CHECK(ParseCommandLine({ L"/SILENT", L"/DEEP", L"/LOG:C:\\ProgramData\\DiskCleaner\\Logs\\cleanup_last_run.log" }, o, err));
        CHECK(o.deep && o.logPath == L"C:\\ProgramData\\DiskCleaner\\Logs\\cleanup_last_run.log");

        CHECK(ParseCommandLine({ L"/LOG:\"C:\\My Logs\\run.log\"" }, o, err));
        CHECK(o.logPath == L"C:\\My Logs\\run.log");

        CHECK(ParseCommandLine({ L"-REPORT", L"/MINAGE:48", L"/ALLUSERS" }, o, err));
        CHECK(o.reportOnly && o.minAgeHours == 48 && o.allUsers && !o.autoRun);

        CHECK(ParseCommandLine({ L"/?" }, o, err) && o.help);

        CHECK(!ParseCommandLine({ L"/BOGUS" }, o, err) && !err.empty());
        CHECK(!ParseCommandLine({ L"/LOG:" }, o, err));
        CHECK(!ParseCommandLine({ L"/MINAGE:abc" }, o, err));
        CHECK(!ParseCommandLine({ L"/MINAGE:-1" }, o, err));
        CHECK(!ParseCommandLine({ L"C:\\Windows" }, o, err));
    }

    // ---- report ---------------------------------------------------------------------

    void ReportContents()
    {
        const auto root = NewRoot(L"report");
        MakeFile(root + L"\\a.tmp", 2048);
        SetAgeHours(root + L"\\a.tmp", 48);
        std::vector<CleanupTarget> targets = { Target(root) };
        CCleanupEngine engine;
        std::vector<TargetResult> results;
        engine.Run(Clean, targets, results);

        ReportInfo info;
        info.version = L"1.0.0.0";
        GetLocalTime(&info.started);
        const std::wstring text = BuildReport(info, targets, results);
        CHECK(text.find(L"cleanup report") != std::wstring::npos);
        CHECK(text.find(L"Freed:") != std::wstring::npos);
        CHECK(text.find(L"2.00 KB") != std::wstring::npos);
        CHECK(text.find(root) != std::wstring::npos);

        const std::wstring logPath = root + L"\\logs\\nested\\run.log";
        unsigned long error = 0;
        CHECK(WriteReport(logPath, text, error));
        CHECK(Exists(logPath));
        RemoveTree(root);
    }

    void Formatting()
    {
        CHECK(FormatBytes(0) == L"0 B");
        CHECK(FormatBytes(1536) == L"1.50 KB");
        CHECK(FormatBytes(150ULL * 1024 * 1024) == L"150 MB");
        CHECK(ToLongPath(L"C:\\Temp\\") == L"\\\\?\\C:\\Temp");
        CHECK(FromLongPath(L"\\\\?\\C:\\Temp") == L"C:\\Temp");
    }

    struct Test { const char* name; std::function<void()> fn; };
}

int main()
{
    const Test tests[] = {
        { "OldFilesDeletedRecentFilesKept", OldFilesDeletedRecentFilesKept },
        { "AnalyzeDeletesNothing", AnalyzeDeletesNothing },
        { "ReadOnlyFilesDeleted", ReadOnlyFilesDeleted },
        { "LockedFilesSkipped", LockedFilesSkipped },
        { "JunctionsNeverFollowed", JunctionsNeverFollowed },
        { "JunctionRootRejected", JunctionRootRejected },
        { "ExtensionFilterApplied", ExtensionFilterApplied },
        { "ExcludedFoldersUntouched", ExcludedFoldersUntouched },
        { "EmptyOldFoldersRemoved", EmptyOldFoldersRemoved },
        { "FolderWithKeptFileNotRemoved", FolderWithKeptFileNotRemoved },
        { "UnsafeRootsRejected", UnsafeRootsRejected },
        { "MissingRootReported", MissingRootReported },
        { "RunResetsCancelFlag", RunResetsCancelFlag },
        { "StandardAndDeepTargets", StandardAndDeepTargets },
        { "CommandLineParsing", CommandLineParsing },
        { "ReportContents", ReportContents },
        { "Formatting", Formatting },
    };

    for (const auto& t : tests)
    {
        g_current = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
