.intel_syntax noprefix
.section .text

.global stage3o_reach_memtype_filter
.global stage3o_clear_aim_seen
.global stage3o_clear_cam_valid
.global stage3o_clear_base_cam_valid
.global stage3o_no_live_hud_curvature
.global stage3o_str_welcome
.global stage3o_str_weapon_desc
.global stage3o_str_hud_desc
.global stage3o_str_muzzle_note1
.global stage3o_str_muzzle_note2
.global stage3o_str_h2_curvature_note
.global stage3o_str_curvature_note

.extern ACTIVE_TITLE_RVA
.extern AIM_SEEN_RVA
.extern CAM_VALID_RVA
.extern BASE_CAM_VALID_RVA
.extern H2_NATIVE_HUD_QUERY_RVA

# Called in place of the old MEM_PRIVATE || (Reach && MEM_MAPPED) block.
# Contract at the patched callsite:
#   eax = mbi.Type
#   rcx = &adapter->scanMappedRegions
# Returns AL only, matching the original block's only data result.
# H3/ODST keep MEM_PRIVATE. Reach uses only MEM_MAPPED, which is where the
# headset-proven six HUD safe-frame records live.
stage3o_reach_memtype_filter:
    cmp byte ptr [rcx], 0
    jne .mapped
    cmp eax, 0x20000
    sete al
    ret
.mapped:
    cmp eax, 0x40000
    sete al
    ret

# Reach teardown can retry after Halo 3 has already become the active title.
# The three camera-valid bytes below are shared by H3/ODST/Reach. The original
# Reach cleanup stores are preserved semantically only while the active title
# is Reach (3), None (0), or Unknown (7). During H3/ODST/H4/H2 ownership the
# cleanup store becomes a no-op. Flags and RAX are preserved because the stock
# MOV byte store changed neither.
.macro CONDITIONAL_CLEAR name, target
\name:
    pushfq
    push rax
    mov al, byte ptr [rip + ACTIVE_TITLE_RVA]
    test al, al
    je 1f
    cmp al, 3
    je 1f
    cmp al, 7
    jne 2f
1:
    mov byte ptr [rip + \target], 0
2:
    pop rax
    popfq
    ret
.endm

CONDITIONAL_CLEAR stage3o_clear_aim_seen, AIM_SEEN_RVA
CONDITIONAL_CLEAR stage3o_clear_cam_valid, CAM_VALID_RVA
CONDITIONAL_CLEAR stage3o_clear_base_cam_valid, BASE_CAM_VALID_RVA

# Menu-only wrapper: preserve the existing Halo 2 native-HUD test and also
# disable the curvature slider while Reach owns the runtime. The caller already
# treats this boolean as the BeginDisabled/EndDisabled condition.
stage3o_no_live_hud_curvature:
    sub rsp, 0x28
    call H2_NATIVE_HUD_QUERY_RVA
    test al, al
    jne .hud_done
    mov al, byte ptr [rip + ACTIVE_TITLE_RVA]
    cmp al, 3
    sete al
.hud_done:
    add rsp, 0x28
    ret

.p2align 4
stage3o_str_welcome:
    .asciz "Hi everyone, pancreations here!\nThank you for taking the time to get this up and running.\nIf you have any issues please reach out and let me know.\nExpect the rest of the collection to be added over time.\n\nPass-on/community build maintained by @holyjolias.\nJoin the Flat2VR Discord to report bugs or share feedback."

stage3o_str_weapon_desc:
    .asciz "Gun placement, per-title calibration, muzzle alignment, and two-handed aiming."

stage3o_str_hud_desc:
    .asciz "Size and position of each supported title's native HUD; curvature availability is title-specific."

stage3o_str_muzzle_note1:
    .asciz "Reach: adjusts its secondary muzzle placement; H3/ODST marker effects follow the gun. H2 Classic muzzle attachment is unchanged in this candidate."

stage3o_str_muzzle_note2:
    .asciz "Raises supported muzzle/effect placement along the gun's own axis."

stage3o_str_h2_curvature_note:
    .asciz "No live curvature for this title: Halo 2 uses its native per-eye basis; Reach bakes curvature when its tag loads."

stage3o_str_curvature_note:
    .asciz "H3/ODST/H4 curvature: 0.00 = flat, 1.00 = curved; 0.50 is authored."
