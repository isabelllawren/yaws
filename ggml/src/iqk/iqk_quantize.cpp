//
// Copyright (C) 2024 Iwan Kawrakow
// MIT license
// SPDX-License-Identifier: MIT
//

#if GGML_USE_IQK_MULMAT
#include "iqk_mul_mat.h"
#endif
#include "ggml-quants.h"
#include "ggml-impl.h"
#define GGML_COMMON_IMPL_C
#include "ggml-common.h"
#include "iqk_quantize.h"

#include <vector>
#include <utility>
#include <cstdint>
#include <cmath>
#include <array>
#include <algorithm>
#include <cstring>

namespace {

inline int nearest_int(float fval) {
    assert(fval <= 4194303.f);
    float val = fval + 12582912.f;
    int i; memcpy(&i, &val, sizeof(int));
    return (i & 0x007fffff) - 0x00400000;
}

struct IQ1BNQuantizer {
    int8_t L[QK_IQ1BN];
    void quantize_one_row_1bn(const float * src, block_iq1_bn * y, int n_per_row, const float * imatrix);
    void quantize_one_row_2bn(const float * src, block_iq2_bn * y, int n_per_row, const float * imatrix);
    static inline float row_max(int n_per_row, const float * src) {
        float max_in_row = 0;
        for (int j = 0; j < n_per_row; ++j) {
            float ax = fabsf(src[j]);
            max_in_row = std::max(max_in_row, ax);
        }
        return max_in_row;
    }
    static constexpr uint8_t k_mult[5] = {81, 27, 9, 3, 1};
};

void IQ1BNQuantizer::quantize_one_row_1bn(const float * src, block_iq1_bn * y, int n_per_row, const float * imatrix) {

    static const int k_nb[6] = {1, 3, 9, 27, 81, 243};
    (void)imatrix;

    const int nblock = n_per_row/QK_IQ1BN;

    for (int ib = 0; ib < nblock; ++ib) {
        std::memset(&y[ib], 0, sizeof(block_iq1_bn));
        auto xb = src + ib*QK_IQ1BN;
        int v13 = 0;
        for (int i16 = 0; i16 < QK_IQ1BN/16; ++i16) {
            for (int k = 0; k < 3; ++k) {
                int idx = 0;
                for (int j = 0; j < 5; ++j) {
                    float v = xb[16*i16 + 5*k + j];
                    int q = fabsf(v) < 1e-6f ? 1 : v < 0 ? 0 : 2;
                    idx += k_nb[j]*q;
                }
                idx = (256*idx + k_nb[5] - 1)/k_nb[5];
                y[ib].ql[3*i16 + k] = idx;
            }
            float v = xb[16*i16 + 15];
            int q = fabsf(v) < 1e-6f ? 1 : v < 0 ? 0 : 2;
            v13 += k_nb[i16]*q;
        }
        y[ib].extra = (256*v13 + k_nb[5] - 1)/k_nb[5];
    }
}

void IQ1BNQuantizer::quantize_one_row_2bn(const float * src, block_iq2_bn * y, int n_per_row, const float * imatrix) {

    (void)imatrix;

    const int nblock = n_per_row/QK_IQ1BN;

    constexpr int Nj = QK_IQ1BN/4;

    for (int ib = 0; ib < nblock; ++ib) {
        auto xb = src + QK_IQ1BN*ib;
        for (int j = 0; j < QK_IQ1BN; ++j) {
            L[j] = fabsf(xb[j]) < 1e-6f ? 1 : xb[j] < 0 ? 0 : 2;
        }
        for (int j = 0; j < Nj; ++j) {
            y[ib].qs[j] = L[j] | (L[j + Nj] << 2) | (L[j + 2*Nj] << 4) | (L[j + 3*Nj] << 6);
        }
    }
}

}

size_t quantize_iq1_bn(const float * src, void * dst, int64_t nrows, int64_t n_per_row, const float * imatrix) {
    IQ1BNQuantizer iq1bn;
    int nblock = n_per_row/QK_IQ1BN;
    block_iq1_bn * y = (block_iq1_bn *)dst;
    for (int row = 0; row < nrows; ++row) {
        iq1bn.quantize_one_row_1bn(src + row*n_per_row, y, n_per_row, imatrix);
        y += nblock;
    }
    return sizeof(block_iq1_bn)*nblock*nrows;
}

void quantize_row_iq1_bn_ref(const float * x, block_iq1_bn * y, int64_t k) {
    quantize_iq1_bn(x, y, 1, k, nullptr);
}

void quantize_row_iq1_bn(const float * x, void * y, int64_t k) {
    quantize_iq1_bn(x, y, 1, k, nullptr);
}

void dequantize_row_iq1_bn(const block_iq1_bn * x, float * y, int64_t k) {
    assert(k%QK_IQ1BN == 0);
    int nblock = k / QK_IQ1BN;

    for (int i = 0; i < nblock; ++i) {
        uint8_t extra = x[i].extra;
        auto ql = x[i].ql;
        for (int i16 = 0; i16 < QK_IQ1BN/16; ++i16) {
            for (int k = 0; k < 3; ++k) {
                for (int j = 0; j < 5; ++j) {
                    uint8_t v = ql[k]*IQ1BNQuantizer::k_mult[j];
                    int8_t vs = ((v + (v >> 1)) >> 7);
                    *y++ = vs - 1;
                }
            }
            ql += 3;
            uint8_t v = extra*IQ1BNQuantizer::k_mult[i16];
            int8_t vs = ((v + (v >> 1)) >> 7);
            *y++ = vs - 1;
        }
    }
}

size_t quantize_iq2_bn(const float * src, void * dst, int64_t nrows, int64_t n_per_row, const float * imatrix) {
    IQ1BNQuantizer iq1bn;
    int nblock = n_per_row/QK_IQ1BN;
    block_iq2_bn * y = (block_iq2_bn *)dst;
    for (int row = 0; row < nrows; ++row) {
        iq1bn.quantize_one_row_2bn(src + row*n_per_row, y, n_per_row, imatrix);
        y += nblock;
    }
    return sizeof(block_iq2_bn)*nblock*nrows;
}

