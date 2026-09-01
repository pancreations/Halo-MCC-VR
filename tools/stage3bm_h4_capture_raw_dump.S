// Stage 3BM - write the published Halo 4 capture texture to disk, raw.
//
// Instrument only.  The Stage 3BH upload tap (0x2FBA32..) already gates a
// one-shot 32x32 map on the third "reticle upload" stats window.  Its
// `mov byte ptr [s3bh_dumped], 1` (7 bytes at 0x2FBA58) becomes
// `call s3bm_tick` + 2 NOPs.  s3bm_tick:
//
//   * counts its own invocations (1..4);
//   * writes the whole published capture (staging copy, Map, one WriteFile
//     per row) to "<log dir>HaloMCCVR-h4-reticle-<n>.raw" - width*4 bytes
//     per row, height rows, DXGI format as logged (28 = R8G8B8A8_UNORM);
//   * re-arms the 3BH window counter to -2 so the tap fires again five
//     windows (10 s) later, and on the fourth dump sets s3bh_dumped itself so
//     the tap goes quiet exactly as before;
//   * returns to the tap, which then runs the 3BH map as it always did.
//
// Every step fails open: any null/failed handle logs one line and leaves the
// game path untouched.  Nothing else in the DLL changes.
//
// Frame after 7 pushes + sub rsp,0x300 (entry rsp%16==8 -> aligned):
//   [0x20..0x38]  outgoing stack args (CreateFileW 5th..7th, Map/WriteFile 5th)
//   [0x40..0x4F]  D3D11_MAPPED_SUBRESOURCE {pData, RowPitch, DepthPitch}
//   [0x50..0x7B]  D3D11_TEXTURE2D_DESC (0x2C bytes)
//   [0x80]        staging texture   [0x88] source texture
//   [0x90]        file handle       [0x98] DWORD bytes written
//   [0xA0..0x2A7] wchar_t path[260]

.intel_syntax noprefix
.text

.global stage3bm_data
.global s3bm_tick

stage3bm_data:
s3bm_count:
    .long 0
    .long 0

s3bm_tick:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0x300
    inc DWORD PTR [rip + s3bm_count]
    mov eax, DWORD PTR [rip + s3bm_count]
    cmp eax, 4
    ja .done
    mov DWORD PTR [rip + S3BH_WINDOWS_RVA], -2
    cmp eax, 4
    jne .path
    mov BYTE PTR [rip + S3BH_DUMPED_RVA], 1
.path:
    call LOGDIR_RVA                               // rax = const wchar_t*
    test rax, rax
    jz .fail
    mov rsi, rax
    lea rdi, [rsp + 0xA0]
    xor ecx, ecx
.copy_dir:
    movzx eax, WORD PTR [rsi + rcx*2]
    test ax, ax
    jz .copy_name
    mov WORD PTR [rdi + rcx*2], ax
    inc ecx
    cmp ecx, 200
    jb .copy_dir
    jmp .fail                                     // directory too long
.copy_name:
    lea rsi, [rip + s3bm_name]
