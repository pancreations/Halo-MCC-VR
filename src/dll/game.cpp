#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <MinHook.h>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "../common/log.h"
#include "../common/config.h"

// M1 head tracking. We hook the game's per-frame camera-update function and,
// each frame, overwrite the authoritative camera's forward/up vectors with the
// direction of the headset. Writing from inside the game's own frame (rather
// than poking memory from a thread) means our value lands at exactly the right
// moment and holds steady instead of flickering.
//
// The hooked function (RVA 0x2A628C for build 1.3528.0.0) is __fastcall(dst,
// src): it copies the camera from src (a double-buffered heap struct) into the
// static gun-camera. src+0x28 = forward (3 floats), src+0x34 = up (3 floats).
// See docs/RE-notes.md. These offsets are build-specific and must become AOB
// signatures before shipping.

namespace
{
    constexpr uintptr_t kCamCopyRva = 0x2A628C; // fastcall(dst, src) camera copy
    constexpr uintptr_t kSrcFwd = 0x28;         // forward vec offset in src
    constexpr uintptr_t kSrcUp = 0x34;          // up vec offset in src

    constexpr uintptr_t kSrcPos = 0x00;         // camera position (x,y,z) in src
    constexpr uintptr_t kSrcProjX = 0x68;       // horizontal projection/FOV scale
    constexpr uintptr_t kSrcProjY = 0x6C;       // vertical projection/FOV scale

    // Gun/overlay camera: first element of the engine's 4-slot camera-object
    // array (0x2820 bytes each; see docs/RE-notes.md). +0x30/+0x34 hold the
    // overlay frustum tangents (~0.858/0.874 = ~81 deg). The overlay is
    // stretched over the whole frame, so in the widened stereo raster
    // (~123 deg) the first-person weapon and HUD appear ~2x oversized unless
    // these tangents are rewritten to match the world projection.
    constexpr uintptr_t kGunCamRva = 0x2D2F680; // expected for build 1.3528
    constexpr uintptr_t kGunProjX = 0x30;
    constexpr uintptr_t kGunProjY = 0x34;

    std::atomic<bool> g_hooked{false};
    std::atomic<bool> g_renderHooked{false};
    std::atomic<bool> g_enabled{false};      // F2
    std::atomic<bool> g_needRecenter{true};   // F3 (yaw + position)
    std::atomic<bool> g_needPosRecenter{false}; // enabling leaning: position only, no yaw snap
    std::atomic<float> g_yawSign{-1.0f};       // F4  (default matches PSVR2 mapping)
    std::atomic<float> g_pitchSign{1.0f};      // F5
    std::atomic<float> g_pitchTrim{0.0f};      // F8/F9, radians
    std::atomic<bool> g_writeUp{true};         // F7
    std::atomic<bool> g_positional{true};      // M2: 6DOF head translation on by default; F6 toggles
    std::atomic<int> g_stereoEye{-1};           // M2: -1 mono, 0 left, 1 right

    // World scale: Halo world units per real meter (1 wu ~= 3.05 m), so ~0.33
    // gives roughly 1:1 leaning. Offset is clamped so a bad value can't fling
    // the camera through the level.
    std::atomic<float> g_worldScale{0.33f};
    std::atomic<float> g_projectionTanX{1.091595f};
    std::atomic<float> g_projectionTanY{1.114286f};
    std::atomic<float> g_renderHalfFovX{atanf(1.091595f)};
    std::atomic<float> g_renderHalfFovY{atanf(1.114286f)};

    std::atomic<uintptr_t> g_gunCamera{0};   // resolved from kGunCamRefSig
    // The overlay frustum is pinned to the exact world projection: the
    // first-person bones are camera-space positions in world units, so only an
    // exact match projects the weapon at the controller's true screen position.
    // Weapon size is a MESH scale (config gun_scale, Home/End), never a
    // frustum scale — 07-15 shipped both at once (2.0 frustum x 0.33 mesh) and
    // the gun shrank to ~1/6 size ("barely visible").

    // M3 VR aim: the game's own aim-driven camera forward, captured each frame
    // BEFORE the head-look overwrite. The XInput hook steers the game's aim
    // toward the right controller by comparing this with the controller ray.
    std::atomic<bool> g_vrAim{true};         // on by default; Insert toggles
    std::atomic<float> g_aimFwdX{1}, g_aimFwdY{0}, g_aimFwdZ{0};
    std::atomic<bool> g_aimSeen{false};

    // Yaw is relative (the game's heading is arbitrary, so we recenter it to
    // the head). Pitch is absolute (head-level == game-level), which avoids
    // capturing a bad reference on recenter.
    float g_headYawRef = 0;
    float g_gameYawRef = 0;
    float g_headPosRef[3] = {0, 0, 0}; // headset position (m) captured at recenter

    using CamCopyFn = void*(__fastcall*)(void* dst, void* src);
    CamCopyFn g_origCamCopy = nullptr;
    using RenderViewFn = void(__fastcall*)(void* view);
    RenderViewFn g_origRenderView = nullptr;
    using PrepareViewFn = void(__fastcall*)(void* view, int viewIndex);
    PrepareViewFn g_prepareView = nullptr;
    using BuildViewportFn = void(__fastcall*)(void* camera, void* temporary);
    using BuildMatricesFn = void(__fastcall*)(void* camera, void* temporary, void* output, float scale);
    BuildViewportFn g_buildViewport = nullptr;
    BuildMatricesFn g_buildMatrices = nullptr;

    // Final first-person pose records proved by offline RE: four players, two
    // held-weapon slots, up to 64 composed 0x34-byte bone matrices per slot.
    struct BoneMatrix;
    using ComposeBonesFn = void(__fastcall*)(void*, int, int, BoneMatrix*, void*, void*);
    using ComposeSpecialBonesFn = void(__fastcall*)(void*, BoneMatrix*, void*, void*, int, int);
    ComposeBonesFn g_origComposeBones = nullptr;
    ComposeSpecialBonesFn g_origComposeSpecialBones = nullptr;

    uint32_t* g_engineTlsIndex = nullptr;
    unsigned char** g_animationTagData = nullptr;
    // Halo real_matrix4x3: uniform scale, then forward/left/up basis vectors,
    // then translation. The first headset build incorrectly put scale last,
    // shifting every basis read by one float and making weapon pieces diverge.
    struct BoneMatrix { float scale; float rotation[9]; float translation[3]; };
    static_assert(sizeof(BoneMatrix) == 0x34);

