@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Build Crimson Pixel into the project folder
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
echo(==^> needs cc
call :crimson_needs "cc"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs make
call :crimson_needs "make"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs pkg-config
call :crimson_needs "pkg-config"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs glib-compile-resources
call :crimson_needs "glib-compile-resources"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg gtk4 ^>= 4.14
call :crimson_pkg "gtk4 >= 4.14"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg libadwaita-1 ^>= 1.6
call :crimson_pkg "libadwaita-1 >= 1.6"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg libpanel-1
call :crimson_pkg "libpanel-1"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg zlib
call :crimson_pkg "zlib"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir build/obj
call :crimson_mkdir "build\obj"
if errorlevel 1 exit /b %errorlevel%
echo(==^> glib-compile-resources --sourcedir data --target build/resources.c --generate-source data/crimson-pixel.gresource.xml
glib-compile-resources --sourcedir data --target build/resources.c --generate-source data/crimson-pixel.gresource.xml
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/*.c
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> each subprojects/crimson-core/src/common/*.c
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -std=gnu11 -O2 -g $^(pkg-config --cflags gio-2.0^) -c build/resources.c -o build/obj/resources.o
cc -std=gnu11 -O2 -g $(pkg-config --cflags gio-2.0) -c build/resources.c -o build/obj/resources.o
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -o build/%PROGRAM% build/obj/*.o $^(pkg-config --libs $PKGS^) $LIBS
cc -o build/%PROGRAM% build/obj/*.o $(pkg-config --libs $PKGS) $LIBS
if errorlevel 1 exit /b %errorlevel%
echo(==^> copy build/%PROGRAM% to .%PROGRAM%.new
call :crimson_copy "build\%PROGRAM%" ".%PROGRAM%.new"
if errorlevel 1 exit /b %errorlevel%
echo(==^> move .%PROGRAM%.new to %PROGRAM%
call :crimson_move ".%PROGRAM%.new" "%PROGRAM%"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir logs
call :crimson_mkdir "logs"
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:crimson_copy
if exist "%~1\*" (
	xcopy "%~1" "%~2\" /e /i /y /q >nul
	exit /b
)
if not exist "%~dp2" mkdir "%~dp2"
copy /y "%~1" "%~2" >nul
exit /b

:crimson_move
if not exist "%~dp2" mkdir "%~dp2"
move /y "%~1" "%~2" >nul
exit /b

:crimson_mkdir
if not exist "%~1" mkdir "%~1"
exit /b

:crimson_needs
where "%~1" >nul 2>&1 || (echo %~1 is needed, install it first & exit /b 1)
exit /b 0

:crimson_pkg
pkg-config --exists "%~1" || (echo The development files for %~1 are missing & exit /b 1)
exit /b 0

:crimson_each
echo Building each file needs a Unix shell with make
exit /b 1
