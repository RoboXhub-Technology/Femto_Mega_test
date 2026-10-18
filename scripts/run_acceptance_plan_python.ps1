param(
    [string]$PythonExe = "python",
    [string]$ScriptPath = (Join-Path $PSScriptRoot "..\python\femto_mega_acceptance.py"),
    [string]$EvidenceRoot = "evidence-python",
    [int]$SecondsPerMode = 60,
    [int]$DeviceIndex = 0,
    [string]$IpAddress = "",
    [int]$Port = 8090
)

$ErrorActionPreference = "Continue"
$Script = (Resolve-Path -LiteralPath $ScriptPath).Path
$Root = [System.IO.Path]::GetFullPath($EvidenceRoot)
New-Item -ItemType Directory -Force -Path $Root | Out-Null

$ConnectionArgs = if($IpAddress) {
    @("--ip", $IpAddress, "--port", "$Port")
} else {
    @("--device-index", "$DeviceIndex")
}

function Invoke-PythonTest {
    param([string]$Name, [string[]]$Arguments)
    $Out = Join-Path $Root $Name
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    Write-Host "Running $Name"
    & $PythonExe $Script @Arguments @ConnectionArgs --out $Out
    if($LASTEXITCODE -ne 0) {
        "exit_code=$LASTEXITCODE" | Set-Content -LiteralPath (Join-Path $Out "FAILED.txt") -Encoding UTF8
    }
}

Invoke-PythonTest "00-survey" @("survey")

$DepthModes = @(
    @("WFOV-unbinned-1024x1024-5", 1024, 1024, 5),
    @("WFOV-unbinned-1024x1024-15", 1024, 1024, 15),
    @("WFOV-binned-512x512-5", 512, 512, 5),
    @("WFOV-binned-512x512-15", 512, 512, 15),
    @("WFOV-binned-512x512-25", 512, 512, 25),
    @("WFOV-binned-512x512-30", 512, 512, 30),
    @("NFOV-unbinned-640x576-5", 640, 576, 5),
    @("NFOV-unbinned-640x576-15", 640, 576, 15),
    @("NFOV-unbinned-640x576-25", 640, 576, 25),
    @("NFOV-unbinned-640x576-30", 640, 576, 30),
    @("NFOV-binned-320x288-5", 320, 288, 5),
    @("NFOV-binned-320x288-15", 320, 288, 15),
    @("NFOV-binned-320x288-25", 320, 288, 25),
    @("NFOV-binned-320x288-30", 320, 288, 30)
)

$Index = 1
foreach($Mode in $DepthModes) {
    $Prefix = "{0:D2}" -f $Index
    Invoke-PythonTest "$Prefix-depth-$($Mode[0])" @(
        "stream", "--sensor", "depth", "--width", "$($Mode[1])", "--height", "$($Mode[2])",
        "--fps", "$($Mode[3])", "--seconds", "$SecondsPerMode", "--roi", "100", "--valid-ratio", "0.95"
    )
    $Index++
}

Invoke-PythonTest "20-color-3840x2160-30" @(
    "stream", "--sensor", "color", "--width", "3840", "--height", "2160", "--fps", "30",
    "--seconds", "$SecondsPerMode", "--save-first-frame"
)
Invoke-PythonTest "21-imu" @("imu", "--seconds", "$SecondsPerMode")
Invoke-PythonTest "22-hdr-query" @("hdr", "--set", "query")

Write-Host "Python acceptance plan finished: $Root"
