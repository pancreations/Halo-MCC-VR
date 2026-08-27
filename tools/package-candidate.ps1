[CmdletBinding()]
param(
    # Force a from-scratch compile. Off by default: the packaged identity comes
    # from the git commit check and the SHA-256 of the installed files, not from
    # discarding object files, and a clean rebuild cost minutes on every single
    # candidate.
    [switch]$Clean
)

# Halo MCC VR is one cumulative build: Halo 3 + ODST + Halo: Reach + Halo 4,
# with Halo 2's cadence-gated same-frame stereo + 6DOF and
# generation-scoped post-claim failure quarantine.
# Reach's camera core is permanent while Halo 4 is still an explicitly
# unaccepted bring-up line. Optional player-visible features fail open
# independently. This stages one unaccepted local candidate under out/candidates
# after a passing build and tests, then automatically installs those
# exact manifest-verified bytes into the dedicated MCC mod directory. It never
# launches MCC and never labels rebuilt bytes as an accepted release.

$ErrorActionPreference = 'Stop'

# Native build tools (cmake, ctest) write progress and deprecation notices to
# stderr. Under ErrorActionPreference=Stop, PowerShell 5.1 turns any native
# stderr line into a terminating error, so run tool invocations with stderr
# tolerated and rely on the explicit $LASTEXITCODE checks that follow each call.
function Invoke-Tool([scriptblock]$Block) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Block } finally { $ErrorActionPreference = $saved }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$candidateRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\candidates'))
$expectedCandidateRoot = [IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out')) + [IO.Path]::DirectorySeparatorChar
$packagePreset = 'release'
$packageBuildDir = 'out\build\release'

if (-not $candidateRoot.StartsWith(
        $expectedCandidateRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Candidate path escaped the repository out directory: $candidateRoot"
}

Push-Location $repoRoot
try {
    $status = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect Git worktree state.'
    }
    if ($status.Count -ne 0) {
        throw ("Refusing to package a dirty worktree. Commit the candidate first:`n" +
            ($status -join "`n"))
    }

    $commit = (& git -C $repoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') {
        throw 'Could not resolve the candidate source commit.'
    }

    # The handoff was imported on top of the last local Alpha 0.3.5 commit.
    # Keep the ancestry guard on a commit that exists in this repository; the
    # handoff's historical source-tree commit names are not local Git objects.
    $importedC50Baseline =
        '8ea6fa4301378c274d28e524975196e958d64b59'
    & git -C $repoRoot merge-base --is-ancestor $importedC50Baseline $commit
    if ($LASTEXITCODE -ne 0) {
        throw "Refusing to package: HEAD does not descend from the imported C50 source snapshot $importedC50Baseline."
    }

    # C-H2-55 shared loader-lifecycle and same-generation rehook gate. Most
    # title lookups remain identity-only. The explicit Stage3T/Stage3V/Stage3AL
    # post-proof ownership pins are the deliberate exception: they retain the
    # exact image while MinHook/native bytes are owned and release it only
    # after verified teardown. The old blanket checks below would reject the
    # lifetime fixes this handoff is deploying.
    $dllSources = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src\dll') `
        -Filter '*.cpp' -File
    $dllSourceText = ($dllSources | ForEach-Object {
        [IO.File]::ReadAllText($_.FullName)
    }) -join "`n"
    $hasVerifiedOwnerPin =
        $dllSourceText -match 'Stage 3AL owns exactly one post-preflight loader reference' -and
        $dllSourceText -match 'FreeLibrary\(retiredModuleReference\)' -and
        $dllSourceText -match 'Stage 3T: once Reach has passed its normal level/liveness proof'
    if ($dllSourceText -match '(?m)^\s*FreeLibrary\s*\(' -and
            -not $hasVerifiedOwnerPin) {
        throw 'C-H2-55 gate failed: an unverified title-DLL FreeLibrary call remains.'
    }
    $gameSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\dll\game.cpp'))
    $titleRuntimeSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\common\title_runtime_state.h'))
    $coreTestsSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'tests\core_tests.cpp'))
    $guardSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\dll\halo2_render_mode_guard.cpp'))
    if ($titleRuntimeSource -notmatch
            'ResolveTitleLevelGateAction' -or
        $titleRuntimeSource -notmatch
            'TitleLevelGateAction::HoldEvidence' -or
        $gameSource -notmatch
            'ResolveTitleLevelGateAction\s*\(\s*soleReachTitle\s*,\s*installed\s*,\s*levelRunning\s*\)' -or
        $coreTestsSource -notmatch
            'for\s*\(\s*uint32_t bits\s*=\s*0\s*;\s*bits\s*<\s*8\s*;' -or
        $coreTestsSource -notmatch
            'ResolveTitleLevelGateAction\(true, false, false\)\s*==\s*\r?\n\s*TitleLevelGateAction::HoldEvidence' -or
        $gameSource -notmatch
            '!soleHalo4Title\s*\|\|\s*!levelRunning' -or
        $gameSource -notmatch
            'Halo 3 hook epoch retired at the level-liveness boundary' -or
        $gameSource -notmatch
            'ODST level-liveness boundary: retiring hooks' -or
        $guardSource -notmatch
            'activeAndRange\s*&&\s*\r?\n\s*levelRunning\s*&&\s*coldPassed') {
        throw 'C-H2-55 gate failed: a required title hook epoch is not level-liveness scoped.'
    }
    $halo2ColdSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\dll\halo2_cold_observation.cpp'))
    $halo2LogicSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\common\halo2_render_logic.h'))
    if ($halo2LogicSource -notmatch
            'Halo2ColdObservationNeedsDerivedRebind' -or
        $halo2ColdSource -notmatch 'cached image proof rebound the') {
        throw 'C-H2-55 gate failed: the same-generation Halo 2 derived-binding rearm is missing.'
    }
    if ($halo2LogicSource -notmatch
            'kHalo2RejectedInterpolatorControllerOwnershipEnabled\s*=\s*false' -or
        $halo2LogicSource -notmatch
            'kHalo2FinalPaletteControllerOwnershipEnabled\s*=\s*false' -or
        $halo2LogicSource -notmatch
            'kHalo2StableFinalPacketControllerOwnershipEnabled\s*=\s*false' -or
        $halo2LogicSource -notmatch
            'kHalo2VisibleConsumerControllerOwnershipEnabled\s*=\s*true') {
        throw 'C-H2-65 gate failed: every rejected Halo 2 hand path must stay off and only the renderer-selected callback/persistent-packet transaction may be enabled.'
    }
    if ($halo2LogicSource -notmatch
            'kHalo2C64GenericLeftPresentationEnabled\s*=\s*false') {
        throw 'C-H2-65 gate failed: the rejected C-H2-64 generic alignment is not disabled.'
    }
    $halo2StereoSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\dll\halo2_stereo_core.cpp'))
    $halo2HudLogicSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\common\halo2_hud_logic.h'))
    $halo2HudShaderSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\common\halo2_hud_shader_logic.h'))
    $d3dSource = [IO.File]::ReadAllText(
        (Join-Path $repoRoot 'src\dll\d3d11_hook.cpp'))
    if ($halo2HudShaderSource -notmatch
            'kCrosshairHash\s*=\s*0x0a9b60d8f40268f6ULL' -or
        $halo2HudShaderSource -notmatch
            'kGameplayHudHashes' -or
        $halo2HudShaderSource -notmatch
            'MigotoFnv1' -or
        $d3dSource -notmatch 'Halo2CreatePixelShaderHook' -or
        $d3dSource -notmatch 'Halo2NativeHud_GetRasterLayout' -or
        $d3dSource -notmatch 'VR_BeginPreparedAuthoredReticleCapture' -or
        $halo2StereoSource -notmatch
            'VR_PrepareAuthoredReticleResources' -or
        $halo2StereoSource -match '&NativeHudAnchorBasisDetour' -or
        $halo2StereoSource -notmatch
            'Halo2NativeHud_DrawPlayer\s*\(' -or
        $coreTestsSource -notmatch
            'Halo 2 field-proven HUD/crosshair shader identities' -or
        $coreTestsSource -notmatch
            'Halo 2 HUD size/aspect/vertical sliders materially alter') {
        throw 'C-H2-77 gate failed: the proven shader identities, D3D raster transform, native-crosshair capture, shared replay, or zero-callback anchor rejection is missing.'
    }

    Invoke-Tool { & cmake --preset $packagePreset }
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset $packagePreset."
    }

    $cachePath = Join-Path $repoRoot "$packageBuildDir\CMakeCache.txt"
    $cache = [IO.File]::ReadAllText($cachePath)
    if ($cache -notmatch
            '(?m)^HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP:BOOL=ON\r?$') {
        throw 'Refusing to package: ODST is not ON in the cumulative build.'
    }
    if ($cache -notmatch
            '(?m)^HALOMCCVR_EXPERIMENTAL_HALO4_CAMERA:BOOL=ON\r?$') {
        throw 'Refusing to package C-H4-D1: the Halo 4 camera core is not ON.'
    }
    if ($cache -notmatch
            '(?m)^HALOMCCVR_EXPERIMENTAL_HALO2_COLD_OBSERVATION:BOOL=ON\r?$') {
        throw 'Refusing to package C-H2-6: prerequisite Halo 2 cold observation is not ON.'
    }
    if ($cache -notmatch
            '(?m)^HALOMCCVR_EXPERIMENTAL_HALO2_TEMPORAL_STEREO:BOOL=OFF\r?$') {
        throw 'Refusing to package C-H2-6: rejected Halo 2 temporal stereo is not OFF.'
    }
    if ($cache -notmatch
            '(?m)^HALOMCCVR_HALO2_STEREO6DOF:BOOL=ON\r?$') {
        throw 'Refusing to package C-H2-6: Halo 2 same-frame stereo + 6DOF is not ON.'
    }
    if ($cache -notmatch '(?m)^BUILD_TESTING:BOOL=ON\r?$') {
        throw 'Refusing to package: BUILD_TESTING is not ON.'
    }

    # Incremental. A clean rebuild was recompiling the whole tree for every
    # candidate, which is minutes per iteration for no safety: the packaged
    # identity is proven by the git commit check above plus the SHA-256 of the
    # exact installed files, not by how the object files were produced. Use
    # -Clean when a build-system change genuinely needs a from-scratch compile.
    $buildArgs = @('--build', '--preset', $packagePreset)
    if ($Clean) { $buildArgs += '--clean-first' }
    Invoke-Tool { & cmake @buildArgs }
    if ($LASTEXITCODE -ne 0) {
        throw 'Release build failed.'
    }

    Invoke-Tool { & ctest --preset $packagePreset }
    if ($LASTEXITCODE -ne 0) {
        throw 'Core tests failed.'
    }

    Invoke-Tool {
        & powershell -NoProfile -ExecutionPolicy Bypass -File `
            (Join-Path $repoRoot 'tools\check-reach-fp-parity.ps1')
    }
    if ($LASTEXITCODE -ne 0) {
        throw 'Reach consistency check failed.'
    }

    $finalCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
    $finalStatus =
        @(& git -C $repoRoot status --porcelain=v1 --untracked-files=normal)
    if ($LASTEXITCODE -ne 0 -or $finalCommit -ne $commit -or
            $finalStatus.Count -ne 0) {
        throw 'Repository state changed during build/test; refusing to label the artifacts.'
    }

    $createdUtc = [DateTime]::UtcNow
    $packageId = '{0}-{1}-{2}' -f $commit.Substring(0, 7),
        'c-h2-77-stage3e-proven-hud-native-crosshair',
        $createdUtc.ToString("yyyyMMdd-HHmmssfff'Z'")
    $packageDir = Join-Path $candidateRoot $packageId
    if (Test-Path -LiteralPath $packageDir) {
        throw "Refusing to reuse candidate directory: $packageDir"
    }

    Invoke-Tool { & cmake --install $packageBuildDir --config Release `
        --prefix $packageDir --component dist }
    if ($LASTEXITCODE -ne 0) {
        throw 'Candidate staging failed.'
    }

    $dllPath = Join-Path $packageDir 'HaloMCCVR.dll'
    $launcherPath = Join-Path $packageDir 'HaloMCCVRLauncher.exe'
    foreach ($requiredPath in @(
            $dllPath,
            $launcherPath,
            (Join-Path $packageDir 'LICENSE'),
            (Join-Path $packageDir 'MANUAL-README.txt'))) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "Candidate package is missing: $requiredPath"
        }
    }

    $dll = Get-Item -LiteralPath $dllPath
    $launcher = Get-Item -LiteralPath $launcherPath
    $dllHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash
    $launcherHash =
        (Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash

    $manifest = [ordered]@{
        schema_version = 26
        status = 'UNTESTED_LOCAL_CANDIDATE'
        accepted = $false
        package_id = $packageId
        created_utc = $createdUtc.ToString('o')
        source_commit = $commit
        package_preset = $packagePreset
        titles = @(
            'Halo 3', 'Halo 3: ODST', 'Halo: Reach', 'Halo 4',
            'Halo 2 Anniversary')
        embedded_build_identity = [ordered]@{
            source_commit = $commit
            odst = $true
            reach = $true
            reach_render = $true
            halo4 = $true
            halo2 = 'BOTH_MODES_STEREO_6DOF'
        }
        deployment_policy = [ordered]@{
            automatic_after_package = $true
            installer = 'tools/install-candidate.ps1'
            launches_mcc = $false
            changes_config = $false
        }
        accepted_halo4_identity = [ordered]@{
            candidate = 'C-H4-43'
            source_commit =
                'dd9946595511d65c9859b536e2727201c107da45'
        }
        halo4_candidate = [ordered]@{
            id = 'C-H4-D1'
            status = 'DIAGNOSTIC_HEADSET_CAPTURE_REQUIRED'
            behavior = 'c-h4-49-player-visible-path-plus-log-only-bounded-gameplay-cui-command-and-transform-identity-census'
            head_tracking = $true
            six_dof = $true
            headset_owned_pitch = $true
            headset_owned_yaw = $true
            controller_aim = $true
            haptics = $true
            head_relative_locomotion = $true
            # Exact accepted C-H4-43 player-visible behavior.
            hud = 'native-inside-captured-scene-no-redirect'
            hud_layout =
                'dormant-after-c-h4-44-headset-rejection'
            hud_controls = @()
            hud_failure_policy =
                'stock-halo4-cui-layout'
            authored_crosshair = $true
            native_face_crosshair_suppressed = $true
            reticle_capture_boundary =
                'bounded-capture-eye-full-gameplay-cui-replay-into-shared-authored-texture'
            reticle_failure_policy =
                'stock-or-procedural-feature-fallback-camera-hands-stereo-and-openxr-remain-armed'
            parity_diagnostic = [ordered]@{
                player_visible_behavior_changed = $false
                automatic_for_this_candidate = $true
                command_bucket_count = 256
                transform_identity_slots = 32
                hot_path = 'bounded-reads-and-atomic-updates-only'
                worker_output = 'HaloMCCVR.log H4DIAG lines'
                overflow_policy = 'explicit-incomplete-census-never-merge-identities'
                protocol = 'docs/HALO4-PARITY-DIAGNOSTIC.md'
            }
            first_person_hands = $true
            arm_ik = $false
            floating_hands = $true
            weapon_follows_hand = $true
            controller_facing_orientation = $true
            orientation_source =
                'free-official-halo4-left_hand-marker-inverse-under-h3-odst-reach-mounted-controller-support-exact-c38-frozen-right-aim'
            left_presentation_trim =
                'free-minus-yaw-plus-pitch-minus-roll-matching-h3-odst-reach-support-exact-c38-shared-prepared-right-aim'
            free_left_palm =
                'official-left_hand-marker-frame-parity-no-c39-c40-c41-c42-layer'
            two_hand_left_pose =
                'byte-identical-c38-right-aim-shared-rotational-parent-with-live-left-wrist-relation-left-translation-unchanged'
            failure_policy =
                'pre-claim-stock-post-claim-frame-drop-core-remains-armed'
            vrik_failure_policy =
                'base-rigid-or-state-parent-invalid-input-leaves-that-palette-stock-while-optional-marker-parity-invalid-input-keeps-the-valid-c38-free-reroot-and-continues-right-hand-held-model-and-camera-core'
        }
        halo2_candidate = [ordered]@{
            id = 'C-H2-77'
            status = 'READY_FOR_HEADSET_TEST_UNACCEPTED'
            module = 'halo2.dll'
            scope = 'campaign-both-renderers-groundhog-excluded'
            behavior =
                'proven-hud-pixel-shader-raster-transform-plus-native-crosshair-capture-shared-both-renderers'
            # C-H2-7, E-H2-3: halo2.dll ships two renderers. The live one is
            # resolved read-only from a unique signature and reported, and the
            # classic stereo core arms only where its hooks can actually fire.
            # C-H2-8, E-H2-4: the observer is the single camera root both
            # halo2.dll renderers consume, so one write owns the headset
            # pose in the classic and the remastered mode alike.
            render_topology_probe = $false
            render_topology_probe_changes_behavior = $false
            anniversary_stereo = $true
            anniversary_stereo_hook_rva = '0x002DF190'
            anniversary_stereo_eye_camera = 'view-record-embedded-camera-rebuilt-by-engine'
            observer_6dof = $true
            observer_6dof_hook_rva = '0x006F0250'
            observer_6dof_owned_user = 0
            observer_6dof_owned_span_count = 3
            observer_6dof_owned_span_bytes = 12
            observer_6dof_writes_field_of_view = $false
            observer_6dof_engine_transform_runs_first = $true
            observer_6dof_requires_restore = $false
            observer_6dof_reaches_both_renderers = $true
            hand_mesh_context_builder_rva = '0x008181F0'
            hand_mesh_visible_consumer_rva = '0x0006BB40'
            hand_mesh_ownership = 'renderer-selected-single-transaction'
            anniversary_hand_mesh_ownership =
                'registered-render-model-callback-before-internal-copy'
            classic_packet_caller_rva = '0x007E5430'
            classic_publish_to_renderer = $false
            classic_hand_mesh_ownership =
                'persistent-packet-post-builder-plus-per-eye-draw-first-person-compensation'
            free_left_hand_presentation = 'restored-c63-controller-reroot'
            two_hand_support_presentation = 'restored-c63-authored-rigid-grip'
            right_hand_gun_transform = 'unchanged-c63-controller-barrel-alignment'
            native_aim_update_rva = '0x008FDF50'
            native_aim_ownership = 'desired-and-current-unit-aiming-vectors'
            rejected_post_return_packet_enabled = $false
            rejected_firing_helper_enabled = $false
            live_renderer_report = $true
            live_renderer_source = 'unique-signature-decoded-classic-render-gate'
            classic_render_gate_rva = '0x00E70CF8'
            applied_render_mode_rva = '0x00E21280'
            observer_result_rva = '0x015F297C'
            observer_stride = '0x368'
            stereo_arms_only_when_classic_render_tree_runs = $false
            remastered_mode_stereo_presentation = 'simultaneous-stereo'
            remastered_mode_engine_writes = $true
            identity_anchor_count = 6
            liveness_anchor_count = 2
            hook_count = 2
            caller_edge_count = 2
            both_eye_renders_per_game_frame = 2
            fresh_eye_count = 2
            both_eye_serial_policy = 'current-prepared-serial'
            both_eye_render_serials_equal_current_prepared_serial = $true
            both_eye_capture_serials_equal_current_prepared_serial = $true
            same_game_frame_pair = $true
            same_generation_resource_epoch_and_attempt_required = $true
            same_generation_level_rehook = $true
            same_generation_rebind_source =
                'cached-image-proof-rebinds-graphics-mode-observer-result-after-level-gate-reopens'
            same_generation_rebind_retry_policy = 'once-per-level-gate-epoch'
            duplicate_or_missing_eye_allowed = $false
            temporal_previous_eye_allowed = $false
            temporal_eye_gap_frames = 0
            intentional_cadence_divisor = 1
            per_eye_render_rate_equals_game_frame_rate = $true
            supported_refresh_hz = @(72, 80, 90, 120, 144)
            foreign_pause_cleared_before_claim = $true
            app_cadence_gate_hz = [ordered]@{
                min = 72
                max = 144
                source = 'current xrWaitFrame predictedDisplayPeriod and same prepared serial predictedDisplayTime delta'
                target_period_source = 'current xrWaitFrame predictedDisplayPeriod'
                delivered_delta_source = 'same prepared serial predictedDisplayTime delta'
                period_ns_min = 6944444
                period_ns_max = 13888889
                both_witnesses_required = $true
                hz_tolerance = 0
                at_45_hz = 'unclaimed-stock-screen-before-eye-render'
                at_60_hz = 'unclaimed-stock-screen-before-eye-render'
                unknown_or_outside = 'unclaimed-stock-screen-before-eye-render'
                target_90_hz_delta_22222222_ns =
                    'unclaimed-stock-screen-before-eye-render'
            }
            runtime_targeted_below_72_h2_stereo_allowed = $false
            measured_gpu_fps_statically_guaranteed = $false
            headset_actual_72_144_validation_required = $true
            post_first_complete_serial_policy =
                'previous-completed-serial-plus-one'
            serial_gap_quarantine_reason = 'CorePreparedSerialGap'
            serial_gap_quarantines_before_eye_render = $true
            serial_gap_frame_presentation = 'unclaimed-stock-screen'
            unclaimed_no_pair_presentation = 'stock-screen'
            unclaimed_no_pair_intentional_zero_layer = $false
            claimed_partial_pair_presentation = 'drop-frame'
            unclaimed_no_pair_fallback_counts_as_eye = $false
            unclaimed_no_pair_fallback_publishes_stereo_or_gameplay_heartbeat = $false
            pre_stereo_screen_path_counts_as_eye = $false
            pre_stereo_screen_path_counts_as_stereo_success = $false
            pre_stereo_screen_path_publishes_stereo_or_gameplay_heartbeat = $false
            post_claim_failure_quarantine = $true
            quarantine_scope = 'module-generation'
            max_claimed_failed_frames_before_quarantine = 1
            touched_failure_frame = 'drop'
            subsequent_untouched_frames = 'stock-screen'
            openxr_remains_available_for_structural_halo2_failure = $true
            strict_unclaimed_stock_screen_transaction = [ordered]@{
                acquire_result = 'XR_SUCCESS'
                wait_result = 'XR_SUCCESS'
                release_result = 'XR_SUCCESS'
                end_frame_result = 'XR_SUCCESS'
                source_texture_required = $true
                destination_texture_required = $true
                fast_copy_resource = [ordered]@{
                    eligibility =
                        'equal-size-equal-format-family-single-sample'
                    target_rtv_required = $false
                }
                shader_blit = [ordered]@{
                    target_rtv_required = $true
                }
                local_d3d_validation_failure_terminates_openxr = $false
                local_d3d_validation_failure_presentation =
                    'drop-current-frame-keep-session'
                local_d3d_next_frame_retry_allowed = $true
                blit_success_required = $true
                named_session_recovery = 'EnterFrameWaitFatalDrain'
                unresolved_xr_transaction_repeated_same_session_retry = $false
            }
            expected_cold_pass_line = 'Halo 2 cold observation PASS (C-H2-1)'
            expected_stereo_line = 'Halo 2 C-H2-6 simultaneous stereo + 6DOF active'
            active_line_requires_complete_exact_current_pair_survived_xr_end_frame =
                $true
            controller_input = $true
            stereo = $true
            six_dof = $true
            headset_rotation = $true
            headset_translation = $true
            controller_aim = $true
            floating_hands = $true
            physical_right_stick_preserved = $true
            controller_camera_writes = $false
            controller_xinput_synthesis = $false
            controller_owned_first_person_slot = 0
            first_person_palette_source =
                'h2ek-first-person-final-render-packet-builder-retail-rva-0x008181f0'
            hand_binding_source =
                'animation-graph-hand-flags-mapped-through-weapon-data-authored-hands-remap'
            left_controller_nodes =
                'exact-remapped-left-wrist-descendant-subtree-free-left-controller-or-two-hand-right-wrist-rigid-support-lock'
            right_controller_nodes =
                'exact-remapped-right-wrist-descendant-subtree-plus-separate-primary-held-gun-packet'
            collapsed_nodes =
                'every-final-hands-packet-node-outside-left-and-right-hand-subtrees'
            controller_rotation_space =
                'final-world-wrist-target-retains-authored-root-to-wrist-rotation-with-physical-controller-translation-and-affine-scale-about-wrist'
            projectile_aim_source =
                'h2ek-firing-helper-completed-direction-stock-muzzle-to-stable-recenter-mapped-fresh-presented-crosshair-point-with-material-deflection-telemetry'
            projectile_presented_reticle_freshness_ms = 250
            interpolation_reset_policy =
                'stock-reset-preserved-rejected-interpolator-controller-path-remains-disabled'
            same_module_hook_lease = $false
            same_module_hook_lease_scope =
                'rejected-c-h2-53-session-long-loader-reference-lease-reverted'
            loader_refcount_policy =
                'non-owning-exact-base-validation-no-game-dll-refcount-increments'
            physical_hook_teardown_policy =
                'retire-at-level-liveness-boundary-while-title-mapping-is-current'
            hud = $true
            hud_layout = 'exact-field-proven-pixel-shader-viewport-scissor-affine'
            native_chud_draw_rva = '0x007FFD70'
            hud_shader_hash_contract = '3dmigoto-unseeded-fnv1'
            hud_gameplay_shader_count = 15
            hud_controls = @(
                'hud_size', 'hud_aspect', 'hud_vertical_offset')
            hud_crosshair_shader_hash = '0x0a9b60d8f40268f6'
            native_crosshair = $true
            hud_crosshair_layout = 'native-authored-controller-aim-quad'
            native_crosshair_fallback = 'procedural-until-first-current-generation-capture'
            hud_curvature = $false
            hud_shared_across_renderer_switch = $true
            hud_failure_policy =
                'unknown-or-unavailable-shader-draws-stock-procedural-crosshair-fallback-stereo-remains-armed'
            haptics = $true
            scene_target_redirect = $false
            native_symmetric_fov_cover = $true
            native_asymmetric_fov_writes = $false
            simultaneous_stereo = $true
            runtime_hooks = $true
            engine_writes = $true
            engine_write_scope =
                'render-and-raster-position-forward-up-six-12-byte-spans-plus-vertical-fov-two-4-byte-spans-restored'
            engine_write_span_count = 8
            engine_pose_write_span_count = 6
            engine_pose_write_span_bytes = 12
            engine_vertical_fov_write_span_count = 2
            engine_vertical_fov_write_span_bytes = 4
            engine_write_restore_required = $true
            generic_draw_distance_write = $false
            halo3_regression_required = $true
            failure_policy =
                'floaty-or-aim-failure-leaves-that-feature-stock-camera-stereo-and-openxr-remain-armed'
            evidence = 'docs/HALO2-SIGNATURE-EVIDENCE.md'
        }
        # Reach support is permanent, while player-visible optional features
        # fail open independently and never disarm the working camera core.
        reach_permanent = $true
        reach_controller_input_enabled = $true
        reach_render_candidate_compiled = $true
        reach_loaded_image_preflight_enabled = $true
        reach_display_copy_readiness_enabled = $true
        reach_camera_core_enabled = $true
        reach_controller_aim_enabled = $true
        reach_two_arm_ik_guarded = $true
        reach_fp_interpolation_palette_transaction = $true
        reach_fp_h3_odst_transaction_parity_gate = $true
        reach_hrek_authored_crosshair_enabled = $true
        reach_hrek_authored_crosshair_mandatory = $true
        reach_flat_crosshair_substitute_enabled = $false
        reach_procedural_crosshair_substitute_enabled = $false
        reach_native_hud_layout_enabled = $false
        reach_projectile_alignment_enabled = $true
        reach_projectile_alignment_scope =
            'exact-local-reach-vehicle-central-line'
        reach_vehicle_view_follow_off_preserved = $true
        reach_vehicle_view_follow_render_matched_enabled = $true
        reach_vehicle_view_follow_refresh_invariant = $true
        reach_vehicle_exact_seat_entry_playspace_recenter_enabled = $true
        reach_vehicle_entry_recenter_view_follow_independent = $true
        reach_vehicle_entry_recenter_refresh_invariant = $true
        reach_vehicle_entry_recenter_heading_policy =
            'render-matched-root-or-carrier'
        reach_vehicle_entry_recenter_openxr_present_owned = $true
        reach_vehicle_entry_recenter_outer_commit_staged = $true
        reach_vehicle_camera_proof_miss_preserves_occupation = $true
        reach_vehicle_yaw_reference_atomic_pair = $true
        reach_vehicle_yaw_reference_requires_committed_frame = $true
        reach_vehicle_exit_recenter_position_only = $true
        reach_vehicle_blender_camera_defaults_enabled = $true
        reach_vehicle_retail_camera_aliases_enabled = $true
        reach_vehicle_body_hide_interval_lease_enabled = $false
        reach_vehicle_unit_camera_scoped_body_hide_enabled = $true
        reach_vehicle_native_fp_body_seated_legs_enabled = $true
        reach_vehicle_fp_body_centered_authored_pose = $true
        reach_vehicle_fp_body_failure_isolated = $true
        reach_vehicle_fp_body_identity_policy =
            'hrek-checksum-count-exact-tag-next-pair'
        reach_vehicle_fp_body_spartan_identity = '0x10041201/82'
        reach_vehicle_fp_body_elite_identity = '0x1404030E/67'
        reach_native_seated_aim_reticle_enabled = $false
        reach_controller_vehicle_reticle_enabled = $true
        reach_personal_weapon_rendered_eye_origin_enabled = $true
        reach_vehicle_barrel_origin_alignment_enabled = $false
        reach_vehicle_barrel_origin_policy = 'stock'
        reach_vehicle_selected_barrel_direction_alignment_enabled = $true
        reach_vehicle_shot_direction_policy =
            'native-selected-origin-to-presented-controller-reticle'
        reach_vehicle_shot_freshness_ms = 50
        reach_workshop_content_dependency = $false
        reach_fp_nested_camera_workspace = $true
        reach_fp_world_projection_execution_status = $true
        reach_forced_floating_hands = $true
        reach_copyresource_enabled = $true
        reach_engine_memory_writes_enabled = $true
        reach_runtime_hooks_enabled = $true
        base_release = 'MCC_VR_ALPHA_0.3.3'
        development_baseline = 'fa1f6422de2de2e94e4d36ee4c731606c30371aa'
        files = [ordered]@{
            'HaloMCCVR.dll' = [ordered]@{
                bytes = $dll.Length
                sha256 = $dllHash
            }
            'HaloMCCVRLauncher.exe' = [ordered]@{
                bytes = $launcher.Length
                sha256 = $launcherHash
            }
        }
        note = 'C-H2-77 rejects Stage 3D after zero anchor callbacks and transforms only the exact Halo 2 HUD pixel shaders proven by a working v1.2 HUD mod, inside the live native CHUD scope. The separately identified native crosshair shader is captured into the existing controller-aim quad; the procedural marker remains until that capture succeeds. Headset validation required.'
    }

    $manifestPath = Join-Path $packageDir 'CANDIDATE-MANIFEST.json'
    $json = $manifest | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText(
        $manifestPath,
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))

    Write-Host "Created untested candidate: $packageDir"
    Write-Host "Source:   $commit"
    Write-Host "DLL:      $dllHash"
    Write-Host "Launcher: $launcherHash"

    & powershell -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $repoRoot 'tools\install-candidate.ps1') `
        -CandidateDir $packageDir
    if ($LASTEXITCODE -ne 0) {
        throw 'Candidate was packaged but automatic installation failed.'
    }
}
finally {
    Pop-Location
}
