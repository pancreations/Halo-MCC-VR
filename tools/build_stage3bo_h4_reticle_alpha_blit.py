"""Stage 3BO - the captured Halo 4 reticle gets an alpha channel on the way
to the VR crosshair quad.

Input: exact Stage 3BN DLL.  Four changes:
  * 0x11E76 (10 bytes): Blit's inline fast-path decision -> call s3bo_fastpath
    + jmp 0x11E82 + 3 nops (0x11E80 `xor bl,bl`, a jump target, is kept);
  * 0x11EB6 (9 bytes): PERF log gate -> call s3bo_loggate + 4 nops;
  * 0x2A6770 (8 bytes): the blit pipeline's HLSL text pointer -> the new text;
  * payload (thunks + HLSL text) in the free 3AS page tail after 3BN.
See stage3bo_h4_reticle_alpha_blit.S.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BN_SHA256 = \
    "1e440f34218b3e86aa03fff9405c6ea1c71b15440f64229c25bd73ec222ddb6c"
IMAGE_BASE = 0x180000000
PAYLOAD_RVA = 0x002F94D0            # 16-aligned, after the 3BN private viewport (ends 0x2F94C8)
PAYLOAD_LIMIT = 0x002FA000
HLSL_RVA = 0x002F9540               # after the thunks (asserted below)
FASTPATH_SITE = 0x00011E76
FASTPATH_OLD = bytes.fromhex("837f140177 04b301eb02".replace(" ", ""))
FASTPATH_RESUME = 0x00011E82
LOGGATE_SITE = 0x00011EB6
LOGGATE_OLD = bytes.fromhex("0fb6c3" "3905b9482900")
HLSL_SLOT_RVA = 0x002A6770          # `static const char* src` in EnsureBlitPipeline
HLSL_OLD_RVA = 0x001B6570           # the shipped text (starts with the raw-string newline)
ACTIVE_TITLE_RVA = 0x002BA6C8       # byte TitleAdapter_GetActiveTitle reads; Halo4 = 4 (3BL)
LOGGED_PATH_RVA = 0x002A6778        # Blit's `static int loggedPath` (0x11EBF + 0x2948B9)

OLD_HLSL = b"""
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float lin(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps_linearize(VSOut i) : SV_Target
{
    float4 c = srcTex.Sample(smp, i.uv);
    return float4(lin(c.r), lin(c.g), lin(c.b), c.a);
}
float4 ps_pass(VSOut i) : SV_Target
{
    return srcTex.Sample(smp, i.uv);
}
"""

# Only the two pixel shaders change, and only for a 512x512 source (the
# reticle capture); a 3786x2730 eye blit returns `c` untouched.
NEW_HLSL = b"""
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float lin(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 fix(float4 c)
{
    uint w, h;
    srcTex.GetDimensions(w, h);
    if (w != 512 || h != 512) return c;
    float a = max(c.a, max(c.r, max(c.g, c.b)));
    return float4(a > 0 ? c.rgb / a : c.rgb, a);
}
float4 ps_linearize(VSOut i) : SV_Target
{
    float4 c = fix(srcTex.Sample(smp, i.uv));
    return float4(lin(c.r), lin(c.g), lin(c.b), c.a);
}
float4 ps_pass(VSOut i) : SV_Target
{
    return fix(srcTex.Sample(smp, i.uv));
}
"""

CONTEXT = (
    (0x00011E80, bytes.fromhex("32db"), "Blit: `xor bl,bl` jump target kept"),
    (0x00011E82, bytes.fromhex("488b0d0fc42900"), "Blit: resume `mov rcx,[g_context]`"),
    (0x00011EBF, bytes.fromhex("7451"), "Blit: `je` consuming the log-gate flags"),
    (0x00011ED6, bytes.fromhex("89059c482900"), "Blit: loggedPath store (0x2A6778)"),
    (0x002FB992, bytes.fromhex("e849daffff"), "3BN gate call -> 0x2F93E0"),
    (0x002FBA58, bytes.fromhex("e8"), "3BM tap splice"),
    (0x002FA2D7, bytes.fromhex("e9"), "3BL splice"),
    (0x00053921, bytes.fromhex("e8"), "3BI capture-selection splice"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o+8]).split(b"\0", 1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    base = struct.unpack_from("<Q", blob, coff + 20 + 24)[0]
    reloc_rva, reloc_size = struct.unpack_from("<II", blob, coff + 20 + 112 + 5 * 8)
    return dict(opt=coff+20, n=n, sections=secs, base=base,
                reloc=(reloc_rva, reloc_size))


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def has_dir64_reloc(blob, pe, rva):
    r, size = pe["reloc"]
    o = rva_off(pe, r); end = o + size
    while o + 8 <= end:
        page, bsize = struct.unpack_from("<II", blob, o)
        if bsize < 8: break
        for k in range(8, bsize, 2):
            e = struct.unpack_from("<H", blob, o + k)[0]
            if (e >> 12) == 10 and page + (e & 0xFFF) == rva:
                return True
        o += bsize
    return False


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BN_SHA256:
        raise SystemExit("wrong Stage3BN input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12 or pe["base"] != IMAGE_BASE:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at 0x{rva:X}, "
                             f"got {bytes(blob[o:o+len(expect)]).hex()}")
    fo = rva_off(pe, FASTPATH_SITE)
    if bytes(blob[fo:fo+10]) != FASTPATH_OLD:
        raise SystemExit("fast-path site unexpected: " + bytes(blob[fo:fo+10]).hex())
    lo = rva_off(pe, LOGGATE_SITE)
    if bytes(blob[lo:lo+9]) != LOGGATE_OLD:
        raise SystemExit("log-gate site unexpected: " + bytes(blob[lo:lo+9]).hex())
    # the displaced `cmp [rip+disp],eax` must resolve to the loggedPath we hand the payload
    if LOGGATE_SITE + 9 + struct.unpack_from("<i", blob, lo + 5)[0] != LOGGED_PATH_RVA:
        raise SystemExit("loggedPath RVA mismatch")
    so = rva_off(pe, HLSL_SLOT_RVA)
    if struct.unpack_from("<Q", blob, so)[0] != IMAGE_BASE + HLSL_OLD_RVA:
        raise SystemExit("HLSL pointer slot does not hold the shipped text")
    if not has_dir64_reloc(blob, pe, HLSL_SLOT_RVA):
        raise SystemExit("HLSL pointer slot has no DIR64 relocation")
    ho = rva_off(pe, HLSL_OLD_RVA)
    if bytes(blob[ho:ho+len(OLD_HLSL)+1]) != OLD_HLSL + b"\0":
        raise SystemExit("shipped HLSL text differs from the source of record")
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bo_h4_reticle_alpha_blit.S",
        PAYLOAD_RVA,
        defs=dict(ACTIVE_TITLE=ACTIVE_TITLE_RVA, LOGGED_PATH=LOGGED_PATH_RVA),
        want_symbols=("s3bo_fastpath", "s3bo_loggate"))
    if PAYLOAD_RVA + len(code) > HLSL_RVA:
        raise SystemExit(f"thunks too large: {len(code)}")
    fast = symbols["s3bo_fastpath"]; gate = symbols["s3bo_loggate"]
    text = NEW_HLSL + b"\0"
    if HLSL_RVA + len(text) > PAYLOAD_LIMIT:
        raise SystemExit("HLSL text too large")
    print(f"thunks {len(code)} bytes at 0x{PAYLOAD_RVA:X} (fastpath 0x{fast:X}, "
          f"loggate 0x{gate:X}); HLSL {len(text)} bytes at 0x{HLSL_RVA:X}")

    blob[po:po+len(code)] = code
    blob[rva_off(pe, HLSL_RVA):rva_off(pe, HLSL_RVA)+len(text)] = text
    blob[fo:fo+10] = (b"\xE8" + struct.pack("<i", fast - (FASTPATH_SITE + 5)) +
                      b"\xEB" + bytes([FASTPATH_RESUME - (FASTPATH_SITE + 7)]) +
                      b"\x90\x90\x90")
    blob[lo:lo+9] = b"\xE8" + struct.pack("<i", gate - (LOGGATE_SITE + 5)) + b"\x90" * 4
    struct.pack_into("<Q", blob, so, IMAGE_BASE + HLSL_RVA)
    print(f"0x{FASTPATH_SITE:X}: call 0x{fast:X}; jmp 0x{FASTPATH_RESUME:X}")
    print(f"0x{LOGGATE_SITE:X}: call 0x{gate:X}")
    print(f"0x{HLSL_SLOT_RVA:X}: text 0x{HLSL_OLD_RVA:X} -> 0x{HLSL_RVA:X}")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