void quantize_row_iq2_bn_ref(const float * x, block_iq2_bn * y, int64_t k) {
    quantize_iq2_bn(x, y, 1, k, nullptr);
}

void quantize_row_iq2_bn(const float * x, void * y, int64_t k) {
    quantize_iq2_bn(x, y, 1, k, nullptr);
}

void dequantize_row_iq2_bn(const block_iq2_bn * x, float * y, int64_t k) {
    assert(k%QK_IQ1BN == 0);
    int nblock = k / QK_IQ1BN;

    auto d1 = 1.f, d2 = 0.25f, d3 = d2*0.25f, d4 = d3*0.25f;
    auto m = -1.f;
    constexpr int Nj = QK_IQ1BN/4;
    for (int i = 0; i < nblock; ++i) {
        for (int j = 0; j < Nj; ++j) {
            y[j+   0] = d1*(x[i].qs[j] & 0x03) + m;
            y[j+1*Nj] = d2*(x[i].qs[j] & 0x0c) + m;
            y[j+2*Nj] = d3*(x[i].qs[j] & 0x30) + m;
            y[j+3*Nj] = d4*(x[i].qs[j] & 0xc0) + m;
        }
        y += QK_IQ1BN;
    }
}

namespace {
inline int8_t iq1bn_dequant(uint8_t q, int i) {
    uint8_t v = IQ1BNQuantizer::k_mult[i]*q;
    //int8_t vs = (v + (v << 1)) >> 8;
    int8_t vs = 3*v >> 8;
    return vs - 1;
}
}

