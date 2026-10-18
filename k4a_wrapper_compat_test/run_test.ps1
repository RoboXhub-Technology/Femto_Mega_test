param(
    [switch]$Full,
    [ValidateRange(1, 100)]
    [int]$Frames = 5,
    [ValidateRange(0, 31)]
    [int]$Device = 0,
    [string]$K4AWrapperRoot = '',
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $cmake) {
    throw 'cmake.exe was not found. Install CMake and the Visual Studio Build Tools C++ workload first.'
}

Push-Location $projectRoot
try {
    $configureArgs = @('--preset', 'vs2022-x64')
    if ($K4AWrapperRoot) {
        $resolvedWrapperRoot = (Resolve-Path -LiteralPath $K4AWrapperRoot).Path
        $configureArgs += "-DK4A_WRAPPER_ROOT=$resolvedWrapperRoot"
    }

    & $cmake.Source @configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    & $cmake.Source --build --preset release
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    $exe = Join-Path $projectRoot 'build\Release\k4a_wrapper_compat_test.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "Test executable was not found: $exe"
    }

    $mode = if ($Full) { '--full' } else { '--quick' }
    $resultName = if ($Full) { 'k4a_wrapper_full.json' } else { 'k4a_wrapper_quick.json' }
    $result = if ($Output) {
        [System.IO.Path]::GetFullPath((Join-Path $projectRoot $Output))
    } else {
        Join-Path $projectRoot "results\current\$resultName"
    }

    $resultDirectory = Split-Path -Parent $result
    New-Item -ItemType Directory -Path $resultDirectory -Force | Out-Null

    & $exe $mode --device $Device --frames $Frames --output $result
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