    // Locate the first-person weapon slot that owns a composed bone array.
    // Pointer compares only, so it is safe to call before composition too.
    bool FindFirstPersonWeapon(BoneMatrix* bones, int& outSlot, unsigned char*& outWeapon)
    {
        if (!bones || !g_engineTlsIndex) return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones)
            { outSlot=candidate; outWeapon=w; return true; }
        }
        return false;
    }

    void RotateByQuat(const float q[4], const float in[3], float out[3])
    {
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        const float tx=2*(y*in[2]-z*in[1]), ty=2*(z*in[0]-x*in[2]), tz=2*(x*in[1]-y*in[0]);
        out[0]=in[0]+w*tx+(y*tz-z*ty);
        out[1]=in[1]+w*ty+(z*tx-x*tz);
        out[2]=in[2]+w*tz+(x*ty-y*tx);
    }

    float Clamp(float v, float lo, float hi);
    float WrapPi(float a);

    void BuildTrackedGameBasis(const float q[4], bool head, float basis[9])
    {
        const float xrForward[3]={0,0,-1}, xrUp[3]={0,1,0};
        float f[3],u[3];
        RotateByQuat(q,xrForward,f);
        RotateByQuat(q,xrUp,u);
        const float yaw=atan2f(f[0],-f[2]);
        const float pitch=asinf(Clamp(f[1],-1.0f,1.0f));

        // Roll is measured around the tracked forward axis exactly as it is in
        // ApplyHeadLook, so head and controller pass through the same mapping.
        float rx=-f[2], rz=f[0];
        float rl=sqrtf(rx*rx+rz*rz);
        if (rl<1e-4f) rl=1e-4f;
        rx/=rl; rz/=rl;
        const float nux=-f[1]*rz, nuy=rl, nuz=f[1]*rx;
        const float roll=atan2f(u[0]*rx+u[2]*rz,u[0]*nux+u[1]*nuy+u[2]*nuz);
        const float gy=g_gameYawRef+g_yawSign.load()*WrapPi(yaw-g_headYawRef);
        const float gp=Clamp(g_pitchSign.load()*pitch+(head?g_pitchTrim.load():0.0f),-1.5f,1.5f);
        const float cp=cosf(gp),sp=sinf(gp),cy=cosf(gy),sy=sinf(gy);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    // Basis columns (forward,left,up) from game-frame yaw/pitch/roll.
    void BasisFromAngles(float yaw, float pitch, float roll, float basis[9])
    {
        const float cp=cosf(pitch),sp=sinf(pitch),cy=cosf(yaw),sy=sinf(yaw);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    bool GetControllerFirstPersonTransform(int slot, float target[3], float desired[9])
    {
        if (slot<0 || slot>1 || !g_vrAim.load() || !g_enabled.load()) return false;
        if (!g_aimSeen.load()) return false;
        float hq[4],hp[3],cq[4],cp[3];
        if (!VR_GetHeadPose(hq,hp) ||
            !(slot == 0 ? VR_GetRightControllerPose(cq,cp) : VR_GetLeftControllerPose(cq,cp))) return false;

        // The bank records are CAMERA-LOCAL (probe log 04:17: wrist at
        // centimeter offsets, camera_control ~identity), and the render root
        // is the camera we write (the head). Head-relative is the correct
        // anchor; the earlier "opposite-to-head drift" that pushed us to an
        // aim-relative experiment was the double-buffer BLEND artifact (the
        // renderer lerps two sim snapshots and only one carried our pose —
        // the user's "weird in-between state"). Both halves are written now.
        const float ih[4]={-hq[0],-hq[1],-hq[2],hq[3]};
        const float dp[3]={cp[0]-hp[0],cp[1]-hp[1],cp[2]-hp[2]};
        float rp[3]; RotateByQuat(ih,dp,rp);
        const float s=g_worldScale.load();
        target[0]=-rp[2]*s; target[1]=-rp[0]*s; target[2]=rp[1]*s; // (fwd,left,up)
        float headBasis[9],controllerBasis[9];
        BuildTrackedGameBasis(hq,true,headBasis);
        BuildTrackedGameBasis(cq,false,controllerBasis);
        float rel[9];
        for(int column=0;column<3;++column)
            for(int row=0;row<3;++row)
                rel[column*3+row]=controllerBasis[column*3+0]*headBasis[row*3+0]+
                                  controllerBasis[column*3+1]*headBasis[row*3+1]+
                                  controllerBasis[column*3+2]*headBasis[row*3+2];
        float mount[9];
        BasisFromAngles(g_config.gun_yaw_deg*0.0174533f,
                        g_config.gun_pitch_deg*0.0174533f,
                        g_config.gun_roll_deg*0.0174533f,mount);
        for(int column=0;column<3;++column)
            for(int row=0;row<3;++row)
                desired[column*3+row]=rel[0*3+row]*mount[column*3+0]+
                                      rel[1*3+row]*mount[column*3+1]+
                                      rel[2*3+row]*mount[column*3+2];

        return true;
    }

    // THE LEVER, HaloCEVR's pattern (root fed INTO composition) applied to the
    // input the mesh actually reads. Proven by elimination across headset
    // tests + the composer disassembly (0x23200C):
    //   output[0] = defaultsRoot * sourceRecord[0]            (0x23203B)
    //   output[i] = output[parent[i]] * sourceRecord[i]       (0x232099)
    // - Writing `defaults` (03:27 build) moved ONLY the muzzle flash: the
    //   composed output feeds markers/effects, and nothing else.
    // - The visible MESH recomposes from the 0x20-byte orientation bank — the
    //   composers' `source` argument — with its own camera-derived root
    //   (that is how it stays head-glued in vanilla). Editing the bank root
    //   is the only lever that has ever moved the actual gun in a headset.
    // So write BANK RECORD 0 (real_quaternion i,j,k,w + translation + scale)
    // with the controller pose expressed in the head-camera frame; the mesh's
    // own camera root then cancels the head and lands the gun on the hand.
    // The composed output inherits the same pose through record 0, so the
    // muzzle flash stays correct WITHOUT touching `defaults` (writing both
    // would double-apply the transform).
    //
    // No new hooks: 0x20 bytes into data the engine hands us and immediately
    // consumes. Detouring additional engine functions is what crashed the game.
    // Write the controller pose into the first bank record on the wrist's
    // ancestry chain BELOW the root (found by walking the tag node table's
    // parent words, never guessed). Rationale, from tonight's falsifications:
    // the renderer rebuilds the mesh from the bank's CHILD records under its
    // own camera-derived root — record 0 is replaced by that root (why the
    // record-0 test moved only the camera feedback, never the mesh), children
    // are kept. The game-thread composition consumes the same record, so the
    // markers/flash inherit the pose with no separate camera_control edit.
    bool ApplyControllerToBankChild(void* model, BoneMatrix* output, float* bank)
    {
        if (!model || !bank || !output || !g_animationTagData || !*g_animationTagData) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            { wristIndex=i; break; }
        if (wristIndex<0) return false;
        // Walk wrist -> root; stop at the child whose parent IS the root (0).
        int child=wristIndex;
        for(int guard=0; guard<16; ++guard)
        {
            const int parent=*reinterpret_cast<const int16_t*>(records+child*0x20+8);
            if (parent<0 || parent>=count) return false;
            if (parent==0) break;
            child=parent;
        }
        float* record=reinterpret_cast<float*>(bank)+static_cast<ptrdiff_t>(child)*8;
        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        // Never hand the engine a non-finite value: that is how a bad frame
        // becomes a crash deep inside the renderer instead of a visible glitch.
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;

        // Column-basis matrix (desired[c*3+r], columns = forward/left/up) ->
        // quaternion, robust in all four trace branches.
        const float m00=desired[0],m10=desired[1],m20=desired[2];
        const float m01=desired[3],m11=desired[4],m21=desired[5];
        const float m02=desired[6],m12=desired[7],m22=desired[8];
        float qx,qy,qz,qw;
        const float tr=m00+m11+m22;
        if (tr>0.0f)
        { const float s=sqrtf(tr+1.0f)*2.0f; qw=0.25f*s; qx=(m21-m12)/s; qy=(m02-m20)/s; qz=(m10-m01)/s; }
        else if (m00>m11 && m00>m22)
        { const float s=sqrtf(1.0f+m00-m11-m22)*2.0f; qw=(m21-m12)/s; qx=0.25f*s; qy=(m01+m10)/s; qz=(m02+m20)/s; }
        else if (m11>m22)
        { const float s=sqrtf(1.0f+m11-m00-m22)*2.0f; qw=(m02-m20)/s; qx=(m01+m10)/s; qy=0.25f*s; qz=(m12+m21)/s; }
        else
        { const float s=sqrtf(1.0f+m22-m00-m11)*2.0f; qw=(m10-m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s; qz=0.25f*s; }
        if (!isfinite(qx)||!isfinite(qy)||!isfinite(qz)||!isfinite(qw)) return false;

        // record[7] (scale) is deliberately NOT written anywhere: camera_control
        // descends from this node, and a scale here zooms the game camera —
        // the user's "scale control scales the entire world" report. The
        // engine's animated value is preserved; a mesh-only size lever is an
        // open follow-up (gun_scale currently has no effect on the mesh).
        auto writeRecord=[&](float* r)
        {
            r[0]=qx; r[1]=qy; r[2]=qz; r[3]=qw; // i,j,k,w
            r[4]=target[0]; r[5]=target[1]; r[6]=target[2];
        };
        writeRecord(record);
        // The renderer interpolates TWO sim snapshots of these records (the
        // engine's 60Hz-sim -> 120Hz-render path). Writing only the half being
        // composed leaves the other half head-glued and the visible gun lands
        // midway between head and hand — the reported "weird in-between
        // state". Write the sibling half too (banks at TLS+0x560, one 0x1000
        // bank per slot, two 0x800 halves of 64 records each).
        auto** tlsSlots=reinterpret_cast<void**>(__readgsqword(0x58));
        auto* tls2=tlsSlots?reinterpret_cast<unsigned char*>(tlsSlots[*g_engineTlsIndex]):nullptr;
        auto* banks=tls2?*reinterpret_cast<unsigned char**>(tls2+0x560):nullptr;
        if (banks)
        {
            auto* slotBank=banks+static_cast<size_t>(slot)*0x1000;
            auto* half=reinterpret_cast<unsigned char*>(bank);
            if (half==slotBank || half==slotBank+0x800)
                writeRecord(reinterpret_cast<float*>(slotBank+((half-slotBank)^0x800))
                            +static_cast<ptrdiff_t>(child)*8);
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: slot %d bank CHILD node %d (wrist ancestor under root) bound to the %s controller (scale %.2f)",
                slot,child,slot==0?"right":"left",meshScale);
        return true;
    }

    bool ApplyControllerToComposedBones(void* model, BoneMatrix* bones)
    {
        if (!model || !bones || !g_engineTlsIndex || !g_animationTagData || !*g_animationTagData)
            return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        int slot=-1;
        unsigned char* weapon=nullptr;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones) { slot=candidate; weapon=w; break; }
        }
        if (!weapon) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        // Runtime animation node records use the raw global string index, not
        // the packed map StringId.  This is also how the engine calls its own
        // node finder at 0x2323AC (for example, edx=0xD9/0x1D9).  r_hand is
        // global string index 0xA6; using 0x0C0000A6 made every pose silently
        // miss the wrist and left the view model attached to the camera.
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            {
                wristIndex=i;
                break;
            }
        if (wristIndex<0)
        {
            static std::atomic<bool> loggedMissing{false};
            if (!loggedMissing.exchange(true))
            {
                LOG("M3: r_hand node 0x%X absent from first-person skeleton (%d bones; first nodes %X,%X,%X,%X)",
                    kRightHandStringIndex,count,
                    count>0?*reinterpret_cast<const uint32_t*>(records):0,
                    count>1?*reinterpret_cast<const uint32_t*>(records+0x20):0,
                    count>2?*reinterpret_cast<const uint32_t*>(records+0x40):0,
                    count>3?*reinterpret_cast<const uint32_t*>(records+0x60):0);
            }
            return false;
        }
        // Halo resolves the per-weapon camera_control node (string index 0xD9)
        // itself and stores its bone index here. Unlike a guessed `gun` name,
        // this exists in the live 43-node skeleton and follows each weapon's
        // authored barrel orientation.
        const int selectedIndex=*reinterpret_cast<int*>(weapon+0x11A4);
        const int orientationIndex=selectedIndex>=0&&selectedIndex<count?selectedIndex:wristIndex;

        // Proven offline (2026-07-15): the root transform handed to the composer
        // at halo3+0x2C4626 is scale 1, rotation IDENTITY, translation ~0, so
        // these composed bones are CAMERA-space. Log the real values once to
        // confirm the magnitudes match that reading in the live game.
        static std::atomic<bool> loggedBones{false};
        if (!loggedBones.exchange(true))
            LOG("M3 PROBE: composed bones camera-space check: wrist[%d] t=(%.4f,%.4f,%.4f) "
                "scale=%.3f | camera_control[%d] t=(%.4f,%.4f,%.4f) fwd=(%.3f,%.3f,%.3f)",
                wristIndex,bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                bones[wristIndex].translation[2],bones[wristIndex].scale,
                selectedIndex,bones[orientationIndex].translation[0],
                bones[orientationIndex].translation[1],bones[orientationIndex].translation[2],
                bones[orientationIndex].rotation[0],bones[orientationIndex].rotation[1],
                bones[orientationIndex].rotation[2]);

        if (g_config.weapon_probe)
        {
            // DECISIVE PROBE. No controller, no head, no rotation: shove the
            // whole composed assembly a fixed 0.3 world units (~1 m) to the
            // LEFT (camera space: +x forward, +y left, +z up). The visible gun
            // either moves or it does not, and that single bit tells us whether
            // the mesh reads these matrices at all — which the disassembly
            // cannot, because our edits provably reach both the effects anchor
            // and the mesh's own render packet.
            for(int i=0;i<count;++i) bones[i].translation[1]+=0.3f;
            static std::atomic<bool> loggedProbe{false};
            if (!loggedProbe.exchange(true))
                LOG("M3 PROBE ACTIVE: all %d bones of slot %d pushed +0.3 left; "
                    "if the GUN MESH does not move, it does not read weapon+0x4A4",
                    count,slot);
            return true;
        }

        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        const float anchor[3]={bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                               bones[wristIndex].translation[2]};
        float current[9];
        for(int column=0;column<3;++column)
        {
            float len=0;
            for(int j=0;j<3;++j)
            {
                current[column*3+j]=bones[orientationIndex].rotation[column*3+j];
                len+=current[column*3+j]*current[column*3+j];
            }
            len=sqrtf(len);
            if (len<0.001f) return false;
            for(int j=0;j<3;++j) current[column*3+j]/=len;
        }
        auto rotateDelta=[&](const float in[3],float out[3])
        {
            float component[3]{};
            for(int column=0;column<3;++column)
                for(int j=0;j<3;++j)
                    component[column]+=in[j]*current[column*3+j];
            for(int j=0;j<3;++j)
                out[j]=desired[j]*component[0]+desired[3+j]*component[1]+desired[6+j]*component[2];
        };

        // Halo 3's weapon vertices are weighted across r_hand, camera_control,
        // root and weapon-specific nodes. Moving only the wrist descendants
        // leaves some weights head-driven, producing the reported dual
        // head+hand motion. Apply one rigid delta and one uniform mesh scale
        // to the complete composed assembly so every influence agrees. The
        // bones are camera-space world units and the overlay frustum matches
        // the world projection, so 1.0 draws the weapon at authored size.
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        for(int i=0;i<count;++i)
        {
            const float d[3]={bones[i].translation[0]-anchor[0],bones[i].translation[1]-anchor[1],
                              bones[i].translation[2]-anchor[2]};
            float rt[3]; rotateDelta(d,rt);
            for(int j=0;j<3;++j) bones[i].translation[j]=target[j]+rt[j]*meshScale;
            bones[i].scale*=meshScale;
            for(int column=0;column<3;++column)
            {
                float rotated[3]; rotateDelta(&bones[i].rotation[column*3],rotated);
                for(int j=0;j<3;++j) bones[i].rotation[column*3+j]=rotated[j];
            }
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: complete first-person slot %d bound to %s controller (%d bones, wrist %d, camera_control %d, scale %.2f)",
                slot,slot==0?"right":"left",count,wristIndex,selectedIndex,meshScale);
        return true;
    }

    void SuppressGameCrosshair()
    {
        if (!g_enabled.load() || !g_engineTlsIndex) return;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return;
        auto* chud=*reinterpret_cast<unsigned char**>(tls+0x220);
        if (!chud) return;
        chud[0x146]=0; // engine script command chud_show_crosshair(false)
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: original head-locked CHUD crosshair suppressed");
    }

    thread_local bool g_insideSpecialCompose=false;

    // Place the assembly by writing the root BEFORE the engine composes.
    // Post-editing the composed output is retired: it is downstream of the
    // engine's own weapon-lag pass (0x2C484B), which rotates every bone except
    // camera_control and so overwrote the mesh while leaving the muzzle flash
    // on our pose — exactly the reported split. `weapon_probe` still drives the
    // old output path, and only that, as a diagnostic.
    // THE MESH FIX — a call-site patch, not a detour (2026-07-15 ~04:00).
    // Proven chain, all read offline: the visible first-person mesh is built by
    // the object-node recomposer at halo3+0x341768 (single caller 0x3424DD),
    // which dequantizes the object's compressed animation and roots the chain
    // with `call 0x3453DC` at +0x341A5B — a generic object-root getter with 56
    // callers that fabricates {MakeTransformFromXZ(fwd@+0x5C, up@+0x68),
    // pos@+0x50} from the object datum. The FP arms/weapon objects sit exactly
    // at the camera every frame; that collocation IS the head-glue, and it is
    // also how the shim identifies them without knowing their handles.
    //
    // We patch the 4 aligned displacement bytes of that ONE call (atomic
    // InterlockedExchange, installed at DLL load before any level runs) to a
    // 12-byte trampoline that reaches FpRootShim. The shim calls the REAL
    // getter, and only if the returned root is camera-collocated replaces it
    // with the controller's world pose — a write into a STACK buffer the
    // renderer consumes immediately. No engine function is detoured, none of
    // the other 55 callers are affected, and no simulation state is touched.
    // Failure mode if MCC updates: signature miss -> log + gun stays glued.
    using FpRootFn = BoneMatrix*(__fastcall*)(uint16_t index, BoneMatrix* out);
    FpRootFn g_realFpRoot = nullptr;

    // THE HEAD-GLUE, finally acted on safely: right after composition, the FP
    // evaluator loops every bone EXCEPT camera_control (cmp r9d,[rdi+0x11A4])
    // and rotates it via `call 0x120DF8` at halo3+0x2C485B with a camera-
    // pitch/turn matrix. That is the exact flash-vs-mesh partition observed in
    // every headset test tonight. Detouring 0x120DF8 crashes on level load
    // (proven, banned); patching THIS ONE CALL SITE affects no other caller —
    // the same aligned-disp32 technique that survived a full session at
    // 0x341A5B. The shim skips the rotation only for bone addresses inside an
    // assembly we re-rooted this frame (thread_local ranges, same thread that
    // composes), and forwards everything else to the real function untouched.
    using SwayApplyFn = void(__fastcall*)(void* sway, BoneMatrix* bone);
    SwayApplyFn g_realSwayApply = nullptr;

    // CRASH LESSON (both fatal errors tonight, same root cause): halo3.dll is
    // LTCG-optimized — the sway loop keeps its counter (r9d), bone pointer
    // (r8) and count (r10d) LIVE IN VOLATILE REGISTERS across `call 0x120DF8`,
    // because the compiler knows that function never touches them. ANY
    // compiled C/C++ interposition (a MinHook detour or a C++ shim) clobbers
    // those registers and corrupts the caller -> wild writes -> fatal error on
    // level load. Interposing engine-internal calls therefore requires a
    // hand-assembled shim restricted to registers the caller provably treats
    // as dead — here, only RAX (verified: reloaded/unused after the call).
    //
    // The emitted shim (see InstallHook) compares rdx (the bone) against
    // these bounds and returns without rotating when it lies inside an
    // assembly ApplyControllerToRoot re-rooted this frame; everything else
    // tail-jumps to the real rotator. Same game thread writes and reads the
    // bounds (compose -> sway loop), so there is no race.
    alignas(8) volatile uintptr_t g_fpSkipBounds[4] = {0, 0, 0, 0}; // lo0,hi0,lo1,hi1
    std::atomic<float> g_camX{0}, g_camY{0}, g_camZ{0};
    std::atomic<bool> g_camValid{false};

    bool ControllerWorldPose(float basis[9], float pos[3], float& scale)
    {
        if (!g_vrAim.load() || !g_enabled.load() || !g_camValid.load()) return false;
        float hq[4], hp[3], cq[4], cp[3];
        if (!VR_GetHeadPose(hq, hp) || !VR_GetRightControllerPose(cq, cp)) return false;
        BuildTrackedGameBasis(cq, false, basis); // controller basis, game world axes
        float headBasis[9];
        BuildTrackedGameBasis(hq, true, headBasis);
        const float ih[4] = {-hq[0], -hq[1], -hq[2], hq[3]};
        const float dp[3] = {cp[0]-hp[0], cp[1]-hp[1], cp[2]-hp[2]};
        float rp[3];
        RotateByQuat(ih, dp, rp);
        const float s = g_worldScale.load();
        const float off[3] = {-rp[2]*s, -rp[0]*s, rp[1]*s}; // (fwd,left,up) comps
        const float cam[3] = {g_camX.load(), g_camY.load(), g_camZ.load()};
        for (int j = 0; j < 3; ++j)
            pos[j] = cam[j] + headBasis[0+j]*off[0] + headBasis[3+j]*off[1] + headBasis[6+j]*off[2];
        scale = Clamp(g_config.gun_scale, 0.3f, 3.0f);
        for (int j = 0; j < 9; ++j) if (!isfinite(basis[j])) return false;
        for (int j = 0; j < 3; ++j) if (!isfinite(pos[j])) return false;
        return true;
    }

    BoneMatrix* __fastcall FpRootShim(uint16_t index, BoneMatrix* out)
    {
        BoneMatrix* r = g_realFpRoot(index, out);
        if (!out || !g_camValid.load()) return r;
        const float dx = out->translation[0] - g_camX.load();
        const float dy = out->translation[1] - g_camY.load();
        const float dz = out->translation[2] - g_camZ.load();
        if (dx*dx + dy*dy + dz*dz > 0.15f*0.15f) return r; // not camera-glued
        float basis[9], pos[3], scale;
        if (!ControllerWorldPose(basis, pos, scale)) return r;
        out->scale *= scale;
        memcpy(out->rotation, basis, sizeof(basis));
        memcpy(out->translation, pos, sizeof(pos));
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: FP MESH root re-anchored to the controller (object %u was camera-collocated)", index);
        return r;
    }

    // The 03:27 lever, restored: write the composers' `defaults` root. Proven
    // side-effect-free in the headset and it puts the muzzle flash/markers on
    // the controller. Does NOT move the mesh (that consumer is still unfound).
    bool ApplyControllerToRoot(BoneMatrix* output, BoneMatrix* root)
    {
        if (!root || !output) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;
        root->scale=meshScale;
        memcpy(root->rotation,desired,sizeof(desired));
        memcpy(root->translation,target,sizeof(target));
        // This assembly now carries the controller pose; exempt exactly these
        // bones from the engine's camera pitch/turn rotation (emitted shim).
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count>0 && count<=64)
        {
            g_fpSkipBounds[slot*2+0]=reinterpret_cast<uintptr_t>(output);
            g_fpSkipBounds[slot*2+1]=g_fpSkipBounds[slot*2+0]+static_cast<size_t>(count)*sizeof(BoneMatrix);
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: first-person slot %d rooted to the %s controller (markers/flash lever, scale %.2f)",
                slot,slot==0?"right":"left",meshScale);
        return true;
    }

    // BANK WRITES ARE BANNED (2026-07-15, 03:4x headset result): writing the
    // controller pose into bank record 0 did NOT move the mesh, but it DID
    // bleed the wrist into the body/camera — record 0 propagates into
    // camera_control, which the game reads back to drive the camera. The
    // ApplyControllerToBankRoot helper is intentionally no longer called;
    // kept only as documentation of the falsified lever.
    void __fastcall ComposeBonesHook(void* model, int start, int count, BoneMatrix* output,
                                     void* source, void* defaults)
    {
        if (!g_insideSpecialCompose)
        {
            g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
            ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        }
        g_origComposeBones(model,start,count,output,source,defaults);
        // 04:17 falsified the output rewrite as a mesh lever for good (43-bone
        // rewrite confirmed running; mesh unmoved; "wrist moves the world" =
        // camera_control feedback). Probe-only again. The defaults-root write
        // is retired for the same reason: both only fed markers + the camera.
        if (!g_insideSpecialCompose && g_config.weapon_probe)
            ApplyControllerToComposedBones(model,output);
    }
    void __fastcall ComposeSpecialBonesHook(void* model, BoneMatrix* output, void* source,
                                            void* defaults, int firstSpecial, int secondSpecial)
    {
        g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
        ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        g_insideSpecialCompose=true;
        g_origComposeSpecialBones(model,output,source,defaults,firstSpecial,secondSpecial);
        g_insideSpecialCompose=false;
        if (g_config.weapon_probe) ApplyControllerToComposedBones(model,output);
    }

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

    // Rodrigues rotation of v (in place) about the unit axis by the angle
    // whose cosine/sine are given.
    void RotateAboutAxis(float* v, const float* axis, float cosA, float sinA)
    {
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float c[3] = {axis[1] * v[2] - axis[2] * v[1],
                            axis[2] * v[0] - axis[0] * v[2],
                            axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * cosA + c[i] * sinA + axis[i] * d * (1.0f - cosA);
    }

    void ApplyHeadLook(void* src)
    {
        if (!src)
            return;

        float q[4], hpos[3];
        if (!VR_GetHeadPose(q, hpos))
            return;
        // Head forward (OpenXR: -Z forward, +Y up).
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float hfx = -2.0f * (w * y + x * z);
        const float hfy =  2.0f * (w * x - y * z);
        const float hfz = -(1.0f - 2.0f * (x * x + y * y));
        const float hy = atan2f(hfx, -hfz);
        const float hp = asinf(Clamp(hfy, -1.0f, 1.0f));

        // Head roll around the forward axis. Compare the headset's actual up
        // vector with a horizon-level up vector at the same yaw/pitch. M1 used
        // only the latter, so rolling your head made the world appear to tilt
        // with you instead of remaining fixed in space.
        const float hux = 2.0f * (x * y - w * z);
        const float huy = 1.0f - 2.0f * (x * x + z * z);
        const float huz = 2.0f * (y * z + w * x);
        float hrx = -hfz, hrz = hfx; // horizon-right = cross(head forward, room up)
        float hrLen = sqrtf(hrx * hrx + hrz * hrz);
        if (hrLen < 1e-4f) hrLen = 1e-4f;
        hrx /= hrLen; hrz /= hrLen;
        const float hnux = -hfy * hrz;
        const float hnuy = hrLen;
        const float hnuz = hfy * hrx;
        const float headRoll = atan2f(hux * hrx + huz * hrz,
                                      hux * hnux + huy * hnuy + huz * hnuz);

        float* fwd = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcFwd);
        float* up = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcUp);
        float* pos = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcPos);

        if (g_needRecenter.exchange(false))
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]); // align current head to current heading
            g_headYawRef = hy;
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
            g_needPosRecenter = false;
            LOG("head tracking recentered (game yaw %.1f deg)", g_gameYawRef * 57.2958f);
        }
        else if (g_needPosRecenter.exchange(false))
        {
            // Enabling leaning: capture the neutral head position only, so the
            // aim/yaw baseline is left untouched (no view snap).
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
        }

        // Rotation: yaw relative + recenter, pitch absolute + trim.
        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(), -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);

        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            const float cr = cosf(headRoll), sr = sinf(headRoll);
            // Horizon-level up plus roll toward the camera's right vector.
            up[0] = (-sgp * cgy) * cr + sgy * sr;
            up[1] = (-sgp * sgy) * cr - cgy * sr;
            up[2] = cgp * cr;
        }

        // Position (leaning): shift the camera by the headset's room-space move,
        // decomposed in the head's horizontal frame and re-applied in the game's
        // frame so it stays correct as you turn. Added to the game's own
        // position each frame (the sim rewrites pos before our hook, so this
        // does not accumulate).
        if (g_positional.load())
        {
            const float dx = hpos[0] - g_headPosRef[0];
            const float dy = hpos[1] - g_headPosRef[1];
            const float dz = hpos[2] - g_headPosRef[2];
            float hlen = sqrtf(hfx * hfx + hfz * hfz);
            if (hlen < 1e-4f) hlen = 1e-4f;
            const float hfhx = hfx / hlen, hfhz = hfz / hlen; // head forward (horizontal)
            const float fwdComp = dx * hfhx + dz * hfhz;       // room move along look dir
            const float rightComp = dx * (-hfhz) + dz * hfhx;  // room move to the right
            const float s = g_worldScale.load();
            float ox = (cgy * fwdComp + sgy * rightComp) * s;  // game forward/right at gy
            float oy = (sgy * fwdComp - cgy * rightComp) * s;
            float oz = dy * s;
            ox = Clamp(ox, -1.5f, 1.5f); oy = Clamp(oy, -1.5f, 1.5f); oz = Clamp(oz, -1.5f, 1.5f);
            pos[0] += ox; pos[1] += oy; pos[2] += oz;
        }

        // M2 alternate-eye proof: offset only the render camera by half the
        // measured PSVR2 IPD. Halo right in the horizontal plane is
        // (sin(yaw), -cos(yaw), 0). This does not accumulate because the game
        // rewrites the source camera before every call.
        // Per-eye separation is applied later by RenderViewHook to the compact
        // render-only camera. Keeping it out of the authoritative source avoids
        // feeding stereo offsets back into simulation or temporal history.
    }

    // M3: snap/smooth turning from the right Sense stick. Rotating the yaw
    // reference turns the head-locked view instantly, and the hand-steered aim
    // follows because its target is expressed relative to the same reference.
    void ApplyVrTurn()
    {
        if (!g_vrAim.load())
            return;
        VrPadState pad;
        VR_GetPadState(pad);
        if (!pad.valid)
            return;
        static DWORD lastMs = GetTickCount();
        const DWORD now = GetTickCount();
        float dt = (now - lastMs) / 1000.0f;
        lastMs = now;
        if (dt > 0.1f) dt = 0.1f;
        const float x = pad.turnX; // stick right = turn right = yaw decreases
        if (g_config.turn_smooth)
        {
            if (fabsf(x) > 0.15f)
                g_gameYawRef = WrapPi(g_gameYawRef -
                    x * (g_config.turn_smooth_deg_s / 57.2958f) * dt);
        }
        else
        {
            static bool latched = false; // one snap per stick flick
            if (!latched && fabsf(x) > 0.6f)
            {
                g_gameYawRef = WrapPi(g_gameYawRef -
                    (x > 0 ? 1.0f : -1.0f) * g_config.turn_snap_deg / 57.2958f);
                latched = true;
            }
            else if (fabsf(x) < 0.3f)
                latched = false;
        }
    }

    void* __fastcall CamCopyHook(void* dst, void* src)
    {
        SuppressGameCrosshair();
        // M2 tracing: this function copies src+0x68/+0x6C into the compact
        // render camera at dst+0x28/+0x2C. Record only the first few calls so
        // we can distinguish world, weapon, and other camera passes without
        // producing a frame-sized log forever.
        static std::atomic<unsigned> traceCount{0};
        if (src)
        {
            g_projectionTanX.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjX));
            g_projectionTanY.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjY));
            // Camera buffers are heap-allocated and move on level changes;
            // log every new one so external tools (camscan aimwrite) can
            // always read the current address from the log.
            static void* seenSrc[16]{};
            static unsigned seenSrcCount = 0;
            bool newSrc = true;
            for (unsigned i = 0; i < seenSrcCount; ++i)
                if (seenSrc[i] == src) { newSrc = false; break; }
            if (newSrc && seenSrcCount < 16)
                seenSrc[seenSrcCount++] = src;
            const unsigned trace = traceCount.fetch_add(1);
            if (trace < 24 || newSrc)
            {
                const float* pos = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                const float projX = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjX);
                const float projY = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjY);
                LOG("M2 camera copy %u: dst=%p src=%p pos=(%.2f,%.2f,%.2f) proj=(%.6f,%.6f)",
                    trace, dst, src, pos[0], pos[1], pos[2], projX, projY);
            }
        }
        if (src)
        {
            // The game recomputes this forward from its aim state every frame,
            // so pre-overwrite it equals the true aim direction (bullets follow
            // it even while head look repaints the view).
            const float* fwd = reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcFwd);
            g_aimFwdX.store(fwd[0]); g_aimFwdY.store(fwd[1]); g_aimFwdZ.store(fwd[2]);
            g_aimSeen = true;
        }
        if (g_enabled.load())
        {
            ApplyVrTurn();
            // The head pose LIVES in this authoritative camera (the proven M3
            // regime). A 07-15 experiment saved/restored the original values
            // around the copy so gameplay would keep the aim pose — but the
            // first-person bone frame is head-camera-relative, and splitting
            // the two frames made the hand-anchored weapon visibly pick up
            // both head and aim motion. Do not scope this write again.
            ApplyHeadLook(src);
            if (src)
            {
                // Post-head-look camera position (includes leaning): the
                // reference FpRootShim uses to recognize camera-glued FP
                // objects and to place the controller in world space.
                const float* p = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                g_camX.store(p[0]); g_camY.store(p[1]); g_camZ.store(p[2]);
                g_camValid.store(true);
            }
        }
        else
            g_camValid.store(false);
        return g_origCamCopy(dst, src);
    }

    void __fastcall RenderViewHook(void* view)
    {
        if (!VR_IsStereoEnabled() || !g_enabled.load() || !view)
        {
            g_origRenderView(view);
            return;
        }

        // Compact camera produced by CamCopy: position +0x00, forward +0x0C,
        // up +0x18. It begins at view+0x08. Snapshot it so both eye calls start
        // from identical frame state and the engine sees its original afterward.
        char* camera = reinterpret_cast<char*>(view) + 8;
        alignas(16) unsigned char saved[0x90];
        alignas(16) unsigned char savedDerived[0x90];
        alignas(16) unsigned char savedCameraCopy[0x90];
        alignas(16) unsigned char savedDerivedCopy[0x90];
        memcpy(saved, camera, sizeof(saved));
        memcpy(savedDerived, reinterpret_cast<char*>(view) + 0x98, sizeof(savedDerived));
        memcpy(savedCameraCopy, reinterpret_cast<char*>(view) + 0x158, sizeof(savedCameraCopy));
        memcpy(savedDerivedCopy, reinterpret_cast<char*>(view) + 0x1E8, sizeof(savedDerivedCopy));
        const float* fwd = reinterpret_cast<const float*>(saved + 0x0C);
        const float* up = reinterpret_cast<const float*>(saved + 0x18);
        float right[3] = {
            fwd[1] * up[2] - fwd[2] * up[1],
            fwd[2] * up[0] - fwd[0] * up[2],
            fwd[0] * up[1] - fwd[1] * up[0]};
        // Stereo depth is deliberately independent from the 6DOF translation
        // scale. The physically converted 67.5 mm baseline read too flat in
        // Halo; use a fixed 2x stereo strength so the scene has clear depth.
        constexpr float kStereoWorldUnitsPerMeter = 0.33f;
        const float halfIpdWorld = 0.5f * 0.0675f * kStereoWorldUnitsPerMeter;

        // STEREO GHOSTING — root cause finally OBSERVED (2026-07-14, the
        // CopyResource probe): between the eye passes, the engine snapshots
        // the full-resolution scene into a sampleable texture
        // (M2 COPY eye=-1, 2912x2100 fmt29 -> fmt29) — its "last frame"
        // source for temporal effects. In stereo that snapshot is made from
        // whichever eye rendered LAST, and BOTH eyes sample it next frame:
        // the last eye reads itself (clean), the first eye reads the other
        // eye (trailing after-images offset by the eye separation). This
        // explains every earlier result: fixed order -> steady first-eye
        // ghost; alternation -> flicker; a discarded warm-up render -> clean
        // (it flushed the foreign snapshot through the effect chain) at the
        // cost of a third render (60 fps).
        //
        // The fix (vr.cpp, VR_Begin/EndRasterEye + the CopyResource hook):
        // keep a per-eye copy of that snapshot — captured after each eye's
        // own render, substituted into the game's snapshot texture right
        // before that eye renders again. Each eye then always samples its own
        // previous frame. Three texture copies per frame instead of a third
        // world render, so this runs at the full two-render rate.
        {
            static std::atomic<unsigned> viewRenders{0};
            static std::atomic<DWORD> lastLog{GetTickCount()};
            viewRenders.fetch_add(1);
            const DWORD now = GetTickCount();
            DWORD last = lastLog.load();
            if (now - last >= 10000 && lastLog.compare_exchange_strong(last, now))
            {
                const unsigned n = viewRenders.exchange(0);
                LOG("M2: view renders %.0f/sec (equals fps => one per frame; "
                    "a multiple => extra engine views)", n * 1000.0 / (now - last));
            }
        }
        const int firstEye = g_config.right_eye_first ? 1 : 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int eye = pass == 0 ? firstEye : 1 - firstEye;
            g_stereoEye = eye;
            VR_BeginRasterEye(eye);
            memcpy(camera, saved, sizeof(saved));
            float* pos = reinterpret_cast<float*>(camera);
            const float sign = eye == 0 ? -1.0f : 1.0f;
            pos[0] += right[0] * halfIpdWorld * sign;
            pos[1] += right[1] * halfIpdWorld * sign;
            pos[2] += right[2] * halfIpdWorld * sign;

            // Cant: PSVR2 mounts each display angled outward a few degrees,
            // and the per-eye FOV OpenXR reports is measured around that
            // canted axis. Turn this eye's raster camera by the same relative
            // rotation (OpenXR view axes +X/+Y/+Z=right/up/-forward mapped
            // onto the camera basis) so the raster covers exactly what the
            // compositor displays; rendering both eyes straight ahead leaves
            // the outward lens edge uncovered = black border per eye. The
            // matching per-eye orientation is submitted in vr.cpp. (Assumes
            // the default yaw/pitch mapping; F4/F5 flips would mirror it.)
            float cantQuat[4];
            if (VR_GetEyeCantQuat(eye, cantQuat))
            {
                const float sinHalf = sqrtf(cantQuat[0] * cantQuat[0] +
                                            cantQuat[1] * cantQuat[1] +
                                            cantQuat[2] * cantQuat[2]);
                if (sinHalf > 1e-5f)
                {
                    float angle = 2.0f * atan2f(sinHalf, cantQuat[3]);
                    if (angle > 3.14159265f) angle -= 6.2831853f; // shortest arc
                    const float ax = cantQuat[0] / sinHalf;
                    const float ay = cantQuat[1] / sinHalf;
                    const float az = cantQuat[2] / sinHalf;
                    const float axis[3] = {
                        ax * right[0] + ay * up[0] - az * fwd[0],
                        ax * right[1] + ay * up[1] - az * fwd[1],
                        ax * right[2] + ay * up[2] - az * fwd[2]};
                    const float cosA = cosf(angle), sinA = sinf(angle);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x0C), axis, cosA, sinA);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x18), axis, cosA, sinA);
                }
            }

            // Rebuild exactly the same derived blocks as the engine's camera
            // setup function. Without this, changing the compact camera here
            // is too late and the GPU keeps using the center-eye matrices.
            alignas(16) unsigned char temporary[0x40]{};
            if (g_buildViewport && g_buildMatrices)
            {
                g_buildViewport(camera, temporary);
                g_buildMatrices(camera, temporary, reinterpret_cast<char*>(view) + 0x98, 0.0f);
                const float* finalProjection = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                // Override Halo's capped projection with the headset's own
                // per-eye lens coverage (angles from xrLocateViews, measured
                // around the canted eye axis this raster now uses). The
                // engine field only takes a centered scale, so cover the
                // wider side of each axis symmetrically. Falls back to
                // PSVR2's measured angles until views are located. This
                // happens before PrepareView so Halo's render setup/culling
                // and OpenXR's dynamic submission see the same projection.
                float eyeFov[4];
                float halfX = 1.07338f, halfY = 0.92502f; // PSVR2: 61.5/53 deg
                if (VR_GetEyeFov(eye, eyeFov))
                {
                    halfX = fmaxf(-eyeFov[0], eyeFov[1]);
                    halfY = fmaxf(eyeFov[2], -eyeFov[3]);
                }
                float* vrProjection = reinterpret_cast<float*>(
                    reinterpret_cast<char*>(view) + 0x98 + 0x78);
                vrProjection[0] = 1.0f / tanf(halfX);
                vrProjection[5] = 1.0f / tanf(halfY);
                finalProjection = vrProjection;
                if (fabsf(finalProjection[0]) > 0.01f && fabsf(finalProjection[5]) > 0.01f)
                {
                    g_renderHalfFovX = atanf(1.0f / fabsf(finalProjection[0]));
                    g_renderHalfFovY = atanf(1.0f / fabsf(finalProjection[5]));
                }
                // Capture the engine's real matrix layout before introducing
                // off-axis center terms. This is logged once and left
                // untouched so the diagnostic build remains distortion-free.
                static std::atomic<bool> loggedProjection{false};
                if (!loggedProjection.exchange(true))
                {
                    const float* p = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                    LOG("M2 projection rows: [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]",
                        p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                        p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
                }
                memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
                memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                       reinterpret_cast<char*>(view) + 0x98, 0x90);
            }
            // The draw routine consumes camera state uploaded to engine globals
            // by this per-view preparation stage, not the view structure
            // directly. Re-run it after each eye matrix rebuild.
            if (g_prepareView)
                g_prepareView(view, 0);
            // Match the gun/HUD overlay frustum EXACTLY to the widened world
            // raster, otherwise the ~81 deg overlay stretched across the
            // ~123 deg frame magnifies the first-person weapon and HUD ~2x —
            // and any deliberate mismatch would also shift where the
            // hand-anchored weapon projects, breaking controller registration.
            // Written just before the render call so it is the last writer
            // (the game recomputes these fields every frame, so no restore is
            // needed and scope zoom keeps working when stereo is off).
            if (const uintptr_t gunCam = g_gunCamera.load())
            {
                float* gunTan = reinterpret_cast<float*>(gunCam + kGunProjX);
                static std::atomic<bool> loggedGunTan{false};
                if (!loggedGunTan.exchange(true))
                    LOG("M2 gun overlay tangents: game (%.4f, %.4f) -> world match (%.4f, %.4f)",
                        gunTan[0], gunTan[1],
                        tanf(g_renderHalfFovX.load()), tanf(g_renderHalfFovY.load()));
                gunTan[0] = tanf(g_renderHalfFovX.load());
                gunTan[1] = tanf(g_renderHalfFovY.load());
                // Experimental HUD sizing: the other three overlay cameras get
                // a scaled frustum (>1 = smaller HUD). Element 0 (the weapon)
                // is never scaled — its projection must match the world for
                // controller registration. Default 1.0 = byte-identical no-op.
                const float hudScale = g_config.hud_scale;
                if (hudScale > 1.001f || hudScale < 0.999f)
                    for (int overlay = 1; overlay < 4; ++overlay)
                    {
                        float* t = reinterpret_cast<float*>(gunCam + overlay*0x2820 + kGunProjX);
                        t[0] = tanf(g_renderHalfFovX.load()) * hudScale;
                        t[1] = tanf(g_renderHalfFovY.load()) * hudScale;
                    }
            }
            g_origRenderView(view);
            VR_CaptureRenderedEye(eye);
            VR_EndRasterEye();
        }
        g_stereoEye = -1;
        memcpy(camera, saved, sizeof(saved));
        memcpy(reinterpret_cast<char*>(view) + 0x98, savedDerived, sizeof(savedDerived));
        memcpy(reinterpret_cast<char*>(view) + 0x158, savedCameraCopy, sizeof(savedCameraCopy));
        memcpy(reinterpret_cast<char*>(view) + 0x1E8, savedDerivedCopy, sizeof(savedDerivedCopy));
    }

    // Byte pattern of the camera-copy function's prologue, with the RIP
    // displacement and the short-jump offset wildcarded. Found by signature so
    // the mod survives MCC updates that shift addresses (per the project rules).
    //   mov [rsp+8],rbx; push rdi; sub rsp,0x30; movaps [rsp+0x20],xmm6;
    //   mov rdi,rdx; mov rbx,rcx; test rdx,rdx; je short ??; movss xmm3,[rip+??]
    const char* kCamCopySig =
        "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B FA 48 8B D9 48 85 D2 74 ?? F3 0F 10 1D ?? ?? ?? ??";

    // halo3.dll+0x286A14 in build 1.3528: inner per-view renderer, called by
    // the engine's native view loop with rcx = the prepared view structure.
    const char* kRenderViewSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 8B 3D ?? ?? ?? ?? 48 8B F1 85 FF 0F 84 ?? ?? ?? ??";
    const char* kPrepareViewSig =
        "48 89 5C 24 08 57 48 83 EC 20 83 3D ?? ?? ?? ?? 03 8B FA 48 8B D9 48 89 0D ?? ?? ?? ??";
    const char* kBuildViewportSig =
        "40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50";
    const char* kBuildMatricesSig =
        "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 E8 48 81 EC 10 01 00 00 80 3D ?? ?? ?? ?? 00";
    // Start of the engine function that constructs the 4-slot camera-object
    // array: mov [rsp+8],rbx; push rdi; sub rsp,0x20; lea rbx,[rip+array];
    // mov edi,4; mov rcx,rbx; call ctor; add rbx,0x2820; sub rdi,1; jnz.
    // The lea's RIP displacement is at match+13 and the instruction ends at
    // match+17, so the array (= gun/overlay camera) is match+17+disp32. The
    // 0x2820 stride distinguishes it from an identical builder of another
    // camera array.
    const char* kGunCamRefSig =
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 20 28 00 00 48 83 EF 01 75 EB";
    // Bone composition boundaries called by the first-person animator at
    // 0x2C4663/0x2C4633. The render packet copy happens immediately afterward.
    const char* kComposeBonesSig =
        "45 85 C0 0F 8E ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 20 45 8B D0 49 8B F9 4C 8B C9";
    const char* kComposeSpecialBonesSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 55 41 56 48 8D 68 B8";

    void InstallHook(uintptr_t base, size_t size)
    {
        uintptr_t hit = sig::Find(base, size, kCamCopySig);
        if (!hit)
        {
            LOG("M1: camera signature NOT FOUND — MCC may have updated. Head tracking is");
            LOG("M1: disabled; the game and the VR screen still work normally.");
            return;
        }
        // Uniqueness check — if the pattern matched twice we can't trust it.
        const uintptr_t after = hit + 1;
        if (sig::Find(after, base + size - after, kCamCopySig))
            LOG("M1: WARNING camera signature is not unique; using the first match");
        LOG("M1: camera-copy found by signature at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
            (unsigned long long)(hit - base), (unsigned long long)kCamCopyRva);

        void* target = reinterpret_cast<void*>(hit);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CamCopyHook),
                          reinterpret_cast<void**>(&g_origCamCopy)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG("M1: FAILED to hook camera-copy at %p; head tracking unavailable", target);
            return;
        }
        LOG("M1: camera hooked. F2 head tracking, F3 recenter, F6 leaning, F8/F9 pitch trim, F10 screen-follow (yaw/pitch/up flips: F1 menu)");

        uintptr_t composeHit=sig::Find(base,size,kComposeBonesSig);
        uintptr_t specialHit=sig::Find(base,size,kComposeSpecialBonesSig);
        if (composeHit && specialHit &&
            !sig::Find(composeHit+1,base+size-composeHit-1,kComposeBonesSig) &&
            !sig::Find(specialHit+1,base+size-specialHit-1,kComposeSpecialBonesSig))
        {
            // Both compositor callers and the allocator use the engine TLS
            // index global at halo3.dll+0xA39F9C. Resolve it through the proven
            // first-person update signature instead of baking that RVA.
            uintptr_t fp=sig::Find(base,size,
                "48 8B C4 44 88 40 18 89 50 10 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57");
            if (fp)
            {
                const int32_t tlsDisp=*reinterpret_cast<const int32_t*>(fp+0x62);
                g_engineTlsIndex=reinterpret_cast<uint32_t*>(fp+0x66+tlsDisp);
            }
            // composeHit+0x50 is mov rax,[rip+tag-data-base]; its displacement
            // resolves the node-record storage used for names and parents.
            const int32_t tagDataDisp=*reinterpret_cast<const int32_t*>(composeHit+0x53);
            g_animationTagData=reinterpret_cast<unsigned char**>(composeHit+0x57+tagDataDisp);
            if (g_engineTlsIndex && g_animationTagData &&
                MH_CreateHook(reinterpret_cast<void*>(composeHit),reinterpret_cast<void*>(&ComposeBonesHook),
                              reinterpret_cast<void**>(&g_origComposeBones)) == MH_OK &&
                MH_CreateHook(reinterpret_cast<void*>(specialHit),reinterpret_cast<void*>(&ComposeSpecialBonesHook),
                              reinterpret_cast<void**>(&g_origComposeSpecialBones)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(composeHit)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(specialHit)) == MH_OK)
                LOG("M3: first-person marker/muzzle matrix hooks installed at halo3.dll+0x%llX/+0x%llX",
                    (unsigned long long)(composeHit-base),(unsigned long long)(specialHit-base));
            else
                LOG("M3: FAILED to hook first-person hand/weapon matrix composers");
        }
        else
            LOG("M3: first-person hand/weapon matrix signatures missing or ambiguous");

        // DO NOT HOOK halo3+0x120DF8. Tried 2026-07-15: it crashes the game on
        // level load, on contact, even as a pure pass-through (proven — the
        // skip range was never armed, the unconditional probe log never
        // printed, and it still died). Surviving the menus proves nothing:
        // halo3.dll's model pipeline does not run there, so the first real call
        // IS the level load. The weapon-lag diagnosis in RE-notes stands; the
        // mechanism for acting on it must not be a detour on this function.

        // FP mesh re-anchor: patch the single root-fetch call inside the object
        // node recomposer (see FpRootShim). lea rdx,[rsp+20]; mov ecx,ebx;
        // call <root>; then the 0x1205AC multiply — unique on disk, verified.
        const char* kFpRootCallSig =
            "48 8D 54 24 20 8B CB E8 ?? ?? ?? ?? 4D 8B C4 48 8D 4C 24 20 49 8B D7 E8";
        uintptr_t callSite = sig::Find(base, size, kFpRootCallSig);
        if (callSite && !sig::Find(callSite+1, base+size-callSite-1, kFpRootCallSig))
        {
            const uintptr_t callInstr = callSite + 7;    // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realFpRoot = reinterpret_cast<FpRootFn>(callInstr + 5 + origRel);
            // 12-byte trampoline (mov rax, imm64; jmp rax) within rel32 range
            // of the call site, since our DLL may sit >2GB away.
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                tramp[0]=0x48; tramp[1]=0xB8;                       // mov rax, imm64
                *reinterpret_cast<void**>(tramp+2) =
                    reinterpret_cast<void*>(&FpRootShim);
                tramp[10]=0xFF; tramp[11]=0xE0;                     // jmp rax
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: FP mesh root call-site patched at halo3.dll+0x%llX "
                        "(real getter halo3.dll+0x%llX; atomic disp32 swap)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realFpRoot)-base));
                }
                else
                    LOG("M3: FP mesh call-site VirtualProtect failed; gun stays camera-glued");
            }
            else
                LOG("M3: FP mesh trampoline allocation failed (rel %lld); gun stays camera-glued",
                    (long long)newRel);
        }
        else
            LOG("M3: FP mesh root call-site signature missing/ambiguous; gun stays camera-glued");

        // Second call-site patch: the camera pitch/turn rotation applied to
        // every FP bone but camera_control (the head-glue; see the emitted
        // rax-only shim below and the LTCG note at g_fpSkipBounds).
        const char* kSwayCallSig = "44 3B 8F A4 11 00 00 74 0C 49 8B D0 48 8D 4D C8 E8";
        uintptr_t swaySite = sig::Find(base, size, kSwayCallSig);
        if (swaySite && !sig::Find(swaySite+1, base+size-swaySite-1, kSwayCallSig))
        {
            const uintptr_t callInstr = swaySite + 16;   // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realSwayApply = reinterpret_cast<SwayApplyFn>(callInstr + 5 + origRel);
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                // Hand-assembled shim, clobbers ONLY rax (the caller keeps its
                // loop state in r8/r9/r10 across this call — LTCG contract; a
                // compiled C++ shim here IS the fatal-error bug). Layout:
                //   [0x00] mov rax,[lo0]; cmp rdx,rax; jb +0x0F (-> lo1 test)
                //   [0x0F] mov rax,[hi0]; cmp rdx,rax; jb +0x2A (-> ret)
                //   [0x1E] mov rax,[lo1]; cmp rdx,rax; jb +0x0F (-> tail)
                //   [0x2D] mov rax,[hi1]; cmp rdx,rax; jb +0x0C (-> ret)
                //   [0x3C] mov rax, real; jmp rax
                //   [0x48] ret
                unsigned char shim[0x49];
                int o = 0;
                auto movRaxAbs = [&](volatile uintptr_t* a) {
                    shim[o++]=0x48; shim[o++]=0xA1;         // mov rax, moffs64
                    const void* p = const_cast<uintptr_t*>(a);
                    memcpy(shim+o, &p, 8); o += 8;
                };
                auto cmpJb = [&](unsigned char disp) {
                    shim[o++]=0x48; shim[o++]=0x39; shim[o++]=0xC2; // cmp rdx, rax
                    shim[o++]=0x72; shim[o++]=disp;                 // jb rel8
                };
                movRaxAbs(&g_fpSkipBounds[0]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[1]); cmpJb(0x2A);
                movRaxAbs(&g_fpSkipBounds[2]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[3]); cmpJb(0x0C);
                shim[o++]=0x48; shim[o++]=0xB8;             // mov rax, imm64
                const void* real = reinterpret_cast<const void*>(g_realSwayApply);
                memcpy(shim+o, &real, 8); o += 8;
                shim[o++]=0xFF; shim[o++]=0xE0;             // jmp rax
                shim[o++]=0xC3;                             // ret (skip path)
                memcpy(tramp, shim, o);
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: camera pitch/turn call-site patched at halo3.dll+0x%llX "
                        "(rotator halo3.dll+0x%llX)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realSwayApply)-base));
                }
                else
                    LOG("M3: pitch/turn call-site VirtualProtect failed; gun stays head-rotated");
            }
            else
                LOG("M3: pitch/turn trampoline allocation failed; gun stays head-rotated");
        }
        else
            LOG("M3: pitch/turn call-site signature missing/ambiguous; gun stays head-rotated");

        uintptr_t gunRef = sig::Find(base, size, kGunCamRefSig);
        if (gunRef && !sig::Find(gunRef + 1, base + size - gunRef - 1, kGunCamRefSig))
        {
            const int32_t disp = *reinterpret_cast<const int32_t*>(gunRef + 13);
            g_gunCamera = gunRef + 17 + disp;
            LOG("M2: gun/overlay camera at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
                (unsigned long long)(g_gunCamera.load() - base),
                (unsigned long long)kGunCamRva);
        }
        else
        {
            LOG("M2: gun-camera signature missing/ambiguous; weapon/HUD will stay oversized in stereo");
        }

        uintptr_t renderHit = sig::Find(base, size, kRenderViewSig);
        uintptr_t prepareHit = sig::Find(base, size, kPrepareViewSig);
        uintptr_t viewportHit = sig::Find(base, size, kBuildViewportSig);
        uintptr_t matricesHit = sig::Find(base, size, kBuildMatricesSig);
        if (!renderHit || sig::Find(renderHit + 1, base + size - renderHit - 1, kRenderViewSig))
        {
            LOG("M2: render-frame signature missing or ambiguous; raw stereo unavailable");
            return;
        }
        if (!prepareHit || !viewportHit || !matricesHit)
        {
            LOG("M2: derived camera matrix signatures missing; raw stereo unavailable");
            return;
        }
        g_prepareView = reinterpret_cast<PrepareViewFn>(prepareHit);
        g_buildViewport = reinterpret_cast<BuildViewportFn>(viewportHit);
        g_buildMatrices = reinterpret_cast<BuildMatricesFn>(matricesHit);
        if (MH_CreateHook(reinterpret_cast<void*>(renderHit), reinterpret_cast<void*>(&RenderViewHook),
                          reinterpret_cast<void**>(&g_origRenderView)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(renderHit)) != MH_OK)
        {
            LOG("M2: FAILED to hook render-frame entry");
            return;
        }
        g_renderHooked = true;
        LOG("M2: inner per-view double-render hook installed at halo3.dll+0x%llX",
            (unsigned long long)(renderHit - base));
    }

    DWORD WINAPI WaitThread(LPVOID)
    {
        // The XInput hook is wanted as soon as MCC loads an xinput DLL (so the
        // Sense controllers drive the frontend menus too), the game hooks only
        // once halo3.dll appears (entering a level). MCC loads xinput DLLs
        // lazily and can add MORE of them later (it hooked only xinput1_3 in
        // one session and read the pad through xinput1_4), so this thread
        // keeps polling forever instead of stopping at the first success.
        bool gameHooked = false;
        for (;;)
        {
            Input_InstallXInputHook();
            Input_ClaimXInputIat(); // re-assert if Steam replaces MCC's import slot
            if (!gameHooked)
            {
                uintptr_t base = 0;
                size_t size = 0;
                if (sig::ModuleRange(L"halo3.dll", base, size))
                {
                    LOG("halo3.dll loaded at %p, size 0x%zX", (void*)base, size);
                    InstallHook(base, size);
                    g_hooked = true;
                    gameHooked = true;
                }
            }
            Sleep(2000);
        }
    }
}

