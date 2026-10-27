// CommandLine.h : /SILENT /AUTO /DEEP /REPORT /LOG:"path" /MINAGE:hours /ALLUSERS /?
#pragma once
#include <string>
#include <vector>

struct CommandLineOptions
{
    bool silent = false;       // no UI; result in the exit code and the log
    bool autoRun = false;      // clean the standard targets without asking
    bool deep = false;         // also Windows Update cache and system logs
    bool reportOnly = false;   // analyse and report, delete nothing
    bool allUsers = false;     // every local user profile, not just the caller's
    bool help = false;
    int  minAgeHours = -1;     // -1 = per-target defaults
    std::wstring logPath;      // empty = default (silent mode) / none (GUI)
};

// args excludes the program name. Returns false and sets error on an unknown or malformed switch.
bool ParseCommandLine(const std::vector<std::wstring>& args, CommandLineOptions& options, std::wstring& error);

std::wstring CommandLineUsage();

// Exit codes for /SILENT runs (documented in the README).
enum ExitCode
{
    ExitSuccess        = 0,   // finished; locked files skipped as expected
    ExitAccessDenied   = 2,   // one or more targets could not be opened (not elevated?)
    ExitInvalidArgs    = 3,
    ExitAlreadyRunning = 4,
    ExitLogFailed      = 5,   // cleanup ran but the log could not be written
    ExitFatal          = 6,
};
