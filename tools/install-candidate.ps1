[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateDir,

    # Every MCC edition installed on this machine. One build serves both: the
    # per-game modules the DLL hooks are byte-identical between the Steam and
    # Microsoft Store editions apart from their Authenticode signature, and the
    # launcher detects the edition at run time. A requested target whose game
    # install is not present is reported and skipped; a target that IS present
    # must install cleanly or the whole deployment fails.
    [string[]]$GameDir = @(
        'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR',
        'N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR'
    )
)

# Installs one already-packaged cumulative candidate. The package manifest is
# the authority: no build tree, loose DLL, old backup, or release artifact may
# enter this path. This script never launches MCC and never changes the shared
# configuration.

$ErrorActionPreference = 'Stop'

$steamExeName = 'MCC-Win64-Shipping.exe'
$storeExeName = 'MCCWinStore-Win64-Shipping.exe'

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Test-ExactBoolean([object]$Value, [bool]$Expected) {
    return ($Value -is [bool]) -and ($Value -eq $Expected)
}

function Test-ExactInt32([object]$Value, [int]$Expected) {
    return ($Value -is [int]) -and ($Value -eq $Expected)
}

# The mod folder is only valid where a real MCC install sits beside it. For the
# Store edition the install root is the package's Content folder.
function Test-InstallRoot([string]$Root) {
    foreach ($exe in @($steamExeName, $storeExeName)) {
        $path = Join-Path $Root (Join-Path 'MCC\Binaries\Win64' $exe)
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            return $true
        }
    }
    return $false
}

function Get-EditionLabel([string]$Root) {
    if (Test-Path -LiteralPath (Join-Path $Root 'MicrosoftGame.config') -PathType Leaf) {
        return 'store'
    }
    if (Test-Path -LiteralPath (Join-Path $Root (Join-Path 'MCC\Binaries\Win64' $storeExeName)) -PathType Leaf) {
        return 'store'
    }
    return 'steam'
}

