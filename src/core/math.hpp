#pragma once

#include <cmath>

// The little matrix math the scene renderer needs: column-major 4x4 like GLSL, so a Mat4
// goes into a uniform block as it is.
namespace glint::math {

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }

    static Mat4 identity() { return {}; }

    static Mat4 translate(float x, float y, float z) {
        Mat4 r;
        r.at(0, 3) = x;
        r.at(1, 3) = y;
        r.at(2, 3) = z;
        return r;
    }

    static Mat4 scale(float x, float y, float z) {
        Mat4 r;
        r.at(0, 0) = x;
        r.at(1, 1) = y;
        r.at(2, 2) = z;
        return r;
    }

    // Rotations in radians about one axis.
    static Mat4 rotate_x(float a) {
        Mat4 r;
        r.at(1, 1) = std::cos(a);
        r.at(1, 2) = -std::sin(a);
        r.at(2, 1) = std::sin(a);
        r.at(2, 2) = std::cos(a);
        return r;
    }
    static Mat4 rotate_y(float a) {
        Mat4 r;
        r.at(0, 0) = std::cos(a);
        r.at(0, 2) = std::sin(a);
        r.at(2, 0) = -std::sin(a);
        r.at(2, 2) = std::cos(a);
        return r;
    }
    static Mat4 rotate_z(float a) {
        Mat4 r;
        r.at(0, 0) = std::cos(a);
        r.at(0, 1) = -std::sin(a);
        r.at(1, 0) = std::sin(a);
        r.at(1, 1) = std::cos(a);
        return r;
    }

    // Maps [left, right] x [bottom, top] to Vulkan clip space, where y points down: bottom
    // ends at +1, top at -1. Depth 0 to 1 over [near, far].
    static Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z) {
        Mat4 r;
        r.at(0, 0) = 2.0f / (right - left);
        r.at(1, 1) = -2.0f / (top - bottom);
        r.at(2, 2) = 1.0f / (far_z - near_z);
        r.at(0, 3) = -(right + left) / (right - left);
        r.at(1, 3) = (top + bottom) / (top - bottom);
        r.at(2, 3) = -near_z / (far_z - near_z);
        return r;
    }

    friend Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += a.at(row, k) * b.at(k, col);
                r.at(row, col) = sum;
            }
        }
        return r;
    }

    // General inverse; the identity for a singular matrix.
    Mat4 inverse() const {
        const float* a = m;
        float inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
                 a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
                 a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
                 a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
                  a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
                 a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
                 a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
                 a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
                  a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
                 a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
                 a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
                  a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
                  a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
                 a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
                 a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
                  a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
                  a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (std::fabs(det) < 1e-20f) return {};
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
        return r;
    }
};

}  // namespace glint::math
