// pch.h : precompiled header for WinDiskCleaner
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

// _WIN32_WINNT / WINVER come from the project (0x0601 = Windows 7 / Server 2008 R2 and later).
#include <sdkddkver.h>

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#define _AFX_ALL_WARNINGS

#include <afxwin.h>
#include <afxext.h>
#include <afxcmn.h>
#include <afxdialogex.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
