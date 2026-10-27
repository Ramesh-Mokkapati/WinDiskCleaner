// CleanupEngine.cpp
#include "CleanupEngine.h"

#include <algorithm>
#include <cwctype>

namespace
{
    const wchar_t kLongPrefix[] = L"\\\\?\\";

    unsigned long long ToU64(const FILETIME& ft)
    {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    unsigned long long NowFileTime()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return ToU64(ft);
    }

    bool IsDotEntry(const wchar_t* name)
    {
        return (name[0] == L'.' && name[1] == 0) || (name[0] == L'.' && name[1] == L'.' && name[2] == 0);
    }

    std::wstring Lower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    }

    // Newest of creation and last-write time: a file copied in recently keeps its old
    // write time, but its creation time is new.
    unsigned long long FileAge(const WIN32_FIND_DATAW& fd)
    {
        return (std::max)(ToU64(fd.ftLastWriteTime), ToU64(fd.ftCreationTime));
    }
}

// ---------------------------------------------------------------------------

std::wstring ToLongPath(const std::wstring& path)
{
    if (path.compare(0, 4, kLongPrefix) == 0)
        return path;
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    return kLongPrefix + p;
}

std::wstring FromLongPath(const std::wstring& path)
{
    return path.compare(0, 4, kLongPrefix) == 0 ? path.substr(4) : path;
}

std::wstring FormatBytes(unsigned long long bytes)
{
    static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4)
    {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[64];
    if (u == 0)
        swprintf_s(buf, L"%llu B", bytes);
    else
        swprintf_s(buf, v < 10.0 ? L"%.2f %s" : v < 100.0 ? L"%.1f %s" : L"%.0f %s", v, units[u]);
    return buf;
}

std::wstring Win32ErrorText(unsigned long error)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text = buf ? buf : L"Unknown error";
    if (buf)
        LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.'))
        text.pop_back();
    wchar_t code[32];
    swprintf_s(code, L" (0x%08lX)", error);
    return text + code;
}

// ---------------------------------------------------------------------------

