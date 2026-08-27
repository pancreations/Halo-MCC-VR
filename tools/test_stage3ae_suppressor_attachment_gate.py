def hide_local_fp_effect(coordinate_system, camera_mode, location, designator):
    if coordinate_system == 0 or camera_mode != 1:
        return False
    if designator >= 0:
        return True
    return location > 0

# Exact Suppressor persistent attachment systems from the supplied H4 tag:
for loc in (1,2,3):
    assert hide_local_fp_effect(1,1,loc,-1)
# Primary location 0 with an engine-special negative designator remains stock.
assert not hide_local_fp_effect(1,1,0,-1)
# Third-person and world-coordinate rows remain stock.
assert not hide_local_fp_effect(1,2,1,-1)
assert not hide_local_fp_effect(0,1,1,-1)
# Existing nonnegative Stage3X first-person path is preserved.
assert hide_local_fp_effect(1,1,0,0)
print('Stage 3AE Suppressor attachment-gate assertions PASS')
