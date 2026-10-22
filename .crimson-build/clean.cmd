@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Remove the build folder
setlocal
cd /d "%~dp0.."

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
echo(==^> remove build
call :crimson_remove "build"
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:crimson_remove
if exist "%~1\*" (
	rmdir /s /q "%~1"
	exit /b
)
if exist "%~1" del /f /q "%~1"
exit /b 0
