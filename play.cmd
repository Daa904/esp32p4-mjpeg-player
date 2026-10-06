@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if "%~2"=="" goto usage
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0play-video.ps1" %*
exit /b %errorlevel%

:usage
 echo Usage: play.cmd "video.mp4" seconds [COM7] [-BuildOnly]
 echo Example: play.cmd "test.mp4" 5
 echo Put your MP4 in the mp4 folder. Default port: COM7.
exit /b 2