bool CCleanupEngine::IsSafeRoot(const std::wstring& path, unsigned long& error)
{
    error = ERROR_SUCCESS;
    const std::wstring p = FromLongPath(path);

    // Must be "X:\something": absolute, local, and never a bare drive root.
    if (p.size() < 4 || !std::iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\' || p.find(L"..") != std::wstring::npos)
    {
        error = ERROR_INVALID_NAME;
        return false;
    }

    const DWORD attrs = GetFileAttributesW(ToLongPath(p).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        error = GetLastError();
        return false;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        error = ERROR_DIRECTORY;
        return false;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
    {
        // A redirected root could point anywhere; refuse it.
        error = ERROR_CANT_ACCESS_FILE;
        return false;
    }
    return true;
}

CCleanupEngine::Progress CCleanupEngine::GetProgress() const
{
    Progress p;
    p.targetIndex = m_targetIndex;
    p.files = m_files;
    p.bytes = m_bytes;
    std::lock_guard<std::mutex> lock(m_pathLock);
    p.currentPath = m_currentPath;
    return p;
}

void CCleanupEngine::SetCurrentPath(const std::wstring& longPath)
{
    std::lock_guard<std::mutex> lock(m_pathLock);
    m_currentPath = FromLongPath(longPath);
}

void CCleanupEngine::Run(Mode mode, const std::vector<CleanupTarget>& targets, std::vector<TargetResult>& results)
{
    m_cancel = false;
    m_files = 0;
    m_bytes = 0;
    results.assign(targets.size(), TargetResult());

    for (size_t i = 0; i < targets.size(); ++i)
    {
        m_targetIndex = i;
        if (m_cancel)
        {
            results[i].rootState = TargetResult::RootState::Cancelled;
            continue;
        }
        ProcessTarget(mode, targets[i], results[i]);
    }
    SetCurrentPath(L"");
}

void CCleanupEngine::ProcessTarget(Mode mode, const CleanupTarget& target, TargetResult& result)
{
    unsigned long error = ERROR_SUCCESS;
    if (!IsSafeRoot(target.path, error))
    {
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            result.rootState = TargetResult::RootState::Missing;
        else if (error == ERROR_ACCESS_DENIED)
            result.rootState = TargetResult::RootState::AccessDenied;
        else
            result.rootState = TargetResult::RootState::Rejected;
        result.errorCode = error;
        return;
    }

    const unsigned long long hour = 36000000000ULL;   // 100 ns units
    const unsigned long long cutoff = NowFileTime() - target.minAgeHours * hour;

    result.rootState = TargetResult::RootState::Ok;
    ProcessFolder(mode, target, ToLongPath(target.path), 0, cutoff, result);

    if (m_cancel)
        result.rootState = TargetResult::RootState::Cancelled;
}

bool CCleanupEngine::Matches(const CleanupTarget& target, const wchar_t* fileName) const
{
    if (target.extensions.empty())
        return true;
    const std::wstring name = Lower(fileName);
    for (const auto& ext : target.extensions)
        if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
            return true;
    return false;
}

void CCleanupEngine::AddError(TargetResult& result, const std::wstring& path, unsigned long error)
{
    if (result.errors.size() < TargetResult::kMaxErrors)
        result.errors.push_back(FromLongPath(path) + L": " + Win32ErrorText(error));
}

bool CCleanupEngine::ProcessFolder(Mode mode, const CleanupTarget& target, const std::wstring& longPath,
                                   int depth, unsigned long long cutoff, TargetResult& result)
{
    SetCurrentPath(longPath);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW((longPath + L"\\*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES)
            return true;   // empty
        if (depth == 0)
        {
            result.rootState = err == ERROR_ACCESS_DENIED ? TargetResult::RootState::AccessDenied
                                                          : TargetResult::RootState::Rejected;
            result.errorCode = err;
        }
        else
        {
            ++result.dirsDenied;
            AddError(result, longPath, err);
        }
        return false;
    }

    bool empty = true;
    do
    {
        if (m_cancel)
        {
            empty = false;
            break;
        }
        if (IsDotEntry(fd.cFileName))
            continue;

        const std::wstring child = longPath + L"\\" + fd.cFileName;

        // Junctions, symlinks and mount points are never followed or deleted: they can point
        // outside the target (e.g. a junction in Temp to C:\Users).
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            ++result.linksSkipped;
            empty = false;
            continue;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth == 0)
            {
                const std::wstring name = Lower(fd.cFileName);
                if (std::find(target.excludedDirs.begin(), target.excludedDirs.end(), name) != target.excludedDirs.end())
                {
                    empty = false;
                    continue;
                }
            }

            const bool childEmpty = ProcessFolder(mode, target, child, depth + 1, cutoff, result);
            // Remove a folder only once it is empty and itself old enough, so an installer that
            // just created a working folder in Temp doesn't lose it.
            if (childEmpty && mode == Mode::Clean && FileAge(fd) <= cutoff && !m_cancel)
            {
                if (RemoveDirectoryW(child.c_str()))
                {
                    ++result.dirsRemoved;
                    continue;
                }
            }
            empty = false;
            continue;
        }

        // Regular file
        if (!Matches(target, fd.cFileName))
        {
            empty = false;
            continue;
        }
        if (FileAge(fd) > cutoff)
        {
            ++result.filesTooNew;
            empty = false;
            continue;
        }

        const unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        ++result.filesFound;
        result.bytesFound += size;
        ++m_files;
        m_bytes += size;

        if (mode == Mode::Analyze)
        {
            empty = false;
            continue;
        }

        const bool readOnly = (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (readOnly)
            SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);

        if (DeleteFileW(child.c_str()))
        {
            ++result.filesDeleted;
            result.bytesDeleted += size;
            continue;
        }

        const DWORD err = GetLastError();
        if (readOnly)
            SetFileAttributesW(child.c_str(), fd.dwFileAttributes);   // put it back as it was
        empty = false;

        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            ++result.filesLocked;            // in use: expected, skipped silently
        else if (err == ERROR_ACCESS_DENIED)
        {
            ++result.filesDenied;
            AddError(result, child, err);
        }
        else
        {
            ++result.filesFailed;
            AddError(result, child, err);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return empty;
}
