$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

& (Join-Path $projectRoot "build.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "build and smoke verification failed"
}

$nativeCompiler = (Get-Command clang -ErrorAction Stop).Source

& python `
    (Join-Path $projectRoot "tools\run_extended_tests.py") `
    "--native-compiler" `
    $nativeCompiler
if ($LASTEXITCODE -ne 0) {
    throw "secondary verification failed"
}

& python (Join-Path $projectRoot "tools\run_picoc_tests.py")
if ($LASTEXITCODE -ne 0) {
    throw "upstream PicoC regression failed"
}

Write-Host "all compiler, VM, extended, differential, and upstream checks passed"