static const int8_t iq1bn_values[1280] = {
    -1, -1, -1, -1, -1,  0,  0,  0,  0,  0,  0, -1, -1, -1, -1,  1, -1, -1, -1, -1, -1,  0, -1, -1, -1,  0,  0, -1, -1, -1,  1,  0,
    -1, -1, -1, -1,  1, -1, -1, -1,  0,  1, -1, -1, -1,  1,  1, -1, -1, -1, -1, -1,  0, -1, -1,  0, -1,  0, -1, -1,  1, -1,  0, -1,
    -1, -1,  0,  0, -1, -1,  0,  0,  0, -1, -1,  1,  0,  0, -1, -1, -1,  1,  0, -1, -1,  0,  1,  0, -1, -1,  1,  1,  0, -1, -1, -1,
    -1,  1, -1, -1,  0,  0,  0,  0,  0,  0, -1,  1, -1, -1,  1, -1,  1, -1, -1, -1,  0,  1, -1, -1,  0,  0,  1, -1, -1,  1,  0,  1,
    -1, -1, -1,  1,  1, -1, -1,  0,  1,  1, -1, -1,  1,  1,  1, -1, -1, -1, -1, -1,  0, -1,  0, -1, -1,  0, -1,  1, -1, -1,  0, -1,
    -1,  0, -1,  0, -1,  0,  0, -1,  0, -1,  1,  0, -1,  0, -1, -1,  1, -1,  0, -1,  0,  1, -1,  0, -1,  1,  1, -1,  0, -1, -1, -1,
     0,  0, -1,  0, -1,  0,  0, -1,  0,  0,  0,  0,  0,  1, -1,  0,  0, -1, -1,  0,  0,  0, -1,  0,  0,  0,  0, -1,  1,  0,  0,  0,
    -1, -1,  1,  0,  0, -1,  0,  1,  0,  0, -1,  1,  1,  0,  0, -1, -1, -1,  1,  0, -1,  0, -1,  1,  0, -1,  1, -1,  1,  0, -1, -1,
     0,  1,  0, -1,  0,  0,  1,  0, -1,  1,  0,  1,  0, -1, -1,  1,  1,  0, -1,  0,  1,  1,  0, -1,  1,  1,  1,  0, -1, -1, -1, -1,
     1, -1,  0, -1, -1,  1, -1,  1, -1, -1,  1, -1,  0,  0,  0,  0,  0, -1,  0, -1,  1, -1,  0,  0, -1,  1, -1,  1,  0, -1,  1, -1,
    -1,  1, -1,  1, -1,  0,  1, -1,  1, -1,  1,  1, -1,  1, -1, -1, -1,  0,  1, -1,  0, -1,  0,  1, -1,  1, -1,  0,  1, -1, -1,  0,
     0,  1, -1,  0,  0,  0,  1, -1,  1,  0,  0,  1, -1, -1,  1,  0,  1, -1,  0,  1,  0,  1, -1,  1,  1,  0,  1, -1, -1, -1,  1,  1,
    -1,  0, -1,  1,  1, -1,  1, -1,  1,  1, -1,  0,  0,  0,  0,  0, -1,  0,  1,  1, -1,  0,  0,  1,  1, -1,  1,  0,  1,  1, -1, -1,
     1,  1,  1, -1,  0,  1,  1,  1, -1,  1,  1,  1,  1, -1, -1, -1, -1, -1,  0,  0, -1, -1, -1,  0,  1, -1, -1, -1,  0, -1,  0, -1,
    -1,  0,  0,  0, -1, -1,  0,  1,  0, -1, -1,  0, -1,  1, -1, -1,  0,  0,  1, -1, -1,  0,  1,  1, -1, -1,  0, -1, -1,  0, -1,  0,
     0, -1,  0, -1,  0,  1, -1,  0, -1,  0, -1,  0,  0, -1,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  0,  1,  0,  0, -1,  0, -1,  1,
     0, -1,  0,  0,  1,  0, -1,  0,  1,  1,  0, -1,  0, -1, -1,  1, -1,  0,  0, -1,  1, -1,  0,  1, -1,  1, -1,  0, -1,  0,  1, -1,
     0,  0,  0,  1, -1,  0,  1,  0,  1, -1,  0, -1,  1,  1, -1,  0,  0,  1,  1, -1,  0,  1,  1,  1, -1,  0, -1, -1, -1,  0,  0,  0,
    -1, -1,  0,  0,  1, -1, -1,  0,  0, -1,  0, -1,  0,  0,  0,  0, -1,  0,  0,  0,  0,  0,  0,  0,  1,  0, -1,  0,  0, -1,  1, -1,
     0,  0,  0,  1, -1,  0,  0,  1,  1, -1,  0,  0, -1, -1,  0,  0,  0,  0, -1,  0,  0,  0,  1, -1,  0,  0,  0, -1,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  1,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  1,  0,  0,  0,  1,  1,  0,  0,  0, -1, -1,  1,  0,  0,  0, -1,
     1,  0,  0,  1, -1,  1,  0,  0, -1,  0,  1,  0,  0,  0,  0,  1,  0,  0,  0,  0,  0,  0,  0,  1,  0,  1,  0,  0, -1,  1,  1,  0,
     0,  0,  1,  1,  0,  0,  1,  1,  1,  0,  0, -1, -1, -1,  1,  0,  0, -1, -1,  1,  0,  1, -1, -1,  1,  0, -1,  0, -1,  1,  0,  0,
     0, -1,  1,  0,  1,  0, -1,  1,  0, -1,  1, -1,  1,  0,  0,  1, -1,  1,  0,  1,  1, -1,  1,  0, -1, -1,  0,  1,  0,  0, -1,  0,
     1,  0,  1, -1,  0,  1,  0, -1,  0,  0,  1,  0,  0,  0,  0,  1,  0,  1,  0,  0,  1,  0,  0,  0,  0,  0,  0, -1,  1,  0,  1,  0,
     0,  1,  0,  1,  0,  1,  1,  0,  1,  0, -1, -1,  1,  1,  0,  0, -1,  1,  1,  0,  1, -1,  1,  1,  0, -1,  0,  1,  1,  0,  0,  0,
     1,  1,  0,  1,  0,  1,  1,  0, -1,  1,  1,  1,  0,  0,  1,  1,  1,  0,  1,  1,  1,  1,  0, -1, -1, -1, -1,  1,  0, -1, -1, -1,
     1,  1, -1, -1, -1,  1, -1,  0, -1, -1,  1,  0,  0, -1, -1,  1,  1,  0, -1, -1,  1, -1,  1, -1, -1,  1,  0,  0,  0,  0,  0,  0,
     1, -1, -1,  1,  1,  1, -1, -1,  1, -1, -1,  0, -1,  1,  0, -1,  0, -1,  1,  1, -1,  0, -1,  1, -1,  0,  0, -1,  1,  0,  0,  0,
    -1,  1,  1,  0,  0, -1,  1, -1,  1,  0, -1,  1,  0,  1,  0, -1,  1,  1,  1,  0, -1,  1, -1, -1,  1, -1,  1,  0, -1,  1, -1,  1,
     1, -1,  1, -1,  1, -1,  0,  1, -1,  1,  0,  0,  1, -1,  1,  1,  0,  1, -1,  1, -1,  1,  1, -1,  1,  0,  0,  0,  0,  0,  0,  1,
     1, -1,  1,  1,  1,  1, -1,  1, -1, -1, -1,  0,  1,  0, -1, -1,  0,  1,  1, -1, -1,  0,  1, -1,  0, -1,  0,  1,  0,  0, -1,  0,
     1,  1,  0, -1,  0,  1, -1,  1, -1,  0,  1,  0,  1, -1,  0,  1,  1,  1, -1,  0,  1, -1, -1,  0,  0,  1,  0, -1,  0,  0,  1,  1,
    -1,  0,  0,  1, -1,  0,  0,  0,  1,  0,  0,  0,  0,  1,  1,  0,  0,  0,  1, -1,  1,  0,  0,  1,  0,  1,  0,  0,  1,  0,  0,  0,
     0,  0,  1,  1,  0,  0,  1, -1, -1,  1,  0,  1,  0, -1,  1,  0,  1,  1, -1,  1,  0,  1, -1,  0,  1,  0,  1,  0,  0,  1,  0,  1,
     1,  0,  1,  0,  1, -1,  1,  1,  0,  1,  0,  1,  1,  0,  1,  1,  1,  1,  0,  1, -1, -1, -1,  1,  1,  0, -1, -1,  1,  1,  1, -1,
    -1,  1,  1, -1,  0, -1,  1,  1,  0,  0, -1,  1,  1,  1,  0, -1,  1,  1, -1,  1, -1,  1,  1,  0,  1, -1,  1,  1,  1,  1, -1,  1,
     1,  0,  0,  0,  0,  0, -1, -1,  0,  1,  1,  0, -1,  0,  1,  1,  1, -1,  0,  1,  1, -1,  0,  0,  1,  1,  0,  0,  0,  1,  1,  1,
     0,  0,  1,  1, -1,  1,  0,  1,  1,  0,  1,  0,  1,  1,  1,  1,  0,  1,  1, -1, -1,  1,  1,  1,  0, -1,  1,  1,  1,  1, -1,  1,
     1,  1, -1,  0,  1,  1,  1,  0,  0,  1,  1,  1,  1,  0,  1,  1,  1, -1,  1,  1,  1,  1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,
};

