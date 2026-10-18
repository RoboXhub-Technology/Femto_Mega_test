param(
    [Parameter(Mandatory = $true)]
    [string]$Candidate,
    [string]$Reference = '',
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Reference) {
    $Reference = Join-Path $projectRoot 'results\reference_CL3N241005X_2026-10-18\k4a_wrapper_full.json'
}

$referencePath = (Resolve-Path -LiteralPath $Reference).Path
$candidatePath = (Resolve-Path -LiteralPath $Candidate).Path
$referenceData = Get-Content -LiteralPath $referencePath -Raw | ConvertFrom-Json
$candidateData = Get-Content -LiteralPath $candidatePath -Raw | ConvertFrom-Json

$referenceRequired = @{}
foreach ($test in $referenceData.tests) {
    if ($test.required) { $referenceRequired[$test.name] = $test.status }
}

$candidateRequired = @{}
foreach ($test in $candidateData.tests) {
    if ($test.required) { $candidateRequired[$test.name] = $test.status }
}

$rows = foreach ($name in ($referenceRequired.Keys | Sort-Object)) {
    $candidateStatus = if ($candidateRequired.ContainsKey($name)) { $candidateRequired[$name] } else { 'MISSING' }
    [pscustomobject]@{
        test = $name
        reference_status = $referenceRequired[$name]
        candidate_status = $candidateStatus
        match = $referenceRequired[$name] -eq $candidateStatus
    }
}

$extra = @($candidateRequired.Keys | Where-Object { -not $referenceRequired.ContainsKey($_) } | Sort-Object)
$mismatches = @($rows | Where-Object { -not $_.match })
$comparison = [ordered]@{
    reference_file = $referencePath
    candidate_file = $candidatePath
    reference_serial = $referenceData.serial_number
    candidate_serial = $candidateData.serial_number
    reference_firmware = $referenceData.firmware_version
    candidate_firmware = $candidateData.firmware_version
    reference_overall_pass = [bool]$referenceData.overall_pass
    candidate_overall_pass = [bool]$candidateData.overall_pass
    required_test_count = $rows.Count
    matching_required_test_count = @($rows | Where-Object { $_.match }).Count
    all_reference_required_statuses_match = $mismatches.Count -eq 0
    extra_candidate_required_tests = $extra
    rows = @($rows)
}

Write-Host "Reference serial: $($comparison.reference_serial)"
Write-Host "Candidate serial: $($comparison.candidate_serial)"
Write-Host "Required status matches: $($comparison.matching_required_test_count)/$($comparison.required_test_count)"
if ($mismatches.Count -gt 0) {
    $mismatches | Format-Table -AutoSize
}
if ($extra.Count -gt 0) {
    Write-Host "Extra candidate tests: $($extra -join ', ')"
}

if ($Output) {
    $outputPath = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $Output))
    New-Item -ItemType Directory -Path (Split-Path -Parent $outputPath) -Force | Out-Null
    $comparison | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding UTF8
    Write-Host "Comparison JSON: $outputPath"
}

if (-not $candidateData.overall_pass -or $mismatches.Count -gt 0) { exit 1 }
exit 0
