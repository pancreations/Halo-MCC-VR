"""Portable assertions for Stage 3U Reach shell-lifecycle policy."""

STALE_MS = 1000

def action(last_camera_ms, now_ms, paused):
    if not last_camera_ms:
        return False
    if paused:
        return False
    return now_ms - last_camera_ms > STALE_MS

assert not action(0, 5000, False), 'no camera sample must not manufacture teardown time'
assert not action(4000, 4999, False), 'fresh real camera must keep core'
assert not action(4000, 5000, False), 'exact 1s boundary is still admitted'
assert action(4000, 5001, False), 'stale unpaused camera must request teardown'
assert not action(4000, 9000, True), 'native pause keeps head-locked 2D core armed'
# Save & Quit shape from the 2026-08-27 headset log: pause exits, then the
# admitted Reach camera path stops while Present continues. Once >1s stale,
# verified worker teardown is required so the shell can regain a stock layer.
assert action(10000, 11001, False)
print('Stage 3U Reach shell lifecycle portable assertions PASS')