void Game_Init()
{
    // Claim MCC's controller path synchronously, before OpenXR startup blocks
    // on SteamVR. The worker keeps re-asserting it if Steam replaces the IAT.
    Input_InstallXInputHook();
    Input_ClaimXInputIat();
    CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}

bool Game_IsHooked() { return g_hooked; }
bool Game_IsHeadTracking() { return g_enabled.load(); }


void Game_ToggleHeadTracking()
{
    const bool on = !g_enabled.load();
    g_enabled = on;
    if (on)
        g_needRecenter = true;
    LOG("head tracking %s", on ? "ON" : "OFF");
}

void Game_Recenter() { g_needRecenter = true; }
void Game_FlipYaw()   { g_yawSign = -g_yawSign.load();   LOG("yaw sign %+.0f", g_yawSign.load()); }
void Game_FlipPitch() { g_pitchSign = -g_pitchSign.load(); LOG("pitch sign %+.0f", g_pitchSign.load()); }
void Game_ToggleUp()  { g_writeUp = !g_writeUp.load();   LOG("write up-vector %s", g_writeUp.load() ? "on" : "off"); }
float Game_GetYawSign()   { return g_yawSign.load(); }
float Game_GetPitchSign() { return g_pitchSign.load(); }
bool Game_GetWriteUp()    { return g_writeUp.load(); }