void ggml_vec_dot_iq1_bn_q8_K64(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {

    GGML_UNUSED(bs);
    GGML_UNUSED(bx);
    GGML_UNUSED(by);
    GGML_UNUSED(nrc);

    static_assert(QK_IQ1BN == 64, "This dot product implementation for iq1_bn requires a block size of 64");

#if GGML_USE_IQK_MULMAT
    if (iqk_mul_mat(1, 1, n, GGML_TYPE_IQ1_BN, vx, 0, GGML_TYPE_Q8_K64, vy, 0, s, 0, 0, 1)) {
        return;
    }
#endif

    const block_iq1_bn * x = (const block_iq1_bn *)vx;

    const float * d8 = (const float *)vy;
    const int8_t * q8 = (const int8_t *)(d8 + 4);
    int nblock = n / QK_IQ1BN;

    int sumi[8] = {};
    int8_t q1[16];

    for (int ii = 0; ii < nblock; ii += 32) {
        int16_t sum16[8] = {};
        int nb = std::min(ii + 32, nblock);
        for (int i = ii; i < nb; ++i) {
            auto ql = x[i].ql;
            const int8_t * extra = iq1bn_values + 5*x[i].extra;
            for (int i16 = 0; i16 < QK_IQ1BN/16; ++i16) {
                for (int k = 0; k < 3; ++k) {
                    uint8_t q = *ql++;
                    const int8_t * vs = iq1bn_values + 5*q;
                    for (int j = 0; j < 5; ++j) q1[5*k+j] = vs[j];
                }
                q1[15] = extra[i16];
                // We collect 8 q8 values per block into each element of sum16
                // => 32 x 8 = 256 values in each loop over i, so this cannot overflow the int16_t range
                //    (q8 is in -127...127, and hence the sum is in -32512...32512
                for (int j = 0; j < 8; ++j) sum16[j] += q8[2*j+0]*q1[2*j+0] + q8[2*j+1]*q1[2*j+1];
                q8 += 16;
            }
        }
        for (int j = 0; j < 8; ++j) sumi[j] += sum16[j];
    }

    *s = d8[0] * (sumi[0] + sumi[1]) + d8[1] * (sumi[2] + sumi[3]) + d8[2] * (sumi[4] + sumi[5]) + d8[3] * (sumi[6] + sumi[7]);
}

void ggml_vec_dot_iq2_bn_q8_K64(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {

    GGML_ASSERT(nrc == 1);
    GGML_UNUSED(bs);
    GGML_UNUSED(bx);
    GGML_UNUSED(by);
    GGML_UNUSED(nrc);

    static_assert(QK_IQ1BN == 64, "This dot product implementation for iq2_bn requires a block size of 64");

    if (iqk_mul_mat(1, 1, n, GGML_TYPE_IQ2_BN, vx, 0, GGML_TYPE_Q8_K64, vy, 0, s, 0, 0, 1)) {
        return;
    }

    constexpr int Nj = QK_IQ1BN/4;

    const block_iq2_bn * x = (const block_iq2_bn *)vx;
    int nblock = n / QK_IQ1BN;

    const float * d = (const float *)vy;
    const int8_t * q8 = (const int8_t *)(d + 4);

    int sum[16] = { };
    int sum0[4] = { };

    for (int i = 0; i < nblock; ++i) {
        for (int j = 0; j < Nj/4; ++j) {
            for (int l = 0; l < 4; ++l) {
                sum[4*j + 0] += q8[4*j + l +    0] * (x[i].qs[4*j+l] & 0x03);
                sum[4*j + 1] += q8[4*j + l + 1*Nj] * (x[i].qs[4*j+l] & 0x0c);
                sum[4*j + 2] += q8[4*j + l + 2*Nj] * (x[i].qs[4*j+l] & 0x30);
                sum[4*j + 3] += q8[4*j + l + 3*Nj] * (x[i].qs[4*j+l] & 0xc0);
                sum0[j] += q8[4*j + l] + q8[4*j + l + 1*Nj] + q8[4*j + l + 2*Nj] + q8[4*j + l + 3*Nj];
            }
        }
        q8 += QK_IQ1BN;
    }

    float sumf = 0;
    for (int j = 0; j < 4; ++j) {
        sumf += d[j] * (sum[4*j + 0] + 0.25f*sum[4*j + 1] + 0.0625*sum[4*j + 2] + 0.015625*sum[4*j + 3] - sum0[j]);
    }
    *s = sumf;

}

void quantize_row_q8_K64_ref(const float * x, block_q8_K64 * y, int64_t k) {

    float * dptr = (float *)y;
    auto qs = (int8_t *)(dptr + 4);
#ifdef __ARM_NEON
    static const uint8_t k_shuffle[16] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60};
    auto shuffle = vld1q_u8(k_shuffle);
    float32x4_t max[4] = { };
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            auto val = vld1q_f32(x + j + 4*i);
            val = vabsq_f32(val);
            max[i] = vmaxq_f32(max[i], val);
        }
    }
    float32x4_t vid[4];
    for (int i = 0; i < 4; ++i) {
        dptr[i] = vmaxvq_f32(max[i])/127;
        float id = dptr[i] > 0 ? 1/dptr[i] : 0.f;
        vid[i] = vdupq_n_f32(id);
    }
    int8x16x4_t q;
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            auto val = vld1q_f32(x + j + 4*i);
            val = vmulq_f32(vid[i], val);
            q.val[i] = vreinterpretq_s8_s32(vcvtnq_s32_f32(val));
        }
        auto qi = vqtbl4q_s8(q, shuffle);
        vst1q_s8(qs, qi);
        qs += 16;
    }
#elif defined __AVX__
    __m128 max[4] = {};
    __m128 sign_bit = _mm_set1_ps(-0.f);
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            auto val = _mm_loadu_ps(x + j + 4*i);
            val = _mm_andnot_ps(sign_bit, val);
            max[i] = _mm_max_ps(max[i], val);
        }
    }
    __m128 vid[4];
    for (int i = 0; i < 4; ++i) {
        max[i] = _mm_max_ps(max[i], _mm_movehl_ps(max[i], max[i]));
        max[i] = _mm_max_ss(max[i], _mm_movehdup_ps(max[i]));
        float maxi = _mm_cvtss_f32(max[i]);
        dptr[i] = maxi/127;
        float id = dptr[i] > 0 ? 1/dptr[i] : 0.f;
        vid[i] = _mm_set1_ps(id);
    }
    __m128i q[4];
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            auto val = _mm_loadu_ps(x + j + 4*i);
            val = _mm_round_ps(_mm_mul_ps(vid[i], val), _MM_ROUND_NEAREST);
            q[i] = _mm_cvtps_epi32(val);
        }
        auto q1 = _mm_packs_epi32(q[0], q[1]);
        auto q2 = _mm_packs_epi32(q[2], q[3]);
        auto qi = _mm_packs_epi16(q1, q2);
        _mm_storeu_si128((__m128i *)qs, qi);
        qs += 16;
    }
