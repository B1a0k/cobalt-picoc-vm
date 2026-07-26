param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x86", "x64")]
    [string]$Architecture
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$compiler = Join-Path $projectRoot "build\$Architecture\cvmc.exe"
$includeRoot = Join-Path $projectRoot (
    "third_party\beacon-interpreter\interpreter-includes")
$officialDemo = Join-Path $projectRoot (
    "third_party\beacon-interpreter\interpreter_demo.c")
$businessRoot = Join-Path $projectRoot "tests\business"
$outputRoot = Join-Path $projectRoot "build\$Architecture\business"

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$cases = @(Get-Item -LiteralPath $officialDemo) + @(
    Get-ChildItem -LiteralPath $businessRoot -Filter "*.c" | Sort-Object Name
)

foreach ($source in $cases) {
    $first = Join-Path $outputRoot "$($source.BaseName)-a.cvm"
    $second = Join-Path $outputRoot "$($source.BaseName)-b.cvm"
    foreach ($output in @($first, $second)) {
        & $compiler --target $Architecture --profile beacon `
            -I $includeRoot $source.FullName $output
        if ($LASTEXITCODE -ne 0) {
            throw "$Architecture failed to compile $($source.Name)"
        }
    }
    $firstHash = (Get-FileHash -LiteralPath $first -Algorithm SHA256).Hash
    $secondHash = (Get-FileHash -LiteralPath $second -Algorithm SHA256).Hash
    if ($firstHash -ne $secondHash) {
        throw "$Architecture package is not deterministic for $($source.Name)"
    }
}

Write-Host (
    "$Architecture official/business corpus: $($cases.Count) scripts compiled " +
    "twice with deterministic bytecode")
