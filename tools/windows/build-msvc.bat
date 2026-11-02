@echo off
rem SPDX-License-Identifier: BSD-2-Clause
rem Copyright (c) 2026 Mario Trombino (Tensor7, LICENSE-Tensor7)
rem
rem Tensor7 per Windows con MSVC (x64), con i makefile nmake di 7-Zip:
rem   7z.dll (codec e formati), 7z.exe (riga di comando), 7zFM.exe (file manager),
rem   7zG.exe (finestre di compressione/estrazione), 7-zip.dll (menu di Explorer).
rem
rem   tools\windows\build-msvc.bat [cartella_di_uscita]     (default: out-msvc)
rem
rem Richiede Visual Studio 2022 o successivo con "Sviluppo di applicazioni desktop C++".

setlocal enabledelayedexpansion
set "ROOT=%~dp0..\.."
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%ROOT%\out-msvc"

if not defined VCINSTALLDIR (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
  if not defined VSDIR (
    echo Visual Studio con gli strumenti C++ non trovato
    exit /b 1
  )
  call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
for %%d in (Bundles\Format7zF UI\Console UI\FileManager UI\GUI UI\Explorer) do (
  echo === %%d
  pushd "%ROOT%\CPP\7zip\%%d" || exit /b 1
  nmake /nologo PLATFORM=x64 || (popd & exit /b 1)
  copy /y x64\*.exe "%OUT%" >nul 2>nul
  copy /y x64\*.dll "%OUT%" >nul 2>nul
  popd
)
echo Tensor7 per Windows in %OUT%
dir /b "%OUT%"
exit /b 0
