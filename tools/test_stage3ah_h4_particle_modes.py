# Pure control-flow assertions for the exact Halo 4 retail switch at
# halo4.dll+0x27BD1B..+0x27BD3E. This is a diagnostic test, not headset proof.

def stage3ah_denies(camera_mode: int) -> bool:
    # mode0 jumps directly to +0x27BD3B; mode1 reaches +0x27BD36; mode2 reaches
    # +0x27BD32. Stage3AH zeroes BL at the latter two allow writes only.
    return camera_mode in (1, 2)

def main():
    assert not stage3ah_denies(0)
    assert stage3ah_denies(1)
    assert stage3ah_denies(2)
    assert not stage3ah_denies(3)
    print('Stage3AH H4 particle-mode contrast logic: PASS')

if __name__ == '__main__': main()