function Assert-FileIdentity(
    [string]$Path,
    [object]$Evidence,
    [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label is missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if ([int64]$Evidence.bytes -ne $item.Length) {
        throw "$Label length mismatch: expected $($Evidence.bytes), got $($item.Length)."
    }
    $expected = ([string]$Evidence.sha256).ToUpperInvariant()
    if ($expected -notmatch '^[0-9A-F]{64}$') {
        throw "$Label manifest SHA-256 is invalid."
    }
    $actual = Get-Sha256 $Path
    if ($actual -cne $expected) {
        throw "$Label SHA-256 mismatch: expected $expected, got $actual."
    }
    return $actual
}

# The binaries were renamed from halo3xr.* to HaloMCCVR.*. A game folder that
# keeps the old pair next to the new one lets an old launcher inject a stale
# DLL while a hash-verified candidate looks like it is under test, so obsolete
# legacy files are moved (never deleted) into the deploy-backup area, loudly.
function Move-LegacyModFiles([string]$GamePath, [string]$QuarantineDir) {
    $moved = @()
    foreach ($name in @(
            'halo3xr.dll', 'halo3xr_launcher.exe',
            'halo3xr.log', 'halo3xr.log.prev', 'halo3xr_launcher.log')) {
        $path = Join-Path $GamePath $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            if (-not (Test-Path -LiteralPath $QuarantineDir -PathType Container)) {
                New-Item -ItemType Directory -Path $QuarantineDir | Out-Null
            }
            Move-Item -LiteralPath $path `
                -Destination (Join-Path $QuarantineDir $name)
            $moved += $name
        }
    }
    if ($moved.Count -ne 0) {
        Write-Host ("  quarantined obsolete legacy files ({0}) into {1}" -f `
            ($moved -join ', '), $QuarantineDir)
    }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$candidateRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\candidates'))
$candidatePrefix = $candidateRoot.TrimEnd(
    [IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$candidatePath = [IO.Path]::GetFullPath($CandidateDir)
if (-not $candidatePath.StartsWith(
        $candidatePrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Candidate escaped the repository candidate directory: $candidatePath"
}
if (-not (Test-Path -LiteralPath $candidatePath -PathType Container)) {
    throw "Candidate directory does not exist: $candidatePath"
}

$manifestPath = Join-Path $candidatePath 'CANDIDATE-MANIFEST.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Candidate manifest is missing: $manifestPath"
}
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$packageId = [IO.Path]::GetFileName(
    $candidatePath.TrimEnd([IO.Path]::DirectorySeparatorChar))
$head = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not resolve current source commit for deployment.'
}
$repoStatus = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=normal)
if ($LASTEXITCODE -ne 0 -or $repoStatus.Count -ne 0) {
    throw 'Repository is dirty; refusing automatic deployment.'
}
if (-not (Test-ExactInt32 $manifest.schema_version 23) -or
        [string]$manifest.status -cne 'UNTESTED_LOCAL_CANDIDATE' -or
        $manifest.accepted -ne $false -or
        [string]$manifest.base_release -cne 'MCC_VR_ALPHA_0.3.3' -or
        [string]$manifest.package_preset -cne 'release' -or
        [string]$manifest.development_baseline -cne
            'fa1f6422de2de2e94e4d36ee4c731606c30371aa' -or
        [string]$manifest.package_id -cne $packageId -or
        [string]$manifest.source_commit -notmatch '^[0-9a-f]{40}$' -or
        [string]$manifest.source_commit -cne $head -or
        -not $packageId.StartsWith(
            $head.Substring(0, 7) + '-halo2-c50-final-palette-reticle-ray-',
            [StringComparison]::Ordinal) -or
        @($manifest.titles).Count -ne 5 -or
        [string]$manifest.titles[0] -cne 'Halo 3' -or
        [string]$manifest.titles[1] -cne 'Halo 3: ODST' -or
        [string]$manifest.titles[2] -cne 'Halo: Reach' -or
        [string]$manifest.titles[3] -cne 'Halo 4' -or
        [string]$manifest.titles[4] -cne 'Halo 2 Anniversary' -or
        $manifest.embedded_build_identity.source_commit -cne
            $manifest.source_commit -or
        $manifest.embedded_build_identity.odst -ne $true -or
        $manifest.embedded_build_identity.reach -ne $true -or
        $manifest.embedded_build_identity.reach_render -ne $true -or
        $manifest.embedded_build_identity.halo4 -ne $true -or
        [string]$manifest.embedded_build_identity.halo2 -cne
            'BOTH_MODES_STEREO_6DOF' -or
        [string]$manifest.accepted_halo4_identity.candidate -cne 'C-H4-43' -or
        [string]$manifest.accepted_halo4_identity.source_commit -cne
            'dd9946595511d65c9859b536e2727201c107da45' -or
        # Producer and installer advance together. This prevents a package for
        # the new source from silently carrying the preceding Halo 4 candidate's
        # behavior block, which happened repeatedly during bring-up.
        [string]$manifest.halo4_candidate.id -cne 'C-H4-D1' -or
        [string]$manifest.halo4_candidate.status -cne
            'DIAGNOSTIC_HEADSET_CAPTURE_REQUIRED' -or
        [string]$manifest.halo4_candidate.behavior -notmatch '\S' -or
        $manifest.halo4_candidate.parity_diagnostic.player_visible_behavior_changed -ne
            $false -or
        $manifest.halo4_candidate.parity_diagnostic.automatic_for_this_candidate -ne
            $true -or
        [int]$manifest.halo4_candidate.parity_diagnostic.command_bucket_count -ne
            256 -or
        [int]$manifest.halo4_candidate.parity_diagnostic.transform_identity_slots -ne
            32 -or
        [string]$manifest.halo4_candidate.parity_diagnostic.hot_path -cne
            'bounded-reads-and-atomic-updates-only' -or
        [string]$manifest.halo4_candidate.parity_diagnostic.worker_output -cne
            'HaloMCCVR.log H4DIAG lines' -or
        [string]$manifest.halo4_candidate.parity_diagnostic.overflow_policy -cne
            'explicit-incomplete-census-never-merge-identities' -or
        [string]$manifest.halo4_candidate.failure_policy -cne
            'pre-claim-stock-post-claim-frame-drop-core-remains-armed' -or
        $manifest.halo4_candidate.authored_crosshair -ne $true -or
        $manifest.halo4_candidate.native_face_crosshair_suppressed -ne $true -or
        [string]$manifest.halo4_candidate.reticle_capture_boundary -cne
            'bounded-capture-eye-full-gameplay-cui-replay-into-shared-authored-texture' -or
        [string]$manifest.halo4_candidate.reticle_failure_policy -cne
            'stock-or-procedural-feature-fallback-camera-hands-stereo-and-openxr-remain-armed' -or
        [string]$manifest.halo4_candidate.hud_layout -cne
            'dormant-after-c-h4-44-headset-rejection' -or
        [string]$manifest.halo4_candidate.hud_failure_policy -cne
            'stock-halo4-cui-layout' -or
        @($manifest.halo4_candidate.hud_controls).Count -ne 0 -or
        [string]$manifest.halo2_candidate.id -cne 'C-H2-50' -or
        [string]$manifest.halo2_candidate.status -cne
            'HEADSET_FINAL_PALETTE_AIM_VALIDATION_REQUIRED' -or
        [string]$manifest.halo2_candidate.module -cne 'halo2.dll' -or
        [string]$manifest.halo2_candidate.scope -cne
            'campaign-both-renderers-groundhog-excluded' -or
        [string]$manifest.halo2_candidate.behavior -cne
            'final-root-composed-world-palette-two-hand-subtrees-all-other-nodes-collapsed-exact-presented-reticle-ray' -or
        $manifest.halo2_candidate.render_topology_probe -ne $false -or
        $manifest.halo2_candidate.render_topology_probe_changes_behavior -ne
            $false -or
        $manifest.halo2_candidate.anniversary_stereo -ne $true -or
        [string]$manifest.halo2_candidate.anniversary_stereo_hook_rva -cne
            '0x002DF190' -or
        [string]$manifest.halo2_candidate.anniversary_stereo_eye_camera -cne
            'view-record-embedded-camera-rebuilt-by-engine' -or
        $manifest.halo2_candidate.observer_6dof -ne $true -or
        [string]$manifest.halo2_candidate.observer_6dof_hook_rva -cne
            '0x006F0250' -or
        -not (Test-ExactInt32 $manifest.halo2_candidate.observer_6dof_owned_user 0) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.observer_6dof_owned_span_count 3) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.observer_6dof_owned_span_bytes 12) -or
        $manifest.halo2_candidate.observer_6dof_writes_field_of_view -ne $false -or
        $manifest.halo2_candidate.observer_6dof_engine_transform_runs_first -ne
            $true -or
        $manifest.halo2_candidate.observer_6dof_requires_restore -ne $false -or
        $manifest.halo2_candidate.observer_6dof_reaches_both_renderers -ne $true -or
        $manifest.halo2_candidate.live_renderer_report -ne $true -or
        [string]$manifest.halo2_candidate.live_renderer_source -cne
            'unique-signature-decoded-classic-render-gate' -or
        [string]$manifest.halo2_candidate.classic_render_gate_rva -cne
            '0x00E70CF8' -or
        [string]$manifest.halo2_candidate.applied_render_mode_rva -cne
            '0x00E21280' -or
        [string]$manifest.halo2_candidate.observer_result_rva -cne
            '0x015F297C' -or
        [string]$manifest.halo2_candidate.observer_stride -cne '0x368' -or
        $manifest.halo2_candidate.stereo_arms_only_when_classic_render_tree_runs -ne
            $false -or
        [string]$manifest.halo2_candidate.remastered_mode_stereo_presentation -cne
            'simultaneous-stereo' -or
        $manifest.halo2_candidate.remastered_mode_engine_writes -ne $true -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.identity_anchor_count 6) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.liveness_anchor_count 2) -or
        -not (Test-ExactInt32 $manifest.halo2_candidate.hook_count 2) -or
        -not (Test-ExactInt32 $manifest.halo2_candidate.caller_edge_count 2) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.both_eye_renders_per_game_frame 2) -or
        -not (Test-ExactInt32 $manifest.halo2_candidate.fresh_eye_count 2) -or
        [string]$manifest.halo2_candidate.both_eye_serial_policy -cne
            'current-prepared-serial' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.both_eye_render_serials_equal_current_prepared_serial `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.both_eye_capture_serials_equal_current_prepared_serial `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.same_game_frame_pair $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.same_generation_resource_epoch_and_attempt_required `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.duplicate_or_missing_eye_allowed `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.temporal_previous_eye_allowed $false) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.temporal_eye_gap_frames 0) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.intentional_cadence_divisor 1) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.per_eye_render_rate_equals_game_frame_rate `
            $true) -or
        @($manifest.halo2_candidate.supported_refresh_hz).Count -ne 5 -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.supported_refresh_hz[0] 72) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.supported_refresh_hz[1] 80) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.supported_refresh_hz[2] 90) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.supported_refresh_hz[3] 120) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.supported_refresh_hz[4] 144) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.foreign_pause_cleared_before_claim `
            $true) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.app_cadence_gate_hz.min 72) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.app_cadence_gate_hz.max 144) -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.source -cne
            'current xrWaitFrame predictedDisplayPeriod and same prepared serial predictedDisplayTime delta' -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.target_period_source -cne
            'current xrWaitFrame predictedDisplayPeriod' -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.delivered_delta_source -cne
            'same prepared serial predictedDisplayTime delta' -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.app_cadence_gate_hz.period_ns_min `
            6944444) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.app_cadence_gate_hz.period_ns_max `
            13888889) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.app_cadence_gate_hz.both_witnesses_required `
            $true) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.app_cadence_gate_hz.hz_tolerance 0) -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.at_45_hz -cne
            'unclaimed-stock-screen-before-eye-render' -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.at_60_hz -cne
            'unclaimed-stock-screen-before-eye-render' -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.unknown_or_outside -cne
            'unclaimed-stock-screen-before-eye-render' -or
        [string]$manifest.halo2_candidate.app_cadence_gate_hz.target_90_hz_delta_22222222_ns -cne
            'unclaimed-stock-screen-before-eye-render' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.runtime_targeted_below_72_h2_stereo_allowed `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.measured_gpu_fps_statically_guaranteed `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.headset_actual_72_144_validation_required `
            $true) -or
        [string]$manifest.halo2_candidate.post_first_complete_serial_policy -cne
            'previous-completed-serial-plus-one' -or
        [string]$manifest.halo2_candidate.serial_gap_quarantine_reason -cne
            'CorePreparedSerialGap' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.serial_gap_quarantines_before_eye_render `
            $true) -or
        [string]$manifest.halo2_candidate.serial_gap_frame_presentation -cne
            'unclaimed-stock-screen' -or
        [string]$manifest.halo2_candidate.unclaimed_no_pair_presentation -cne
            'stock-screen' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.unclaimed_no_pair_intentional_zero_layer `
            $false) -or
        [string]$manifest.halo2_candidate.claimed_partial_pair_presentation -cne
            'drop-frame' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.unclaimed_no_pair_fallback_counts_as_eye `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.unclaimed_no_pair_fallback_publishes_stereo_or_gameplay_heartbeat `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.pre_stereo_screen_path_counts_as_eye `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.pre_stereo_screen_path_counts_as_stereo_success `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.pre_stereo_screen_path_publishes_stereo_or_gameplay_heartbeat `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.post_claim_failure_quarantine $true) -or
        [string]$manifest.halo2_candidate.quarantine_scope -cne
            'module-generation' -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.max_claimed_failed_frames_before_quarantine `
            1) -or
        [string]$manifest.halo2_candidate.touched_failure_frame -cne 'drop' -or
        [string]$manifest.halo2_candidate.subsequent_untouched_frames -cne
            'stock-screen' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.openxr_remains_available_for_structural_halo2_failure `
            $true) -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.acquire_result -cne
            'XR_SUCCESS' -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.wait_result -cne
            'XR_SUCCESS' -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.release_result -cne
            'XR_SUCCESS' -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.end_frame_result -cne
            'XR_SUCCESS' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.source_texture_required `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.destination_texture_required `
            $true) -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.fast_copy_resource.eligibility -cne
            'equal-size-equal-format-family-single-sample' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.fast_copy_resource.target_rtv_required `
            $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.shader_blit.target_rtv_required `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.local_d3d_validation_failure_terminates_openxr `
            $false) -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.local_d3d_validation_failure_presentation -cne
            'drop-current-frame-keep-session' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.local_d3d_next_frame_retry_allowed `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.blit_success_required `
            $true) -or
        [string]$manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.named_session_recovery -cne
            'EnterFrameWaitFatalDrain' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.strict_unclaimed_stock_screen_transaction.unresolved_xr_transaction_repeated_same_session_retry `
            $false) -or
        [string]$manifest.halo2_candidate.expected_cold_pass_line -cne
            'Halo 2 cold observation PASS (C-H2-1)' -or
        [string]$manifest.halo2_candidate.expected_stereo_line -cne
            'Halo 2 C-H2-6 simultaneous stereo + 6DOF active' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.active_line_requires_complete_exact_current_pair_survived_xr_end_frame `
            $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.controller_input $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.stereo $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.six_dof $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.headset_rotation $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.headset_translation $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.controller_aim $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.floating_hands $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.physical_right_stick_preserved $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.controller_camera_writes $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.controller_xinput_synthesis $false) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.controller_owned_first_person_slot 0) -or
        [string]$manifest.halo2_candidate.first_person_palette_source -cne
            'native-frame-interpolator-read-0x34-byte-camera-relative-node-matrices' -or
        [string]$manifest.halo2_candidate.hand_binding_source -cne
            'animation-graph-model-flags-left-0x08-right-0x10-parent-transitive-closure' -or
        [string]$manifest.halo2_candidate.left_controller_nodes -cne
            'exact-left-wrist-descendant-subtree' -or
        [string]$manifest.halo2_candidate.right_controller_nodes -cne
            'exact-right-wrist-descendant-subtree-including-authored-weapon-descendants' -or
        [string]$manifest.halo2_candidate.collapsed_nodes -cne
            'wrist-to-root-upper-arm-and-forearm-ancestors-excluding-wrists-root-camera-control-and-unrelated-controls' -or
        [string]$manifest.halo2_candidate.controller_rotation_space -cne
            'camera-local-H-transpose-times-C-invariant-under-common-world-body-turn' -or
        [string]$manifest.halo2_candidate.projectile_aim_source -cne
            'fresh-compositor-presented-reticle-pose-through-h2ek-bsim-matched-firing-helper-retail-rva-0x8f0f70-output-user-0-unit-guard' -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.projectile_presented_reticle_freshness_ms 250) -or
        [string]$manifest.halo2_candidate.interpolation_reset_policy -cne
            'bypass-only-while-floaty-requested-hooks-live-and-head-plus-aim-tracked' -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.hud $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.haptics $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.scene_target_redirect $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.native_symmetric_fov_cover $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.native_asymmetric_fov_writes $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.simultaneous_stereo $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.runtime_hooks $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.engine_writes $true) -or
        [string]$manifest.halo2_candidate.engine_write_scope -cne
            'render-and-raster-position-forward-up-six-12-byte-spans-plus-vertical-fov-two-4-byte-spans-restored' -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.engine_write_span_count 8) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.engine_pose_write_span_count 6) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.engine_pose_write_span_bytes 12) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.engine_vertical_fov_write_span_count 2) -or
        -not (Test-ExactInt32 `
            $manifest.halo2_candidate.engine_vertical_fov_write_span_bytes 4) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.engine_write_restore_required $true) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.generic_draw_distance_write $false) -or
        -not (Test-ExactBoolean `
            $manifest.halo2_candidate.halo3_regression_required $true) -or
        [string]$manifest.halo2_candidate.failure_policy -cne
            'floaty-or-aim-failure-leaves-that-feature-stock-camera-stereo-and-openxr-remain-armed' -or
        [string]$manifest.halo2_candidate.evidence -cne
            'docs/HALO2-SIGNATURE-EVIDENCE.md' -or
        $manifest.deployment_policy.automatic_after_package -ne $true -or
        [string]$manifest.deployment_policy.installer -cne
            'tools/install-candidate.ps1' -or
        $manifest.deployment_policy.launches_mcc -ne $false -or
        $manifest.deployment_policy.changes_config -ne $false -or
        $manifest.reach_fp_h3_odst_transaction_parity_gate -ne $true -or
        $manifest.reach_fp_nested_camera_workspace -ne $true -or
        $manifest.reach_fp_world_projection_execution_status -ne $true -or
        $manifest.reach_forced_floating_hands -ne $true -or
        $manifest.reach_vehicle_view_follow_off_preserved -ne $true -or
        $manifest.reach_vehicle_view_follow_render_matched_enabled -ne $true -or
        $manifest.reach_vehicle_view_follow_refresh_invariant -ne $true -or
        $manifest.reach_vehicle_exact_seat_entry_playspace_recenter_enabled -ne
            $true -or
        $manifest.reach_vehicle_entry_recenter_view_follow_independent -ne
            $true -or
        $manifest.reach_vehicle_entry_recenter_refresh_invariant -ne $true -or
        [string]$manifest.reach_vehicle_entry_recenter_heading_policy -cne
            'render-matched-root-or-carrier' -or
        $manifest.reach_vehicle_entry_recenter_openxr_present_owned -ne $true -or
        $manifest.reach_vehicle_entry_recenter_outer_commit_staged -ne $true -or
        $manifest.reach_vehicle_camera_proof_miss_preserves_occupation -ne
            $true -or
        $manifest.reach_vehicle_yaw_reference_atomic_pair -ne $true -or
        $manifest.reach_vehicle_yaw_reference_requires_committed_frame -ne
            $true -or
        $manifest.reach_vehicle_exit_recenter_position_only -ne $true -or
        $manifest.reach_vehicle_blender_camera_defaults_enabled -ne $true -or
        $manifest.reach_vehicle_retail_camera_aliases_enabled -ne $true -or
        $manifest.reach_projectile_alignment_enabled -ne $true -or
        [string]$manifest.reach_projectile_alignment_scope -cne
            'exact-local-reach-vehicle-central-line' -or
        $manifest.reach_vehicle_body_hide_interval_lease_enabled -ne $false -or
        $manifest.reach_vehicle_unit_camera_scoped_body_hide_enabled -ne $true -or
        $manifest.reach_vehicle_native_fp_body_seated_legs_enabled -ne $true -or
        $manifest.reach_vehicle_fp_body_centered_authored_pose -ne $true -or
        $manifest.reach_vehicle_fp_body_failure_isolated -ne $true -or
        [string]$manifest.reach_vehicle_fp_body_identity_policy -cne
            'hrek-checksum-count-exact-tag-next-pair' -or
        [string]$manifest.reach_vehicle_fp_body_spartan_identity -cne
            '0x10041201/82' -or
        [string]$manifest.reach_vehicle_fp_body_elite_identity -cne
            '0x1404030E/67' -or
        $manifest.reach_native_seated_aim_reticle_enabled -ne $false -or
        $manifest.reach_controller_vehicle_reticle_enabled -ne $true -or
        $manifest.reach_personal_weapon_rendered_eye_origin_enabled -ne $true -or
        $manifest.reach_vehicle_barrel_origin_alignment_enabled -ne $false -or
        [string]$manifest.reach_vehicle_barrel_origin_policy -cne 'stock' -or
        $manifest.reach_vehicle_selected_barrel_direction_alignment_enabled -ne
            $true -or
        [string]$manifest.reach_vehicle_shot_direction_policy -cne
            'native-selected-origin-to-presented-controller-reticle' -or
        [int]$manifest.reach_vehicle_shot_freshness_ms -ne 50 -or
        $manifest.reach_workshop_content_dependency -ne $false -or
        $manifest.reach_runtime_hooks_enabled -ne $true) {
    throw 'Candidate manifest identity or cumulative-title contract is invalid.'
}

