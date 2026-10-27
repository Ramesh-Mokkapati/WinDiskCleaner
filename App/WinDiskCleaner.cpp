// WinDiskCleaner.cpp : entry point, single instance, command line and silent mode
#include "pch.h"
#include "WinDiskCleaner.h"
#include "CleanerDlg.h"
#include "CrashHandler.h"
#include "..\Core\CleanupTargets.h"
#include "..\Core\Report.h"

#include <shellapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#pragma comment(lib, "Version.lib")

BEGIN_MESSAGE_MAP(CWinDiskCleanerApp, CWinApp)
END_MESSAGE_MAP()

CWinDiskCleanerApp theApp;

CWinDiskCleanerApp::CWinDiskCleanerApp()
{
}

std::wstring CWinDiskCleanerApp::Version()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, _countof(exe));
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe, &handle);
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (size && GetFileVersionInfoW(exe, 0, size, data.data()) &&
        VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) && info)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
        return buf;
    }
    return L"?";
}

bool CWinDiskCleanerApp::AcquireSingleInstance()
{
    // Global\ also catches a scheduled SYSTEM run in session 0 while someone uses the GUI.
    // Standard users may not create global objects, so fall back to the session namespace.
    static const wchar_t kGlobal[] = L"Global\\WinDiskCleaner.SingleInstance";
    static const wchar_t kLocal[]  = L"Local\\WinDiskCleaner.SingleInstance";

    m_hMutex = CreateMutexW(nullptr, TRUE, kGlobal);
    if (!m_hMutex)
        m_hMutex = CreateMutexW(nullptr, TRUE, kLocal);
    if (!m_hMutex)
        return true;   // cannot tell; run anyway
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
        return false;
    }
    return true;
}

BOOL CWinDiskCleanerApp::InitInstance()
{
    InstallCrashHandler();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CWinApp::InitInstance();

#ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    // Command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i)
    {
        args.push_back(argv[i]);
        m_rawArgs += (m_rawArgs.empty() ? L"" : L" ") + std::wstring(argv[i]);
    }
    LocalFree(argv);

    std::wstring error;
    if (!::ParseCommandLine(args, m_options, error))   // not CWinApp::ParseCommandLine
    {
        // /SILENT may not have been parsed yet, so look for it directly.
        bool silent = false;
        for (const auto& a : args)
            if (_wcsicmp(a.c_str(), L"/SILENT") == 0 || _wcsicmp(a.c_str(), L"-SILENT") == 0)
                silent = true;
        if (!silent)
            AfxMessageBox((error + L"\n\n" + CommandLineUsage()).c_str(), MB_ICONERROR);
        m_exitCode = ExitInvalidArgs;
        return FALSE;
    }

    if (m_options.help)
    {
        if (!m_options.silent)
            AfxMessageBox(CommandLineUsage().c_str(), MB_ICONINFORMATION);
        return FALSE;
    }

    if (!AcquireSingleInstance())
    {
        if (!m_options.silent)
            AfxMessageBox(L"WinDiskCleaner is already running (possibly as a scheduled task).", MB_ICONINFORMATION);
        m_exitCode = ExitAlreadyRunning;
        return FALSE;
    }

    if (m_options.silent)
    {
        m_exitCode = RunSilent();
        return FALSE;
    }

    CCleanerDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}

void CWinDiskCleanerApp::ReleaseSingleInstance()
{
    if (m_hMutex)
    {
        ReleaseMutex(m_hMutex);
        CloseHandle(m_hMutex);
        m_hMutex = nullptr;
    }
}

int CWinDiskCleanerApp::ExitInstance()
{
    ReleaseSingleInstance();
    CWinApp::ExitInstance();
    return m_exitCode;
}

int CWinDiskCleanerApp::RunSilent()
{
    TargetOptions to;
    to.deep = m_options.deep;
    to.allUsers = m_options.allUsers;
    to.minAgeHoursOverride = m_options.minAgeHours;
    const std::vector<CleanupTarget> targets = BuildTargets(to);

    ReportInfo info;
    info.version = Version();
    info.commandLine = m_rawArgs;
    info.analyzeOnly = m_options.reportOnly;
    info.deep = m_options.deep;
    info.elevated = IsProcessElevated();
    GetLocalTime(&info.started);
    const ULONGLONG start = GetTickCount64();

    CCleanupEngine engine;
    std::vector<TargetResult> results;
    engine.Run(m_options.reportOnly ? CCleanupEngine::Mode::Analyze : CCleanupEngine::Mode::Clean, targets, results);
    info.elapsedMs = GetTickCount64() - start;

    int code = ExitSuccess;
    for (const auto& r : results)
        if (r.rootState == TargetResult::RootState::AccessDenied)
            code = ExitAccessDenied;

    const std::wstring logPath = m_options.logPath.empty() ? DefaultLogPath() : m_options.logPath;
    unsigned long error = 0;
    if (!WriteReport(logPath, BuildReport(info, targets, results), error) && code == ExitSuccess)
        code = ExitLogFailed;
    return code;
}
