// CleanerDlg.cpp
#include "pch.h"
#include "WinDiskCleaner.h"
#include "CleanerDlg.h"
#include "..\Core\CleanupTargets.h"
#include "..\Core\Report.h"

#include <shellapi.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
    const UINT WM_WORKER_DONE = WM_APP + 1;
    const UINT_PTR kProgressTimer = 1;

    enum Column { colLocation, colFolder, colRules, colFiles, colSize, colStatus };

    struct WorkerArgs
    {
        CCleanerDlg* dlg;
        CCleanupEngine* engine;
        CCleanupEngine::Mode mode;
        std::vector<CleanupTarget>* targets;
        std::vector<TargetResult>* results;
        std::mutex* lock;
        HWND hwnd;
    };

    CString Number(unsigned long long v)
    {
        CString digits;
        digits.Format(L"%llu", v);
        CString out;
        for (int i = 0; i < digits.GetLength(); ++i)
        {
            out += digits[i];
            const int left = digits.GetLength() - i - 1;
            if (left > 0 && left % 3 == 0)
                out += L',';
        }
        return out;
    }

    CString RulesText(const CleanupTarget& t)
    {
        CString s;
        if (t.minAgeHours == 0)
            s = L"Any age";
        else if (t.minAgeHours % 24 == 0)
            s.Format(L"Older than %u day%s", t.minAgeHours / 24, t.minAgeHours == 24 ? L"" : L"s");
        else
            s.Format(L"Older than %u h", t.minAgeHours);
        if (!t.extensions.empty())
            s += L", log files only";
        return s;
    }
}

BEGIN_MESSAGE_MAP(CCleanerDlg, CDialogEx)
    ON_BN_CLICKED(IDC_ANALYZE, &CCleanerDlg::OnAnalyze)
    ON_BN_CLICKED(IDC_CLEAN, &CCleanerDlg::OnClean)
    ON_BN_CLICKED(IDC_STOP, &CCleanerDlg::OnStop)
    ON_BN_CLICKED(IDC_SAVE_REPORT, &CCleanerDlg::OnSaveReport)
    ON_BN_CLICKED(IDC_RESTART_ADMIN, &CCleanerDlg::OnRestartAdmin)
    ON_BN_CLICKED(IDC_ALL_USERS, &CCleanerDlg::OnOptionChanged)
    ON_BN_CLICKED(IDC_MIN_AGE, &CCleanerDlg::OnOptionChanged)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_TARGETS, &CCleanerDlg::OnTargetItemChanged)
    ON_MESSAGE(WM_WORKER_DONE, &CCleanerDlg::OnWorkerDone)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

CCleanerDlg::CCleanerDlg(CWnd* pParent)
    : CDialogEx(IDD_CLEANER_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CCleanerDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TARGETS, m_list);
    DDX_Control(pDX, IDC_PROGRESS, m_progress);
    DDX_Control(pDX, IDC_ALL_USERS, m_allUsers);
    DDX_Control(pDX, IDC_MIN_AGE, m_minAge);
}

