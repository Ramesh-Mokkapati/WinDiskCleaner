// Report.cpp
#include "Report.h"

#include <shlobj.h>
#include <sstream>

namespace
{
    std::wstring Pad(const std::wstring& s, size_t width)
    {
        return s.size() >= width ? s + L" " : s + std::wstring(width - s.size(), L' ');
    }

    std::wstring Number(unsigned long long v)
    {
        std::wstring digits = std::to_wstring(v), out;
        for (size_t i = 0; i < digits.size(); ++i)
        {
            out += digits[i];
            const size_t left = digits.size() - i - 1;
            if (left > 0 && left % 3 == 0)
                out += L',';
        }
        return out;
    }

    std::wstring ComputerName()
    {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD cch = _countof(buf);
        return GetComputerNameW(buf, &cch) ? buf : L"?";
    }

    std::wstring UserName()
    {
        wchar_t buf[256] = {};
        DWORD cch = _countof(buf);
        return GetUserNameW(buf, &cch) ? buf : L"?";
    }
}

std::wstring RootStateText(const TargetResult& r)
{
    switch (r.rootState)
    {
    case TargetResult::RootState::NotRun:       return L"Not run";
    case TargetResult::RootState::Ok:           return L"OK";
    case TargetResult::RootState::Missing:      return L"Folder not found";
    case TargetResult::RootState::AccessDenied: return L"Access denied (run as administrator)";
    case TargetResult::RootState::Rejected:     return L"Skipped (unsafe or invalid folder)";
    case TargetResult::RootState::Cancelled:    return L"Cancelled";
    }
    return L"?";
}

std::wstring DefaultLogPath()
{
    PWSTR programData = nullptr;
    std::wstring base = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) && programData)
        base = programData;
    CoTaskMemFree(programData);
    return base + L"\\DiskCleaner\\Logs\\cleanup_last_run.log";
}

std::wstring BuildReport(const ReportInfo& info, const std::vector<CleanupTarget>& targets,
                         const std::vector<TargetResult>& results)
{
    std::wostringstream out;
    wchar_t when[64];
    swprintf_s(when, L"%04u-%02u-%02u %02u:%02u:%02u", info.started.wYear, info.started.wMonth, info.started.wDay,
               info.started.wHour, info.started.wMinute, info.started.wSecond);

    out << L"WinDiskCleaner " << info.version << L" - " << (info.analyzeOnly ? L"analysis report" : L"cleanup report") << L"\r\n";
    out << L"Started:      " << when << L"  (" << (info.elapsedMs / 1000.0) << L" s)\r\n";
    out << L"Computer:     " << ComputerName() << L"   User: " << UserName()
        << L"   Elevated: " << (info.elevated ? L"Yes" : L"No") << L"\r\n";
    out << L"Mode:         " << (info.analyzeOnly ? L"Report only (nothing deleted)" : L"Clean")
        << (info.deep ? L", deep" : L"") << (info.cancelled ? L", CANCELLED" : L"") << L"\r\n";
    if (!info.commandLine.empty())
        out << L"Command line: " << info.commandLine << L"\r\n";
    out << L"\r\n";

    unsigned long long found = 0, freed = 0, foundFiles = 0, deletedFiles = 0, locked = 0, denied = 0, failed = 0;
    for (size_t i = 0; i < targets.size() && i < results.size(); ++i)
    {
        const CleanupTarget& t = targets[i];
        const TargetResult& r = results[i];
        found += r.bytesFound;
        freed += r.bytesDeleted;
        foundFiles += r.filesFound;
        deletedFiles += r.filesDeleted;
        locked += r.filesLocked;
        denied += r.filesDenied + r.dirsDenied;
        failed += r.filesFailed;

        out << L"[" << t.name << L"]\r\n";
        out << L"  Path:        " << t.path << L"\r\n";
        out << L"  Status:      " << RootStateText(r);
        if (r.errorCode && r.rootState != TargetResult::RootState::Ok)
            out << L" - " << Win32ErrorText(r.errorCode);
        out << L"\r\n";
        if (t.minAgeHours == 0)
            out << L"  Rules:       files of any age";
        else
            out << L"  Rules:       files older than " << t.minAgeHours << L" h";
        if (!t.extensions.empty())
        {
            out << L"; types";
            for (const auto& e : t.extensions)
                out << L" " << e;
        }
        if (!t.excludedDirs.empty())
        {
            out << L"; never touches";
            for (const auto& d : t.excludedDirs)
                out << L" " << d << L"\\";
        }
        out << L"\r\n";
        out << L"  Eligible:    " << Number(r.filesFound) << L" files, " << FormatBytes(r.bytesFound) << L"\r\n";
        if (!info.analyzeOnly)
            out << L"  Removed:     " << Number(r.filesDeleted) << L" files, " << FormatBytes(r.bytesDeleted)
                << L"; " << Number(r.dirsRemoved) << L" empty folders\r\n";
        out << L"  Skipped:     " << Number(r.filesLocked) << L" in use, " << Number(r.filesDenied) << L" access denied, "
            << Number(r.filesFailed) << L" other errors, " << Number(r.filesTooNew) << L" too new, "
            << Number(r.linksSkipped) << L" links, " << Number(r.dirsDenied) << L" unreadable folders\r\n";
        if (!r.errors.empty())
        {
            out << L"  Errors:\r\n";
            for (const auto& e : r.errors)
                out << L"    " << e << L"\r\n";
            if (r.errors.size() >= TargetResult::kMaxErrors)
                out << L"    (further errors not listed)\r\n";
        }
        out << L"\r\n";
    }

    out << L"Summary\r\n";
    if (info.analyzeOnly)
        out << L"  Can be removed: " << Number(foundFiles) << L" files, " << FormatBytes(found) << L"\r\n";
    else
        out << L"  Freed:          " << FormatBytes(freed) << L" (" << Number(deletedFiles) << L" of "
            << Number(foundFiles) << L" eligible files)\r\n";
    out << L"  In use:         " << Number(locked) << L" files skipped\r\n";
    out << L"  Access denied:  " << Number(denied) << L"\r\n";
    out << L"  Other errors:   " << Number(failed) << L"\r\n";
    return out.str();
}

bool WriteReport(const std::wstring& path, const std::wstring& text, unsigned long& error)
{
    error = ERROR_SUCCESS;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        const std::wstring dir = path.substr(0, slash);
        const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
        {
            error = static_cast<unsigned long>(rc);
            return false;
        }
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], bytes, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
        return false;
    }
    static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };   // so Notepad shows the text correctly
    DWORD written = 0;
    bool ok = WriteFile(h, bom, sizeof(bom), &written, nullptr) &&
              WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
    if (!ok)
        error = GetLastError();
    CloseHandle(h);
    return ok;
}
