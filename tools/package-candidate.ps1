[CmdletBinding()]
param(
    # Force a from-scratch compile. Off by default: the packaged identity comes
    # from the git commit check and the SHA-256 of the installed files, not from
    # discarding object files, and a clean rebuild cost minutes on every single
    # candidate.
    [switch]$Clean
)

# Halo MCC VR is one cumulative build: Halo 3 + ODST + Halo: Reach + Halo 4,
# with Halo 2 C-H2-6's cadence-gated same-frame stereo + 6DOF and
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

    $acceptedSources = [ordered]@{
        'cumulative Halo 3/ODST/Reach' =
            'a5524d3fe58e4ed5507c27429ccca52a3d4fdf7d'
        'accepted Halo 4 C-H4-43' =
            'dd9946595511d65c9859b536e2727201c107da45'
    }
    foreach ($acceptedLine in $acceptedSources.GetEnumerator()) {
        & git -C $repoRoot merge-base --is-ancestor `
            $acceptedLine.Value $commit
        if ($LASTEXITCODE -ne 0) {
            throw "Refusing to package: HEAD does not descend from $($acceptedLine.Name) source $($acceptedLine.Value)."
        }
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
        'halo2-d1-render-probe',
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
        schema_version = 18
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
            halo2 = 'OBSERVER_6DOF_PLUS_D_H2_1_RENDER_PROBE'
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
            id = 'C-H2-8'
            status = 'DIAGNOSTIC_RENDER_TOPOLOGY_CENSUS_REQUIRED'
            module = 'halo2.dll'
            scope = 'campaign-classic-only-groundhog-excluded'
            behavior =
                'headset-owned-observer-camera-position-and-orientation-in-both-renderers-plus-live-renderer-report'
            # C-H2-7, E-H2-3: halo2.dll ships two renderers. The live one is
            # resolved read-only from a unique signature and reported, and the
            # classic stereo core arms only where its hooks can actually fire.
            # C-H2-8, E-H2-4: the observer is the single camera root both
            # halo2.dll renderers consume, so one write owns the headset
            # pose in the classic and the remastered mode alike.
            render_topology_probe = $true
            render_topology_probe_changes_behavior = $false
            observer_6dof = $true
            observer_6dof_hook_rva = '0x006F0250'
            observer_6dof_owned_user = 0
            observer_6dof_owned_span_count = 3
            observer_6dof_owned_span_bytes = 12
            observer_6dof_writes_field_of_view = $false
            observer_6dof_engine_transform_runs_first = $true
            observer_6dof_requires_restore = $false
            observer_6dof_reaches_both_renderers = $true
            live_renderer_report = $true
            live_renderer_source = 'unique-signature-decoded-classic-render-gate'
            classic_render_gate_rva = '0x00E70CF8'
            applied_render_mode_rva = '0x00E21280'
            observer_result_rva = '0x015F297C'
            observer_stride = '0x368'
            stereo_arms_only_when_classic_render_tree_runs = $true
            remastered_mode_stereo_presentation = 'stock-screen'
            remastered_mode_engine_writes = $false
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
            controller_input = $false
            stereo = $true
            six_dof = $true
            headset_rotation = $true
            headset_translation = $true
            controller_aim = $false
            hud = $false
            haptics = $false
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
                'dual-cadence-and-consecutive-serial-preclaim-screen-post-claim-drop-generation-quarantine-local-d3d-failure-keeps-openxr-xr-transaction-failure-enters-session-recovery'
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
        development_baseline = 'f4c641f7b1b707991f2bda71ba485090a16f1e9a'
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
        note = 'C-H2-6 retains C-H2-5''s true same-frame contract: two fresh Halo 2 eye renders in one game frame, exact current render/capture serials, no temporal or previous-eye reuse, intentional cadence divisor 1, full headset rotation/translation, symmetric binocular FOV cover, dual current-frame cadence witnesses in the exact 6944444..13888889 ns bounds (nominal inclusive 72-144 Hz), and consecutive prepared serials after the first Complete. The only behavioral delta corrects C-H2-5''s false pre-stereo RTV fatal: source and destination textures are always required; the equal-size/equal-format-family/single-sample CopyResource path does not require an RTV; the shader blit path does require an RTV; and a local pre-stereo D3D validation failure drops only that frame without terminating OpenXR, then permits the next frame to retry. Exact XR_SUCCESS acquire/wait/release/xrEndFrame and successful Blit remain required; an actual XR transaction failure retains named session recovery through EnterFrameWaitFatalDrain and does not retry that unresolved XR transaction in the same session. The pre-stereo screen path does not count as an eye, publish stereo, or satisfy gameplay heartbeat. The first post-claim structural failure still drops that touched frame and quarantines only H2 stereo for the module generation. Engine writes remain six restored 12-byte render/raster position/forward/up spans plus two restored 4-byte vertical-FOV spans. Controller admission/aim, HUD, haptics, scene-target redirect, native asymmetric writes, and generic draw-distance writes remain disabled. Halo 2 headset validation must prove true same-frame stereo, full headset rotation/translation, and actual 72-144 Hz; the same DLL then requires a Halo 3 regression.'
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
