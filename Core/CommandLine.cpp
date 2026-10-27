// CommandLine.cpp
#include "CommandLine.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace
{
    std::wstring Upper(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        return s;
    }

    std::wstring Unquote(std::wstring s)
    {
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
            s = s.substr(1, s.size() - 2);
        return s;
    }
}

bool ParseCommandLine(const std::vector<std::wstring>& args, CommandLineOptions& options, std::wstring& error)
{
    options = CommandLineOptions();
    error.clear();

    for (const auto& raw : args)
    {
        if (raw.empty())
            continue;
        if (raw[0] != L'/' && raw[0] != L'-')
        {
            error = L"Unexpected argument: " + raw;
            return false;
        }

        const std::wstring body = raw.substr(1);
        const size_t colon = body.find(L':');
        const std::wstring name = Upper(body.substr(0, colon));
        const std::wstring value = colon == std::wstring::npos ? L"" : Unquote(body.substr(colon + 1));
        const bool hasValue = colon != std::wstring::npos;

        if (name == L"SILENT" && !hasValue)        options.silent = true;
        else if (name == L"AUTO" && !hasValue)     options.autoRun = true;
        else if (name == L"DEEP" && !hasValue)     options.deep = true;
        else if (name == L"REPORT" && !hasValue)   options.reportOnly = true;
        else if (name == L"ALLUSERS" && !hasValue) options.allUsers = true;
        else if ((name == L"?" || name == L"HELP") && !hasValue) options.help = true;
        else if (name == L"LOG")
        {
            if (value.empty())
            {
                error = L"/LOG needs a file path, e.g. /LOG:\"C:\\ProgramData\\DiskCleaner\\Logs\\run.log\"";
                return false;
            }
            options.logPath = value;
        }
        else if (name == L"MINAGE")
        {
            wchar_t* end = nullptr;
            const long hours = value.empty() ? -1 : std::wcstol(value.c_str(), &end, 10);
            if (value.empty() || !end || *end != 0 || hours < 0 || hours > 24 * 3650)
            {
                error = L"/MINAGE needs a number of hours between 0 and 87600, e.g. /MINAGE:48";
                return false;
            }
            options.minAgeHours = static_cast<int>(hours);
        }
        else
        {
            error = L"Unknown switch: " + raw;
            return false;
        }
    }

    // A silent run with no explicit scope cleans the standard targets.
    if (options.silent && !options.deep && !options.reportOnly)
        options.autoRun = true;
    return true;
}

std::wstring CommandLineUsage()
{
    return
        L"WinDiskCleaner.exe [/SILENT] [/AUTO] [/DEEP] [/REPORT] [/ALLUSERS] [/MINAGE:hours] [/LOG:\"path\"]\n"
        L"\n"
        L"  (no switches)  Open the cleanup window.\n"
        L"  /SILENT        No window. Writes a log and returns an exit code.\n"
        L"  /AUTO          Clean the standard targets: Windows Temp, user Temp, Internet cache.\n"
        L"  /DEEP          Also clean the Windows Update download cache and old system logs.\n"
        L"  /REPORT        Only measure what would be removed; delete nothing.\n"
        L"  /ALLUSERS      Include the Temp and Internet cache of every local user profile.\n"
        L"  /MINAGE:hours  Only remove files older than this (default: 24 h temp, 7 days logs).\n"
        L"  /LOG:\"path\"    Write the report here (default for /SILENT:\n"
        L"                 %ProgramData%\\DiskCleaner\\Logs\\cleanup_last_run.log).\n"
        L"\n"
        L"Exit codes: 0 success, 2 access denied, 3 invalid arguments, 4 already running,\n"
        L"5 log not written, 6 unexpected error.";
}
