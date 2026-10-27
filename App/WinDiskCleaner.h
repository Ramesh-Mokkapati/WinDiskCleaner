// WinDiskCleaner.h : application class
#pragma once

#ifndef __AFXWIN_H__
#error "include 'pch.h' before including this file for PCH"
#endif

#include "resource.h"
#include "..\Core\CommandLine.h"

class CWinDiskCleanerApp : public CWinApp
{
public:
    CWinDiskCleanerApp();

    BOOL InitInstance() override;
    int  ExitInstance() override;

    const CommandLineOptions& Options() const { return m_options; }
    const std::wstring& RawCommandLine() const { return m_rawArgs; }
    static std::wstring Version();

    // Used around "Restart as administrator" so the elevated copy can start.
    bool AcquireSingleInstance();
    void ReleaseSingleInstance();

private:
    int  RunSilent();

    CommandLineOptions m_options;
    std::wstring       m_rawArgs;
    HANDLE             m_hMutex = nullptr;
    int                m_exitCode = ExitSuccess;

    DECLARE_MESSAGE_MAP()
};

extern CWinDiskCleanerApp theApp;