$candidateDll = Join-Path $candidatePath 'HaloMCCVR.dll'
$candidateLauncher = Join-Path $candidatePath 'HaloMCCVRLauncher.exe'
$dllHash = Assert-FileIdentity `
    $candidateDll $manifest.files.'HaloMCCVR.dll' 'Candidate DLL'
$launcherHash = Assert-FileIdentity `
    $candidateLauncher $manifest.files.'HaloMCCVRLauncher.exe' 'Candidate launcher'

$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.ProcessName -in @(
        'MCC-Win64-Shipping', 'MCCWinStore-Win64-Shipping', 'HaloMCCVRLauncher', 'halo3xr_launcher')
})
if ($running.Count -ne 0) {
    $owners = ($running | ForEach-Object {
        '{0}:{1}' -f $_.ProcessName, $_.Id
    }) -join ', '
    throw "MCC or its launcher is running ($owners); automatic install made no changes."
}

$backupRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\deploy-backups'))
$expectedBackupPrefix = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out')) + [IO.Path]::DirectorySeparatorChar
if (-not $backupRoot.StartsWith(
        $expectedBackupPrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Backup root escaped the repository out directory: $backupRoot"
}

# Resolve every requested target up front. A target whose MCC install is absent
# is reported and skipped - that edition simply is not on this machine - but at
# least one must be present, and every present one must install cleanly.
$targets = @()
foreach ($requested in $GameDir) {
    $resolved = [IO.Path]::GetFullPath($requested)
    $installRoot = Split-Path -Parent $resolved
    $targets += [pscustomobject]@{
        GameDir     = $resolved
        InstallRoot = $installRoot
        Present     = (Test-InstallRoot $installRoot)
        Edition     = (Get-EditionLabel $installRoot)
    }
}
if (@($targets | Where-Object { $_.Present }).Count -eq 0) {
    throw ('No Halo: MCC install was found for any requested deployment target: ' +
        (($targets | ForEach-Object { $_.GameDir }) -join ', '))
}

# A mod folder created for the first time inherits the live configuration from
# an edition that already has one, so both editions run identical settings. An
# existing halomccvr.cfg is never touched.
$seedConfig = $null
foreach ($target in $targets) {
    $existingConfig = Join-Path $target.GameDir 'halomccvr.cfg'
    if ($target.Present -and (Test-Path -LiteralPath $existingConfig -PathType Leaf)) {
        $seedConfig = $existingConfig
        break
    }
}

$createdUtc = [DateTime]::UtcNow
$results = @()

foreach ($target in $targets) {
    $gamePath = $target.GameDir
    if (-not $target.Present) {
        Write-Warning ("SKIPPED - no Halo: MCC install under {0}, so nothing was written to {1}" -f `
            $target.InstallRoot, $gamePath)
        $results += [pscustomobject]@{
            GameDir = $gamePath; Edition = $target.Edition
            Action = 'skipped-not-installed'; Backup = $null
        }
        continue
    }

    $installedDll = Join-Path $gamePath 'HaloMCCVR.dll'
    $installedLauncher = Join-Path $gamePath 'HaloMCCVRLauncher.exe'
    $configPath = Join-Path $gamePath 'halomccvr.cfg'
    $logPath = Join-Path $gamePath 'HaloMCCVR.log'
    $hasExistingPair =
        (Test-Path -LiteralPath $gamePath -PathType Container) -and
        (Test-Path -LiteralPath $installedDll -PathType Leaf) -and
        (Test-Path -LiteralPath $installedLauncher -PathType Leaf)

    if (-not $hasExistingPair) {
        # First install into a verified MCC install root. There is no prior pair
        # to preserve, so the folder is seeded instead of refused.
        Write-Host ("First install for the {0} edition: {1}" -f $target.Edition, $gamePath)
        if (-not (Test-Path -LiteralPath $gamePath -PathType Container)) {
            New-Item -ItemType Directory -Path $gamePath | Out-Null
        }
        foreach ($extra in @('LICENSE', 'MANUAL-README.txt')) {
            $extraSource = Join-Path $candidatePath $extra
            if (Test-Path -LiteralPath $extraSource -PathType Leaf) {
                Copy-Item -LiteralPath $extraSource `
                    -Destination (Join-Path $gamePath $extra) -Force
            }
        }
        if ($null -ne $seedConfig -and
                -not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
            Copy-Item -LiteralPath $seedConfig -Destination $configPath
            Write-Host "  seeded halomccvr.cfg from $seedConfig"
        }
        Copy-Item -LiteralPath $candidateDll -Destination $installedDll -Force
        Copy-Item -LiteralPath $candidateLauncher -Destination $installedLauncher -Force
        $newDllHash = Get-Sha256 $installedDll
        $newLauncherHash = Get-Sha256 $installedLauncher
        if ($newDllHash -cne $dllHash -or $newLauncherHash -cne $launcherHash) {
            throw ("First-install hash mismatch at {0}: DLL={1} launcher={2}" -f `
                $gamePath, $newDllHash, $newLauncherHash)
        }
        Write-Host "  installed DLL:      $newDllHash"
        Write-Host "  installed launcher: $newLauncherHash"
        Move-LegacyModFiles $gamePath (Join-Path $backupRoot `
            ('legacy-{0}-{1}' -f $target.Edition,
                $createdUtc.ToString("yyyyMMdd-HHmmssfff'Z'")))
        $results += [pscustomobject]@{
            GameDir = $gamePath; Edition = $target.Edition
            Action = 'first-install'; Backup = $null
        }
        continue
    }

    $priorDllHash = Get-Sha256 $installedDll
    $priorLauncherHash = Get-Sha256 $installedLauncher
    if ($priorDllHash -ceq $dllHash -and
            $priorLauncherHash -ceq $launcherHash) {
        Write-Host ("Already installed for the {0} edition: {1}" -f $target.Edition, $gamePath)
        Move-LegacyModFiles $gamePath (Join-Path $backupRoot `
            ('legacy-{0}-{1}' -f $target.Edition,
                $createdUtc.ToString("yyyyMMdd-HHmmssfff'Z'")))
        $results += [pscustomobject]@{
            GameDir = $gamePath; Edition = $target.Edition
            Action = 'already-installed'; Backup = $null
        }
        continue
    }

    $backupId = '{0}-{1}-before-{2}-{3}' -f `
        $priorDllHash.Substring(0, 7).ToLowerInvariant(),
        $target.Edition,
        $manifest.source_commit.Substring(0, 7),
        $createdUtc.ToString("yyyyMMdd-HHmmssfff'Z'")
    $backupDir = Join-Path $backupRoot $backupId
    if (Test-Path -LiteralPath $backupDir) {
        throw "Refusing to reuse deployment backup directory: $backupDir"
    }
    New-Item -ItemType Directory -Path $backupDir | Out-Null

    $configHashBefore = $null
    Copy-Item -LiteralPath $installedDll -Destination `
        (Join-Path $backupDir 'HaloMCCVR.dll')
    Copy-Item -LiteralPath $installedLauncher -Destination `
        (Join-Path $backupDir 'HaloMCCVRLauncher.exe')
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $configHashBefore = Get-Sha256 $configPath
        Copy-Item -LiteralPath $configPath -Destination `
            (Join-Path $backupDir 'halomccvr.cfg')
    }
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        Copy-Item -LiteralPath $logPath -Destination `
            (Join-Path $backupDir 'HaloMCCVR.log')
    }

    if ((Get-Sha256 (Join-Path $backupDir 'HaloMCCVR.dll')) -cne $priorDllHash -or
            (Get-Sha256 (Join-Path $backupDir 'HaloMCCVRLauncher.exe')) -cne
                $priorLauncherHash) {
        throw 'Deployment backup verification failed; automatic install made no changes.'
    }

    $stagedDll = Join-Path $gamePath ("HaloMCCVR.dll.$packageId.pending")
    $stagedLauncher = Join-Path $gamePath `
        ("HaloMCCVRLauncher.exe.$packageId.pending")
    if ((Test-Path -LiteralPath $stagedDll) -or
            (Test-Path -LiteralPath $stagedLauncher)) {
        throw 'A candidate staging file already exists; refusing to overwrite it.'
    }

    try {
        Copy-Item -LiteralPath $candidateDll -Destination $stagedDll
        Copy-Item -LiteralPath $candidateLauncher -Destination $stagedLauncher
        if ((Get-Sha256 $stagedDll) -cne $dllHash -or
                (Get-Sha256 $stagedLauncher) -cne $launcherHash) {
            throw 'Staged candidate hash verification failed; installed files remain unchanged.'
        }

        Copy-Item -LiteralPath $stagedDll -Destination $installedDll -Force
        Copy-Item -LiteralPath $stagedLauncher -Destination $installedLauncher -Force
    }
    finally {
        if (Test-Path -LiteralPath $stagedDll -PathType Leaf) {
            Remove-Item -LiteralPath $stagedDll -Force
        }
        if (Test-Path -LiteralPath $stagedLauncher -PathType Leaf) {
            Remove-Item -LiteralPath $stagedLauncher -Force
        }
    }

    $installedDllHash = Get-Sha256 $installedDll
    $installedLauncherHash = Get-Sha256 $installedLauncher
    if ($installedDllHash -cne $dllHash -or
            $installedLauncherHash -cne $launcherHash) {
        throw ("Post-install hash mismatch at {0}: DLL={1} launcher={2}" -f `
            $gamePath, $installedDllHash, $installedLauncherHash)
    }
    if ($null -ne $configHashBefore -and
            (Get-Sha256 $configPath) -cne $configHashBefore) {
        throw 'Shared configuration changed during deployment.'
    }
    Move-LegacyModFiles $gamePath (Join-Path $backupDir 'legacy')

    $deployment = [ordered]@{
        schema_version = 2
        deployed_utc = $createdUtc.ToString('o')
        package_id = $packageId
        source_commit = [string]$manifest.source_commit
        game_dir = $gamePath
        edition = $target.Edition
        previous = [ordered]@{
            halo3xr_dll_sha256 = $priorDllHash
            halo3xr_launcher_sha256 = $priorLauncherHash
        }
        installed = [ordered]@{
            halo3xr_dll_sha256 = $installedDllHash
            halo3xr_launcher_sha256 = $installedLauncherHash
            config_sha256 = $configHashBefore
        }
        launched = $false
    }
    $deploymentPath = Join-Path $backupDir 'DEPLOYMENT-MANIFEST.json'
    [IO.File]::WriteAllText(
        $deploymentPath,
        ($deployment | ConvertTo-Json -Depth 5) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))

    Write-Host ("Installed for the {0} edition: {1}" -f $target.Edition, $gamePath)
    Write-Host "  installed DLL:      $installedDllHash"
    Write-Host "  installed launcher: $installedLauncherHash"
    Write-Host "  preserved previous: $backupDir"
    $results += [pscustomobject]@{
        GameDir = $gamePath; Edition = $target.Edition
        Action = 'installed'; Backup = $backupDir
    }
}

Write-Host ''
Write-Host "Automatically installed candidate: $packageId"
Write-Host "Installed source:   $($manifest.source_commit)"
Write-Host "Candidate DLL:      $dllHash"
Write-Host "Candidate launcher: $launcherHash"
foreach ($result in $results) {
    Write-Host ("  [{0,-6}] {1,-22} {2}" -f $result.Edition, $result.Action, $result.GameDir)
}
Write-Host 'MCC was not launched and no existing halomccvr.cfg was changed.'
