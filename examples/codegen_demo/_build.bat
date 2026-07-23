@echo off
setlocal
set VSWHERE="C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
set MSBUILD=
if exist %VSWHERE% (
  for /f "tokens=*" %%a in ('%VSWHERE% -latest -requires Microsoft.Component.MSBuild -find MSBuild\Current\Bin\MSBuild.exe') do set MSBUILD="%%a"
)
if not defined MSBUILD (
  set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
)
if not exist %MSBUILD% (
  echo ERROR: MSBuild.exe not found.
  exit /b 1
)

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set PLATFORM=%2
if "%PLATFORM%"=="" set PLATFORM=x64

set PROJECT=%~dp0\build\dcb_codegen_demo.vcxproj
%MSBUILD% %PROJECT% /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m /verbosity:minimal
exit /b %ERRORLEVEL%
