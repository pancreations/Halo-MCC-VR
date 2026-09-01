// Stage 3BT - the 3BM capture dumps roll for the whole session.
//
// The 3BM instrument writes HaloMCCVR-h4-reticle-1..4.raw once, in the first
// ~8 seconds, then stops (`cmp eax,4; ja skip`). The 16:20 session showed
// "other elements" on the crosshair later in play, which the four early
// dumps never saw. This wraps the counter 4 -> 1 instead of stopping, so the
// four files always hold the LAST four snapshots (~8 s) of the session.
//
// Splice: 9 bytes at 0x2F9066 inside the 3BM payload (`cmp eax,4` +
// `ja 0x2F92C5`) become a call here + 4 nops; the tick then always continues
// with eax = the (possibly wrapped) count 1..4, stored back so the filename
// digit (read from the counter at 0x2F9040) matches.

.intel_syntax noprefix
.text

.globl s3bt_wrap
s3bt_wrap:
    cmp     eax, 4
    jbe     .Lok
    mov     eax, 1
    mov     dword ptr [rip + DUMP_COUNTER], eax
.Lok:
    ret
