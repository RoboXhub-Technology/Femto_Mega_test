param(
    [Parameter(Mandatory = $true)]
    [string]$OrbbecSdkRoot,
    [string]$BuildDir = "build",
    [switch]$BuildK4A,
    [string]$K4AWrapperRoot = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = Join-Path $ProjectRoot $BuildDir
$SdkPath = (Resolve-Path -LiteralPath $OrbbecSdkRoot).Path
New-Item -ItemType Directory -Force -Path $BuildPath | Out-Null

$ConfigureArgs = @(
    "-S", $ProjectRoot,
    "-B", $BuildPath,
    "-DORBBEC_SDK_ROOT=$SdkPath"
)

if($BuildK4A) {
    if([string]::IsNullOrWhiteSpace($K4AWrapperRoot)) {
        throw "-BuildK4A requires -K4AWrapperRoot"
    }
    $K4APath = (Resolve-Path -LiteralPath $K4AWrapperRoot).Path
    $ConfigureArgs += "-DBUILD_K4A_WRAPPER_SMOKE=ON"
    $ConfigureArgs += "-DK4A_WRAPPER_ROOT=$K4APath"
}

cmake @ConfigureArgs
cmake --build $BuildPath --config Release

Write-Host "Build complete: $BuildPath"
