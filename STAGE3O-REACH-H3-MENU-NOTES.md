# Stage 3O - Reach HUD, H3 teardown isolation, menu refresh

Protected binary base: Stage 3N SHA-256
`9646afa98cacab7b688aff10ac93fd529ba084ebb7f81be79cc2a1f1d308ed6b`.

## Accepted-for-test changes

- Reach safe-frame discovery selects `MEM_MAPPED` when the adapter's
  `scanMappedRegions` flag is set. H3/ODST remain `MEM_PRIVATE`.
- Reach teardown clears shared `g_aimSeen`, `g_camValid`, and
  `g_baseCamValid` only when active title is Reach/None/Unknown.
- Post-link menu layer disables HUD curvature for either Halo 2 native HUD or
  active Reach, and refreshes welcome/category/title applicability strings.

## Halo 2 Classic muzzle

No runtime H2 muzzle patch is included in this candidate. H2EK/public evidence
supports a marker/attachment solve around `primary_trigger`, but the exact
retail visual effect resolver remains unpinned. The rejected/retired H2 firing
helper is not re-enabled because that would touch gameplay-owned projectile
origin/direction rather than the visual effect transaction.

## Reproduce

`python tools/build_stage3o_reach_h3_menu.py <Stage3N-HaloMCCVR.dll> <output.dll>`
