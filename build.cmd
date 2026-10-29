@echo off
setlocal
rem Build with Visual Studio 2022+ C++ x64 tools and a Windows SDK.
rem Run from an x64 Native Tools prompt, or let vswhere locate the tools.
rem Optional overrides: AVOIDANCE_VCVARS (vcvars64.bat), AVOIDANCE_TOOLS_DIR.
rem Outputs are written to AVOIDANCE_TOOLS_DIR\build, outside source control.
set "AVOIDANCE_PROJECT_DIR=%~dp0"
if defined AVOIDANCE_VCVARS goto explicit_tools
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    where cl >nul 2>nul
    if not errorlevel 1 goto tools_ready
)
set "AVOIDANCE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%AVOIDANCE_VSWHERE%" goto missing_tools
for /f "usebackq tokens=*" %%i in (`"%AVOIDANCE_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "AVOIDANCE_VS_PATH=%%i"
if not defined AVOIDANCE_VS_PATH goto missing_tools
set "AVOIDANCE_VCVARS=%AVOIDANCE_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
:explicit_tools
if not exist "%AVOIDANCE_VCVARS%" goto missing_tools
call "%AVOIDANCE_VCVARS%"
if errorlevel 1 exit /b %errorlevel%
:tools_ready
if not defined AVOIDANCE_TOOLS_DIR set "AVOIDANCE_TOOLS_DIR=%TEMP%\BG3_Camera_Raycast_Fix"
if not exist "%AVOIDANCE_TOOLS_DIR%\build" mkdir "%AVOIDANCE_TOOLS_DIR%\build"
if errorlevel 1 exit /b %errorlevel%
pushd "%AVOIDANCE_TOOLS_DIR%\build"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /W4 /WX /O2 /Zi /MD /EHsc /LD "%AVOIDANCE_PROJECT_DIR%startup.cpp" "%AVOIDANCE_PROJECT_DIR%camera_avoidance_fix.cpp" /link /MACHINE:X64 /INCREMENTAL:NO /OPT:REF /OPT:ICF /OUT:BG3_Camera_Raycast_Fix.dll /PDB:BG3_Camera_Raycast_Fix.pdb /PDBALTPATH:BG3_Camera_Raycast_Fix.pdb
set "AVOIDANCE_BUILD_RESULT=%errorlevel%"
popd
exit /b %AVOIDANCE_BUILD_RESULT%
:missing_tools
echo Install Visual Studio C++ x64 tools, run an x64 Native Tools prompt, or set AVOIDANCE_VCVARS to vcvars64.bat. 1>&2
exit /b 1
