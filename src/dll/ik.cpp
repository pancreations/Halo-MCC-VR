#include <cmath>
#include "ik.h"

// Standard analytic two-bone IK (law of cosines), with the elbow placed on the
// circle of valid solutions closest to the pole hint. No engine dependencies so
// the math can be exercised standalone against fixed poses.

namespace
{
    float Dot(const float* a, const float* b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    void Cross(const float* a, const float* b, float* out)
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }
    bool Normalize(float* v)
    {
        const float len = sqrtf(Dot(v, v));
        if (!(len > 1e-6f)) return false;
        v[0] /= len; v[1] /= len; v[2] /= len;
        return true;
    }
}

bool IK_SolveTwoBone(const float shoulder[3], const float target[3],
                     float upperLen, float lowerLen, const float poleHint[3],
                     float outElbow[3])
{
    if (!(upperLen > 1e-5f) || !(lowerLen > 1e-5f)) return false;
    float toTarget[3] = {target[0] - shoulder[0], target[1] - shoulder[1],
                         target[2] - shoulder[2]};
    float reach = sqrtf(Dot(toTarget, toTarget));
    for (int i = 0; i < 3; ++i)
        if (!isfinite(shoulder[i]) || !isfinite(target[i]) || !isfinite(poleHint[i]))
            return false;

    // Fully (or over-) extended: straight chain toward the target. The wrist
    // itself is NEVER pulled back — only the elbow stops bending.
    const float maxReach = upperLen + lowerLen;
    if (reach >= maxReach - 1e-5f || reach < 1e-5f)
    {
        float dir[3] = {toTarget[0], toTarget[1], toTarget[2]};
        if (!Normalize(dir)) { dir[0] = 1; dir[1] = 0; dir[2] = 0; }
        outElbow[0] = shoulder[0] + dir[0] * upperLen;
        outElbow[1] = shoulder[1] + dir[1] * upperLen;
        outElbow[2] = shoulder[2] + dir[2] * upperLen;
        return true;
    }
    // Also handle a target closer than |upper-lower| (arm folded past limit):
    // place the elbow on the fold line.
    const float minReach = fabsf(upperLen - lowerLen);
    if (reach <= minReach + 1e-5f)
        reach = minReach + 1e-5f;

    // Law of cosines: distance of the elbow's projection along the chain axis.
    const float a = (reach * reach + upperLen * upperLen - lowerLen * lowerLen) /
                    (2.0f * reach);
    float h2 = upperLen * upperLen - a * a;
    if (h2 < 0.0f) h2 = 0.0f;
    const float h = sqrtf(h2);

    float axis[3] = {toTarget[0], toTarget[1], toTarget[2]};
    Normalize(axis);
    // Bend direction: pole hint made perpendicular to the chain axis.
    float bend[3] = {poleHint[0], poleHint[1], poleHint[2]};
    const float along = Dot(bend, axis);
    bend[0] -= axis[0] * along;
    bend[1] -= axis[1] * along;
    bend[2] -= axis[2] * along;
    if (!Normalize(bend))
    {
        // Pole parallel to the chain: pick any stable perpendicular.
        float up[3] = {0, 0, 1};
        Cross(axis, up, bend);
        if (!Normalize(bend))
        {
            float side[3] = {0, 1, 0};
            Cross(axis, side, bend);
            Normalize(bend);
        }
    }
    outElbow[0] = shoulder[0] + axis[0] * a + bend[0] * h;
    outElbow[1] = shoulder[1] + axis[1] * a + bend[1] * h;
    outElbow[2] = shoulder[2] + axis[2] * a + bend[2] * h;
    return isfinite(outElbow[0]) && isfinite(outElbow[1]) && isfinite(outElbow[2]);
}

void IK_WristPosition(const float target[3], float outWrist[3])
{
    outWrist[0] = target[0];
    outWrist[1] = target[1];
    outWrist[2] = target[2];
}
