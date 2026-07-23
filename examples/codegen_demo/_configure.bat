@echo off
setlocal
set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist %CMAKE% (
  echo ERROR: cmake.exe not found.
  exit /b 1
)
set SRC=%~dp0
if "%SRC:~-1%"=="\" set SRC=%SRC:~0,-1%
set BUILD=%SRC%\build
%CMAKE% -S "%SRC%" -B "%BUILD%"
exit /b %ERRORLEVEL%