BOOL CCleanerDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetIcon(m_hIcon, TRUE);
    SetIcon(m_hIcon, FALSE);

    m_elevated = IsProcessElevated();
    const CommandLineOptions& opt = theApp.Options();

    CString title;
    title.Format(L"Enterprise Disk Cleanup Utility %s%s", theApp.Version().c_str(), m_elevated ? L" (Administrator)" : L"");
    SetWindowText(title);

    SetDlgItemText(IDC_ADMIN_STATUS, m_elevated
        ? L"Running as administrator: all locations are available."
        : L"Not running as administrator: system locations (Windows Temp, update cache, logs) are not available.");
    GetDlgItem(IDC_RESTART_ADMIN)->ShowWindow(m_elevated ? SW_HIDE : SW_SHOW);
    if (!m_elevated)
        Button_SetElevationRequiredState(GetDlgItem(IDC_RESTART_ADMIN)->GetSafeHwnd(), TRUE);

    m_list.SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    CRect rc;
    m_list.GetClientRect(&rc);
    const int w = rc.Width();
    m_list.InsertColumn(colLocation, L"Location", LVCFMT_LEFT, w * 22 / 100);
    m_list.InsertColumn(colFolder, L"Folder", LVCFMT_LEFT, w * 30 / 100);
    m_list.InsertColumn(colRules, L"Removes", LVCFMT_LEFT, w * 15 / 100);
    m_list.InsertColumn(colFiles, L"Files", LVCFMT_RIGHT, w * 8 / 100);
    m_list.InsertColumn(colSize, L"Size", LVCFMT_RIGHT, w * 9 / 100);
    m_list.InsertColumn(colStatus, L"Status", LVCFMT_LEFT, w * 16 / 100);

    m_allUsers.SetCheck(opt.allUsers ? BST_CHECKED : BST_UNCHECKED);
    m_allUsers.EnableWindow(m_elevated);
    m_minAge.SetCheck(opt.minAgeHours == 0 ? BST_UNCHECKED : BST_CHECKED);

    LoadTargets();
    SetDlgItemText(IDC_SUMMARY, L"Select locations, then Analyze to see how much space can be freed.");
    UpdateControls();

    // /REPORT analyses immediately; /AUTO cleans the standard targets without asking.
    if (opt.autoRun)
    {
        m_confirmClean = false;
        PostMessage(WM_COMMAND, MAKEWPARAM(IDC_CLEAN, BN_CLICKED));
    }
    else if (opt.reportOnly)
    {
        PostMessage(WM_COMMAND, MAKEWPARAM(IDC_ANALYZE, BN_CLICKED));
    }
    return TRUE;
}

void CCleanerDlg::LoadTargets()
{
    const CommandLineOptions& opt = theApp.Options();

    // Remember the user's check marks across a rebuild (e.g. toggling "all users").
    std::vector<std::pair<std::wstring, bool>> previous;
    for (int i = 0; i < m_list.GetItemCount() && i < int(m_targets.size()); ++i)
        previous.emplace_back(m_targets[i].id, m_list.GetCheck(i) != FALSE);

    TargetOptions to;
    to.deep = true;   // list every location; deep ones start unchecked unless /DEEP
    to.allUsers = m_allUsers.GetCheck() == BST_CHECKED;
    if (m_minAge.GetCheck() != BST_CHECKED)
        to.minAgeHoursOverride = 0;
    else if (opt.minAgeHours > 0)
        to.minAgeHoursOverride = opt.minAgeHours;
    m_targets = BuildTargets(to);

    m_loading = true;
    m_list.DeleteAllItems();
    for (int i = 0; i < int(m_targets.size()); ++i)
    {
        const CleanupTarget& t = m_targets[i];
        const bool available = m_elevated || !t.requiresAdmin;

        bool checked = available && (!t.deep || opt.deep);
        for (const auto& p : previous)
            if (p.first == t.id)
                checked = p.second && available;

        m_list.InsertItem(i, t.name.c_str());
        m_list.SetItemText(i, colFolder, t.path.c_str());
        m_list.SetItemText(i, colRules, RulesText(t));
        m_list.SetItemText(i, colStatus, available ? L"" : L"Requires administrator");
        m_list.SetCheck(i, checked);
    }
    m_loading = false;
    m_analysisFresh = false;
}

std::vector<size_t> CCleanerDlg::CheckedTargets() const
{
    std::vector<size_t> rows;
    for (int i = 0; i < m_list.GetItemCount(); ++i)
        if (m_list.GetCheck(i))
            rows.push_back(size_t(i));
    return rows;
}

void CCleanerDlg::OnTargetItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = 0;
    const auto* pNM = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    if (m_loading || !(pNM->uChanged & LVIF_STATE) || ((pNM->uNewState ^ pNM->uOldState) & LVIS_STATEIMAGEMASK) == 0)
        return;

    // Unavailable locations cannot be selected.
    const size_t row = size_t(pNM->iItem);
    if (row < m_targets.size() && m_targets[row].requiresAdmin && !m_elevated && m_list.GetCheck(pNM->iItem))
    {
        m_loading = true;
        m_list.SetCheck(pNM->iItem, FALSE);
        m_loading = false;
        MessageBeep(MB_ICONWARNING);
    }
    m_analysisFresh = false;
    UpdateControls();
}

void CCleanerDlg::OnOptionChanged()
{
    LoadTargets();
    UpdateControls();
}

