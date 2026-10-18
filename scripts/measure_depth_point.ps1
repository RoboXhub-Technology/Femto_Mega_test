param(
    [Parameter(Mandatory = $true)] [string]$ExePath,
    [Parameter(Mandatory = $true)] [string]$ModeName,
    [Parameter(Mandatory = $true)] [int]$Width,
    [Parameter(Mandatory = $true)] [int]$Height,
    [Parameter(Mandatory = $true)] [int]$Fps,
    [Parameter(Mandatory = $true)] [int]$DistanceMm,
    [string]$EvidenceRoot = "evidence",
    [int]$Frames = 100,
    [int]$DeviceIndex = 0
)

$ErrorActionPreference = "Stop"
$Exe = (Resolve-Path -LiteralPath $ExePath).Path
$Root = [System.IO.Path]::GetFullPath($EvidenceRoot)
$SafeMode = $ModeName -replace '[^A-Za-z0-9_-]', '_'
$Out = Join-Path $Root ("03-range-{0}-{1}mm" -f $SafeMode, $DistanceMm)
New-Item -ItemType Directory -Force -Path $Out | Out-Null

& $Exe stream --sensor depth --width $Width --height $Height --fps $Fps `
    --frames $Frames --seconds 0 --roi 100 --valid-ratio 0.95 --reference-mm $DistanceMm `
    --device-index $DeviceIndex --out $Out

if($LASTEXITCODE -ne 0) {
    throw "Depth point measurement failed with exit code $LASTEXITCODE"
}
Write-Host "Measurement complete: $Out"
