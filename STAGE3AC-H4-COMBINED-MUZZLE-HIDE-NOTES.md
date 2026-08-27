# Stage 3AC — Halo 4 combined muzzle / residual hide

Status: headset test candidate built directly from the headset-proven Stage 3Z
DLL (`154f31b3...`). Stage 3AA and Stage 3AB are rejected.

## Why 3AA / 3AB black-screened

Both rejected candidates placed executable helper bytes at RVA 0x2F3000 because
the on-disk bytes were zero. That address is not a code cave: Stage 3X/3Z uses
.s3qd+0 as writable runtime state/BSS. Existing code reads/writes 0x2F3000,
0x2F3008, 0x2F3010, the state array beginning 0x2F3020, and later state near
0x2F31C0..0x2F31F0. Overwriting those bytes corrupted the H4 stereo transaction
state and produced the headset log's `transaction exception` + `layers=0` path.

Stage 3AC writes nothing in .s3qd+0..+0x200. That entire live-state range is
byte-identical to Stage 3Z.

## H4 behavior

Stage 3AC combines two pieces already present/proven in the stable lineage:

1. Stage 3Z's headset-proven camera-mode-1 particle admission deny at
   halo4.dll+0x27BD36. This removed the visible AR muzzle particle on headset.
2. Stage 3X's already-integrated local first-person residual effect splice at
   halo4.dll+0x1012D5. Stage 3Z bypassed this installer when it introduced the
   particle gate; Stage 3AC simply calls the existing Stage 3X installer again.

No V11 postbuild transplant, global particle_render toggle, new MinHook target,
or new H4 effect resolver is introduced.

On H4 teardown the existing Stage 3X effect restore runs first, then the exact
Stage 3Z particle instruction restore. HUD/curvature cleanup and loader release
continue through the unchanged Stage 3X lifecycle afterward.

## Binary layout

- Input Stage 3Z SHA-256: `154f31b34049ef2797eb8993a252a51c411df3ec6a8b30f583b0b8137e66ebce`
- Output Stage 3AC SHA-256: `3a09288b5b8de4420ffc08695ebeb7431456971ccfe59de1f8c43f999caf700d`
- 12 PE sections, unchanged.
- SizeOfImage remains 0x2F4000.
- PE headers/import table unchanged.
- .s3qd RVA 0x2F3000..0x2F31FF is byte-identical to Stage 3Z.
- Only 134 file bytes differ from Stage 3Z.
