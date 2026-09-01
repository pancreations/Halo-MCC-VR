"""Stage 3BQ - reticle swapchain RTV creation retries with an explicit format.

Input: exact Stage 3BP DLL.  One 6-byte change: the inlined GetRtv create at
0x2A9DA (`mov rax,[rcx]` + `call [rax+0x48]`, null desc in r8) becomes a call
to s3bq_create_rtv + 1 nop.  See stage3bq_h4_reticle_rtv_typeless.S.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BP_SHA256 = \
    "4298f4a7b9997b93123534d9c15906eeb94fb17a544a5192a1a63723765af0c5"
PAYLOAD_RVA = 0x002F9A30            # 16-aligned, after the 3BP data (ends 0x2F9A28)
PAYLOAD_LIMIT = 0x002FA000
SPLICE_RVA = 0x0002A9DA
SPLICE_OLD = bytes.fromhex("488b01ff5048")   # mov rax,[rcx]; call [rax+0x48]
G_DEVICE_RVA = 0x002AE290
XR_FORMAT_RVA = 0x002AE2A0

CONTEXT = (
    # the site: device load, null desc, images[idx] load, then the create
    (0x0002A9CC, bytes.fromhex("488b0dbd382800"), "rcx = g_device (0x2AE290)"),
    (0x0002A9D3, bytes.fromhex("4533c0"), "r8d = 0 (null desc)"),
    (0x0002A9D6, bytes.fromhex("488b14f2"), "rdx = images[idx]"),
    (0x0002A9E0, bytes.fromhex("8b4c2450"), "site reloads after the call"),
    # Blit's inlined IsSrgb(g_xrFormat) proves the XR_FORMAT rva
    (0x00011F61, bytes.fromhex("8b0539c32900"), "Blit reads g_xrFormat (0x2AE2A0)"),
    # chain artifacts
    (0x00053921, bytes.fromhex("e8"), "3BP splice"),
    (0x002F98A0, bytes.fromhex("4883ec58"), "3BP selector present"),
    (0x00011E76, bytes.fromhex("e8"), "3BO fast-path call"),
    (0x00011EB6, bytes.fromhex("e8"), "3BO log-gate call"),
    (0x002FB992, bytes.fromhex("e849daffff"), "3BN gate call"),
    (0x002FBA58, bytes.fromhex("e8"), "3BM tap splice"),
    (0x002FA2D7, bytes.fromhex("e9"), "3BL splice"),
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
    return dict(n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BP_SHA256:
        raise SystemExit("wrong Stage3BP input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        if not expect:
            continue
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at 0x{rva:X}, "
                             f"got {bytes(blob[o:o+len(expect)]).hex()}")
    # the two rip-relative operands must resolve to the RVAs the payload uses
    if 0x2A9CC + 7 + struct.unpack_from("<i", blob, rva_off(pe, 0x2A9CF))[0] != G_DEVICE_RVA:
        raise SystemExit("g_device RVA mismatch")
    if 0x11F61 + 6 + struct.unpack_from("<i", blob, rva_off(pe, 0x11F63))[0] != XR_FORMAT_RVA:
        raise SystemExit("g_xrFormat RVA mismatch")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+6]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+6]).hex())
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bq_h4_reticle_rtv_typeless.S",
        PAYLOAD_RVA,
        defs=dict(G_DEVICE=G_DEVICE_RVA, XR_FORMAT=XR_FORMAT_RVA),
        want_symbols=("s3bq_create_rtv",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bq_create_rtv"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+6] = b"\xE8" + struct.pack("<i", thunk - (SPLICE_RVA + 5)) + b"\x90"
    print(f"splice 0x{SPLICE_RVA:X}: mov rax,[rcx]+call [rax+0x48] -> call 0x{thunk:X} + nop")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
