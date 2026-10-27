<#
.SYNOPSIS
    Non-destructive checks of a built WinDiskCleaner.exe: silent report mode, exit codes,
    the single-instance mutex, and that the window opens and closes cleanly.

.DESCRIPTION
    Only /REPORT runs are used, so nothing is deleted from the machine running the test.
    Deletion itself is covered by CleanerTests.exe, which works in throw-away folders.
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug',
    [ValidateSet('x64', 'x86')]       [string] $Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

# The solution platform x86 builds the projects' Win32 platform, which is the folder name under bin\.
$platformDir = if ($Platform -eq 'x86') { 'Win32' } else { 'x64' }
$exe = Join-Path $PSScriptRoot "..\bin\$platformDir\$Configuration\WinDiskCleaner.exe"
if (-not (Test-Path $exe)) { throw "WinDiskCleaner.exe not found at $exe - build $Configuration|$Platform first." }
$exe = (Resolve-Path $exe).Path

$work = Join-Path ([IO.Path]::GetTempPath()) ("WdcSmoke_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null

function Invoke-Cleaner([string[]] $Arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $Arguments -PassThru
    if (-not $p.WaitForExit(300000)) { $p.Kill(); throw "Timed out: $($Arguments -join ' ')" }
    return $p.ExitCode
}

try {
    # 1. Silent analysis of every target, including /DEEP and /ALLUSERS.
    $log = Join-Path $work 'nested\report.log'
    $code = Invoke-Cleaner @('/SILENT', '/REPORT', '/DEEP', '/ALLUSERS', "/LOG:`"$log`"")
    Write-Host "Silent report exit code: $code"
    # 0 when elevated; 2 when a system folder needs administrator rights.
    if ($code -ne 0 -and $code -ne 2) { throw "Unexpected exit code $code for /SILENT /REPORT." }
    if (-not (Test-Path $log)) { throw "Report was not written to $log." }
    $text = Get-Content $log -Raw -Encoding UTF8
    foreach ($expected in 'analysis report', 'Report only (nothing deleted)', 'System temporary files',
                          'Windows Update download cache', 'Summary') {
        if ($text -notlike "*$expected*") { throw "Report is missing '$expected'." }
    }
    # Per-target "  Removed:" lines are only written for cleanup runs.
    if ($text -cmatch '(?m)^\s+Removed:') { throw 'A /REPORT run must not remove anything.' }
    Write-Host ($text -split "`r?`n" | Select-String -Pattern '^  (Can be removed|In use|Access denied)' | Out-String).Trim()

    # 2. Invalid switch -> exit code 3, no window.
    $code = Invoke-Cleaner @('/SILENT', '/BOGUS')
    if ($code -ne 3) { throw "Expected exit code 3 for an unknown switch, got $code." }
    Write-Host 'Unknown switch rejected with exit code 3.'

    # 3. Window opens; a second instance is refused; window closes cleanly.
    $gui = Start-Process -FilePath $exe -PassThru
    $deadline = (Get-Date).AddSeconds(30)
    do { Start-Sleep -Milliseconds 500; $gui.Refresh() } while ($gui.MainWindowHandle -eq [IntPtr]::Zero -and -not $gui.HasExited -and (Get-Date) -lt $deadline)
    if ($gui.HasExited) { throw "The window process exited during startup (code $($gui.ExitCode))." }
    if ($gui.MainWindowHandle -eq [IntPtr]::Zero) { throw 'No window within 30 seconds.' }
    Start-Sleep -Seconds 2
    $gui.Refresh()
    if ($gui.MainWindowTitle -notlike 'Enterprise Disk Cleanup Utility*') { throw "Unexpected window '$($gui.MainWindowTitle)' (assertion or error dialog?)." }
    Write-Host "Window: '$($gui.MainWindowTitle)'"

    $code = Invoke-Cleaner @('/SILENT', '/REPORT', "/LOG:`"$(Join-Path $work 'second.log')`"")
    if ($code -ne 4) { throw "Expected exit code 4 while another instance runs, got $code." }
    Write-Host 'Second instance refused with exit code 4.'

    if (-not $gui.CloseMainWindow()) { throw 'Could not close the window.' }
    if (-not $gui.WaitForExit(15000)) { $gui.Kill(); throw 'The window did not close within 15 seconds.' }
    if ($gui.ExitCode -ne 0) { throw "Window process exited with code $($gui.ExitCode)." }

    Write-Host 'Smoke test passed.'
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