#else
    float aux[4] = {0.f, 0.f, 0.f, 0.f};
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            for (int l = 0; l < 4; ++l) {
                float ax = fabsf(x[j+4*i+l]);
                aux[i] = std::max(aux[i], ax);
            }
        }
    }
    for (int i = 0; i < 4; ++i) {
        dptr[i] = aux[i]/127;
        aux[i] = dptr[i] > 0 ? 1/dptr[i] : 0.f;
    }
    for (int j = 0; j < k; j += 16) {
        for (int i = 0; i < 4; ++i) {
            for (int l = 0; l < 4; ++l) qs[j+4*i+l] = nearest_int(aux[i]*x[j+4*i+l]);
        }
    }
#endif
}

void quantize_row_q8_K64(const float * x, void * y, int64_t k) {
    quantize_row_q8_K64_ref(x, (block_q8_K64 *)y, k);
}

//
// ============================================== iq4_K
//
void dequantize_row_iq4_k(const block_iq4_k * x, float * y, int64_t k) {
    assert(k % QK_K == 0);
    const int nb = k / QK_K;

    for (int i = 0; i < nb; i++) {

        const uint8_t * qs = x[i].qs;

        const float d = GGML_FP16_TO_FP32(x[i].d);

        uint16_t extra = x[i].extra;

        for (int ib = 0; ib < QK_K/32; ++ib) {
            const uint8_t sh = x[i].scales_h[ib/2] >> 4*(ib%2);
            const float dl1 = d * (((x[i].scales_l[ib] & 0xf) | ((sh << 4) & 0x30)) - 32);
            const float dl2 = d * (((x[i].scales_l[ib] >>  4) | ((sh << 2) & 0x30)) - 32);
            const int8_t * values1 = extra & 1 ? iq4k_values + 16 : iq4k_values;
            const int8_t * values2 = extra & 2 ? iq4k_values + 16 : iq4k_values;
            extra >>= 2;
            for (int j = 0; j < 16; ++j) {
                y[j+ 0] = dl1 * values1[qs[j] & 0xf];
                y[j+16] = dl2 * values2[qs[j] >>  4];
            }
            y  += 32;
            qs += 16;
        }
    }
}

void vec_dot_iq4_k_q8_k(int n, float * s, size_t bs, const void * vx, size_t bx, const void * vy, size_t by, int nrc) {
    assert(n % QK_K == 0);
    assert(nrc == 1);
    GGML_UNUSED(nrc);
    GGML_UNUSED(bx);
    GGML_UNUSED(by);
    GGML_UNUSED(bs);

    if (iqk_mul_mat(1, 1, n, GGML_TYPE_IQ4_K, vx, 0, GGML_TYPE_Q8_K, vy, 0, s, 0, 0, 1)) {
        return;
    }

    const int nb = n / QK_K;

    const block_iq4_k * x = (const block_iq4_k *)vx;
    const block_q8_K  * y = (const block_q8_K *)vy;

    float sumf = 0;
    for (int ibl = 0; ibl < nb; ++ibl) {
        const float d4d8 = GGML_FP16_TO_FP32(x[ibl].d) * y[ibl].d;
        uint16_t extra = x[ibl].extra;
        uint32_t h = *((const uint32_t *)x[ibl].scales_h);
        const uint8_t * qs = x[ibl].qs;
        const int8_t  * q8 = y[ibl].qs;
        int32_t sum = 0;
        for (int ib = 0; ib < QK_K/32; ++ib) {
            const int ls1 = ((x[ibl].scales_l[ib] & 0xf) | ((h << 4) & 0x30)) - 32;
            const int ls2 = ((x[ibl].scales_l[ib] >>  4) | ((h << 2) & 0x30)) - 32;
            h >>= 4;
            const int8_t * values1 = iq4k_values + 16*(extra & 1);
            const int8_t * values2 = iq4k_values +  8*(extra & 2);
            extra >>= 2;
            int sumi1 = 0, sumi2 = 0;
            for (int j = 0; j < 16; ++j) {
                sumi1 += q8[j+ 0] * values1[qs[j] & 0xf];
                sumi2 += q8[j+16] * values2[qs[j] >>  4];
            }
            sum += ls1*sumi1 + ls2*sumi2;
            qs += 16;
            q8 += 32;
        }
        sumf += d4d8 * sum;
    }
    *s = sumf;

}

namespace {
const int8_t iq4nl_index[241] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  3,  3,
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,
     5,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
     8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14
};
inline int best_index_iq4nl(const int8_t * values, float x) {
    if (x <= values[ 0]) return  0;
    if (x >= values[15]) return 15;
    int index = iq4nl_index[(int)x - values[0]];
    return x - values[index] < values[index+1] - x ? index : index + 1;
}

