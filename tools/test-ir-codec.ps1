<#
.SYNOPSIS
    Builds and runs the host unit tests for src/app/app_09/ir_flipper_codec.cpp,
    src/app/app_09/ir_raw_tools.cpp and src/app/app_11/ac_store.cpp.

.DESCRIPTION
    Both modules are pure C++17 with no Arduino dependency, so they are compiled with
    the MSVC toolchain found through vswhere (Visual Studio 2022 Build Tools or
    any edition with the C++ workload) and executed on the PC. Nothing is
    flashed and PlatformIO is not involved.

    Exit code 0 = every assertion passed. Non-zero = compile failure or at
    least one failing assertion; the test binary prints each failure.

.PARAMETER Compiler
    Optional path to an alternative compiler driver (g++/clang++). When given,
    GCC-style flags are used instead of MSVC.
#>
[CmdletBinding()]
param(
    [string]$Compiler = ''
)

$ErrorActionPreference = 'Stop'
$firmwareRoot = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $firmwareRoot 'src\app\app_09\ir_flipper_codec.cpp'
$raw  = Join-Path $firmwareRoot 'src\app\app_09\ir_raw_tools.cpp'
$acs  = Join-Path $firmwareRoot 'src\app\app_11\ac_store.cpp'
$test = Join-Path $firmwareRoot 'test\ir_codec\test_ir_codec.cpp'
$out  = Join-Path $firmwareRoot '.pio\host-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$exe  = Join-Path $out 'test_ir_codec.exe'

foreach ($f in @($src, $raw, $acs, $test)) {
    if (-not (Test-Path $f)) { throw "Missing source file: $f" }
}

if ($Compiler) {
    & $Compiler -std=c++17 -Wall -Wextra -O1 -I (Join-Path $firmwareRoot 'src\app\app_09') -I (Join-Path $firmwareRoot 'src\app\app_11') $src $raw $acs $test -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Compile failed ($Compiler)" }
} else {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio Build Tools with the C++ workload, or pass -Compiler.' }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'No Visual Studio installation with the C++ toolset was found.' }
    $vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsRoot" }

    $inc   = Join-Path $firmwareRoot 'src\app\app_09'
    $incAc = Join-Path $firmwareRoot 'src\app\app_11'
    $cmd = "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++17 /EHsc /W4 /O1 /I`"$inc`" /I`"$incAc`" /Fo`"$out\\`" /Fe`"$exe`" `"$src`" `"$raw`" `"$acs`" `"$test`""
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'Compile failed (MSVC)' }
}

Write-Host "Running $exe"
& $exe
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Host "ir_codec tests FAILED (exit $rc)" -ForegroundColor Red }
else           { Write-Host 'ir_codec tests passed' -ForegroundColor Green }
exit $rc
