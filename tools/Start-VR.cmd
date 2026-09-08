@echo off
setlocal
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Start-VR.ps1" %*
set "vrExit=%ERRORLEVEL%"
if not "%vrExit%"=="0" (
  echo.
  echo VR did not start or exited with an error. See the message above.
  pause
)
exit /b %vrExit%
