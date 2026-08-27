# Pure assertions for the retail control-flow fact Stage 3Z relies on.
# They do not replace headset testing; the binary audit proves the actual patch.

def reaches_mode1_gate(camera_mode: int) -> bool:
    # Retail +0x27BD1B/+0x27BD1F/+0x27BD22 sequence.
    if camera_mode == 0:
        return False
    after_sub = camera_mode - 1
    return after_sub == 0


def final_classifier_result(bl_allow: bool, admitted_bucket: int) -> int:
    # Retail +0x27BDBA..+0x27BDCB: eax=5; cmovne eax,edi.
    return admitted_bucket if bl_allow else 5


def caller_admits(result: int) -> bool:
    # Retail +0x27BE73: cmp eax,5 / jge skip.
    return result < 5


def main():
    assert not reaches_mode1_gate(0)
    assert reaches_mode1_gate(1)
    assert not reaches_mode1_gate(2)
    assert not reaches_mode1_gate(3)
    assert final_classifier_result(False, 3) == 5
    assert not caller_admits(final_classifier_result(False, 3))
    assert caller_admits(final_classifier_result(True, 3))
    print('Stage3Z H4 FP particle logic: PASS')

if __name__ == '__main__':
    main()
