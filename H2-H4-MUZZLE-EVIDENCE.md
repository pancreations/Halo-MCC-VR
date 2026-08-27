# H2 / H4 muzzle-effect evidence used for Stage 3V

## User-supplied official tag bundle

Archive SHA-256:
`119d4c1cb875d6f458098f26172ce3f2d6f00d1d0cddad830a9fae6e29228544`

The archive was generated from the installed official H2EK and H4EK tag roots. Stage 3V preserves the compact index/tables under `evidence/muzzle_bundle_tables/`.

Key conclusions used by this pass:

- Halo 2 Classic muzzle effects are multi-marker and sometimes multi-instance. The Battle Rifle long and center components do not share one authored transform.
- Halo 4 Promethean effects span weapon-specific marker families. Suppressor notably uses `primary_trigger_muzzle`; other families include vents, plates, rails, barrels and source ports.
- Those tag facts are sufficient to reject a one-marker generic relocation algorithm, but not sufficient by themselves to identify an exact retail runtime consumer/ABI.

## Historical H4 runtime captures

Preserved under `evidence/h4_runtime_capture/`:

- `phase2_counts.txt`: firing-specific `halo4.dll+0x1012E2` and the later matrix-copy path are idle-zero and firing-live.
- `left_shot_matrix.txt` / `right_shot_matrix.txt`: the returned 0x34-byte local/location-0 matrix differs with the held-hand position.
- `phase2_findings.txt`: the matrix is preserved through the queue producer; the evidence explicitly says no additional matrix-retarget fix is justified and the visible culprit may be downstream/different.
- `phase3_counts.txt`: the tested location-2 predicate did not fire in the controlled AR firing windows.

That is why Stage 3V chooses the narrow hide fallback for H4 rather than layering an unproven transform onto an already hand-dependent matrix.

## H2 runtime boundary status

No exact H2EK-to-retail runtime effect resolver has been established in the accepted source/history. Stage 3V does not copy a Reach/H4 offset or ABI into Halo 2. That restraint is intentional regression protection, not a claim that the H2 muzzle issue is solved.