static void quantize_row_iq4_k_impl_bs16(const int super_block_size, const int block_size, const float * x,
        block_iq4_k * y,
        float * scales, float * weight, uint8_t * L,
        const int8_t * values,
        const float * quant_weights,
        const int ntry) {

    GGML_ASSERT(super_block_size == 256 && block_size == 16);

    float sigma2 = 0;
    for (int j = 0; j < super_block_size; ++j) sigma2 += x[j]*x[j];
    sigma2 *= 2.f/super_block_size;

    memset(y, 0, sizeof(block_iq4_k));
    y->d = GGML_FP32_TO_FP16(0.f);

    uint16_t * scales_h = (uint16_t *)y->scales_h;

    const int8_t * shifted_values = values + 16;

    float max_scale = 0, amax_scale = 0;
    uint16_t extra = 0;
    for (int ib = 0; ib < super_block_size/block_size; ++ib) {
        const float * xb = x + ib*block_size;
        if (quant_weights) {
            const float * qw = quant_weights + ib*block_size;
            for (int j = 0; j < block_size; ++j) weight[j] = qw[j] * sqrtf(sigma2 + xb[j]*xb[j]);
        } else {
            for (int j = 0; j < block_size; ++j) weight[j] = xb[j]*xb[j];
        }
        float amax = 0, max = 0;
        for (int j = 0; j < block_size; ++j) {
            float ax = fabsf(xb[j]);
            if (ax > amax) {
                amax = ax; max = xb[j];
            }
        }
        if (!amax) {
            scales[ib] = 0;
            continue;
        }
        float d = ntry > 0 ? -max/values[0] : max/values[0];
        float id = 1/d;
        float sumqx_p = 0, sumq2_p = 0;
        float sumqx_m = 0, sumq2_m = 0;
        for (int j = 0; j < block_size; ++j) {
            float w = weight[j];
            float al = id*xb[j];
            int l = best_index_iq4nl(values, al);
            float q = values[l];
            sumqx_p += w*q*xb[j];
            sumq2_p += w*q*q;
            l = best_index_iq4nl(values, -al);
            q = values[l];
            sumqx_m += w*q*xb[j];
            sumq2_m += w*q*q;
        }
        d = sumqx_p/sumq2_p;
        bool is_shifted = false;
        float best = d*sumqx_p;
        if (sumq2_m > 0 && sumqx_m*sumqx_m > best*sumq2_m) {
            d = sumqx_m/sumq2_m; best = d*sumqx_m;
        }
        for (int itry = -ntry; itry <= ntry; ++itry) {
            id = (itry + values[0])/max;
            sumqx_p = sumq2_p = 0;
            sumqx_m = sumq2_m = 0;
            for (int j = 0; j < block_size; ++j) {
                float w = weight[j];
                float al = id*xb[j];
                int l = best_index_iq4nl(values, al);
                float q = values[l];
                sumqx_p += w*q*xb[j];
                sumq2_p += w*q*q;
                l = best_index_iq4nl(values, -al);
                q = values[l];
                sumqx_m += w*q*xb[j];
                sumq2_m += w*q*q;
            }
            if (sumq2_p > 0 && sumqx_p*sumqx_p > best*sumq2_p) {
                d = sumqx_p/sumq2_p; best = d * sumqx_p; is_shifted = false;
            }
            if (sumq2_m > 0 && sumqx_m*sumqx_m > best*sumq2_m) {
                d = sumqx_m/sumq2_m; best = d * sumqx_m; is_shifted = false;
            }
            id = (itry + shifted_values[0])/max;
            sumqx_p = sumq2_p = 0;
            sumqx_m = sumq2_m = 0;
            for (int j = 0; j < block_size; ++j) {
                float w = weight[j];
                float al = id*xb[j];
                int l = best_index_iq4nl(shifted_values, al);
                float q = shifted_values[l];
                sumqx_p += w*q*xb[j];
                sumq2_p += w*q*q;
                l = best_index_iq4nl(shifted_values, -al);
                q = shifted_values[l];
                sumqx_m += w*q*xb[j];
                sumq2_m += w*q*q;
            }
            if (sumq2_p > 0 && sumqx_p*sumqx_p > best*sumq2_p) {
                d = sumqx_p/sumq2_p; best = d * sumqx_p; is_shifted = true;
            }
            if (sumq2_m > 0 && sumqx_m*sumqx_m > best*sumq2_m) {
                d = sumqx_m/sumq2_m; best = d * sumqx_m; is_shifted = true;
            }
        }
        if (is_shifted) extra |= (1 << ib);
        scales[ib] = d;
        float abs_d = fabsf(d);
        if (abs_d > amax_scale) {
            amax_scale = abs_d; max_scale = d;
        }
    }
    float d = -max_scale/32;
    y->d = GGML_FP32_TO_FP16(d);
    y->extra = extra;
    float id = d ? 1/d : 0.f;
    float sumqx = 0, sumq2 = 0;
    for (int ib = 0; ib < super_block_size/block_size; ++ib) {
        const int8_t * block_values = extra & (1 << ib) ? shifted_values : values;
        int l = nearest_int(id*scales[ib]);
        l = MAX(-32, MIN(31, l));
        float dl = d * l;
        float idl = dl ? 1/dl : 0.f;
        uint8_t * Lb = L + ib*block_size;
        const float * xb = x + ib*block_size;
        if (quant_weights) {
            const float * qw = quant_weights + ib*block_size;
            for (int j = 0; j < block_size; ++j) weight[j] = qw[j] * sqrtf(sigma2 + xb[j]*xb[j]);
        } else {
            for (int j = 0; j < block_size; ++j) weight[j] = xb[j]*xb[j];
        }
        for (int j = 0; j < block_size; ++j) {
            Lb[j] = best_index_iq4nl(block_values, idl*xb[j]);
            float w = weight[j];
            float q = block_values[Lb[j]]*l;
            sumqx += w*q*xb[j];
            sumq2 += w*q*q;
        }
        l += 32;
        uint8_t l_l = l & 0xf;
        uint8_t l_h = l >>  4;
        if (ib%2 == 0) y->scales_l[ib/2] = l_l;
        else y->scales_l[ib/2] |= (l_l << 4);
        scales_h[ib/8] |= (l_h << 2*(ib%8));
    }
    if (sumq2 > 0) y->d = GGML_FP32_TO_FP16(sumqx/sumq2);

    for (int i = 0; i < super_block_size/32; ++i) {
        for (int j = 0; j < 16; ++j) {
            y->qs[16*i + j] = L[32*i + j] | (L[32*i + 16 + j] << 4);
        }
    }
}

}

void quantize_row_iq4_k_ref(const float * x, block_iq4_k * y, int64_t k) {
    assert(k % QK_K == 0);
    quantize_iq4_k(x, (void *)y, 1, k, nullptr);
}

void quantize_row_iq4_k(const float * x, void * vy, int64_t k) {
    assert(k % QK_K == 0);
    block_iq4_k * y = (block_iq4_k *)vy;
    quantize_row_iq4_k_ref(x, y, k);
}

