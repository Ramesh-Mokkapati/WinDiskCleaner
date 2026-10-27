// Report.h : plain-text run report / log file
#pragma once
#include "CleanupEngine.h"

struct ReportInfo
{
    std::wstring version;
    std::wstring commandLine;
    bool         analyzeOnly = false;
    bool         deep = false;
    bool         elevated = false;
    SYSTEMTIME   started = {};
    unsigned long long elapsedMs = 0;
    bool         cancelled = false;
};

std::wstring BuildReport(const ReportInfo& info, const std::vector<CleanupTarget>& targets,
                         const std::vector<TargetResult>& results);

// Writes the report as UTF-8, creating the folder if needed.
bool WriteReport(const std::wstring& path, const std::wstring& text, unsigned long& error);

std::wstring DefaultLogPath();   // %ProgramData%\DiskCleaner\Logs\cleanup_last_run.log

std::wstring RootStateText(const TargetResult& r);
