#pragma once

// API basis vectors to engine quaternion (DESIGN-FORMATS §5.2; packet P3). Called by the echoreplay
// reader per pose; the tape reader takes its quaternions as given.

namespace replay::import {

enum class BasisVerdict : unsigned char {
    Ok,
    Mirrored,    // dot(recomputed left, API left) < 0.9: the caller reuses its prior quaternion and counts it
    Degenerate,  // zero or parallel forward/up; handled like Mirrored
};

// Re-orthonormalises with forward taking priority, then Shepperd's method on columns [left|up|forward].
// q is xyzw and is written only when the verdict is Ok.
BasisVerdict BasisToQuat(const float forward[3], const float up[3], const float left[3], float q[4]);

// Negates q when dot(previous, q) < 0 so slerp between them takes the short arc.
void AlignHemisphere(const float previous[4], float q[4]);

}  // namespace replay::import