size_t quantize_iq4_k(const float * src, void * dst, int64_t nrows, int64_t n_per_row, const float * imatrix) {
    GGML_ASSERT(n_per_row%QK_K == 0);
    int nblock = n_per_row/QK_K;
    char * qrow = (char *)dst;
    uint8_t L[QK_K];
    float weight[16];
    float scales[QK_K/16];
    for (int64_t row = 0; row < nrows; ++row) {
        block_iq4_k * iq4 = (block_iq4_k *)qrow;
        for (int ibl = 0; ibl < nblock; ++ibl) {
            const float * qw = imatrix ? imatrix + QK_K*ibl : NULL;
            quantize_row_iq4_k_impl_bs16(QK_K, 16, src + QK_K*ibl, iq4 + ibl,
                    scales, weight, L, iq4k_values, qw, 7);
        }
        src += n_per_row;
        qrow += nblock*sizeof(block_iq4_k);
    }
    return nrows * nblock * sizeof(block_iq4_k);
}

//
// ============================================== iq2_K
//

namespace {

inline int best_index_iq2nl(const int8_t * values, float x) {
    int idx = x < values[1] ? 0 : x > values[2] ? 2 : 1;
    return x - values[idx] < values[idx+1] - x ? idx : idx + 1;
}

void quantize_row_iq2_k_impl(const float * x, void * vy, int n_per_row, const float * quant_weights) {

    constexpr int kBlockSize = 16;

    block_iq2_k * y = (block_iq2_k *)vy;

    float scales[QK_K/kBlockSize];
    float weight[kBlockSize];
    float sumx[kBlockSize+1], sumw[kBlockSize+1];

    std::array<std::pair<float,int>, kBlockSize> pairs;

    const int8_t * shifted_values = iq2nl_values + 4;

    for (int ibl = 0; ibl < n_per_row/QK_K; ++ibl) {

        memset(&y[ibl], 0, sizeof(block_iq2_k));
        y[ibl].d = GGML_FP32_TO_FP16(0.f);

        const float * xbl = x + ibl*QK_K;
        float sumx2 = 0;
        for (int j = 0; j < QK_K; ++j) sumx2 += xbl[j]*xbl[j];
        const float sigma2 = 1.5f*sumx2/QK_K;

        uint16_t extra = 0;

        float max_abs_scale = 0;

        for (int ib = 0; ib < QK_K/kBlockSize; ++ib) {
            const float * xb = xbl + kBlockSize*ib;
            if (quant_weights) {
                const float * qw = quant_weights + ibl*QK_K + ib*kBlockSize;
                for (int j = 0; j < kBlockSize; ++j) weight[j] = qw[j] * sqrtf(sigma2 + xb[j]*xb[j]);
            } else {
                for (int j = 0; j < kBlockSize; ++j) weight[j] = 0.25f*sigma2 + xb[j]*xb[j];
            }
            for (int j = 0; j < kBlockSize; ++j) pairs[j] = {xb[j], j};
            std::sort(pairs.begin(), pairs.end());
            sumx[0] = sumw[0] = 0;
            for (int j = 0; j < kBlockSize; ++j) {
                int jj = pairs[j].second;
                sumw[j+1] = sumw[j] + weight[jj];
                sumx[j+1] = sumx[j] + weight[jj]*xb[jj];
            }
            float best = 0, d = 0;
            bool is_shifted = false;
            float sumqx, sumq2;
            for (int i1 = 0; i1 < kBlockSize; ++i1) {
                for (int i2 = i1; i2 < kBlockSize; ++i2) {
                    for (int i3 = i2; i3 < kBlockSize; ++i3) {
                        sumqx = (sumx[i1] - sumx[ 0])*iq2nl_values[0] + (sumx[i2] - sumx[i1])*iq2nl_values[1]
                              + (sumx[i3] - sumx[i2])*iq2nl_values[2] + (sumx[kBlockSize] - sumx[i3])*iq2nl_values[3];
                        sumq2 = (sumw[i1] - sumw[ 0])*iq2nl_values[0]*iq2nl_values[0] + (sumw[i2] - sumw[i1])*iq2nl_values[1]*iq2nl_values[1]
                              + (sumw[i3] - sumw[i2])*iq2nl_values[2]*iq2nl_values[2] + (sumw[kBlockSize] - sumw[i3])*iq2nl_values[3]*iq2nl_values[3];
                        if (sumq2 > 0 && sumqx*sumqx > best*sumq2) {
                            d = sumqx/sumq2; best = d*sumqx; is_shifted = false;
                        }
                        sumqx = (sumx[i1] - sumx[ 0])*shifted_values[0] + (sumx[i2] - sumx[i1])*shifted_values[1]
                              + (sumx[i3] - sumx[i2])*shifted_values[2] + (sumx[kBlockSize] - sumx[i3])*shifted_values[3];
                        sumq2 = (sumw[i1] - sumw[ 0])*shifted_values[0]*shifted_values[0] + (sumw[i2] - sumw[i1])*shifted_values[1]*shifted_values[1]
                              + (sumw[i3] - sumw[i2])*shifted_values[2]*shifted_values[2] + (sumw[kBlockSize] - sumw[i3])*shifted_values[3]*shifted_values[3];
                        if (sumq2 > 0 && sumqx*sumqx > best*sumq2) {
                            d = sumqx/sumq2; best = d*sumqx; is_shifted = true;
                        }
                        sumqx = (sumx[i1] - sumx[ 0])*iq2nl_values[3] + (sumx[i2] - sumx[i1])*iq2nl_values[2]
                              + (sumx[i3] - sumx[i2])*iq2nl_values[1] + (sumx[kBlockSize] - sumx[i3])*iq2nl_values[0];
                        sumq2 = (sumw[i1] - sumw[ 0])*iq2nl_values[3]*iq2nl_values[3] + (sumw[i2] - sumw[i1])*iq2nl_values[2]*iq2nl_values[2]
                              + (sumw[i3] - sumw[i2])*iq2nl_values[1]*iq2nl_values[1] + (sumw[kBlockSize] - sumw[i3])*iq2nl_values[0]*iq2nl_values[0];
                        if (sumq2 > 0 && sumqx*sumqx > best*sumq2) {
                            d = sumqx/sumq2; best = d*sumqx; is_shifted = false;
                        }
                        sumqx = (sumx[i1] - sumx[ 0])*shifted_values[3] + (sumx[i2] - sumx[i1])*shifted_values[2]
                              + (sumx[i3] - sumx[i2])*shifted_values[1] + (sumx[kBlockSize] - sumx[i3])*shifted_values[0];
                        sumq2 = (sumw[i1] - sumw[ 0])*shifted_values[3]*shifted_values[3] + (sumw[i2] - sumw[i1])*shifted_values[2]*shifted_values[2]
                              + (sumw[i3] - sumw[i2])*shifted_values[1]*shifted_values[1] + (sumw[kBlockSize] - sumw[i3])*shifted_values[0]*shifted_values[0];
                        if (sumq2 > 0 && sumqx*sumqx > best*sumq2) {
                            d = sumqx/sumq2; best = d*sumqx; is_shifted = true;
                        }
                    }
                }
            }
            scales[ib] = d;
            if (is_shifted) extra |= (1 << ib);

            float abs_scale = fabsf(scales[ib]);
            max_abs_scale = MAX(max_abs_scale, abs_scale);
        }

        if (!max_abs_scale) continue;

        float d = max_abs_scale/15;
        y[ibl].d = GGML_FP32_TO_FP16(d);
        y[ibl].extra = extra;
        float id = 1/d;

        float sumqx = 0, sumq2 = 0;
        for (int ib = 0; ib < QK_K/kBlockSize; ++ib) {
            int ls = nearest_int(0.5f*(id*scales[ib]+15));
            ls = MAX(0, MIN(15, ls));
            y[ibl].scales[ib/2] |= (ls << 4*(ib%2));
            ls = 2*ls - 15;
            float dl = d * ls;
            if (dl) {
                const int8_t * block_values = y[ibl].extra & (1 << ib) ? shifted_values : iq2nl_values;
                const float * xb = xbl + kBlockSize*ib;
                if (quant_weights) {
                    const float * qw = quant_weights + ibl*QK_K + ib*kBlockSize;
                    for (int j = 0; j < kBlockSize; ++j) weight[j] = qw[j] * sqrtf(sigma2 + xb[j]*xb[j]);
                } else {
                    for (int j = 0; j < kBlockSize; ++j) weight[j] = 0.25f*sigma2 + xb[j]*xb[j];
                }
                float idl = 1/dl;
                int ib32 = ib/2;
                int offset = 16*(ib%2);
                uint8_t * qs = y[ibl].qs + 32*(ib32/4) + offset;
                for (int j = 0; j < 16; ++j) {
                    const float al = idl*xb[j];
                    int ibest = best_index_iq2nl(block_values, al);
                    qs[j] |= (ibest << 2*(ib32%4));
                    float w = weight[j];
                    float q = block_values[ibest]*ls;
                    sumqx += w*q*xb[j];
                    sumq2 += w*q*q;
                }
            }
        }
        if (sumq2 > 0) y[ibl].d = GGML_FP32_TO_FP16(sumqx/sumq2);

    }
}
}

