if ($null -eq $script:CvmBaseEnvironment) {
    $script:CvmBaseEnvironment = @{}
    Get-ChildItem Env: | ForEach-Object {
        $script:CvmBaseEnvironment[$_.Name] = $_.Value
    }
}

function Reset-CvmBuildEnvironment {
    Get-ChildItem Env: | ForEach-Object {
        if (-not $script:CvmBaseEnvironment.ContainsKey($_.Name)) {
            Remove-Item -LiteralPath "Env:$($_.Name)"
        }
    }
    foreach ($entry in $script:CvmBaseEnvironment.GetEnumerator()) {
        Set-Item -Path "Env:$($entry.Key)" -Value $entry.Value
    }
}

function Find-CvmVcVars {
    param([Parameter(Mandatory = $true)][string]$ScriptName)

    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\$ScriptName",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\$ScriptName",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\$ScriptName",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\$ScriptName"
    )
    $result = $candidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if ($null -eq $result) {
        throw "Visual Studio 2022 $ScriptName was not found."
    }
    return $result
}

function Import-CvmVcEnvironment {
    param([Parameter(Mandatory = $true)][string]$VcVars)

    Reset-CvmBuildEnvironment
    $environmentLines = & cmd.exe /d /s /c "`"$VcVars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to import $VcVars"
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf("=")
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            Set-Item -Path "Env:$name" -Value $value
        }
    }
}
