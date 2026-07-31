param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath
)

$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath $SourcePath -Raw

# Parent indices from the shipped 76-node Spartan FP assault-rifle skeleton,
# exported through Baboon. All shipped Campaign Evolved FP weapon skeletons
# use this topology.
$parents = @(
    -1, 0, 0, 0, 1, 1, 1, 1, 7, 6, 5, 5, 5, 6, 6, 6, 5, 16, 16,
    16, 16, 10, 8, 15, 9, 9, 9, 9, 25, 25, 19, 19, 25, 25, 20, 24,
    19, 19, 25, 25, 19, 19, 28, 30, 31, 40, 33, 32, 37, 36, 29, 38,
    39, 41, 42, 43, 44, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 65, 72
)

function Get-Descendants([int]$Root) {
    $result = @()
    for ($node = 0; $node -lt $parents.Count; $node++) {
        $candidate = $node
        while ($candidate -ge 0) {
            if ($candidate -eq $Root) {
                $result += $node
                break
            }
            $candidate = $parents[$candidate]
        }
    }
    return @($result | Sort-Object)
}

function Read-NodeArray([string]$Name) {
    $pattern =
        "std::array<std::uint8_t,\s*(\d+)>\s+$Name\s*\{([^}]*)\}"
    $match = [regex]::Match($source, $pattern)
    if (-not $match.Success) {
        throw "Missing node array: $Name"
    }

    $declared = [int]$match.Groups[1].Value
    $values = @(
        [regex]::Matches($match.Groups[2].Value, '\d+') |
            ForEach-Object { [int]$_.Value }
    )
    if ($declared -ne $values.Count) {
        throw "$Name declares $declared entries but initializes $($values.Count)"
    }
    if (@($values | Sort-Object -Unique).Count -ne $values.Count) {
        throw "$Name contains duplicate nodes"
    }
    return @($values | Sort-Object)
}

$roots = @{
    weapon_nodes = 7
    right_shoulder_nodes = 5
    right_elbow_nodes = 16
    right_wrist_nodes = 19
    left_shoulder_nodes = 6
    left_elbow_nodes = 9
    left_wrist_nodes = 25
}

foreach ($entry in $roots.GetEnumerator()) {
    $actual = @(Read-NodeArray $entry.Key)
    $expected = @(Get-Descendants $entry.Value)
    $difference = @(Compare-Object $expected $actual)
    if ($difference.Count -ne 0) {
        throw "$($entry.Key) does not match Baboon skeleton descendants"
    }
}

$requiredFailOpenTokens = @(
    'apply_split_controllers_to_first_person_palette',
    'place_wrist_subtree',
    'solve_visual_arm_for_floating_wrist',
    'std::array<BlamMatrix4x3, kFirstPersonNodeCount> visual_palette',
    'The floating hand remains authoritative',
    'std::copy(stock.begin(), stock.end(), palette)',
    'apply_legacy_controller_to_first_person_palette',
    'UEVR_HALO_ARM_IK',
    'UEVR_HALO_TWO_HAND_IK'
)
foreach ($token in $requiredFailOpenTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing fail-open token: $token"
    }
}

$requiredArmAnchorTokens = @(
    'torso_basis_from_root',
    'anchor_shoulder_to_torso',
    'kShoulderBackMeters',
    'kClavicleAssistMaxMeters',
    'kGripToWristBackMeters',
    'g_left_wrist_stock_relative'
)
foreach ($token in $requiredArmAnchorTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing arm-anchor token: $token"
    }
}

$requiredTwoHandHoldTokens = @(
    'effective_controller_basis',
    'update_two_hand_hold',
    'kTwoHandMinimumAgreement',
    'kTwoHandFullAgreement',
    'kTwoHandZoneRadiusMeters',
    'g_two_hand_last_forward',
    'halo_motion_reticle_hide.active',
    'UEVR_HALO_TWO_HAND_HOLD',
    'kStatusTwoHandHoldActive'
)
foreach ($token in $requiredTwoHandHoldTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing two-hand hold token: $token"
    }
}

$requiredScopedFireTokens = @(
    'g_local_zoomed',
    'IsGamePaused',
    'update_game_paused'
)
foreach ($token in $requiredScopedFireTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing scoped-fire token: $token"
    }
}

$requiredLateTrackingTokens = @(
    'get_late_tracking_snapshot',
    'predicted_display_time',
    'coherent late OpenXR tracking is ',
    'g_local_fire_tracking = capture_tracking_snapshot()',
    'auto tracking = capture_tracking_snapshot()'
)
foreach ($token in $requiredLateTrackingTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing late-tracking token: $token"
    }
}

$requiredLocomotionTokens = @(
    'on_xinput_get_state',
    'get_left_joystick_source',
    'XINPUT_GAMEPAD_DPAD_UP',
    'Do not re-inject analog movement on top of the shifted buttons',
    'sThumbLX',
    'sThumbLY'
)
foreach ($token in $requiredLocomotionTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing locomotion bridge token: $token"
    }
}

$requiredBallisticTokens = @(
    'kReticleDistanceMeters',
    'reticle_position - destination.position',
    'direction_override->reticle_position - *start',
    'primary_sweep_consumed',
    'direction_override->desired_direction'
)
foreach ($token in $requiredBallisticTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing converged center-ray token: $token"
    }
}

$requiredWorldReticleTokens = @(
    'one-sided Widget3D pass uses local +X',
    'std::atan2(-direction.y, -direction.x)',
    'disable_world_reticle_depth_test',
    'restore_world_reticle_depth_test',
    'L"bDisableDepthTest"'
)
foreach ($token in $requiredWorldReticleTokens) {
    if (-not $source.Contains($token)) {
        throw "Missing front-facing world-reticle token: $token"
    }
}

if (-not [regex]::IsMatch($source, 'std::atan2\(\s*-direction\.z,')) {
    throw 'World reticle pitch does not point local +X back toward the camera'
}

Write-Output 'Halo motion-control source validation passed.'