void CCleanerDlg::UpdateControls()
{
    const bool busy = m_phase != Phase::Idle;
    const bool anyChecked = !CheckedTargets().empty();
    GetDlgItem(IDC_ANALYZE)->EnableWindow(!busy && anyChecked);
    GetDlgItem(IDC_CLEAN)->EnableWindow(!busy && anyChecked);
    GetDlgItem(IDC_STOP)->EnableWindow(busy);
    GetDlgItem(IDC_SAVE_REPORT)->EnableWindow(!busy && m_haveReport);
    GetDlgItem(IDC_RESTART_ADMIN)->EnableWindow(!busy);
    m_list.EnableWindow(!busy);
    m_allUsers.EnableWindow(!busy && m_elevated);
    m_minAge.EnableWindow(!busy);
}

void CCleanerDlg::SetProgressMarquee(bool marquee)
{
    const DWORD style = m_progress.GetStyle();
    if (marquee && !(style & PBS_MARQUEE))
    {
        m_progress.ModifyStyle(0, PBS_MARQUEE);
        m_progress.SetMarquee(TRUE, 30);
    }
    else if (!marquee && (style & PBS_MARQUEE))
    {
        m_progress.SetMarquee(FALSE, 0);
        m_progress.ModifyStyle(PBS_MARQUEE, 0);
    }
}

void CCleanerDlg::OnAnalyze()
{
    m_cleanAfterAnalysis = false;
    StartRun(CCleanupEngine::Mode::Analyze);
}

