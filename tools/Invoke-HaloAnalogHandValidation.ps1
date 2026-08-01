[CmdletBinding()]
param(
    [string]$OperatorPackageRoot = (
        'E:\Github\UEVRMetaXROperator\dist\release\' +
        'UEVR-Meta-XR-Operator-205.1-nightly-01139-analog-hands-v1'),

    [string]$OutputDirectory = '',

    [ValidateSet('left', 'right')]
    [string]$Hand = 'left',

    [ValidateRange(100, 3000)]
    [int]$SettleMilliseconds = 650,

    [switch]$SkipImages
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path -Parent $PSScriptRoot) (
        'artifacts\validation\analog-hands-live')
}

$helper = Join-Path $OperatorPackageRoot 'Invoke-MetaXROperatorTool.ps1'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
    throw "Meta XR Operator helper was not found: $helper"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$samples = @(0.0, 0.25, 0.5, 0.75, 1.0)
$opposite = if ($Hand -eq 'left') { 'right' } else { 'left' }
$results = [System.Collections.Generic.List[object]]::new()

function Invoke-OperatorTool {
    param(
        [Parameter(Mandatory)]
        [string]$Name,
        [hashtable]$Arguments = @{},
        [string]$OutputImage = ''
    )

    $json = $Arguments | ConvertTo-Json -Depth 10 -Compress
    if ($OutputImage) {
        & $helper -ToolName $Name -ArgumentsJson $json `
            -OutputImage $OutputImage | Out-Null
        return $null
    }
    $response = & $helper -ToolName $Name -ArgumentsJson $json
    if ($response.result.isError) {
        throw "Operator tool $Name failed: $($response.result.content[0].text)"
    }
    return $response
}

function Set-ControllerInput {
    param(
        [Parameter(Mandatory)]
        [string]$Component,
        [Parameter(Mandatory)]
        [double]$Value
    )
    [void](Invoke-OperatorTool -Name 'openxr_set_controller_input' `
        -Arguments @{
            hand = $Hand
            component = $Component
            value = $Value
            auto_release = $false
        })
}

function Get-HaloDiagnostics {
    $response = Invoke-OperatorTool -Name 'UEVR_Status'
    $status = $response.result.content[0].text | ConvertFrom-Json
    if (-not $status.halo_motion_controls.loaded -or
        -not $status.halo_motion_controls.pose_diagnostics.available) {
        throw 'Halo motion-control pose diagnostics are not live.'
    }
    return $status.halo_motion_controls.pose_diagnostics
}

function Assert-Near {
    param(
        [double]$Actual,
        [double]$Expected,
        [double]$Tolerance,
        [string]$Message
    )
    if ([math]::Abs($Actual - $Expected) -gt $Tolerance) {
        throw "$Message (actual=$Actual expected=$Expected tolerance=$Tolerance)"
    }
}

try {
    Set-ControllerInput -Component Grip -Value 0
    Set-ControllerInput -Component Trigger -Value 0
    Start-Sleep -Milliseconds $SettleMilliseconds

    foreach ($component in @('Grip', 'Trigger')) {
        Set-ControllerInput -Component Grip -Value 0
        Set-ControllerInput -Component Trigger -Value 0
        Start-Sleep -Milliseconds $SettleMilliseconds

        foreach ($value in $samples) {
            Set-ControllerInput -Component $component -Value $value
            Start-Sleep -Milliseconds $SettleMilliseconds
            $diagnostics = Get-HaloDiagnostics

            $metric = if ($component -eq 'Grip') {
                "${Hand}_grip_curl"
            } else {
                "${Hand}_index_curl"
            }
            $oppositeMetric = if ($component -eq 'Grip') {
                "${opposite}_grip_curl"
            } else {
                "${opposite}_index_curl"
            }
            $actual = [double]$diagnostics.$metric
            $oppositeActual = [double]$diagnostics.$oppositeMetric
            Assert-Near $actual $value 0.035 `
                "$Hand $component did not preserve the requested analog value"
            Assert-Near $oppositeActual 0.0 0.035 `
                "$Hand $component changed the opposite hand"

            if ($component -eq 'Grip') {
                Assert-Near ([double]$diagnostics."${Hand}_thumb_curl") `
                    $value 0.035 'Grip did not curl the thumb proportionally'
                Assert-Near ([double]$diagnostics."${Hand}_index_curl") `
                    0.0 0.035 'Grip incorrectly changed the index finger'
            } else {
                Assert-Near ([double]$diagnostics."${Hand}_grip_curl") `
                    0.0 0.035 'Trigger incorrectly changed the grip fingers'
            }

            $imagePath = $null
            if ($component -eq 'Grip' -and -not $SkipImages) {
                $percent = [int][math]::Round($value * 100)
                $imagePath = Join-Path $OutputDirectory (
                    "${Hand}-grip-{0:D3}.png" -f $percent)
                Invoke-OperatorTool -Name 'openxr_capture_composited_image' `
                    -Arguments @{ eye = 'left' } -OutputImage $imagePath
            }

            $results.Add([pscustomobject]@{
                Hand = $Hand
                Component = $component
                Requested = $value
                Observed = $actual
                OppositeObserved = $oppositeActual
                IndexTipDistanceBlam = [double](
                    $diagnostics."${Hand}_index_tip_distance_blam")
                VisualSequence = [uint32]$diagnostics.sequence
                Image = $imagePath
            })
        }
    }

    $triggerResults = @($results | Where-Object Component -eq 'Trigger')
    for ($index = 1; $index -lt $triggerResults.Count; $index++) {
        if ($triggerResults[$index].IndexTipDistanceBlam -gt
            $triggerResults[$index - 1].IndexTipDistanceBlam + 0.00001) {
            throw 'Index fingertip distance did not decrease monotonically as trigger curl increased.'
        }
    }

    $jsonPath = Join-Path $OutputDirectory 'analog-hand-validation.json'
    $csvPath = Join-Path $OutputDirectory 'analog-hand-validation.csv'
    @($results) | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $jsonPath -Encoding utf8
    @($results) | Export-Csv -LiteralPath $csvPath -NoTypeInformation

    [pscustomobject]@{
        Passed = $true
        Hand = $Hand
        SampleCount = $results.Count
        Json = $jsonPath
        Csv = $csvPath
        Images = @($results | Where-Object Image | ForEach-Object Image)
    }
} finally {
    try { Set-ControllerInput -Component Grip -Value 0 } catch {}
    try { Set-ControllerInput -Component Trigger -Value 0 } catch {}
}
