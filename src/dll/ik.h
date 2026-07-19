#pragma once

// Two-bone analytic IK for upper-body VRIK (docs/VRIK-ROADMAP.md, Stage B).
// Pure math, no engine types: positions are float[3] in any consistent space.
// The solver never distance-clamps the hand — at full reach the chain goes
// straight; past full reach the hand STILL sits at the target (the arm just
// cannot bend), per the project requirement "push my arm completely out and
// have the controller follow".

// Solve the elbow position for a shoulder->elbow->wrist chain.
//   shoulder:  chain root (from the biped chest socket)
//   target:    where the wrist must be (the real controller)
//   upperLen:  shoulder->elbow bone length
//   lowerLen:  elbow->wrist bone length
//   poleHint:  direction the elbow should prefer to bend toward (unit-ish,
//              e.g. down+behind from the torso frame)
//   outElbow:  solved elbow position
// Returns false only on degenerate input (zero lengths / non-finite).
bool IK_SolveTwoBone(const float shoulder[3], const float target[3],
                     float upperLen, float lowerLen, const float poleHint[3],
                     float outElbow[3]);

// Effective wrist position: the target itself (never clamped), provided for
// symmetry/clarity at call sites.
void IK_WristPosition(const float target[3], float outWrist[3]);
