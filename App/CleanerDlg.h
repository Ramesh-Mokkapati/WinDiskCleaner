// CleanerDlg.h : interactive cleanup window
#pragma once
#include "..\Core\CleanupEngine.h"

class CCleanerDlg : public CDialogEx
{
public:
    explicit CCleanerDlg(CWnd* pParent = nullptr);
    enum { IDD = IDD_CLEANER_DIALOG };

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override {}                // Enter must not close the window
    void OnCancel() override;              // Close / Escape

    afx_msg void OnAnalyze();
    afx_msg void OnClean();
    afx_msg void OnStop();
    afx_msg void OnSaveReport();
    afx_msg void OnRestartAdmin();
    afx_msg void OnOptionChanged();
    afx_msg void OnTargetItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnWorkerDone(WPARAM wParam, LPARAM lParam);
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()

private:
    enum class Phase { Idle, Analyzing, Cleaning };

    void LoadTargets();
    std::vector<size_t> CheckedTargets() const;
    void StartRun(CCleanupEngine::Mode mode);
    void ShowResults();
    void UpdateControls();
    void SetProgressMarquee(bool marquee);
    std::wstring CurrentReport() const;
    static UINT __cdecl WorkerThread(LPVOID pParam);

    CListCtrl     m_list;
    CProgressCtrl m_progress;
    CButton       m_allUsers;
    CButton       m_minAge;

    std::vector<CleanupTarget> m_targets;         // all rows in the list
    std::vector<size_t>        m_runIndexes;      // rows included in the last / current run
    std::vector<CleanupTarget> m_runTargets;
    std::vector<TargetResult>  m_runResults;      // written by the worker, read after WM_WORKER_DONE
    std::mutex                 m_resultLock;

    CCleanupEngine  m_engine;
    CWinThread*     m_pThread = nullptr;
    Phase           m_phase = Phase::Idle;
    CCleanupEngine::Mode m_lastMode = CCleanupEngine::Mode::Analyze;
    bool            m_analysisFresh = false;      // last analysis matches the current selection
    bool            m_cleanAfterAnalysis = false;
    bool            m_confirmClean = true;        // false for /AUTO
    bool            m_elevated = false;
    bool            m_haveReport = false;
    bool            m_loading = false;
    unsigned long long m_cleanTotalBytes = 0;
    SYSTEMTIME      m_runStarted = {};
    ULONGLONG       m_runStartTick = 0;
    ULONGLONG       m_runElapsed = 0;
    HICON           m_hIcon = nullptr;
};
