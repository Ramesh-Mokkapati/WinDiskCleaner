// CrashHandler.cpp
#include "pch.h"
#include "CrashHandler.h"

#include <dbghelp.h>
#include <shlobj.h>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
    LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* pException)
    {
        // Same location Windows Error Reporting uses for LocalDumps, as the runbook expects.
        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData)
            return EXCEPTION_CONTINUE_SEARCH;
        CString dir;
        dir.Format(L"%s\\CrashDumps", localAppData);
        CoTaskMemFree(localAppData);
        CreateDirectoryW(dir, nullptr);

        const CString path = dir + L"\\WinDiskCleaner.exe.dmp";
        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei = {};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = pException;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                         MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                              &mei, nullptr, nullptr);
            CloseHandle(hFile);
        }
        // Let Windows terminate the process (and WER report it) as usual.
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void InstallCrashHandler()
{
    SetUnhandledExceptionFilter(WriteCrashDump);
}
