# Stage 3AT — restore the accepted CREDIT + ODST-unpin bytes

Input: Stage 3AS, SHA-256
`311036c85a491fb9b65f3fe4ad17e1b331d03bb2bf7450e641c3142d5ffd1058`.

Output: 2,919,424 bytes, SHA-256
`250da86094b9eb35d295f73b850ff84165a900a77c249c4539477caccca77671`.

## The regression this repairs

The Halo 4 chain (3AP → 3AQ → 3AR → 3AS) was mistakenly stacked on **plain**
Stage 3AL. The user's accepted state from 2026-08-27 evening was
`built/Stage3AO-ODST-FIX.dll` = Stage 3AL **plus 59 bytes**:

| Layer | Bytes | Site (file offset) | Effect |
|---|---|---|---|
| CREDIT | 56 | 0x2C0589 (.s3ic) | F1 welcome line: "Maintained by pancreations and @MeWhenINameMyself." |
| 3AN ODST-UNPIN | 2 | 0x2C5C16, 0x2C5C6B (.s3qd) | `mov ecx,4→6`: the Stage 3AL teardown pin's GetModuleHandleEx gains UNCHANGED_REFCOUNT, so it stops holding `halo3odst.dll`; `je→jmp` skips the matching release |
| 3AO ODST-FIX | 1 | 0x2BFF0C (.s3ic) | `je→jmp`: skip one saved-pointer release in the pin helper |

Dropping them reproduced the exact pre-fix ODST failure in the 2026-08-28
08:25 Steam log: ODST detected, camera core installed, but runtime mode never
left Loading and stereo never armed — every fps line "(stereo off)".

`Stage3AM-ODST-THRASH-FIX.dll` (2 bytes at 0x88010) was the DISPROVEN
title-thrash hypothesis and is deliberately **not** applied; the accepted
`Stage3AO` reference correctly carries `7510` there and so does 3AT.

## Method

`tools/build_stage3at_restore_credit_odst.py` recomputes the byte diff
directly from `built/Stage3AO-ODST-FIX.dll` against
`built/Stage3AL-HaloMCCVR.dll` (both hash-pinned), asserts the diff is exactly
the 59 bytes in the table, asserts Stage 3AS still carries 3AL's original
value at every one of those offsets (i.e. no overlap with the Halo 4 chain),
and applies them. Nothing is hand-encoded.

## Deployed

2026-08-28, both editions, hash-verified `250da860…`. The matched launcher
(`8e1da430…`, 231,424 bytes, from the Stage 3AL package) was also installed to
both editions, replacing the older 230,400-byte one.
