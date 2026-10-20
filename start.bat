@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Retrac OGFN - external

REM ===========================================================================
REM  start.bat : configure + compile RetracExternal.exe puis le lance.
REM
REM  Usage :
REM     start.bat            -> build + lancement
REM     start.bat build      -> build seulement
REM     start.bat run        -> lancement seulement (aucun build)
REM     start.bat clean      -> supprime le dossier build/
REM
REM  Place ce fichier a la racine du repo, a cote du CMakeLists.txt.
REM  SeDebugPrivilege / OpenProcess sur Fortnite exigent les droits admin.
REM ===========================================================================

set "ROOT=%~dp0"
set "BUILD=%ROOT%build"
set "TARGET=RetracExternal"
set "OUT=%BUILD%\bin\%TARGET%.exe"
set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

REM --- auto-elevation admin --------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo   [info] demande des droits administrateur...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs -ArgumentList '%MODE%'"
    exit /b
)

cd /d "%ROOT%"

if /i "%MODE%"=="clean" goto :clean
if /i "%MODE%"=="run"   goto :run

REM --- localiser cmake -------------------------------------------------------
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "CMAKE="

where cmake >nul 2>&1 && set "CMAKE=cmake"

if not defined CMAKE (
    for %%P in (
        "%PF%\CMake\bin\cmake.exe"
        "%PF86%\CMake\bin\cmake.exe"
        "%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe"
    ) do if not defined CMAKE if exist "%%~P" set "CMAKE=%%~P"
)

if not defined CMAKE (
    set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    )
    if defined VSPATH (
        set "CANDIDATE=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if exist "!CANDIDATE!" set "CMAKE=!CANDIDATE!"
    )
)

if not defined CMAKE (
    echo.
    echo   [erreur] cmake introuvable.
    echo            Installe CMake, ou Visual Studio 2022 avec la charge "Developpement Desktop C++".
    goto :fail
)

REM --- configuration ---------------------------------------------------------
echo.
echo   [info] configuration CMake ^(x64, Visual Studio^)...
"%CMAKE%" -S "%ROOT%." -B "%BUILD%" -A x64
if errorlevel 1 goto :fail

REM --- compilation -----------------------------------------------------------
echo.
echo   [info] compilation de %TARGET% ^(Release^)...
"%CMAKE%" --build "%BUILD%" --config Release --target %TARGET% --parallel
if errorlevel 1 goto :fail

if not exist "%OUT%" (
    echo   [erreur] binaire introuvable apres compilation : %OUT%
    goto :fail
)

REM --- offsets.json a cote de l'exe si dispo dans le repo --------------------
for %%C in (
    "%ROOT%offsets.json"
    "%ROOT%assets\offsets.json"
    "%ROOT%src\cheat\offsets.json"
    "%ROOT%offsets.example.json"
) do (
    if exist "%%~C" if not exist "%BUILD%\bin\offsets.json" (
        copy /y "%%~C" "%BUILD%\bin\offsets.json" >nul
        echo   [info] offsets copie depuis %%~nxC
    )
)

if /i "%MODE%"=="build" goto :done

:run
if not exist "%OUT%" (
    echo   [erreur] %OUT% absent. Lance d'abord : start.bat build
    goto :fail
)

echo.
echo   [ok] lancement de %TARGET%.exe
echo        Inser = ESP, clic droit = aimbot, Y = bullet TP, F8 = quitter
start "" "%OUT%"
goto :done

:clean
echo   [info] suppression de %BUILD%...
if exist "%BUILD%" rmdir /s /q "%BUILD%"
echo   [ok] nettoye.
goto :done

:done
endlocal
exit /b 0

:fail
echo.
echo   Entree pour fermer...
pause >nul
endlocal
exit /b 1
