$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $projectRoot "build"

. (Join-Path $projectRoot "tools\windows-toolchain.ps1")

function Build-Architecture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$VcVars,
        [Parameter(Mandatory = $true)][string]$ClangArchitecture
    )

    Import-CvmVcEnvironment -VcVars $VcVars
    $outputDirectory = Join-Path $buildRoot $Name
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

    $commonArguments = @(
        $ClangArchitecture,
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
        "-o", (Join-Path $outputDirectory "smoke_vm.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "$Name smoke_vm build failed with exit code $LASTEXITCODE"
    }

    & (Join-Path $outputDirectory "smoke_vm.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "$Name smoke_vm failed with exit code $LASTEXITCODE"
    }

    & clang++ `
        $ClangArchitecture `
        "-std=c++17" `
        "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH" `
        "-Wall" `
        "-Wextra" `
        "-Werror" `
        "-I", (Join-Path $projectRoot "include") `
        (Join-Path $projectRoot "src\compiler\cvmc.cpp") `
        "-o", (Join-Path $outputDirectory "cvmc.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "$Name cvmc build failed with exit code $LASTEXITCODE"
    }

    & clang @commonArguments `
        (Join-Path $projectRoot "src\runtime\runtime.c") `
        (Join-Path $projectRoot "src\runtime\native_windows.c") `
        (Join-Path $projectRoot "src\tools\cvmrun.c") `
        "-o", (Join-Path $outputDirectory "cvmrun.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "$Name cvmrun build failed with exit code $LASTEXITCODE"
    }

    $addBytecode = Join-Path $outputDirectory "add.cvm"
    & (Join-Path $outputDirectory "cvmc.exe") `
        (Join-Path $projectRoot "tests\add.c") `
        $addBytecode
    if ($LASTEXITCODE -ne 0) {
        throw "$Name cvmc smoke compile failed with exit code $LASTEXITCODE"
    }

    $result = & (Join-Path $outputDirectory "cvmrun.exe") `
        "--print-result" `
        $addBytecode
    if ($LASTEXITCODE -ne 0 -or $result.Trim() -ne "5") {
        throw "$Name compiler-to-VM smoke failed: expected 5, got '$result'"
    }

    Write-Host "$Name pipeline: tests/add.c -> add.cvm -> 5"
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

$targets = @(
    @{
        Name = "x86"
        VcVars = Find-CvmVcVars -ScriptName "vcvars32.bat"
        ClangArchitecture = "-m32"
    },
    @{
        Name = "x64"
        VcVars = Find-CvmVcVars -ScriptName "vcvars64.bat"
        ClangArchitecture = "-m64"
    }
)

foreach ($target in $targets) {
    Build-Architecture `
        -Name $target.Name `
        -VcVars $target.VcVars `
        -ClangArchitecture $target.ClangArchitecture
}

Write-Host "x86 and x64 build products are available under build\<architecture>"
