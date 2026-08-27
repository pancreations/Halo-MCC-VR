# Stage 3L — Halo 4 procedural-reticle safety rollback

Status: unaccepted headset recovery candidate after Stage 3K crash.

Base: exact accepted Stage 3I DLL SHA-256
`36eec37e0778b5a23726c724a4ebb5f9fcd9b46ebd05c4344ace12b2d3778677`.

This candidate deliberately makes ONE H4 recovery change: it restores the five
Halo 4 CUI/reticle code spans changed by Stage 3H to their exact Stage 3G /
C-H4-50 bytes. This removes the Stage 3H native-position experiment and returns
H4 to its known-safe procedural weapon-ray crosshair / fail-closed authored
reticle behavior.

Not included:
- no Stage 3J capture code;
- no Stage 3K controller-projected native-reticle helper;
- no Stage 3K V6 muzzle matrix publication/reroot;
- no new muzzle/effect modification at all.

Halo 2 remains Stage 3I: the Stage 3H H2 HUD-slider helper and Stage 3I Classic
cleanup pin are retained byte-for-byte. The appended dormant .s3hc/.s3hd helper
sections are left in place but are no longer referenced by the restored H4 code.

Headset check:
1. Launch Halo 4 and confirm no crash for at least 60 seconds.
2. Confirm the procedural VR weapon-ray crosshair is present and tracks the gun.
3. Zoom/unzoom and swap weapons; there must be no square/rectangle capture art.
4. Do not judge muzzle behavior in this candidate; it is intentionally unchanged.
5. Quick-smoke H2 Classic and Anniversary only if H4 remains stable.
