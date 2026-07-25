$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDirectory = Join-Path $projectRoot "build"
$vcvarsCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

$vcvars = $vcvarsCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1

if ($null -eq $vcvars) {
    throw "A Visual Studio 2022 C toolchain was not found."
}

# Import the developer-command-prompt environment into this process so Clang
# can find the Windows SDK headers and MSVC runtime libraries.
$environmentLines = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf("=")
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path "Env:$name" -Value $value
    }
}

if (-not (Test-Path -LiteralPath $buildDirectory)) {
    New-Item -ItemType Directory -Path $buildDirectory | Out-Null
}

$commonArguments = @(
    "-std=c11",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-I", (Join-Path $projectRoot "include")
)

& clang @commonArguments `
    (Join-Path $projectRoot "src\runtime\runtime.c") `
    (Join-Path $projectRoot "tests\smoke_vm.c") `
    "-o", (Join-Path $buildDirectory "smoke_vm.exe")

if ($LASTEXITCODE -ne 0) {
    throw "clang failed with exit code $LASTEXITCODE"
}

& (Join-Path $buildDirectory "smoke_vm.exe")
if ($LASTEXITCODE -ne 0) {
    throw "smoke_vm failed with exit code $LASTEXITCODE"
}

& clang++ `
    "-std=c++17" `
    "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" `
    "-Wall" `
    "-Wextra" `
    "-Werror" `
    "-I", (Join-Path $projectRoot "include") `
    (Join-Path $projectRoot "src\compiler\cvmc.cpp") `
    "-o", (Join-Path $buildDirectory "cvmc.exe")

if ($LASTEXITCODE -ne 0) {
    throw "cvmc build failed with exit code $LASTEXITCODE"
}

& clang @commonArguments `
    (Join-Path $projectRoot "src\runtime\runtime.c") `
    (Join-Path $projectRoot "src\tools\cvmrun.c") `
    "-o", (Join-Path $buildDirectory "cvmrun.exe")

if ($LASTEXITCODE -ne 0) {
    throw "cvmrun build failed with exit code $LASTEXITCODE"
}

$addBytecode = Join-Path $buildDirectory "add.cvm"
& (Join-Path $buildDirectory "cvmc.exe") `
    (Join-Path $projectRoot "tests\add.c") `
    $addBytecode
if ($LASTEXITCODE -ne 0) {
    throw "cvmc smoke compile failed with exit code $LASTEXITCODE"
}

$result = & (Join-Path $buildDirectory "cvmrun.exe") `
    "--print-result" `
    $addBytecode
if ($LASTEXITCODE -ne 0 -or $result.Trim() -ne "5") {
    throw "compiler-to-VM smoke failed: expected 5, got '$result'"
}

Write-Host "pipeline: tests/add.c -> add.cvm -> 5"