.copy_n:
    movzx eax, WORD PTR [rsi]
    mov WORD PTR [rdi + rcx*2], ax
    add rsi, 2
    inc ecx
    test ax, ax
    jnz .copy_n
    // ecx = dirlen + 27 (name incl. NUL); the digit sits at name index 21
    mov eax, DWORD PTR [rip + s3bm_count]
    add eax, 0x30
    mov WORD PTR [rdi + rcx*2 - 12], ax

    mov rax, QWORD PTR [rip + AUTHORED_TEX_RVA]
    test rax, rax
    jz .fail
    mov rbx, QWORD PTR [rip + G_CONTEXT_RVA]
    test rbx, rbx
    jz .fail
    mov QWORD PTR [rsp + 0x88], rax
    mov rcx, rax
    lea rdx, [rsp + 0x50]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x50]                   // ID3D11Texture2D::GetDesc
    mov DWORD PTR [rsp + 0x6C], 3                 // Usage = STAGING
    mov DWORD PTR [rsp + 0x70], 0                 // BindFlags = 0
    mov DWORD PTR [rsp + 0x74], 0x20000           // CPUAccess = READ
    mov DWORD PTR [rsp + 0x78], 0                 // MiscFlags = 0
    mov rcx, QWORD PTR [rip + G_DEVICE_RVA]
    test rcx, rcx
    jz .fail
    lea rdx, [rsp + 0x50]
    xor r8d, r8d
    lea r9, [rsp + 0x80]
    mov QWORD PTR [rsp + 0x80], 0
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x28]                   // CreateTexture2D
    test eax, eax
    js .fail
    mov r12, QWORD PTR [rsp + 0x80]
    test r12, r12
    jz .fail
    mov rcx, rbx
    mov rdx, r12
    mov r8, QWORD PTR [rsp + 0x88]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x178]                  // CopyResource(staging, src)
    mov rcx, rbx
    mov rdx, r12
    xor r8d, r8d
    mov r9d, 1                                    // D3D11_MAP_READ
    mov QWORD PTR [rsp + 0x20], 0
    lea rax, [rsp + 0x40]
    mov QWORD PTR [rsp + 0x28], rax
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x70]                   // Map
    test eax, eax
    js .release

    lea rcx, [rsp + 0xA0]
    mov edx, 0x40000000                           // GENERIC_WRITE
    xor r8d, r8d                                  // no sharing
    xor r9d, r9d                                  // default security
    mov DWORD PTR [rsp + 0x20], 2                 // CREATE_ALWAYS
    mov DWORD PTR [rsp + 0x28], 0x80              // FILE_ATTRIBUTE_NORMAL
    mov QWORD PTR [rsp + 0x30], 0
    call QWORD PTR [rip + IAT_CREATEFILEW_RVA]
    cmp rax, -1
    je .unmap
    mov QWORD PTR [rsp + 0x90], rax
    mov rsi, QWORD PTR [rsp + 0x40]               // pData
    mov r13d, DWORD PTR [rsp + 0x48]              // RowPitch
    mov r14d, DWORD PTR [rsp + 0x54]              // Height
    mov r15d, DWORD PTR [rsp + 0x50]              // Width
    shl r15d, 2                                   // bytes per row
    xor edi, edi
.row:
    mov rcx, QWORD PTR [rsp + 0x90]
    mov rdx, rsi
    mov r8d, r15d
    lea r9, [rsp + 0x98]
    mov QWORD PTR [rsp + 0x20], 0
    call QWORD PTR [rip + IAT_WRITEFILE_RVA]
    test eax, eax
    jz .close
    add rsi, r13
    inc edi
    cmp edi, r14d
    jb .row
.close:
    mov rcx, QWORD PTR [rsp + 0x90]
    call QWORD PTR [rip + IAT_CLOSEHANDLE_RVA]
    lea rcx, [rip + s3bm_msg_ok]
    lea rdx, [rsp + 0xA0]
    mov r8d, edi
    mov r9d, DWORD PTR [rsp + 0x50]
    mov eax, DWORD PTR [rsp + 0x54]
    mov DWORD PTR [rsp + 0x20], eax
    mov eax, DWORD PTR [rsp + 0x60]               // Format
    mov DWORD PTR [rsp + 0x28], eax
    mov eax, DWORD PTR [rsp + 0x48]
    mov DWORD PTR [rsp + 0x30], eax
    call LOG_RVA
.unmap:
    mov rcx, rbx
    mov rdx, r12
    xor r8d, r8d
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x78]                   // Unmap
.release:
    mov rcx, r12
    mov rax, QWORD PTR [r12]
    call QWORD PTR [rax + 0x10]                   // Release staging
    jmp .done
.fail:
    lea rcx, [rip + s3bm_msg_fail]
    call LOG_RVA
.done:
    add rsp, 0x300
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

s3bm_name:
    .short 'H','a','l','o','M','C','C','V','R','-','h','4','-'
    .short 'r','e','t','i','c','l','e','-','0','.','r','a','w', 0
s3bm_msg_ok:
    .asciz "S3BM dump: wrote %ls (%u rows of %ux%u, fmt=%u, pitch=%u): raw R8G8B8A8 rows, no header"
s3bm_msg_fail:
    .asciz "S3BM dump: unavailable this session (log dir/texture/device/map step refused); behaviour otherwise unchanged"
