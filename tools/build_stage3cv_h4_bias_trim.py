"""Stage 3CV - vertical bias trim from the 3CU headset session's own captures.

Stage 3CU (14:28 session, layout -1635.814/731.878, theatre confirmed working
at 14:29:50) framed the FULL reticle for the first time on this layout:
four identical dumps measure 15783 ink texels, bbox x 113..398 (286 wide),
y 103..334 (232 tall), centroid (256, 218) - horizontally perfect, riding
58px high of the accepted placement (accepted 3BT look: centroid 15px below
centre at reticle height 174 -> scaled to today's 232-tall reticle = 20px
below centre = target centroid 276).

58px post-zoom = 23.2px pre-zoom off the bias: 39.95 - 23.2 = 16.75px of
H=4719.3 -> the measured anchor at |y|=731.878 becomes f = 0.003549 (3CU used
the estimate 0.008465, derived before the reticle's true height was
observable). The accepted-layout anchor is untouched; the law keeps its form

    KBIAS = max(0, B/|baseY| - A)

refit through (336.549, 0.0772547) and (731.878, 0.003549):
B = 45.9231, A = 0.0591976. Positive across every observed gameplay layout
(|y| 336.549..731.878; zero-clamp at 775.8). Identical payload structure to
3CU - only the two embedded floats change.
"""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import build_stage3cu_h4_two_point_bias as base

# refit the second anchor from the 3CU session's measured captures
base.NEW_F = 0.003549
base.B = (base.CAL_F - base.NEW_F) / (1.0 / base.CAL_Y - 1.0 / base.NEW_Y)
base.A = base.B / base.CAL_Y - base.CAL_F

if __name__ == "__main__":
    print("Stage 3CV trim: anchor f(%.3f) %.6f -> %.6f; B=%.5f A=%.7f"
          % (base.NEW_Y, 0.008465, base.NEW_F, base.B, base.A))
    base.main()
