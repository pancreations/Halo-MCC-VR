# Stage 3T — Q-R1 loader-preserved Reach re-entry

Stage 3T fixes the packaging regression in Stage 3R/3S and ports the Reach
hook-lifetime fix onto the exact **Stage 3Q-R1** DLL that Windows accepted.

## Root cause of the 0xC000007B regression

The first Stage 3Q candidate placed `.s3qd` at RVA `0x2F4000` and reported
`SizeOfImage=0x2F5000`. Windows rejected that package as Bad Image / 0xC000007B.
Stage 3Q-R1 corrected `.s3qd` to the immediately adjacent RVA `0x2F3000` and
`SizeOfImage=0x2F4000` without changing the cross-title logic.

Stage 3R and Stage 3S were mistakenly based on the rejected pre-R1 Stage 3Q
binary (`e3c440...`) instead of the loader-fixed Stage 3Q-R1 binary
(`7c788d...`). Stage 3T starts from `7c788d...` and preserves its PE header and
section table byte-for-byte.

## Reach re-entry fix

The second-Reach failure is independent of the PE loader bug. The headset log
showed old Reach MinHook targets surviving after `haloreach.dll` disappeared,
with repeated status 10 / `MH_ERROR_MEMORY_PROTECT` teardown failures. Stage 3T
retains the exact Reach module only after the normal level/liveness proof when
hook installation begins, releases unretained pins on failed installs, and
releases the retained pin only after successful hook removal/restoration and
level-gate rearm.

Cold title detection stays identity-only, so it does not recreate the earlier
load-bounce/module-pin behavior.

## Protected behavior

All Stage 3Q-R1 cross-title logic and all Stage 3P/3O/3M protected title-specific
bytes remain unchanged except the four guarded Reach hook-lifetime sites. The
90-byte helper occupies the already-existing zero cave at RVA `0x2F1B00` inside
`.s3ic`; no PE section/header/import geometry is changed.
