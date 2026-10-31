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
echo(==^> glib-compile-resources --sourcedir assets --target build/resources.c --generate-source assets/crimson-pixel.gresource.xml
glib-compile-resources --sourcedir assets --target build/resources.c --generate-source assets/crimson-pixel.gresource.xml
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/*.c
set "CRIMSON_EACH_FROM=src/*.c"
set "CRIMSON_EACH_TO=build/obj/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $INCLUDES $(pkg-config --cflags $PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=src/*.h src/lang/*.h subprojects/crimson-core/src/common/*.h subprojects/crimson-build/src/build/*.h"
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> each subprojects/crimson-core/src/common/*.c
set "CRIMSON_EACH_FROM=subprojects/crimson-core/src/common/*.c"
set "CRIMSON_EACH_TO=build/obj/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $INCLUDES $(pkg-config --cflags $PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=src/*.h src/lang/*.h subprojects/crimson-core/src/common/*.h subprojects/crimson-build/src/build/*.h"
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
powershell -NoProfile -ExecutionPolicy Bypass -Command "$text = [IO.File]::ReadAllText('%~f0'); Invoke-Expression $text.Substring($text.LastIndexOf('<# crimson_each #>'))"
exit /b

<# crimson_each #>
# Runs one each step for crimson_each above, which cmd cannot do by itself
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path

function Get-Matches([string] $patterns) {
	foreach ($pattern in $patterns.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
		foreach ($file in Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue) {
			(Resolve-Path -LiteralPath $file.FullName -Relative) -replace '^\.[\\/]', '' -replace '\\', '/'
		}
	}
}

function Get-FullPath([string] $path) {
	return [IO.Path]::Combine($root, $path)
}

$template = $env:CRIMSON_EACH_TO
$folder = Split-Path -Parent ($template.Substring(0, $template.IndexOf('{id}')) + 'x')
if (-not $folder) { $folder = '.' }
$md5 = [Security.Cryptography.MD5]::Create()
$hash = $md5.ComputeHash([Text.Encoding]::UTF8.GetBytes("$env:CRIMSON_EACH_FROM|$template"))
$list = Get-FullPath "$folder/.crimson-each-$([BitConverter]::ToString($hash, 0, 4).Replace('-', ''))"
[void][IO.Directory]::CreateDirectory((Get-FullPath $folder))
$previous = @()
if (Test-Path -LiteralPath $list) { $previous = @([IO.File]::ReadAllLines($list)) }

# A changed command or variable makes every earlier result stale
$settings = $env:CRIMSON_EACH_RUN
$oldSettings = $null
if (Test-Path -LiteralPath "$list.run") { $oldSettings = [IO.File]::ReadAllText("$list.run") }
if ($oldSettings -ne $settings) {
	foreach ($old in $previous) { Remove-Item -LiteralPath (Get-FullPath $old) -Force -ErrorAction SilentlyContinue }
	[IO.File]::WriteAllText("$list.run", $settings)
}

$newestExtra = [DateTime]::MinValue
foreach ($extra in Get-Matches $env:CRIMSON_EACH_ALSO) {
	$time = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $extra))
	if ($time -gt $newestExtra) { $newestExtra = $time }
}

$outputs = @()
$work = @()
foreach ($source in @(Get-Matches $env:CRIMSON_EACH_FROM | Sort-Object -Unique)) {
	$base = $source
	if ($source.LastIndexOf('.') -ge 0) { $base = $source.Substring(0, $source.LastIndexOf('.')) }
	$to = $template.Replace('{id}', $base.Replace('/', '_').Replace('.', '_')).Replace('{name}', (Split-Path -Leaf $base))
	$outputs += $to
	$newest = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $source))
	if ($newestExtra -gt $newest) { $newest = $newestExtra }
	$target = Get-FullPath $to
	if ((Test-Path -LiteralPath $target) -and [IO.File]::GetLastWriteTimeUtc($target) -ge $newest) { continue }
	$command = $settings.Replace('{from}', $source.Replace('/', '\')).Replace('{to}', $to.Replace('/', '\'))
	$work += [pscustomobject]@{ From = $source; To = $to; Command = $command }
}

# Results of files that are gone would still be picked up by the steps after this one
foreach ($old in $previous) {
	if ($outputs -notcontains $old) { Remove-Item -LiteralPath (Get-FullPath $old) -Force -ErrorAction SilentlyContinue }
}
[IO.File]::WriteAllLines($list, [string[]]$outputs)

# Runs as many commands at once as there are processors, and starts no new one after a failure
$running = New-Object 'Collections.Generic.List[Diagnostics.Process]'
$hasFailed = $false
foreach ($item in $work) {
	while ($running.Count -ge [Environment]::ProcessorCount) {
		$running[0].WaitForExit()
		if ($running[0].ExitCode -ne 0) { $hasFailed = $true }
		$running.RemoveAt(0)
	}
	if ($hasFailed) { break }
	[void][IO.Directory]::CreateDirectory((Split-Path -Parent (Get-FullPath $item.To)))
	Write-Host "==> $($item.From)"
	$start = New-Object Diagnostics.ProcessStartInfo $env:ComSpec, "/d /s /c `"$($item.Command)`""
	$start.UseShellExecute = $false
	$start.WorkingDirectory = $root
	$running.Add([Diagnostics.Process]::Start($start))
}
foreach ($process in $running) {
	$process.WaitForExit()
	if ($process.ExitCode -ne 0) { $hasFailed = $true }
}
if ($hasFailed) { exit 1 }
exit 0