void CCleanerDlg::OnClean()
{
    // Always measure first so the confirmation shows what will go and the progress bar has a total.
    if (!m_analysisFresh)
    {
        m_cleanAfterAnalysis = true;
        StartRun(CCleanupEngine::Mode::Analyze);
        return;
    }

    unsigned long long files = 0, bytes = 0;
    {
        std::lock_guard<std::mutex> lock(m_resultLock);
        for (const auto& r : m_runResults)
        {
            files += r.filesFound;
            bytes += r.bytesFound;
        }
    }
    if (files == 0)
    {
        SetDlgItemText(IDC_SUMMARY, L"Nothing to clean in the selected locations.");
        return;
    }

    if (m_confirmClean)
    {
        CString prompt;
        prompt.Format(L"Permanently delete %s files (%s) from %u location(s)?\n\n"
                      L"Deleted files do not go to the Recycle Bin. Files that are in use are skipped.",
                      Number(files).GetString(), FormatBytes(bytes).c_str(), unsigned(m_runIndexes.size()));
        if (AfxMessageBox(prompt, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    m_cleanTotalBytes = bytes;
    StartRun(CCleanupEngine::Mode::Clean);
}

void CCleanerDlg::StartRun(CCleanupEngine::Mode mode)
{
    if (m_phase != Phase::Idle)
        return;

    if (mode == CCleanupEngine::Mode::Analyze)
    {
        m_runIndexes = CheckedTargets();
        m_runTargets.clear();
        for (size_t row : m_runIndexes)
            m_runTargets.push_back(m_targets[row]);
    }
    if (m_runTargets.empty())
        return;

    for (size_t row : m_runIndexes)
    {
        m_list.SetItemText(int(row), colStatus, mode == CCleanupEngine::Mode::Analyze ? L"Waiting..." : L"Waiting to clean...");
        if (mode == CCleanupEngine::Mode::Analyze)
        {
            m_list.SetItemText(int(row), colFiles, L"");
            m_list.SetItemText(int(row), colSize, L"");
        }
    }

    m_phase = mode == CCleanupEngine::Mode::Analyze ? Phase::Analyzing : Phase::Cleaning;
    m_lastMode = mode;
    GetLocalTime(&m_runStarted);
    m_runStartTick = GetTickCount64();

    SetProgressMarquee(mode == CCleanupEngine::Mode::Analyze);
    if (mode == CCleanupEngine::Mode::Clean)
    {
        m_progress.SetRange32(0, 1000);
        m_progress.SetPos(0);
    }
    SetDlgItemText(IDC_SUMMARY, mode == CCleanupEngine::Mode::Analyze ? L"Analyzing..." : L"Cleaning...");
    UpdateControls();

    auto* args = new WorkerArgs{ this, &m_engine, mode, &m_runTargets, &m_runResults, &m_resultLock, GetSafeHwnd() };
    m_pThread = AfxBeginThread(WorkerThread, args, THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
    if (!m_pThread)
    {
        delete args;
        m_phase = Phase::Idle;
        UpdateControls();
        AfxMessageBox(L"Could not start the worker thread.", MB_ICONERROR);
        return;
    }
    m_pThread->m_bAutoDelete = FALSE;
    m_pThread->ResumeThread();
    SetTimer(kProgressTimer, 200, nullptr);
}

UINT __cdecl CCleanerDlg::WorkerThread(LPVOID pParam)
{
    std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(pParam));
    std::vector<TargetResult> results;
    args->engine->Run(args->mode, *args->targets, results);
    {
        std::lock_guard<std::mutex> lock(*args->lock);
        args->results->swap(results);
    }
    ::PostMessage(args->hwnd, WM_WORKER_DONE, 0, 0);
    return 0;
}

void CCleanerDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent != kProgressTimer)
    {
        CDialogEx::OnTimer(nIDEvent);
        return;
    }

    const auto p = m_engine.GetProgress();
    CString text;
    if (m_phase == Phase::Analyzing)
        text.Format(L"Analyzing: %s files, %s found  |  %s", Number(p.files).GetString(), FormatBytes(p.bytes).c_str(), p.currentPath.c_str());
    else if (m_phase == Phase::Cleaning)
    {
        text.Format(L"Cleaning: %s files, %s processed  |  %s", Number(p.files).GetString(), FormatBytes(p.bytes).c_str(), p.currentPath.c_str());
        if (m_cleanTotalBytes)
            m_progress.SetPos(int(min(1000.0, 1000.0 * double(p.bytes) / double(m_cleanTotalBytes))));
    }
    if (p.targetIndex < m_runIndexes.size())
        m_list.SetItemText(int(m_runIndexes[p.targetIndex]), colStatus,
                           m_phase == Phase::Analyzing ? L"Analyzing..." : L"Cleaning...");
    SetDlgItemText(IDC_PROGRESS_TEXT, text);
}

LRESULT CCleanerDlg::OnWorkerDone(WPARAM, LPARAM)
{
    KillTimer(kProgressTimer);
    if (m_pThread)
    {
        WaitForSingleObject(m_pThread->m_hThread, INFINITE);
        delete m_pThread;
        m_pThread = nullptr;
    }

    const bool wasAnalysis = m_phase == Phase::Analyzing;
    m_runElapsed = GetTickCount64() - m_runStartTick;
    m_phase = Phase::Idle;
    SetProgressMarquee(false);
    m_progress.SetRange32(0, 1000);
    m_progress.SetPos(wasAnalysis ? 0 : 1000);
    SetDlgItemText(IDC_PROGRESS_TEXT, L"");

    m_haveReport = true;
    ShowResults();
    m_analysisFresh = wasAnalysis && !m_engine.IsCancelled();
    UpdateControls();

    // /LOG in GUI mode: keep the latest report at that path.
    const std::wstring& logPath = theApp.Options().logPath;
    if (!logPath.empty())
    {
        unsigned long error = 0;
        if (!WriteReport(logPath, CurrentReport(), error))
            AfxMessageBox((L"Could not write the log to " + logPath + L":\n" + Win32ErrorText(error)).c_str(), MB_ICONWARNING);
    }

    if (wasAnalysis && m_cleanAfterAnalysis && m_analysisFresh)
    {
        m_cleanAfterAnalysis = false;
        OnClean();
    }
    m_cleanAfterAnalysis = false;
    return 0;
}

void CCleanerDlg::ShowResults()
{
    std::lock_guard<std::mutex> lock(m_resultLock);
    const bool analysis = m_lastMode == CCleanupEngine::Mode::Analyze;

    unsigned long long found = 0, freed = 0, locked = 0, denied = 0, failed = 0, deniedRoots = 0;
    for (size_t i = 0; i < m_runResults.size() && i < m_runIndexes.size(); ++i)
    {
        const TargetResult& r = m_runResults[i];
        const int row = int(m_runIndexes[i]);
        found += r.bytesFound;
        freed += r.bytesDeleted;
        locked += r.filesLocked;
        denied += r.filesDenied + r.dirsDenied;
        failed += r.filesFailed;
        if (r.rootState == TargetResult::RootState::AccessDenied)
            ++deniedRoots;

        CString status;
        if (r.rootState != TargetResult::RootState::Ok)
            status = RootStateText(r).c_str();
        else if (analysis)
            status = r.filesFound ? L"Ready to clean" : L"Nothing to clean";
        else
        {
            status.Format(L"Freed %s", FormatBytes(r.bytesDeleted).c_str());
            if (r.filesLocked)
                status.AppendFormat(L", %s in use", Number(r.filesLocked).GetString());
            if (r.filesDenied + r.filesFailed)
                status.AppendFormat(L", %s failed", Number(r.filesDenied + r.filesFailed).GetString());
        }

        m_list.SetItemText(row, colFiles, Number(analysis ? r.filesFound : r.filesDeleted));
        m_list.SetItemText(row, colSize, FormatBytes(analysis ? r.bytesFound : r.bytesDeleted).c_str());
        m_list.SetItemText(row, colStatus, status);
    }

    CString summary;
    if (m_engine.IsCancelled())
        summary = L"Stopped. ";
    if (analysis)
        summary.AppendFormat(L"%s can be freed.", FormatBytes(found).c_str());
    else
        summary.AppendFormat(L"Freed %s of %s.", FormatBytes(freed).c_str(), FormatBytes(found).c_str());
    if (locked)
        summary.AppendFormat(L"  %s files in use were skipped.", Number(locked).GetString());
    if (denied + failed)
        summary.AppendFormat(L"  %s items could not be removed (see report).", Number(denied + failed).GetString());
    if (deniedRoots)
        summary += L"  Some locations need administrator rights.";
    SetDlgItemText(IDC_SUMMARY, summary);
}

std::wstring CCleanerDlg::CurrentReport() const
{
    ReportInfo info;
    info.version = theApp.Version();
    info.commandLine = theApp.RawCommandLine();
    info.analyzeOnly = m_lastMode == CCleanupEngine::Mode::Analyze;
    info.deep = false;
    for (const auto& t : m_runTargets)
        info.deep = info.deep || t.deep;
    info.elevated = m_elevated;
    info.started = m_runStarted;
    info.elapsedMs = m_runElapsed;
    info.cancelled = m_engine.IsCancelled();

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_resultLock));
    return BuildReport(info, m_runTargets, m_runResults);
}

