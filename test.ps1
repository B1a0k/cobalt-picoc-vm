$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $projectRoot "build"
. (Join-Path $projectRoot "tools\windows-toolchain.ps1")

& (Join-Path $projectRoot "build.ps1")
if ($LASTEXITCODE -ne 0) {
    throw "x86/x64 build and smoke verification failed"
}

$nativeCompiler = (Get-Command clang -ErrorAction Stop).Source
$targets = @(
    @{
        Name = "x86"
        VcVars = Find-CvmVcVars -ScriptName "vcvars32.bat"
    },
    @{
        Name = "x64"
        VcVars = Find-CvmVcVars -ScriptName "vcvars64.bat"
    }
)

foreach ($target in $targets) {
    $name = $target.Name
    $buildDirectory = Join-Path $buildRoot $name
    Import-CvmVcEnvironment -VcVars $target.VcVars

    & python `
        (Join-Path $projectRoot "tools\run_extended_tests.py") `
        "--build" `
        $buildDirectory `
        "--target" `
        $name `
        "--native-compiler" `
        $nativeCompiler `
        "--report" `
        (Join-Path $buildDirectory "extended-test-report.json")
    if ($LASTEXITCODE -ne 0) {
        throw "$name secondary verification failed"
    }

    & python `
        (Join-Path $projectRoot "tools\run_picoc_tests.py") `
        "--build" `
        $buildDirectory `
        "--target" `
        $name `
        "--report" `
        (Join-Path $buildDirectory "picoc-test-report.json")
    if ($LASTEXITCODE -ne 0) {
        throw "$name upstream PicoC regression failed"
    }

    & (Join-Path $projectRoot "tools\run_ffi_tests.ps1") `
        -Architecture $name
}

& python `
    (Join-Path $projectRoot "tools\run_architecture_tests.py") `
    "--build-root" `
    $buildRoot `
    "--report" `
    (Join-Path $buildRoot "architecture-test-report.json")
if ($LASTEXITCODE -ne 0) {
    throw "cross-architecture verification failed"
}

Write-Host (
    "all x86/x64 compiler, VM, layout, isolation, extended, " +
    "differential, and upstream checks passed"
)
