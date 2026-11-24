@echo off
setlocal
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Install Visual Studio 2022 Build Tools with Desktop development with C++.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo Visual Studio C++ x86 build tools were not found.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /W4 /WX /std:c++17 /O2 /MT /LD /Fo"%OUT%\FFXProceduralMouth.obj" "%ROOT%src\FFXProceduralMouth.cpp" /link /IMPLIB:"%OUT%\FFXProceduralMouth.lib" /OUT:"%OUT%\FFXProceduralMouth.dll"
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /std:c++17 /O2 /MT /DFFX_MOUTH_TEST /Fo"%OUT%\FFXProceduralMouth-test.obj" /Fe"%OUT%\FFXProceduralMouth-test.exe" "%ROOT%src\FFXProceduralMouth.cpp" /link /IMPLIB:"%OUT%\FFXProceduralMouth-test.lib"
if errorlevel 1 exit /b 1
"%OUT%\FFXProceduralMouth-test.exe"
if errorlevel 1 exit /b 1
echo Tests passed. Built "%OUT%\FFXProceduralMouth.dll".
