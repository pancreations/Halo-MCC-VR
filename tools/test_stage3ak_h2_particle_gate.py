#!/usr/bin/env python3
"""Portable logic checks for the Stage 3AK H2 Classic particle gate."""

def suppress(renderer_gate: int, dl: int) -> bool:
    return (dl & 0xFF) != 0 and (renderer_gate & 0xFF) == 0

# Anniversary is never suppressed, regardless of first-person ownership.
for dl in (0,1,2,255):
    assert not suppress(1, dl)
# Classic suppresses current-user/FP dispatches only.
assert not suppress(0,0)
for dl in (1,2,255):
    assert suppress(0,dl)
# Unknown/nonzero renderer states fail open to stock.
assert not suppress(2,1)
print("Stage3AK portable H2 particle-gate logic: PASS")