void Game_TogglePositional()
{
    if (VR_IsStereoEnabled())
    {
        g_positional = true;
        LOG("positional remains ON (required for stereo VR)");
        return;
    }
    const bool on = !g_positional.load();
    g_positional = on;
    if (on)
        g_needPosRecenter = true; // capture neutral head position, no yaw snap
    LOG("positional (leaning) %s", on ? "ON" : "OFF");
}

void Game_ForcePositional()
{
    g_positional = true;
    g_needPosRecenter = true;
    LOG("positional 6DOF forced ON for stereo VR");
}

void Game_PitchTrim(int dir)
{
    const float t = Clamp(g_pitchTrim.load() + dir * 0.035f, -0.8f, 0.8f); // ~2 deg steps
    g_pitchTrim = t;
    LOG("pitch trim %.1f deg", t * 57.2958f);
}

void Game_LeanScale(int dir)
{
    const float s = Clamp(g_worldScale.load() + dir * 0.05f, 0.05f, 2.0f);
    g_worldScale = s;
    LOG("lean scale %.2f (game units per meter)", s);
}

void Game_ToggleVrAim()
{
    const bool on = !g_vrAim.load();
    g_vrAim = on;
    LOG("VR aim (right controller steers weapon) %s", on ? "ON" : "OFF");
}

