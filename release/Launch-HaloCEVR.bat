@echo off
setlocal

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-HaloCEVR.ps1" -UevrRoot "E:\Downloads\uevr" -ForceSteamExitRecovery %*
set "HALOCEVR_EXIT_CODE=%ERRORLEVEL%"

if not "%HALOCEVR_EXIT_CODE%"=="0" (
    echo.
    echo HaloCEVR failed to launch. Exit code: %HALOCEVR_EXIT_CODE%
    pause
)

exit /b %HALOCEVR_EXIT_CODE%
