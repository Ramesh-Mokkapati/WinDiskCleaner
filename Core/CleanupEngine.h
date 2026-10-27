// CleanupEngine.h : scans and deletes files under cleanup targets (Win32 only, no MFC)
#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// One location to clean, e.g. %WINDIR%\Temp.
struct CleanupTarget
{
    std::wstring id;                         // stable identifier, e.g. "SystemTemp"
    std::wstring name;                       // display name
    std::wstring path;                       // expanded absolute root folder
    bool         deep = false;               // only cleaned in /DEEP mode
    bool         requiresAdmin = false;
    unsigned     minAgeHours = 0;            // files changed more recently are kept
    std::vector<std::wstring> extensions;    // lower-case, with dot (".log"); empty = all files
    std::vector<std::wstring> excludedDirs;  // first-level sub-folder names that are never touched
};

struct TargetResult
{
    // Files that match the target's rules (age, extension) and would be / were deleted.
    unsigned long long filesFound = 0;
    unsigned long long bytesFound = 0;
    unsigned long long filesDeleted = 0;
    unsigned long long bytesDeleted = 0;

    unsigned long long filesLocked = 0;      // ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION
    unsigned long long filesDenied = 0;      // ERROR_ACCESS_DENIED
    unsigned long long filesFailed = 0;      // any other error
    unsigned long long filesTooNew = 0;      // kept because of the minimum age
    unsigned long long dirsRemoved = 0;      // empty sub-folders removed
    unsigned long long dirsDenied = 0;       // sub-folders that could not be listed
    unsigned long long linksSkipped = 0;     // junctions / symlinks (never followed)

    enum class RootState { NotRun, Ok, Missing, AccessDenied, Rejected, Cancelled };
    RootState    rootState = RootState::NotRun;
    unsigned long errorCode = 0;            // Win32 error for Missing / AccessDenied

    std::vector<std::wstring> errors;       // first kMaxErrors failures: "path: message"
    static const size_t kMaxErrors = 200;
};

class CCleanupEngine
{
public:
    enum class Mode { Analyze, Clean };

    struct Progress
    {
        size_t             targetIndex = 0;
        unsigned long long files = 0;        // processed in the current run
        unsigned long long bytes = 0;
        std::wstring       currentPath;
    };

    // Runs every target in order. results is resized to targets.size().
    // Safe to call from a worker thread; Cancel() and GetProgress() may be called from any thread.
    void Run(Mode mode, const std::vector<CleanupTarget>& targets, std::vector<TargetResult>& results);

    void Cancel()            { m_cancel = true; }
    bool IsCancelled() const { return m_cancel; }
    Progress GetProgress() const;

    // A root is only accepted if it is an absolute local folder deeper than a drive root and
    // is not itself a junction or symlink.
    static bool IsSafeRoot(const std::wstring& path, unsigned long& error);

private:
    void ProcessTarget(Mode mode, const CleanupTarget& target, TargetResult& result);
    // Returns true when the folder ended up empty (all children removed).
    bool ProcessFolder(Mode mode, const CleanupTarget& target, const std::wstring& longPath,
                       int depth, unsigned long long cutoff, TargetResult& result);
    bool Matches(const CleanupTarget& target, const wchar_t* fileName) const;
    void AddError(TargetResult& result, const std::wstring& path, unsigned long error);
    void SetCurrentPath(const std::wstring& longPath);

    std::atomic<bool>               m_cancel { false };
    std::atomic<size_t>             m_targetIndex { 0 };
    std::atomic<unsigned long long> m_files { 0 };
    std::atomic<unsigned long long> m_bytes { 0 };
    mutable std::mutex              m_pathLock;
    std::wstring                    m_currentPath;
};

// Helpers shared by the app, the report and the tests.
std::wstring FormatBytes(unsigned long long bytes);
std::wstring Win32ErrorText(unsigned long error);
std::wstring ToLongPath(const std::wstring& path);     // adds the \\?\ prefix
std::wstring FromLongPath(const std::wstring& path);   // strips it again for display