bool Game_ComputeAimStick(float& outRx, float& outRy)
{
    // Closed-loop aim: emit a right-stick deflection proportional to the
    // angular error between the game's aim and the controller ray. The game
    // integrates it through its normal turn-rate path, so bullets, reticle
    // logic, vehicles and turrets all behave as if the player aimed manually.
    // Diagnostic: when aim steering is not running, log WHICH precondition
    // failed (once per distinct reason) so a dead aim is explainable from the
    // log alone.
    static std::atomic<int> lastAimBlock{-1};
    auto blocked = [](int reason, const char* what) {
        int prev = lastAimBlock.exchange(reason);
        if (prev != reason)
            LOG("M3 DIAG: aim steering blocked: %s", what);
        return false;
    };
    if (!g_vrAim.load())
        return blocked(1, "VR aim toggled OFF (press Insert)");
    if (!g_enabled.load())
        return blocked(2, "head tracking OFF (press F2)");
    if (!g_aimSeen.load())
        return blocked(3, "camera hook not running (not in a level?)");
    float q[4], p[3];
    if (!VR_GetRightControllerPose(q, p))
        return blocked(4, "right controller not tracked");
    float hq[4], hp[3];
    if (!VR_GetHeadPose(hq, hp))
        return blocked(5, "headset not tracked");
    lastAimBlock = 0;

    // Controller forward (-Z of the aim pose), same mapping as the head so
    // hand and head share the F3 recenter reference.
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (w * y + x * z);
    const float fy =  2.0f * (w * x - y * z);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));

    // Halo spawns first-person projectiles at the CAMERA — the head — and no
    // steering can move that origin. Aiming the bullet ray PARALLEL to the
    // hand ray therefore leaves a permanent head-to-hand parallax miss (the
    // 07-15 report "bullets shoot from my head"). Instead steer the head-
    // origin ray through the point the hand ray reaches at the crosshair
    // distance: every shot then passes exactly through the floating reticle,
    // and beyond it the two rays are effectively identical.
    const float d = Clamp(g_config.crosshair_distance_m, 2.0f, 50.0f);
    float tx = p[0] + fx * d - hp[0];
    float ty = p[1] + fy * d - hp[1];
    float tz = p[2] + fz * d - hp[2];
    const float tl = sqrtf(tx * tx + ty * ty + tz * tz);
    if (tl > 1e-3f) { tx /= tl; ty /= tl; tz /= tl; }
    const float cy = atan2f(tx, -tz);
    const float cp = asinf(Clamp(ty, -1.0f, 1.0f));
    const float desiredYaw = g_gameYawRef + g_yawSign.load() * WrapPi(cy - g_headYawRef);
    const float desiredPitch = Clamp(g_pitchSign.load() * cp, -1.45f, 1.45f);

    const float ax = g_aimFwdX.load(), ay = g_aimFwdY.load(), az = g_aimFwdZ.load();
    const float aimYaw = atan2f(ay, ax);
    const float aimPitch = asinf(Clamp(az, -1.0f, 1.0f));

    const float errYaw = WrapPi(desiredYaw - aimYaw);
    const float errPitch = desiredPitch - aimPitch;

    // Full deflection at ~10 deg of error. Game yaw grows counterclockwise
    // (left); positive stick X turns right, hence the minus.
    const float k = 5.7f;
    outRx = Clamp(-errYaw * k, -1.0f, 1.0f);
    outRy = Clamp(errPitch * k, -1.0f, 1.0f);
    return true;
}