void quantize_row_iq2_k_ref(const float * GGML_RESTRICT x, block_iq2_k  * GGML_RESTRICT y, int64_t k) {
    assert(k % QK_K == 0);
    quantize_iq2_k(x, (void *)y, 1, k, nullptr);
}

void quantize_row_iq2_k(const float * GGML_RESTRICT x, void * GGML_RESTRICT vy, int64_t k) {
    assert(k % QK_K == 0);
    block_iq2_k * y = (block_iq2_k *)vy;
    quantize_row_iq2_k_ref(x, y, k);
}

size_t quantize_iq2_k(const float * src, void * dst, int64_t nrows, int64_t n_per_row, const float * imatrix) {
    GGML_ASSERT(n_per_row%QK_K == 0);
    int nblock = n_per_row/QK_K;
    char * qrow = (char *)dst;
    for (int64_t row = 0; row < nrows; ++row) {
        quantize_row_iq2_k_impl(src, (void *)qrow, n_per_row, imatrix);
        src += n_per_row;
        qrow += nblock*sizeof(block_iq2_k);
    }
    return nrows * nblock * sizeof(block_iq2_k);
}

void dequantize_row_iq2_k(const block_iq2_k  * GGML_RESTRICT x, float * GGML_RESTRICT y, int64_t k) {
    assert(k % QK_K == 0);
    const int nb = k / QK_K;

    for (int i = 0; i < nb; i++) {

        const float d = GGML_FP16_TO_FP32(x[i].d);
        const uint8_t * qs = x[i].qs;

        uint16_t extra = x[i].extra;

        int shift = 0;
        for (int ib32 = 0; ib32 < QK_K/32; ++ib32) {
            float dl1 = d * (2*(x[i].scales[ib32] & 0xf) - 15);
            float dl2 = d * (2*(x[i].scales[ib32] >>  4) - 15);
            const int8_t * values1 = extra & 1 ? iq2nl_values + 4 : iq2nl_values;
            const int8_t * values2 = extra & 2 ? iq2nl_values + 4 : iq2nl_values;
            extra >>= 2;
            for (int j = 0; j < 16; ++j) {
                y[j+ 0] = dl1 * values1[(qs[j+ 0] >> shift) & 3];
                y[j+16] = dl2 * values2[(qs[j+16] >> shift) & 3];
            }
            y += 32;
            shift += 2;
            if (shift == 8) { qs += 32; shift = 0; }
        }

    }

}

void vec_dot_iq2_k_q8_k(int n, float * GGML_RESTRICT s, size_t bs, const void * GGML_RESTRICT vx, size_t bx, const void * GGML_RESTRICT vy, size_t by, int nrc) {
    assert(n % QK_K == 0);
    assert(nrc == 1);
    GGML_UNUSED(nrc);
    GGML_UNUSED(bx);
    GGML_UNUSED(by);
    GGML_UNUSED(bs);

    if (iqk_mul_mat(1, 1, n, GGML_TYPE_IQ2_K, vx, 0, GGML_TYPE_Q8_K, vy, 0, s, 0, 0, 1)) {
        return;
    }

    const int nb = n / QK_K;

    const block_iq2_k * x = (const block_iq2_k *)vx;
    const block_q8_K  * y = (const block_q8_K *)vy;
}
