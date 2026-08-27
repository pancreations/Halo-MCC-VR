.intel_syntax noprefix
.section .text,"ax"
.global stage3ae_h4_suppressor_attachment_gate

# Called in place of Stage3X's old `cmp designator,0 / jl continue` pair.
# Returning normally falls through the preserved Stage3X finite-XYZ hide call.
# For a negative engine-special designator at authored location <= 0, advance
# our caller's return address from 0x2EF73B to 0x2EF744, skipping the preserved
# hide call and leaving that case stock. Negative designators at non-primary
# authored locations (>0) are newly admitted; this is the Suppressor plate/
# slide attachment family proven by the supplied H4 tags.
stage3ae_h4_suppressor_attachment_gate:
    cmp WORD PTR [r15 + 2], 0
    jge .admit
    cmp DWORD PTR [r14 + rsi*4 + 0x14], 0
    jg .admit
    add QWORD PTR [rsp], 9
.admit:
    ret