void Game_MapMoveStick(float& mx, float& my)
{
    // The game moves relative to its aim heading, which VR aim points at the
    // hand. Rotate the move vector by (head - aim) yaw so pushing forward
    // walks where you look instead of where the gun points.
    if (!g_enabled.load() || !g_aimSeen.load() || !g_vrAim.load())
        return;
    float q[4], p[3];
    if (!VR_GetHeadPose(q, p))
        return;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (w * y + x * z);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float hy = atan2f(fx, -fz);
    const float headYaw = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
    const float aimYaw = atan2f(g_aimFwdY.load(), g_aimFwdX.load());
    const float delta = WrapPi(headYaw - aimYaw);
    const float c = cosf(delta), s = sinf(delta);
    const float nx = mx * c - my * s;
    const float ny = mx * s + my * c;
    mx = nx;
    my = ny;
}

void Game_GunScale(int dir)
{
    // Uniform mesh scale of the hand-anchored arms+gun assembly around the
    // wrist. Home = bigger, End = smaller. Persisted next time settings save.
    const float s = Clamp(g_config.gun_scale * (dir > 0 ? 1.05f : 1.0f / 1.05f),
                          0.3f, 3.0f);
    g_config.gun_scale = s;
    LOG("weapon size %.2fx", s);
}

float Game_GetWorldScale() { return g_worldScale.load(); }

void Game_GetProjectionTangents(float& tanX, float& tanY)
{
    tanX = g_projectionTanX.load();
    tanY = g_projectionTanY.load();
}

void Game_GetRenderHalfFov(float& halfX, float& halfY)
{
    halfX = g_renderHalfFovX.load();
    halfY = g_renderHalfFovY.load();
}


void Game_SetStereoEye(int eye)
{
    g_stereoEye = (eye == 0 || eye == 1) ? eye : -1;
}
