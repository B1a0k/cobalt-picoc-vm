param(
    [Parameter(Mandatory = $true)][ValidateSet("x86", "x64")]
    [string]$Architecture
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$buildDirectory = Join-Path $projectRoot "build\$Architecture"
$compiler = Join-Path $buildDirectory "cvmc.exe"
$runner = Join-Path $buildDirectory "cvmrun.exe"
$includeDirectory = Join-Path $projectRoot "third_party\beacon-interpreter\interpreter-includes"
$testDirectory = Join-Path $projectRoot "tests\ffi"

if (!(Test-Path -LiteralPath $compiler) -or !(Test-Path -LiteralPath $runner)) {
    throw "build products are missing for $Architecture"
}
if (!(Test-Path -LiteralPath $includeDirectory)) {
    throw "official interpreter headers are missing; run git submodule update --init"
}

function Compile-Test {
    param([string]$Name)
    $source = Join-Path $testDirectory "$Name.c"
    $package = Join-Path $buildDirectory "$Name.cvm"
    & $compiler --target $Architecture -I $includeDirectory $source $package
    if ($LASTEXITCODE -ne 0) {
        throw "$Architecture failed to compile $Name"
    }
    return $package
}

Compile-Test "dfr_beacon" | Out-Null
Compile-Test "dfr_variadic" | Out-Null
Compile-Test "official_beacon_header" | Out-Null

$loaderPackage = Compile-Test "official_windows_loader"
$loaderResult = & $runner --print-result $loaderPackage
if ($LASTEXITCODE -ne 0 -or $loaderResult.Trim() -ne "1") {
    throw "$Architecture loader result mismatch: '$loaderResult'"
}

$pointerPackage = Compile-Test "native_function_pointer"
$pointerResult = & $runner --print-result $pointerPackage
if ($LASTEXITCODE -ne 0 -or $pointerResult.Trim() -ne "5") {
    throw "$Architecture function pointer result mismatch: '$pointerResult'"
}

$i64Package = Compile-Test "native_i64"
$i64Result = & $runner --print-result $i64Package
if ($LASTEXITCODE -ne 0 -or $i64Result.Trim() -ne "9") {
    throw "$Architecture i64 native result mismatch: '$i64Result'"
}

$negativeSource = Join-Path $testDirectory "dfr_stdcall_varargs.c"
$negativeOutput = Join-Path $buildDirectory "dfr_stdcall_varargs.cvm"
$oldPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$diagnostic = (& $compiler --target $Architecture $negativeSource $negativeOutput 2>&1) -join "`n"
$negativeExit = $LASTEXITCODE
$ErrorActionPreference = $oldPreference
if ($negativeExit -eq 0 -or
    $diagnostic -notmatch "stdcall native function cannot be variadic") {
    throw "$Architecture stdcall varargs rejection mismatch: $diagnostic"
}

Write-Host "$Architecture FFI: DFR, official headers, loader APIs, typed indirect calls, and i64 ABI passed"
