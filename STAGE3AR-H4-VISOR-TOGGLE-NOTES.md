# Stage 3AR — Halo 4 Mjolnir visor toggle

Input: Stage 3AQ, SHA-256
`e68b39b1e1f054d1b3db69bf9fa111943a40eaf2709f0395699d09f17d7f186a`.

Output: 2,919,424 bytes, SHA-256
`bd6a8eb33bbb9ca82686b41730c2949a5b1e5fbccef0ad3a8441e698aff15496`.

## Evidence

`ui\hud\player_huds\mc_hud\mc_hud.cui_screen` carries exactly two data bindings
for the helmet:

```
player_hud_view_reader.prop_hud_helmet_visible -> container_visor.prop_visible
player_hud_view_reader.prop_hud_helmet_visible -> container_visor_glow.prop_visible
```

One engine property owns the whole visor, and `container_visor` /
`container_visor_glow` are the **only** widgets in `mc_hud` carrying
`prop_parallax_multiplier_x/y`. The live gameplay CUI stream carries exactly two
`0x26`/`0x27` parallax brackets per pass, which matches one-for-one. So a
parallax bracket in the gameplay pass is a visor bracket.

The visor's art is eight polyart assets under `mc_hud/polyart` —
`helmet_top/bottom/left/right` and `glow_top/bottom/left/right` — every one of
them drawn by CUI command `0x20`.

## The change

Two edges are redirected into one appended page:

**`HaloMCCVR.dll+0x53780`** — `Halo4CuiRenderCommandDetour`'s prologue. The
filter drops command `0x20` while inside a parallax bracket:

* it does nothing at all unless the visor is switched off;
* it admits only the gameplay CUI root, using the exact TLS callback-depth field
  Stage 3X's HUD bridge gates on (`+0x38C`, index at `HaloMCCVR.dll+0x2D5E48`),
  because this dispatcher also serves the auxiliary texture pass and Halo 4's
  menus — and Halo 4 menus use parallax;
* `0x26` increments the bracket depth, `0x27` decrements it (never below zero);
* inside a bracket, `0x20` returns success without being dispatched.

Only leaf draws are dropped. The `0x28`/`0x29` transform push/pop pairs and the
`0x22`/`0x23` bitmap pairs inside the bracket still execute, so nothing can go
out of balance. An earlier attempt that dropped *every* command inside the
bracket threw the HUD offscreen for exactly that reason.

**`HaloMCCVR.dll+0x5CA0`** — the config parser's unknown-key edge, where `rbx`
is the key and `rsi` the value. `halomccvr.cfg` gains:

```
halo4_visor = 0        # 0 hides the Mjolnir visor (default), 1 draws it
```

A key that is not `halo4_visor` replays the displaced `cmp byte [rbx], r14b /
je` unchanged and falls through to the existing "unknown key ignored" line.

## Scope

Halo 4 only. The CUI filter lives inside a detour that exists solely for Halo 4
and is additionally gated on Halo 4's gameplay CUI TLS depth. The config edge is
shared, but any key other than `halo4_visor` takes the byte-identical original
path.

## Known limitation

The F1 menu has no row for this yet, and saving from the F1 menu rewrites
`halomccvr.cfg` from the fields the C++ knows about — which will drop a
`halo4_visor` line. The default is "hidden", so a dropped line only matters if
the visor had been switched back on. Adding the menu row needs the C++ layer.

## Reproduction

```text
py -3 tools/build_stage3ar_h4_visor_toggle.py \
    built/Stage3AQ-HaloMCCVR.dll built/Stage3AR-HaloMCCVR.dll
```

The builder refuses any input but the exact Stage 3AQ SHA-256 and guards both
spliced sequences plus three surrounding edges before writing. The payload is
assembled by `tools/postlink.py`, which uses clang's integrated assembler and
resolves the relocations itself — this machine has no GNU binutils, and the LLVM
`ld.lld` substitute stopped loading partway through this work.
