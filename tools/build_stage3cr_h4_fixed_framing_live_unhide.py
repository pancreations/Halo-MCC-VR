"""Stage 3CR - accepted fixed capture framing + LIVE selector un-hide, on 3CQ.

Evidence (all sessions ever logged, `S3BH vp: engine viewport at captured
draw`):

    canvas (1033.578, 336.549)  working  -> engine vp 4134x1346 @ (-1811,-417)
    canvas (1304.277, 675.936)  blank    -> engine vp 4134x1346 @ (-1811,-417)
    canvas (1635.814, 731.878)  today    -> engine vp 4134x1346 @ (-1811,-417)

The engine's own CUI viewport for the reticle draw is INVARIANT. The accepted
3BH/3BR framing constants (4134.312 x 1346.196) still match it exactly today,
so the capture framing was never stale and never the regression.

Two different quantities were conflated because they were EQUAL on the one
calibrated canvas:
  * the engine's CUI viewport width          4134.312  (fixed, framing)
  * the hide shift 4*|baseX| = live hide     4134.312 on that canvas only
                                             6543.256 today  (live, selector)

3CE/3CL/Codex-3CO/3CQ all fed the live canvas value into the FRAMING, making
the window 1.58x too wide today (16358x11801 instead of 10335x7452) - which is
the wrong-element "black square" the headset reported. Conversely the frozen
4134.312 in the SELECTOR under-shot the live hide by 2409u on any other canvas,
leaving the reticle outside a 4134-wide window: the byte-empty captures.

Stage 3CR keeps exactly one live value, in the selector, and restores the
accepted framing:
  * gate call 0x2FB992 -> back to the accepted 3BR thunk 0x2F9A90 (stock bytes)
  * 3CQ's per-axis payload zeroed back to the dead 3BN region it occupied
  * selector un-hide at 0x2F99D7 keeps reading the LIVE hide 0x2A8368 (3CQ)

Net vs the accepted 3BU/3BR crosshair chain: the selector displacement only.
Cutscene/theatre stack (3BV/3BW/3BX + Codex's theatre camera) untouched.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "cc956a5e397bf29fddf7e7eb708d1bae068c1c944454b3d67c4aa2272ca74a4e"
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
BASELINE_3CB_SHA256 = \
    "9ee60ca1b97934002473a0f970c4af8e94a79aa34cfc92116f1c06b4f5690885"
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_STOCK = bytes.fromhex("e8f9e0ffff")        # call 0x2F9A90 (accepted 3BR)
S3BR_THUNK_RVA = 0x002F9A90
SELECTOR_LOAD_RVA = 0x002F99D7
LIVE_HIDE_X_RVA = 0x002A8368
FIXED_FRAMING_RVA = 0x002FB800

CONTEXT = (
    (0x002F995D, bytes.fromhex("66837c24420c752f"),
     "type-0x28 payload-size discriminator (kept)"),
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "accepted 3BR thunk head"),
    (0x002F9B29, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (0x002F9BD0, bytes.fromhex("000000440000003fb9379e3d"), "3BR constants"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "live hide publisher"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ absent"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CQ input: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")

    # the 3CQ gate must currently reach the per-axis payload we are removing
    go = rva_off(pe, GATE_CALL_RVA)
    if blob[go] != 0xE8:
        raise SystemExit("gate is not a call")
    cur = GATE_CALL_RVA + 5 + struct.unpack_from("<i", blob, go+1)[0]
    if cur != PAYLOAD_RVA:
        raise SystemExit(f"gate targets 0x{cur:X}, expected the 3CQ payload")

    # the selector un-hide must already read the LIVE hide (3CQ) and stays that way
    so = rva_off(pe, SELECTOR_LOAD_RVA)
    if bytes(blob[so:so+4]) != bytes.fromhex("f30f100d"):
        raise SystemExit("selector load opcode unexpected")
    sel = SELECTOR_LOAD_RVA + 8 + struct.unpack_from("<i", blob, so+4)[0]
    if sel != LIVE_HIDE_X_RVA:
        raise SystemExit(f"selector reads 0x{sel:X}, expected the live hide")

    # 1. restore the accepted framing: gate -> 3BR thunk (exact stock bytes)
    blob[go:go+5] = GATE_STOCK
    tgt = GATE_CALL_RVA + 5 + struct.unpack_from("<i", blob, go+1)[0]
    if tgt != S3BR_THUNK_RVA:
        raise SystemExit("restored gate does not reach the 3BR thunk")

    # 2. restore the region 3CQ borrowed to its pre-3CQ contents (the dead 3BN
    #    thunk), so this image differs from the 3CB theatre baseline by the
    #    selector displacement ALONE and by nothing else.
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    baseline = bytearray(Path(sys.argv[3]).read_bytes())
    if hashlib.sha256(bytes(baseline)).hexdigest() != BASELINE_3CB_SHA256:
        raise SystemExit("third argument must be the exact Stage 3CB image")
    blob[po:pl] = baseline[po:pl]
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("restored region is not the dead 3BN thunk")

    # 3. prove no rel32 anywhere still reaches the retired region
    hits = []
    for s in pe["sections"]:
        start, end = s["rp"], s["rp"] + s["rs"]
        for off in range(start, min(end, len(blob)) - 5):
            if blob[off] not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from("<i", blob, off+1)[0]
            rva = s["va"] + (off - s["rp"])
            if PAYLOAD_RVA <= rva + 5 + rel < PAYLOAD_LIMIT:
                hits.append(hex(rva))
    if hits:
        raise SystemExit("retired region still referenced from " + ", ".join(hits))

    print(f"gate 0x{GATE_CALL_RVA:X} -> accepted 3BR 0x{S3BR_THUNK_RVA:X} "
          f"(fixed 4134.312 framing); per-axis payload retired; "
          f"selector un-hide stays LIVE -> 0x{LIVE_HIDE_X_RVA:X} "
          f"(fixed framing store still at 0x{FIXED_FRAMING_RVA:X})")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
