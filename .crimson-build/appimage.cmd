@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Pack Crimson Pixel into an AppImage in dist
setlocal
cd /d "%~dp0.."

call "%~dp0build.cmd"
if errorlevel 1 exit /b %errorlevel%
set "PROGRAM=crimson-pixel"
set "APPLICATION_ID=io.github.CrimsonOrg.CrimsonPixel"
set "VERSION=0.1.0"
set "PREFIX=%HOME%/.local"
set "STD=c11"
set "WARN=-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
set "OPT=-O2 -g"
set "PKGS=gtk4 libadwaita-1 libpanel-1 zlib"
set "LIBS=-lm"
set "DEFINES=-DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src"
echo(==^> needs quick-sharun
call :crimson_needs "quick-sharun"
if errorlevel 1 exit /b %errorlevel%
echo(==^> sh subprojects/crimson-core/scripts/appimage.sh
sh subprojects/crimson-core/scripts/appimage.sh
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:crimson_needs
where "%~1" >nul 2>&1 || (echo %~1 is needed, install it first & exit /b 1)
exit /b 0