void CCleanerDlg::OnSaveReport()
{
    CFileDialog dlg(FALSE, L"log", L"WinDiskCleaner_report.log", OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                    L"Log files (*.log)|*.log|Text files (*.txt)|*.txt|All files (*.*)|*.*||", this);
    if (dlg.DoModal() != IDOK)
        return;
    unsigned long error = 0;
    if (!WriteReport(std::wstring(dlg.GetPathName()), CurrentReport(), error))
        AfxMessageBox((L"Could not save the report:\n" + Win32ErrorText(error)).c_str(), MB_ICONERROR);
}

void CCleanerDlg::OnStop()
{
    m_engine.Cancel();
    SetDlgItemText(IDC_SUMMARY, L"Stopping...");
}

void CCleanerDlg::OnRestartAdmin()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, _countof(exe));
    const std::wstring args = theApp.RawCommandLine();

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.hwnd = GetSafeHwnd();

    // The new instance would hit the single-instance check, so release it first and take it back
    // if the user declines the UAC prompt.
    theApp.ReleaseSingleInstance();
    if (ShellExecuteExW(&sei))
    {
        EndDialog(IDCANCEL);
        return;
    }
    const DWORD error = GetLastError();
    theApp.AcquireSingleInstance();
    if (error != ERROR_CANCELLED)
        AfxMessageBox((L"Could not restart as administrator:\n" + Win32ErrorText(error)).c_str(), MB_ICONERROR);
}

void CCleanerDlg::OnCancel()
{
    if (m_phase != Phase::Idle)
    {
        if (AfxMessageBox(L"A cleanup is in progress. Stop it and close?", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        m_engine.Cancel();
    }
    CDialogEx::OnCancel();
}

void CCleanerDlg::OnDestroy()
{
    KillTimer(kProgressTimer);
    if (m_pThread)
    {
        m_engine.Cancel();
        WaitForSingleObject(m_pThread->m_hThread, INFINITE);
        delete m_pThread;
        m_pThread = nullptr;
    }
    CDialogEx::OnDestroy();
}
