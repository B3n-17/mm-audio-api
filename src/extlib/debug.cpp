#include <extlib/debug.hpp>
#include <audio_debug_html.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Include dr_wav header only (implementation is compiled once in decoder/wav.cpp).
#define DR_WAV_NO_STDIO
#include <dr_wav.h>

#include <plog/Log.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace {

constexpr size_t EVENT_CAPACITY = 4096;

using Clock = std::chrono::system_clock;

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct EventStore {
    std::array<Debug::Event, EVENT_CAPACITY> events;
    size_t head = 0;
    size_t size = 0;
    uint64_t nextId = 1;
    uint64_t dropped = 0;
    std::mutex mutex;
};

EventStore sEventStore;
Debug::Snapshot sSnapshot = { 0 };
std::mutex sSnapshotMutex;

std::atomic<bool> sEnabled = false;
std::once_flag sHttpStartOnce;

std::unordered_map<uint32_t, std::string> sTagRegistry;
std::mutex sTagRegistryMutex;

// Streamed audio state: keyed by (resourceId << 8 | trackNo)
std::unordered_map<uint64_t, Debug::StreamState> sStreamStates;
std::mutex sStreamMutex;

// DJ mixer overrides: indexed by [playerIndex][channelIndex]
constexpr size_t MAX_MIX_CHANNELS = 16;
std::array<std::array<Debug::MixOverride, MAX_MIX_CHANNELS>, Debug::MAX_PLAYERS> sMixOverrides;
std::mutex sMixMutex;
std::atomic<bool> sMixerOpen = false;

// ============================================================
// VADPCM (N64 ADPCM) Encoder
// Ported from mm-decomp/tools/audio/sampleconv (CC0-1.0, ZeldaRET).
// tabledesign_run + expand_codebook + my_encodeframe.
// ============================================================

static constexpr int VADPCM_FRAME_SIZE    = 9;   // bytes per 16-sample frame
static constexpr int VADPCM_SAMPLES_FRAME = 16;
static constexpr int VADPCM_ORDER         = 2;   // predictor order
static constexpr int VADPCM_BITS          = 2;   // log2(npredictors) → 4 entries
static constexpr uint32_t PATCH_CODEC_ADPCM = 0;
static constexpr uint32_t PATCH_CODEC_S16   = 5;

// --------------- tabledesign helpers (verbatim port) ---------------

static int venc_durbin(double *acvec, int order, double *refl, double *pred, double *err) {
    pred[0] = 1.0;
    double E = acvec[0];
    int ret = 0;
    for (int i = 1; i <= order; i++) {
        double sum = 0.0;
        for (int j = 1; j <= i - 1; j++) sum += pred[j] * acvec[i - j];
        pred[i] = (E > 0.0) ? (-(acvec[i] + sum) / E) : 0.0;
        refl[i] = pred[i];
        if (std::fabs(refl[i]) > 1.0) ret++;
        for (int j = 1; j < i; j++) pred[j] += pred[i - j] * pred[i];
        E *= 1.0 - pred[i] * pred[i];
    }
    *err = E;
    return ret;
}

static void venc_afromk(double *k, double *ai, int order) {
    ai[0] = 1.0;
    for (int i = 1; i <= order; i++) {
        ai[i] = k[i];
        for (int j = 1; j <= i - 1; j++) ai[j] += ai[i - j] * ai[i];
    }
}

static int venc_kfroma(double *in, double *out, int order) {
    out[order] = in[order];
    int ret = 0;
    for (int i = order - 1; i >= 1; i--) {
        double next[order + 1];
        for (int j = 0; j <= i; j++) {
            double temp = out[i + 1];
            double div  = 1.0 - temp * temp;
            if (div == 0.0) return 1;
            next[j] = (in[j] - in[i - j + 1] * temp) / div;
        }
        for (int j = 0; j <= i; j++) in[j] = next[j];
        out[i] = next[i];
        if (std::fabs(out[i]) > 1.0) ret++;
    }
    return ret;
}

static void venc_rfroma(double *in, int n, double *out) {
    std::vector<std::vector<double>> mat(n + 1, std::vector<double>(n + 1, 0.0));
    mat[n][0] = 1.0;
    for (int i = 1; i <= n; i++) mat[n][i] = -in[i];
    for (int i = n; i >= 1; i--) {
        double div = 1.0 - mat[i][i] * mat[i][i];
        for (int j = 1; j <= i - 1; j++)
            mat[i - 1][j] = (mat[i][i - j] * mat[i][i] + mat[i][j]) / div;
    }
    out[0] = 1.0;
    for (int i = 1; i <= n; i++) {
        out[i] = 0.0;
        for (int j = 1; j <= i; j++) out[i] += mat[i][j] * out[i - j];
    }
}

static double venc_model_dist(double *mean_pred, double *frame_pred, int order) {
    std::vector<double> ac_frame(order + 1), ac_mean(order + 1);
    venc_rfroma(frame_pred, order, ac_frame.data());
    for (int i = 0; i <= order; i++) {
        ac_mean[i] = 0.0;
        for (int j = 0; j <= order - i; j++) ac_mean[i] += mean_pred[j] * mean_pred[i + j];
    }
    double ret = ac_frame[0] * ac_mean[0];
    for (int i = 1; i <= order; i++) ret += 2.0 * ac_frame[i] * ac_mean[i];
    return ret;
}

static void venc_acmat(const int16_t *x, int order, int xlen, std::vector<std::vector<double>>& ac) {
    for (int i = 1; i <= order; i++)
        for (int j = 1; j <= order; j++) {
            ac[i][j] = 0.0;
            for (int k = 0; k < xlen; k++) ac[i][j] += (double)x[k - i] * (double)x[k - j];
        }
}

static void venc_acvect(const int16_t *x, int order, int xlen, double *ac) {
    for (int i = 0; i <= order; i++) {
        ac[i] = 0.0;
        for (int j = 0; j < xlen; j++) ac[i] -= (double)x[j - i] * (double)x[j];
    }
}

static int venc_lud(std::vector<std::vector<double>>& a, int n, int *perm, int *d) {
    std::vector<double> vv(n + 1);
    *d = 1;
    for (int i = 1; i <= n; i++) {
        double big = 0.0;
        for (int j = 1; j <= n; j++) { double t = std::fabs(a[i][j]); if (t > big) big = t; }
        if (big == 0.0) return 1;
        vv[i] = 1.0 / big;
    }
    for (int j = 1; j <= n; j++) {
        for (int i = 1; i < j; i++) {
            double sum = a[i][j];
            for (int k = 1; k < i; k++) sum -= a[i][k] * a[k][j];
            a[i][j] = sum;
        }
        double big = 0.0; int imax = 0;
        for (int i = j; i <= n; i++) {
            double sum = a[i][j];
            for (int k = 1; k < j; k++) sum -= a[i][k] * a[k][j];
            a[i][j] = sum;
            double dum = vv[i] * std::fabs(sum);
            if (dum >= big) { big = dum; imax = i; }
        }
        if (j != imax) {
            for (int k = 1; k <= n; k++) std::swap(a[imax][k], a[j][k]);
            *d = -(*d); vv[imax] = vv[j];
        }
        perm[j] = imax;
        if (a[j][j] == 0.0) return 1;
        if (j != n) { double dum = 1.0 / a[j][j]; for (int i = j + 1; i <= n; i++) a[i][j] *= dum; }
    }
    double mn = 1e10, mx = 0.0;
    for (int i = 1; i <= n; i++) { double t = std::fabs(a[i][i]); if (t < mn) mn = t; if (t > mx) mx = t; }
    return (mn / mx < 1e-10) ? 1 : 0;
}

static void venc_lubksb(std::vector<std::vector<double>>& a, int n, int *perm, double *b) {
    int ii = 0;
    for (int i = 1; i <= n; i++) {
        int ip = perm[i]; double sum = b[ip]; b[ip] = b[i];
        if (ii) { for (int j = ii; j <= i - 1; j++) sum -= a[i][j] * b[j]; }
        else if (sum) { ii = i; }
        b[i] = sum;
    }
    for (int i = n; i >= 1; i--) {
        double sum = b[i];
        for (int j = i + 1; j <= n; j++) sum -= a[i][j] * b[j];
        b[i] = sum / a[i][i];
    }
}

static void venc_split(std::vector<std::vector<double>>& predictors, double *delta, int order,
                       int npredictors, double scale) {
    for (int i = 0; i < npredictors; i++)
        for (int j = 0; j <= order; j++)
            predictors[i + npredictors][j] = predictors[i][j] + delta[j] * scale;
}

static void venc_refine(std::vector<std::vector<double>>& predictors, int order, int npredictors,
                        const std::vector<double>& all_frame_pred, int num_frame_pred, int refine_iters) {
    int num_order = order + 1;
    for (int iter = 0; iter < refine_iters; iter++) {
        std::vector<std::vector<double>> rsums(npredictors, std::vector<double>(num_order, 0.0));
        std::vector<int> counts(npredictors, 0);
        std::vector<double> vec(num_order);
        for (int i = 0; i < num_frame_pred; i++) {
            double best_val = 1e30; int best_idx = 0;
            for (int j = 0; j < npredictors; j++) {
                double dist = venc_model_dist(predictors[j].data(),
                                              const_cast<double*>(&all_frame_pred[(num_order) * i]), order);
                if (dist < best_val) { best_val = dist; best_idx = j; }
            }
            venc_rfroma(const_cast<double*>(&all_frame_pred[num_order * i]), order, vec.data());
            for (int j = 0; j <= order; j++) rsums[best_idx][j] += vec[j];
            counts[best_idx]++;
        }
        for (int i = 0; i < npredictors; i++)
            if (counts[i] > 1)
                for (int j = 0; j <= order; j++) rsums[i][j] /= counts[i];

        for (int i = 0; i < npredictors; i++) {
            double dummy;
            venc_durbin(rsums[i].data(), order, vec.data(), predictors[i].data(), &dummy);
            for (int j = 1; j <= order; j++) {
                if (vec[j] >= 1.0) vec[j] = 0.9999999999;
                if (vec[j] <= -1.0) vec[j] = -0.9999999999;
            }
            venc_afromk(vec.data(), predictors[i].data(), order);
        }
    }
}

static int venc_read_row(int16_t *out, double *predictors, int order) {
    double table[8][8] = {};  // [row][col], col <= order
    for (int i = 0; i < order; i++) {
        for (int j = 0; j < i; j++) table[i][j] = 0.0;
        for (int j = i; j < order; j++) table[i][j] = -predictors[order - j + i];
    }
    for (int i = 1; i < 8; i++)
        for (int j = 1; j <= order; j++)
            if (i >= j)
                for (int k = 0; k < order; k++) table[i][k] -= predictors[j] * table[i - j][k];

    int overflows = 0;
    for (int i = 0; i < order; i++)
        for (int j = 0; j < 8; j++) {
            double fval = table[j][i] * (double)(1 << 11);
            int ival;
            if (fval < 0.0) { ival = (int)(fval - 0.5); if (ival < -0x8000) overflows++; }
            else             { ival = (int)(fval + 0.5); if (ival >= 0x8000) overflows++; }
            *out++ = (int16_t)ival;
        }
    return overflows;
}

struct VadpcmBook {
    int32_t order;
    int32_t numPredictors;
    std::vector<int16_t> codeBook; // 8 * order * numPredictors entries
};

// Port of tabledesign_run.  design=NULL uses defaults (order=2, bits=2, refine=2, thresh=10, frame=16).
static VadpcmBook buildVadpcmCodebook(const int16_t* pcm, size_t numSamples) {
    const int order         = VADPCM_ORDER;
    const int bits          = VADPCM_BITS;
    const int npredictors   = 1 << bits;
    const int refine_iters  = 2;
    const double thresh     = 10.0;
    const int frame_size    = 16;
    const int num_order     = order + 1;

    VadpcmBook book;
    book.order = order;
    book.numPredictors = npredictors;
    book.codeBook.resize(8 * order * npredictors, 0);

    if (numSamples == 0) return book;

    // Working buffer: two frames side-by-side (prev + current)
    std::vector<int16_t> buffer(2 * frame_size, 0);
    size_t nframes_aligned = numSamples - (numSamples % frame_size);

    std::vector<double> all_frame_pred;
    all_frame_pred.reserve(nframes_aligned * num_order);
    int num_frame_pred = 0;

    std::vector<std::vector<double>> acmat(num_order, std::vector<double>(num_order, 0.0));
    std::vector<double> vec(num_order), refl(num_order);
    std::vector<int> perm(num_order);

    for (size_t s = 0; s < nframes_aligned; s += frame_size) {
        std::memcpy(buffer.data() + frame_size, pcm + s, frame_size * sizeof(int16_t));
        venc_acvect(buffer.data() + frame_size, order, frame_size, vec.data());

        if (std::fabs(vec[0]) > thresh) {
            venc_acmat(buffer.data() + frame_size, order, frame_size, acmat);
            int d;
            if (venc_lud(acmat, order, perm.data(), &d) == 0) {
                venc_lubksb(acmat, order, perm.data(), vec.data());
                vec[0] = 1.0;
                std::vector<double> refl2(num_order);
                if (venc_kfroma(vec.data(), refl2.data(), order) == 0) {
                    // append to all_frame_pred
                    all_frame_pred.push_back(1.0);
                    for (int i = 1; i <= order; i++) {
                        if (refl2[i] >= 1.0) refl2[i] = 0.9999999999;
                        if (refl2[i] <= -1.0) refl2[i] = -0.9999999999;
                    }
                    all_frame_pred.resize((num_frame_pred + 1) * num_order, 0.0);
                    all_frame_pred[num_frame_pred * num_order] = 1.0;
                    venc_afromk(refl2.data(), &all_frame_pred[num_frame_pred * num_order], order);
                    num_frame_pred++;
                }
            }
        }
        std::memcpy(buffer.data(), buffer.data() + frame_size, frame_size * sizeof(int16_t));
    }

    // Average autocorrelation → initial mean predictor
    std::vector<std::vector<double>> predictors(npredictors, std::vector<double>(num_order, 0.0));

    vec[0] = 1.0;
    for (int i = 1; i < num_order; i++) vec[i] = 0.0;

    if (num_frame_pred > 0) {
        for (int i = 0; i < num_frame_pred; i++) {
            venc_rfroma(&all_frame_pred[i * num_order], order, predictors[0].data());
            for (int k = 1; k < num_order; k++) vec[k] += predictors[0][k];
        }
        for (int i = 1; i < num_order; i++) vec[i] /= num_frame_pred;
    }

    double dummy;
    venc_durbin(vec.data(), order, refl.data(), predictors[0].data(), &dummy);
    for (int i = 1; i < num_order; i++) {
        if (refl[i] >= 1.0) refl[i] = 0.9999999999;
        if (refl[i] <= -1.0) refl[i] = -0.9999999999;
    }
    venc_afromk(refl.data(), predictors[0].data(), order);

    // k-means splitting
    for (int cur_bits = 0; cur_bits < bits; cur_bits++) {
        std::vector<double> split_delta(num_order, 0.0);
        split_delta[order - 1] = -1.0;
        venc_split(predictors, split_delta.data(), order, 1 << cur_bits, 0.01);
        venc_refine(predictors, order, 1 << (1 + cur_bits), all_frame_pred, num_frame_pred, refine_iters);
    }

    // Convert to book entries
    for (int i = 0; i < npredictors; i++)
        venc_read_row(&book.codeBook[8 * order * i], predictors[i].data(), order);

    return book;
}

// --------------- expand_codebook (verbatim port) ---------------
// Builds the FIR filter matrices used by my_encodeframe.
// Returns table[npredictors][8][order+8].
using CoefTable = std::vector<std::vector<std::vector<int32_t>>>;

static CoefTable venc_expand_codebook(const VadpcmBook& book) {
    int order = book.order;
    int np    = book.numPredictors;
    const int16_t* book_data = book.codeBook.data();

    CoefTable table(np, std::vector<std::vector<int32_t>>(8, std::vector<int32_t>(order + 8, 0)));

    for (int i = 0; i < np; i++) {
        auto& te = table[i];
        for (int j = 0; j < order; j++)
            for (int k = 0; k < 8; k++)
                te[k][j] = *book_data++;

        for (int k = 1; k < 8; k++) te[k][order] = te[k - 1][order - 1];
        te[0][order] = 1 << 11; // 1.0 in qs4.11

        for (int k = 1; k < 8; k++) {
            int j = 0;
            for (; j < k; j++) te[j][k + order] = 0;
            for (; j < 8; j++) te[j][k + order] = te[j - k][order];
        }
    }
    return table;
}

// --------------- inner_product helper ---------------
static int32_t venc_inner_product(int len, const int32_t *v1, const int32_t *v2) {
    int32_t out = 0;
    for (int i = 0; i < len; i++) out += v1[i] * v2[i];
    int32_t dout = out / (1 << 11);
    int32_t fiout = dout * (1 << 11);
    return dout - (out - fiout < 0 ? 1 : 0);
}

static int16_t venc_qsample(float x, int32_t scale) {
    if (x > 0.0f) return (int16_t)((x / scale) + 0.4999999f);
    else           return (int16_t)((x / scale) - 0.4999999f);
}

static int16_t venc_clamp_bits(int32_t x, int bits) {
    int lim = 1 << (bits - 1);
    if (x < -lim) return (int16_t)(-lim);
    if (x > lim - 1) return (int16_t)(lim - 1);
    return (int16_t)x;
}

static void venc_clamp_to_s16(float *in, int32_t *out) {
    for (int i = 0; i < 16; i++) {
        if (in[i] > 0x7fff) in[i] = 0x7fff;
        if (in[i] < -0x8000) in[i] = -0x8000;
        out[i] = (in[i] > 0.0f) ? (int32_t)(in[i] + 0.5f) : (int32_t)(in[i] - 0.5f);
    }
}

// Port of vencodeframe.
// state: last 16 decoded samples (state[16-order..15] are the 'order' most recent).
// After the call, state is updated with the newly decoded 16 samples.
static void venc_encodeframe(uint8_t *out, const int16_t *in_buf, int32_t *orig_state,
                             const CoefTable& coef_tbl, int order, int npredictors) {
    const int enc_bits   = 4;
    const int llevel     = -(1 << (enc_bits - 1));
    const int ulevel     = -llevel - 1;
    const int scale_factor = 16 - enc_bits;

    int16_t ix[16];
    int32_t prediction[16], in_vec[16], ie[16];
    float   e[16];
    int32_t optimalp = 0;
    float   min_err  = 1e30f;

    // Find best predictor
    for (int k = 0; k < npredictors; k++) {
        for (int j = 0; j < 2; j++) {
            if (j == 0) {
                for (int i = 0; i < order; i++) {
                    in_vec[i] = orig_state[16 - order + i];
                }
            } else {
                for (int i = 0; i < order; i++) {
                    in_vec[i] = prediction[8 - order + i] + in_vec[8 + i];
                }
            }
            for (int i = 0; i < 8; i++) {
                prediction[j*8+i] = venc_inner_product(order + i, coef_tbl[k][i].data(), in_vec);
                in_vec[i + order] = (int32_t)in_buf[j*8+i] - prediction[j*8+i];
                e[j*8+i] = (float)in_vec[i + order];
            }
        }
        float se = 0.0f;
        for (int j = 0; j < 16; j++) se += e[j] * e[j];
        if (se < min_err) { min_err = se; optimalp = k; }
    }

    // Re-compute errors with best predictor
    for (int j = 0; j < 2; j++) {
        if (j == 0) {
            for (int i = 0; i < order; i++) {
                in_vec[i] = orig_state[16 - order + i];
            }
        } else {
            for (int i = 0; i < order; i++) {
                in_vec[i] = prediction[8 - order + i] + in_vec[8 + i];
            }
        }
        for (int i = 0; i < 8; i++) {
            prediction[j*8+i] = venc_inner_product(order + i, coef_tbl[optimalp][i].data(), in_vec);
            e[j*8+i] = (float)(in_vec[i + order] = (int32_t)in_buf[j*8+i] - prediction[j*8+i]);
        }
    }
    venc_clamp_to_s16(e, ie);

    // Find scale
    int32_t maxv = 0;
    for (int i = 0; i < 16; i++) if (std::abs(ie[i]) > std::abs(maxv)) maxv = ie[i];
    int32_t scale = 0;
    for (; scale <= scale_factor; scale++) { if (maxv <= ulevel && maxv >= llevel) break; maxv /= 2; }

    // Encode (with possible scale bump)
    int32_t state[16];
    for (int i = 0; i < 16; i++) state[i] = orig_state[i];

    bool again = true;
    for (int nIter = 0; nIter < 2 && again; nIter++) {
        again = false;
        if (nIter == 1) scale++;
        if (scale > scale_factor) scale = scale_factor;

        for (int j = 0; j < 2; j++) {
            int base = j * 8;
            for (int i = 0; i < order; i++)
                in_vec[i] = (j == 0) ? orig_state[16 - order + i] : state[8 - order + i];
            for (int i = 0; i < 8; i++) {
                prediction[base+i] = venc_inner_product(order + i, coef_tbl[optimalp][i].data(), in_vec);
                float se = (float)in_buf[base+i] - (float)prediction[base+i];
                ix[base+i] = venc_qsample(se, 1 << scale);
                int32_t cV = venc_clamp_bits(ix[base+i], enc_bits) - ix[base+i];
                if (cV > 1 || cV < -1) again = true;
                ix[base+i] += cV;
                in_vec[i + order] = ix[base+i] * (1 << scale);
                state[base+i] = prediction[base+i] + in_vec[i + order];
            }
        }
    }

    // Write header + nibbles
    out[0] = (uint8_t)(((scale & 0xF) << 4) | (optimalp & 0xF));
    for (int i = 0; i < 16; i += 2)
        out[1 + i/2] = (uint8_t)(((ix[i] & 0xF) << 4) | (ix[i+1] & 0xF));

    // Update orig_state with the 16 newly decoded samples
    for (int i = 0; i < 16; i++) orig_state[i] = state[i];
}

struct LoopPoints {
    uint32_t loopStart  = 0;
    uint32_t loopEnd    = 0;
    int32_t  loopCount  = 0;   // 0 = no loop, -1 = infinite
    std::array<int16_t, 16> predictorState = {};
};

struct EncodedSample {
    std::vector<uint8_t> adpcmData;
    VadpcmBook book;
    LoopPoints loop;
    uint32_t numSamples = 0;  // total s16 samples
};

// Encode PCM s16 mono (resampled to target_rate if needed — we skip resampling for now,
// game will tune pitch via the tuning float). Mix to mono if stereo.
static EncodedSample encodePcmToVadpcm(const int16_t* pcm, size_t numSamples, uint32_t channels,
                                        LoopPoints loopPoints) {
    EncodedSample result;

    // Mix down to mono if needed.
    std::vector<int16_t> mono;
    if (channels > 1) {
        mono.resize(numSamples / channels);
        for (size_t i = 0; i < mono.size(); i++) {
            int32_t sum = 0;
            for (uint32_t c = 0; c < channels; c++) {
                sum += pcm[i * channels + c];
            }
            mono[i] = static_cast<int16_t>(sum / static_cast<int32_t>(channels));
        }
        numSamples = mono.size();
        pcm = mono.data();
    }

    result.numSamples = static_cast<uint32_t>(numSamples);
    result.loop = loopPoints;

    // Pad to multiple of 16.
    size_t paddedLen = (numSamples + 15) & ~15u;
    std::vector<int16_t> padded(paddedLen, 0);
    std::memcpy(padded.data(), pcm, numSamples * sizeof(int16_t));

    result.book = buildVadpcmCodebook(padded.data(), paddedLen);
    CoefTable coefTable = venc_expand_codebook(result.book);

    size_t numFrames = paddedLen / VADPCM_SAMPLES_FRAME;
    result.adpcmData.resize(numFrames * VADPCM_FRAME_SIZE, 0);

    // orig_state: 16-slot ring used by venc_encodeframe (init to zero)
    std::array<int32_t, 16> state = {};
    for (size_t f = 0; f < numFrames; f++) {
        venc_encodeframe(result.adpcmData.data() + f * VADPCM_FRAME_SIZE,
                         padded.data() + f * VADPCM_SAMPLES_FRAME,
                         state.data(), coefTable,
                         result.book.order, result.book.numPredictors);
    }

    // Compute predictor state at loop point if looping.
    if (loopPoints.loopCount != 0 && loopPoints.loopEnd > loopPoints.loopStart) {
        uint32_t loopFrame = loopPoints.loopStart / VADPCM_SAMPLES_FRAME;
        if (loopFrame > numFrames) loopFrame = static_cast<uint32_t>(numFrames);
        state = {};
        for (uint32_t f = 0; f < loopFrame; f++) {
            venc_encodeframe(result.adpcmData.data() + f * VADPCM_FRAME_SIZE,
                             padded.data() + f * VADPCM_SAMPLES_FRAME,
                             state.data(), coefTable,
                             result.book.order, result.book.numPredictors);
        }
        // Loop predictor state = last 'order' samples of state (state[16-order..15])
        for (int i = 0; i < 16; i++) {
            int32_t v = state[i];
            result.loop.predictorState[i] = static_cast<int16_t>(
                std::max(-32768, std::min(32767, v)));
        }
    }

    return result;
}

// ============================================================
// Sample Patch Queue
// ============================================================

// Identifies which sample slot to replace in a font.
// One of (instId, drumId, sfxId) is >= 0; the others are -1.
// pitchRegion: 0=low, 1=normal, 2=high (only meaningful for instruments).
struct SamplePatch {
    int32_t  fontId       = -1;
    int32_t  instId       = -1;
    int32_t  drumId       = -1;
    int32_t  sfxId        = -1;
    uint8_t  pitchRegion  = 1;  // default: normal
    float    tuning       = 1.0f;
    uint32_t codec        = PATCH_CODEC_S16;
    EncodedSample encoded;
};

static std::optional<SamplePatch> sPendingSamplePatch;
static std::mutex sSamplePatchMutex;

// ============================================================
// Soundfont info (pushed from game side each tick)
// ============================================================

struct FontInfo {
    int32_t  numInstruments;
    int32_t  numDrums;
    int32_t  numSfx;
    std::vector<int32_t> instRegions; // per-instrument: bits 0/1/2 = has lo/mid/hi sample
    std::vector<int32_t> instTuningsLo;
    std::vector<int32_t> instTuningsMid;
    std::vector<int32_t> instTuningsHi;
    std::vector<int32_t> drumTunings;
    std::vector<int32_t> sfxTunings;
};

struct SoundFontState {
    std::vector<FontInfo> fonts;
    std::vector<int32_t>  activeFonts; // defaultFont per seq player
};

static SoundFontState sSoundFontState;
static std::mutex sSoundFontInfoMutex;

// ============================================================
// Socket type
// ============================================================

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
inline void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
inline void closeSocket(SocketHandle s) { close(s); }
#endif

// ============================================================
// HTTP helpers
// ============================================================

// Read a complete HTTP request, handling Content-Length for binary bodies.
// Returns the full request string (headers + body).
static std::string readHttpRequest(SocketHandle client) {
    std::string request;
    request.reserve(4096);

    // Read until we have headers (\r\n\r\n).
    char buf[4096];
    while (true) {
        int got = recv(client, buf, sizeof(buf), 0);
        if (got <= 0) break;
        request.append(buf, got);
        if (request.find("\r\n\r\n") != std::string::npos) break;
        if (request.size() > 8192) break; // safety: header-only cap
    }

    size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return request;
    }
    size_t bodyStart = headerEnd + 4;

    // Parse Content-Length.
    size_t contentLength = 0;
    std::string headers = request.substr(0, headerEnd);
    std::string lowerHeaders = headers;
    for (char& c : lowerHeaders) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t clPos = lowerHeaders.find("content-length:");
    if (clPos != std::string::npos) {
        size_t valPos = clPos + 15;
        while (valPos < lowerHeaders.size() && lowerHeaders[valPos] == ' ') valPos++;
        try {
            contentLength = static_cast<size_t>(std::stoull(lowerHeaders.substr(valPos)));
        } catch (...) {}
    }

    // Reject oversized bodies before we allocate for them.
    constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;
    if (contentLength > kMaxBodyBytes) {
        return std::string();
    }

    // Keep reading until we have the full body.
    size_t alreadyHaveBody = request.size() - bodyStart;
    while (alreadyHaveBody < contentLength) {
        size_t need = contentLength - alreadyHaveBody;
        size_t chunk = std::min(need, sizeof(buf));

#if defined(_WIN32)
        int got = recv(client, buf, static_cast<int>(chunk), 0);
#else
        int got = recv(client, buf, chunk, 0);
#endif
        if (got <= 0) break;
        request.append(buf, got);
        alreadyHaveBody += got;
    }

    return request;
}

// Parse multipart/form-data body. Returns map of field-name → raw bytes.
static std::unordered_map<std::string, std::vector<uint8_t>>
parseMultipart(const std::string& body, const std::string& boundary) {
    std::unordered_map<std::string, std::vector<uint8_t>> result;
    if (boundary.empty()) return result;

    std::string delim = "--" + boundary;

    size_t pos = 0;
    while (true) {
        size_t delimPos = body.find(delim, pos);
        if (delimPos == std::string::npos) break;
        pos = delimPos + delim.size();
        if (pos + 1 < body.size() && body[pos] == '-' && body[pos + 1] == '-') break; // end boundary

        // Skip CRLF after boundary.
        if (pos + 1 < body.size() && body[pos] == '\r') pos += 2;

        // Parse part headers.
        size_t partHeadersEnd = body.find("\r\n\r\n", pos);
        if (partHeadersEnd == std::string::npos) break;
        std::string partHeaders = body.substr(pos, partHeadersEnd - pos);
        pos = partHeadersEnd + 4;

        // Extract field name from Content-Disposition.
        std::string fieldName;
        {
            std::string lph = partHeaders;
            for (char& c : lph) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            size_t cd = lph.find("content-disposition:");
            if (cd == std::string::npos) continue;
            size_t fn = lph.find("name=\"", cd);
            if (fn == std::string::npos) continue;
            fn += 6;
            size_t fe = partHeaders.find('"', fn);
            if (fe == std::string::npos) continue;
            fieldName = partHeaders.substr(fn, fe - fn);
        }

        // Find next boundary to know part body length.
        size_t nextDelim = body.find(delim, pos);
        if (nextDelim == std::string::npos) break;
        // Body ends 2 bytes before next boundary (\r\n before --boundary).
        size_t bodyEnd = nextDelim;
        if (bodyEnd >= 2 && body[bodyEnd - 2] == '\r') bodyEnd -= 2;

        if (bodyEnd > pos) {
            result[fieldName] = std::vector<uint8_t>(body.begin() + pos, body.begin() + bodyEnd);
        } else {
            result[fieldName] = {};
        }

        pos = nextDelim;
    }

    return result;
}

// Extract multipart boundary from Content-Type header.
static std::string extractBoundary(const std::string& headers) {
    std::string lh = headers;
    for (char& c : lh) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t ct = lh.find("content-type:");
    if (ct == std::string::npos) return {};
    size_t bd = lh.find("boundary=", ct);
    if (bd == std::string::npos) return {};
    bd += 9;
    size_t be = lh.find("\r\n", bd);
    std::string boundary = headers.substr(bd, be == std::string::npos ? std::string::npos : be - bd);
    // Trim whitespace.
    while (!boundary.empty() && (boundary.back() == ' ' || boundary.back() == '\r' || boundary.back() == '\n')) {
        boundary.pop_back();
    }
    return boundary;
}

// Handle POST /soundfont-patch-sample
// Form fields: fontId, instId (or drumId or sfxId), pitchRegion (0/1/2),
//              loopStart, loopEnd, loopCount, tuning, wav_data (file).
static std::string handleSamplePatch(const std::string& request) {
    size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return "{\"error\":\"bad request\"}";
    }
    std::string headers = request.substr(0, headerEnd);
    std::string body    = request.substr(headerEnd + 4);

    std::string boundary = extractBoundary(headers);
    if (boundary.empty()) {
        return "{\"error\":\"missing multipart boundary\"}";
    }

    auto fields = parseMultipart(body, boundary);

    auto getInt = [&](const std::string& name, int32_t def) -> int32_t {
        auto it = fields.find(name);
        if (it == fields.end() || it->second.empty()) return def;
        std::string s(it->second.begin(), it->second.end());
        try { return static_cast<int32_t>(std::stol(s)); } catch (...) { return def; }
    };
    auto getFloat = [&](const std::string& name, float def) -> float {
        auto it = fields.find(name);
        if (it == fields.end() || it->second.empty()) return def;
        std::string s(it->second.begin(), it->second.end());
        try { return std::stof(s); } catch (...) { return def; }
    };

    SamplePatch patch;
    patch.fontId      = getInt("fontId", -1);
    patch.instId      = getInt("instId", -1);
    patch.drumId      = getInt("drumId", -1);
    patch.sfxId       = getInt("sfxId",  -1);
    patch.pitchRegion = static_cast<uint8_t>(getInt("pitchRegion", 1));
    patch.tuning      = getFloat("tuning", 1.0f);

    if (patch.fontId < 0) {
        return "{\"error\":\"fontId required\"}";
    }
    if (patch.instId < 0 && patch.drumId < 0 && patch.sfxId < 0) {
        return "{\"error\":\"one of instId/drumId/sfxId required\"}";
    }

    auto wavIt = fields.find("wav_data");
    if (wavIt == fields.end() || wavIt->second.empty()) {
        return "{\"error\":\"wav_data required\"}";
    }

    // Decode WAV.
    const auto& wavBytes = wavIt->second;
    drwav wav;
    bool opened = drwav_init_memory(&wav, wavBytes.data(), wavBytes.size(), nullptr);
    if (!opened) {
        return "{\"error\":\"failed to decode WAV\"}";
    }

    uint32_t sampleRate = wav.sampleRate;
    uint32_t channels   = wav.channels;
    uint64_t frameCount = wav.totalPCMFrameCount;

    // Reject WAVs whose declared size would blow up the int16 PCM buffer.
    // 64 MB of int16 = 32M samples; e.g. ~6 minutes of 44.1 kHz stereo.
    constexpr uint64_t kMaxPcmSamples = (64ULL * 1024 * 1024) / sizeof(int16_t);
    if (channels == 0 || frameCount == 0 || frameCount > kMaxPcmSamples / channels) {
        drwav_uninit(&wav);
        return "{\"error\":\"WAV too large or malformed\"}";
    }

    std::vector<int16_t> pcm(static_cast<size_t>(frameCount * channels));
    drwav_uint64 read = drwav_read_pcm_frames_s16(&wav, frameCount, pcm.data());

    // Pull smpl loop if present and not overridden by user.
    LoopPoints loopPts;
    loopPts.loopStart = static_cast<uint32_t>(getInt("loopStart", -1));
    loopPts.loopEnd   = static_cast<uint32_t>(getInt("loopEnd",   -1));
    loopPts.loopCount = getInt("loopCount", 0);

    if (loopPts.loopStart == static_cast<uint32_t>(-1)) {
        // Try to read from WAV metadata.
        for (drwav_uint32 m = 0; m < wav.metadataCount; m++) {
            drwav_metadata& meta = wav.pMetadata[m];
            if (meta.type == drwav_metadata_type_smpl && meta.data.smpl.sampleLoopCount > 0) {
                drwav_smpl_loop& sl = meta.data.smpl.pLoops[0];
                loopPts.loopStart = sl.firstSampleOffset;
                loopPts.loopEnd   = sl.lastSampleOffset;
                loopPts.loopCount = (sl.playCount == 0) ? -1 : static_cast<int32_t>(sl.playCount);
                break;
            }
        }
        if (loopPts.loopStart == static_cast<uint32_t>(-1)) {
            loopPts.loopStart = 0;
        }
    }
    if (loopPts.loopEnd == static_cast<uint32_t>(-1)) {
        loopPts.loopEnd = static_cast<uint32_t>(read > 0 ? read - 1 : 0);
    }

    drwav_uninit(&wav);

    if (read == 0) {
        return "{\"error\":\"WAV has no PCM frames\"}";
    }

    // For the debug patcher, prefer uncompressed PCM16-BE over ADPCM.
    // It is larger, but it avoids the many failure modes in runtime ADPCM encoding
    // and matches the engine's existing CODEC_S16 path.
    patch.codec = PATCH_CODEC_S16;
    patch.encoded.loop = loopPts;
    patch.encoded.numSamples = static_cast<uint32_t>(read);

    std::vector<int16_t> mono;
    const int16_t* monoPcm = pcm.data();
    size_t monoSamples = static_cast<size_t>(read);
    if (channels > 1) {
        mono.resize(monoSamples);
        for (size_t i = 0; i < monoSamples; i++) {
            int32_t sum = 0;
            for (uint32_t c = 0; c < channels; c++) {
                sum += pcm[i * channels + c];
            }
            mono[i] = static_cast<int16_t>(sum / static_cast<int32_t>(channels));
        }
        monoPcm = mono.data();
    }

    patch.encoded.adpcmData.resize(monoSamples * sizeof(int16_t));
    std::memcpy(patch.encoded.adpcmData.data(), monoPcm, monoSamples * sizeof(int16_t));

    // Capture sizes before moving.
    size_t adpcmBytes = patch.encoded.adpcmData.size();

    // Store patch for game side to pick up.
    {
        std::lock_guard<std::mutex> lock(sSamplePatchMutex);
        sPendingSamplePatch = std::move(patch);
    }

    std::ostringstream out;
    out << "{\"ok\":true"
        << ",\"sampleRate\":" << sampleRate
        << ",\"numSamples\":" << read
        << ",\"adpcmBytes\":" << adpcmBytes
        << "}";
    return out.str();
}

// GET /soundfonts — return font info previously pushed from game side.
static std::string soundfontsJson() {
    std::lock_guard<std::mutex> lock(sSoundFontInfoMutex);
    std::ostringstream out;
    out << "{\"activeFonts\":[";
    for (size_t i = 0; i < sSoundFontState.activeFonts.size(); i++) {
        if (i != 0) out << ',';
        out << sSoundFontState.activeFonts[i];
    }
    out << "],\"fonts\":[";
    for (size_t i = 0; i < sSoundFontState.fonts.size(); i++) {
        if (i != 0) out << ',';
        const auto& f = sSoundFontState.fonts[i];
        out << "{\"id\":" << i
            << ",\"numInstruments\":" << f.numInstruments
            << ",\"numDrums\":" << f.numDrums
            << ",\"numSfx\":" << f.numSfx
            << ",\"instRegions\":[";
        for (size_t j = 0; j < f.instRegions.size(); j++) {
            if (j != 0) out << ',';
            out << f.instRegions[j];
        }
        out << "]"
            << ",\"instTuningsLo\":[";
        for (size_t j = 0; j < f.instTuningsLo.size(); j++) {
            if (j != 0) out << ',';
            out << f.instTuningsLo[j];
        }
        out << "]"
            << ",\"instTuningsMid\":[";
        for (size_t j = 0; j < f.instTuningsMid.size(); j++) {
            if (j != 0) out << ',';
            out << f.instTuningsMid[j];
        }
        out << "]"
            << ",\"instTuningsHi\":[";
        for (size_t j = 0; j < f.instTuningsHi.size(); j++) {
            if (j != 0) out << ',';
            out << f.instTuningsHi[j];
        }
        out << "]"
            << ",\"drumTunings\":[";
        for (size_t j = 0; j < f.drumTunings.size(); j++) {
            if (j != 0) out << ',';
            out << f.drumTunings[j];
        }
        out << "]"
            << ",\"sfxTunings\":[";
        for (size_t j = 0; j < f.sfxTunings.size(); j++) {
            if (j != 0) out << ',';
            out << f.sfxTunings[j];
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

uint64_t parseU64(const std::string& value, uint64_t fallback) {
    if (value.empty()) {
        return fallback;
    }

    try {
        return std::stoull(value);
    } catch (...) {
        return fallback;
    }
}

std::string queryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t eq = query.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }
        size_t amp = query.find('&', eq + 1);
        std::string k = query.substr(pos, eq - pos);
        std::string v = query.substr(eq + 1, amp == std::string::npos ? std::string::npos : amp - (eq + 1));
        if (k == key) {
            return v;
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return {};
}

void sendHttp(SocketHandle client, const char* status, const char* contentType, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;

    std::string payload = out.str();
    send(client, payload.data(), static_cast<int>(payload.size()), 0);
}

std::string snapshotJson() {
    auto snapshot = Debug::getSnapshot();

    auto roleName = [](size_t i) -> const char* {
        switch (i) {
            case 0: return "BGM_MAIN";
            case 1: return "FANFARE";
            case 2: return "SFX";
            case 3: return "BGM_SUB";
            case 4: return "AMBIENCE";
            default: return "UNUSED";
        }
    };

    std::ostringstream out;
    out << "{\"ts_ms\":" << snapshot.tsMs
        << ",\"init_phase\":" << snapshot.initPhase
        << ",\"khz_mode\":" << (snapshot.khzMode ? "true" : "false")
        << ",\"main_seq_id\":" << snapshot.mainSeqId
        << ",\"sub_seq_id\":" << snapshot.subSeqId
        << ",\"fanfare_seq_id\":" << snapshot.fanfareSeqId
        << ",\"ambience_seq_id\":" << snapshot.ambienceSeqId
        << ",\"players\":[";

    for (size_t i = 0; i < snapshot.players.size(); i++) {
        if (i != 0) {
            out << ',';
        }

        const auto& p = snapshot.players[i];
        out << "{\"player\":" << i
            << ",\"role\":\"" << roleName(i) << "\""
            << ",\"seq_id\":" << p.seqId
            << ",\"enabled\":" << p.enabled
            << ",\"waiting_for_fonts\":" << p.waitingForFonts
            << ",\"setup_cmd_num\":" << p.setupCmdNum
            << ",\"active_channel_count\":" << p.activeChannelCount
            << ",\"active_channel_mask\":" << p.activeChannelMask
            << ",\"notes_active\":" << p.notesActive
            << ",\"notes_decaying\":" << p.notesDecaying
            << ",\"notes_releasing\":" << p.notesReleasing
            << ",\"pan_average\":" << p.panAverage
            << ",\"pan_left_count\":" << p.panLeftCount
            << ",\"pan_center_count\":" << p.panCenterCount
            << ",\"pan_right_count\":" << p.panRightCount
            << ",\"seq_io\":[";

        for (size_t io = 0; io < p.seqIo.size(); io++) {
            if (io != 0) {
                out << ',';
            }
            out << p.seqIo[io];
        }

        out << "]"
            << ",\"ch_enabled\":[";

        for (size_t ch = 0; ch < p.chEnabled.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chEnabled[ch];
        }

        out << "]"
            << ",\"ch_pan\":[";

        for (size_t ch = 0; ch < p.chPan.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chPan[ch];
        }

        out << "]"
            << ",\"ch_vol_milli\":[";

        for (size_t ch = 0; ch < p.chVolMilli.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chVolMilli[ch];
        }

        out << "]"
            << ",\"ch_reverb_vol\":[";

        for (size_t ch = 0; ch < p.chReverbVol.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chReverbVol[ch];
        }

        out << "]"
            << ",\"layer_enabled_mask\":[";

        for (size_t ch = 0; ch < p.layerEnabledMask.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.layerEnabledMask[ch];
        }

        out << "]"
            << ",\"layer_pan\":[";

        for (size_t idx = 0; idx < p.layerPan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerPan[idx];
        }

        out << "]"
            << ",\"layer_note_pan\":[";

        for (size_t idx = 0; idx < p.layerNotePan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNotePan[idx];
        }

        out << "]"
            << ",\"layer_note_attr_pan\":[";

        for (size_t idx = 0; idx < p.layerNoteAttrPan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteAttrPan[idx];
        }

        out << "]"
            << ",\"layer_note_target_l\":[";

        for (size_t idx = 0; idx < p.layerNoteTargetL.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteTargetL[idx];
        }

        out << "]"
            << ",\"layer_note_target_r\":[";

        for (size_t idx = 0; idx < p.layerNoteTargetR.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteTargetR[idx];
        }

        out << "]"
            << ",\"layer_note_cur_l\":[";

        for (size_t idx = 0; idx < p.layerNoteCurL.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteCurL[idx];
        }

        out << "]"
            << ",\"layer_note_cur_r\":[";

        for (size_t idx = 0; idx < p.layerNoteCurR.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteCurR[idx];
        }

        out << "]"
            << ",\"layer_sample_medium_codec\":[";

        for (size_t idx = 0; idx < p.layerSampleMediumCodec.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerSampleMediumCodec[idx];
        }

        out << "]"
            << ",\"ch_font_id\":[";

        for (size_t ch = 0; ch < p.chFontId.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chFontId[ch];
        }

        out << "]"
            << ",\"ch_inst_or_wave\":[";

        for (size_t ch = 0; ch < p.chInstOrWave.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chInstOrWave[ch];
        }

        out << "]}";
    }

    out << "]}";
    return out.str();
}

std::string eventsJson(uint64_t sinceId, size_t limit) {
    auto events = Debug::getEventsSince(sinceId, limit);
    uint64_t lastId = Debug::lastEventId();
    uint64_t dropped = Debug::droppedEventCount();

    std::ostringstream out;
    out << "{\"since\":" << sinceId
        << ",\"last_event_id\":" << lastId
        << ",\"dropped_total\":" << dropped
        << ",\"events\":[";

    for (size_t i = 0; i < events.size(); i++) {
        if (i != 0) {
            out << ',';
        }
        const auto& e = events[i];
        out << "{\"id\":" << e.id
            << ",\"ts_ms\":" << e.tsMs
            << ",\"tag\":" << e.tag
            << ",\"a\":" << e.a
            << ",\"b\":" << e.b
            << ",\"c\":" << e.c
            << ",\"d\":" << e.d << '}';
    }

    out << "]}";
    return out.str();
}

// Parse a JSON integer field: { ..., "key": value, ... }
// Returns defaultVal if the key is not found or the value is not a plain integer.
static int32_t jsonInt(const std::string& body, const std::string& key, int32_t defaultVal) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return defaultVal;
    }
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) {
        pos++;
    }
    if (pos >= body.size()) {
        return defaultVal;
    }
    // Accept optional leading '-'
    size_t start = pos;
    if (body[start] == '-') {
        start++;
    }
    if (start >= body.size() || !std::isdigit(static_cast<unsigned char>(body[start]))) {
        return defaultVal;
    }
    try {
        size_t eaten = 0;
        int32_t v = static_cast<int32_t>(std::stol(body.substr(pos), &eaten));
        (void)eaten;
        return v;
    } catch (...) {
        return defaultVal;
    }
}

std::string streamsJson() {
    std::lock_guard<std::mutex> lock(sStreamMutex);
    std::ostringstream out;
    out << "{\"streams\":[";
    bool first = true;
    for (const auto& [key, s] : sStreamStates) {
        if (!first) out << ',';
        first = false;
        out << "{\"resource_id\":" << s.resourceId
            << ",\"track_no\":" << s.trackNo
            << ",\"pos\":" << s.pos
            << ",\"sample_count\":" << s.sampleCount
            << ",\"loop_start\":" << s.loopStart
            << ",\"loop_end\":" << s.loopEnd
            << ",\"loop_count\":" << s.loopCount
            << ",\"ts_ms\":" << s.tsMs
            << "}";
    }
    out << "]}";
    return out.str();
}

std::string mixOverridesJson() {
    std::lock_guard<std::mutex> lock(sMixMutex);
    std::ostringstream out;
    out << "{\"overrides\":[";
    bool first = true;
    for (size_t p = 0; p < Debug::MAX_PLAYERS; p++) {
        for (size_t c = 0; c < MAX_MIX_CHANNELS; c++) {
            const auto& ov = sMixOverrides[p][c];
            if (!ov.active) {
                continue;
            }
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"player\":" << p
                << ",\"channel\":" << c
                << ",\"pan\":" << ov.pan
                << ",\"vol_milli\":" << ov.volMilli
                << ",\"reverb\":" << ov.reverb
                << ",\"muted\":" << (ov.muted ? "true" : "false")
                << "}";
        }
    }
    out << "]}";
    return out.str();
}

void handleClient(SocketHandle client) {
    std::string request = readHttpRequest(client);
    if (request.empty()) {
        return;
    }

    size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        sendHttp(client, "400 Bad Request", "text/plain; charset=utf-8", "Bad request");
        return;
    }

    std::string line = request.substr(0, lineEnd);

    // Handle POST /mixer-open (must be checked before POST /mix due to prefix overlap)
    if (line.rfind("POST /mixer-open", 0) == 0) {
        size_t bodyStart = request.find("\r\n\r\n");
        std::string body = bodyStart != std::string::npos ? request.substr(bodyStart + 4) : "";
        int32_t open = jsonInt(body, "open", 0);
        Debug::setMixerOpen(open != 0);
        sendHttp(client, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return;
    }

    // Handle POST /mix
    if (line.rfind("POST /mix", 0) == 0) {
        // Find body after \r\n\r\n
        size_t bodyStart = request.find("\r\n\r\n");
        std::string body = bodyStart != std::string::npos ? request.substr(bodyStart + 4) : "";

        int32_t player  = jsonInt(body, "player",   -1);
        int32_t channel = jsonInt(body, "channel",  -1);
        int32_t pan     = jsonInt(body, "pan",      -1);
        int32_t vol     = jsonInt(body, "vol_milli", -1);
        int32_t reverb  = jsonInt(body, "reverb",   -1);
        int32_t muted   = jsonInt(body, "muted",     0);
        int32_t clear   = jsonInt(body, "clear",     0);

        if (player < 0 || player >= static_cast<int32_t>(Debug::MAX_PLAYERS) ||
            channel < 0 || channel >= static_cast<int32_t>(MAX_MIX_CHANNELS)) {
            sendHttp(client, "400 Bad Request", "application/json; charset=utf-8",
                     "{\"error\":\"player/channel out of range\"}");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(sMixMutex);
            auto& ov = sMixOverrides[static_cast<size_t>(player)][static_cast<size_t>(channel)];
            if (clear) {
                ov = Debug::MixOverride{};
            } else {
                ov.active   = true;
                ov.pan      = pan;
                ov.volMilli = vol;
                ov.reverb   = reverb;
                ov.muted    = muted != 0;
            }
        }

        sendHttp(client, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return;
    }

    // Handle POST /soundfont-patch-sample
    if (line.rfind("POST /soundfont-patch-sample", 0) == 0) {
        std::string result = handleSamplePatch(request);
        sendHttp(client, "200 OK", "application/json; charset=utf-8", result);
        return;
    }

    // Handle POST /soundfonts (push font info from game side — called from C bridge)
    if (line.rfind("POST /soundfonts", 0) == 0) {
        size_t bodyStart = request.find("\r\n\r\n");
        std::string body = bodyStart != std::string::npos ? request.substr(bodyStart + 4) : "";
        Debug::updateSoundFontInfos(body);
        sendHttp(client, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return;
    }

    // Handle OPTIONS preflight (CORS)
    if (line.rfind("OPTIONS ", 0) == 0) {
        std::string preflightBody;
        std::ostringstream out;
        out << "HTTP/1.1 204 No Content\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: Content-Type\r\n"
            << "Connection: close\r\n\r\n";
        std::string payload = out.str();
        send(client, payload.data(), static_cast<int>(payload.size()), 0);
        return;
    }

    if (line.rfind("GET ", 0) != 0) {
        sendHttp(client, "405 Method Not Allowed", "text/plain; charset=utf-8", "Only GET and POST /mix supported");
        return;
    }

    size_t pathStart = 4;
    size_t pathEnd = line.find(' ', pathStart);
    if (pathEnd == std::string::npos) {
        sendHttp(client, "400 Bad Request", "text/plain; charset=utf-8", "Malformed request");
        return;
    }

    std::string rawPath = line.substr(pathStart, pathEnd - pathStart);
    std::string path = rawPath;
    std::string query;
    size_t qPos = rawPath.find('?');
    if (qPos != std::string::npos) {
        path = rawPath.substr(0, qPos);
        query = rawPath.substr(qPos + 1);
    }

    if (path == "/health") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8",
                 "{\"ok\":true,\"enabled\":" + std::to_string(Debug::isEnabled() ? 1 : 0) +
                     ",\"port\":" + std::to_string(Debug::HTTP_PORT) + "}");
        return;
    }

    if (path == "/snapshot") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", snapshotJson());
        return;
    }

    if (path == "/events") {
        uint64_t sinceId = parseU64(queryValue(query, "since"), 0);
        uint64_t limit = parseU64(queryValue(query, "limit"), 256);
        if (limit == 0) {
            limit = 1;
        }
        if (limit > 2048) {
            limit = 2048;
        }

        sendHttp(client, "200 OK", "application/json; charset=utf-8", eventsJson(sinceId, static_cast<size_t>(limit)));
        return;
    }

    if (path == "/tags") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", Debug::getTagsJson());
        return;
    }

    if (path == "/mix") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", mixOverridesJson());
        return;
    }

    if (path == "/streams") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", streamsJson());
        return;
    }

    if (path == "/soundfonts") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", soundfontsJson());
        return;
    }

    if (path == "/" || path == "/audio-debug.html") {
        sendHttp(client, "200 OK", "text/html; charset=utf-8", Debug::buildAudioDebugHtml());
        return;
    }

    sendHttp(client, "404 Not Found", "text/plain; charset=utf-8", "Not found");
}

void runHttpServer() {
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        PLOG_ERROR << "Debug HTTP failed to init WinSock";
        return;
    }
#endif

    SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET_HANDLE) {
        PLOG_ERROR << "Debug HTTP failed to create socket";
        return;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(Debug::HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        PLOG_ERROR << "Debug HTTP failed to bind 127.0.0.1:" << Debug::HTTP_PORT;
        closeSocket(server);
        return;
    }

    if (listen(server, 8) < 0) {
        PLOG_ERROR << "Debug HTTP failed to listen";
        closeSocket(server);
        return;
    }

    PLOG_INFO << "Audio debug HTTP running at http://127.0.0.1:" << Debug::HTTP_PORT;

    while (true) {
        SocketHandle client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET_HANDLE) {
            continue;
        }

        handleClient(client);
        closeSocket(client);
    }
}

} // namespace

namespace Debug {

void setEnabled(bool enabled) {
    sEnabled.store(enabled);
    if (!enabled) {
        return;
    }

    std::call_once(sHttpStartOnce, [] {
        std::thread serverThread(runHttpServer);
        serverThread.detach();
    });
}

bool isEnabled() {
    return sEnabled.load();
}

void pushEvent(uint32_t tag, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (!isEnabled()) {
        return;
    }

    if (!sEventStore.mutex.try_lock()) {
        sEventStore.dropped++;
        return;
    }

    EventStore& store = sEventStore;
    const size_t idx = (store.head + store.size) % EVENT_CAPACITY;

    store.events[idx] = Event {
        .id = store.nextId++,
        .tsMs = nowMs(),
        .tag = tag,
        .a = a,
        .b = b,
        .c = c,
        .d = d,
    };

    if (store.size == EVENT_CAPACITY) {
        store.head = (store.head + 1) % EVENT_CAPACITY;
        store.dropped++;
    } else {
        store.size++;
    }

    sEventStore.mutex.unlock();
}

void setKhzMode(bool enabled) {
    std::lock_guard<std::mutex> lock(sSnapshotMutex);
    sSnapshot.khzMode = enabled;
}

void setSnapshot(uint32_t initPhase, int32_t mainSeqId, int32_t subSeqId, int32_t fanfareSeqId, int32_t ambienceSeqId) {
    if (!isEnabled()) {
        return;
    }

    if (!sSnapshotMutex.try_lock()) {
        return;
    }

    sSnapshot.tsMs = nowMs();
    sSnapshot.initPhase = initPhase;
    sSnapshot.mainSeqId = mainSeqId;
    sSnapshot.subSeqId = subSeqId;
    sSnapshot.fanfareSeqId = fanfareSeqId;
    sSnapshot.ambienceSeqId = ambienceSeqId;

    sSnapshotMutex.unlock();
}

void setSeqPlayerPacked(uint32_t playerIndex, int32_t seqId, const int32_t* packed) {
    if (!isEnabled() || playerIndex >= sSnapshot.players.size()) {
        return;
    }

    if (!sSnapshotMutex.try_lock()) {
        return;
    }

    sSnapshot.players[playerIndex] = PlayerState {
        .seqId = seqId,
        .enabled = static_cast<uint32_t>(packed[0]),
        .waitingForFonts = static_cast<uint32_t>(packed[1]),
        .setupCmdNum = static_cast<uint32_t>(packed[2]),
        .activeChannelCount = static_cast<uint32_t>(packed[3]),
        .activeChannelMask = static_cast<uint32_t>(packed[4]),
        .notesActive = packed[5],
        .notesDecaying = packed[6],
        .notesReleasing = packed[7],
        .panAverage = packed[8],
        .panLeftCount = static_cast<uint32_t>(packed[9]),
        .panCenterCount = static_cast<uint32_t>(packed[10]),
        .panRightCount = static_cast<uint32_t>(packed[11]),
        .seqIo = {
            packed[12], packed[13], packed[14], packed[15],
            packed[16], packed[17], packed[18], packed[19],
        },
        .chEnabled = {
            static_cast<uint32_t>(packed[20]), static_cast<uint32_t>(packed[21]),
            static_cast<uint32_t>(packed[22]), static_cast<uint32_t>(packed[23]),
            static_cast<uint32_t>(packed[24]), static_cast<uint32_t>(packed[25]),
            static_cast<uint32_t>(packed[26]), static_cast<uint32_t>(packed[27]),
            static_cast<uint32_t>(packed[28]), static_cast<uint32_t>(packed[29]),
            static_cast<uint32_t>(packed[30]), static_cast<uint32_t>(packed[31]),
            static_cast<uint32_t>(packed[32]), static_cast<uint32_t>(packed[33]),
            static_cast<uint32_t>(packed[34]), static_cast<uint32_t>(packed[35]),
        },
        .chPan = {
            packed[36], packed[37], packed[38], packed[39],
            packed[40], packed[41], packed[42], packed[43],
            packed[44], packed[45], packed[46], packed[47],
            packed[48], packed[49], packed[50], packed[51],
        },
        .chVolMilli = {
            packed[52], packed[53], packed[54], packed[55],
            packed[56], packed[57], packed[58], packed[59],
            packed[60], packed[61], packed[62], packed[63],
            packed[64], packed[65], packed[66], packed[67],
        },
        .chReverbVol = {
            packed[68], packed[69], packed[70], packed[71],
            packed[72], packed[73], packed[74], packed[75],
            packed[76], packed[77], packed[78], packed[79],
            packed[80], packed[81], packed[82], packed[83],
        },
        .layerEnabledMask = {
            static_cast<uint32_t>(packed[84]),  static_cast<uint32_t>(packed[85]),
            static_cast<uint32_t>(packed[86]),  static_cast<uint32_t>(packed[87]),
            static_cast<uint32_t>(packed[88]),  static_cast<uint32_t>(packed[89]),
            static_cast<uint32_t>(packed[90]),  static_cast<uint32_t>(packed[91]),
            static_cast<uint32_t>(packed[92]),  static_cast<uint32_t>(packed[93]),
            static_cast<uint32_t>(packed[94]),  static_cast<uint32_t>(packed[95]),
            static_cast<uint32_t>(packed[96]),  static_cast<uint32_t>(packed[97]),
            static_cast<uint32_t>(packed[98]),  static_cast<uint32_t>(packed[99]),
        },
        .layerPan = {
            packed[100], packed[101], packed[102], packed[103],
            packed[104], packed[105], packed[106], packed[107],
            packed[108], packed[109], packed[110], packed[111],
            packed[112], packed[113], packed[114], packed[115],
        },
        .layerNotePan = {
            packed[116], packed[117], packed[118], packed[119],
            packed[120], packed[121], packed[122], packed[123],
            packed[124], packed[125], packed[126], packed[127],
            packed[128], packed[129], packed[130], packed[131],
        },
        .layerNoteAttrPan = {
            packed[132], packed[133], packed[134], packed[135],
            packed[136], packed[137], packed[138], packed[139],
            packed[140], packed[141], packed[142], packed[143],
            packed[144], packed[145], packed[146], packed[147],
        },
        .layerNoteTargetL = {
            packed[148], packed[149], packed[150], packed[151],
            packed[152], packed[153], packed[154], packed[155],
            packed[156], packed[157], packed[158], packed[159],
            packed[160], packed[161], packed[162], packed[163],
        },
        .layerNoteTargetR = {
            packed[164], packed[165], packed[166], packed[167],
            packed[168], packed[169], packed[170], packed[171],
            packed[172], packed[173], packed[174], packed[175],
            packed[176], packed[177], packed[178], packed[179],
        },
        .layerNoteCurL = {
            packed[180], packed[181], packed[182], packed[183],
            packed[184], packed[185], packed[186], packed[187],
            packed[188], packed[189], packed[190], packed[191],
            packed[192], packed[193], packed[194], packed[195],
        },
        .layerNoteCurR = {
            packed[196], packed[197], packed[198], packed[199],
            packed[200], packed[201], packed[202], packed[203],
            packed[204], packed[205], packed[206], packed[207],
            packed[208], packed[209], packed[210], packed[211],
        },
        .layerSampleMediumCodec = {
            packed[212], packed[213], packed[214], packed[215],
            packed[216], packed[217], packed[218], packed[219],
            packed[220], packed[221], packed[222], packed[223],
            packed[224], packed[225], packed[226], packed[227],
        },
        .chFontId = {
            packed[228], packed[229], packed[230], packed[231],
            packed[232], packed[233], packed[234], packed[235],
            packed[236], packed[237], packed[238], packed[239],
            packed[240], packed[241], packed[242], packed[243],
        },
        .chInstOrWave = {
            packed[244], packed[245], packed[246], packed[247],
            packed[248], packed[249], packed[250], packed[251],
            packed[252], packed[253], packed[254], packed[255],
            packed[256], packed[257], packed[258], packed[259],
        },
    };

    sSnapshotMutex.unlock();
}

Snapshot getSnapshot() {
    std::lock_guard<std::mutex> lock(sSnapshotMutex);
    return sSnapshot;
}

std::vector<Event> getEventsSince(uint64_t sinceId, size_t limit) {
    std::vector<Event> result;
    result.reserve(limit);

    std::lock_guard<std::mutex> lock(sEventStore.mutex);

    for (size_t i = 0; i < sEventStore.size; i++) {
        const size_t idx = (sEventStore.head + i) % EVENT_CAPACITY;
        const Event& event = sEventStore.events[idx];

        if (event.id <= sinceId) {
            continue;
        }

        result.push_back(event);
        if (result.size() >= limit) {
            break;
        }
    }

    return result;
}

uint64_t droppedEventCount() {
    std::lock_guard<std::mutex> lock(sEventStore.mutex);
    return sEventStore.dropped;
}

uint64_t lastEventId() {
    std::lock_guard<std::mutex> lock(sEventStore.mutex);
    return sEventStore.nextId == 0 ? 0 : sEventStore.nextId - 1;
}

void startHttpServer() {
    setEnabled(true);
}

void registerTag(uint32_t tag, const char* name) {
    if (!name || tag == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(sTagRegistryMutex);
    sTagRegistry[tag] = name;
}

std::string getTagsJson() {
    std::lock_guard<std::mutex> lock(sTagRegistryMutex);
    std::ostringstream out;
    out << "{\"tags\":{";
    bool first = true;
    for (const auto& [tag, name] : sTagRegistry) {
        if (!first) {
            out << ',';
        }
        first = false;
        // Escape the name string
        out << '"' << tag << "\":\"";
        for (char ch : name) {
            if (ch == '"' || ch == '\\') out << '\\';
            out << ch;
        }
        out << '"';
    }
    out << "}}";
    return out.str();
}

void setStreamState(uint32_t resourceId, uint32_t trackNo, int32_t pos,
                    uint32_t sampleCount, uint32_t loopStart, uint32_t loopEnd, int32_t loopCount) {
    if (!isEnabled()) {
        return;
    }
    uint64_t key = (static_cast<uint64_t>(resourceId) << 8) | (trackNo & 0xFF);
    std::lock_guard<std::mutex> lock(sStreamMutex);
    sStreamStates[key] = StreamState {
        resourceId, trackNo, pos, sampleCount, loopStart, loopEnd, loopCount, nowMs()
    };
}

std::string getStreamsJson() {
    return streamsJson();
}

std::string buildAudioDebugHtml() {
    return kAudioDebugHtml;
}

bool hasSamplePatch() {
    std::lock_guard<std::mutex> lock(sSamplePatchMutex);
    return sPendingSamplePatch.has_value();
}

static SamplePatchData patchToData(const SamplePatch& p) {
    SamplePatchData out;
    out.fontId       = p.fontId;
    out.instId       = p.instId;
    out.drumId       = p.drumId;
    out.sfxId        = p.sfxId;
    out.pitchRegion  = p.pitchRegion;
    out.tuning       = p.tuning;
    out.codec        = p.codec;
    out.numSamples   = p.encoded.numSamples;
    out.adpcmData    = p.encoded.adpcmData;
    out.bookOrder        = p.encoded.book.order;
    out.bookNumPredictors = p.encoded.book.numPredictors;
    out.bookCoeffs       = p.encoded.book.codeBook;
    out.loopStart    = p.encoded.loop.loopStart;
    out.loopEnd      = p.encoded.loop.loopEnd;
    out.loopCount    = p.encoded.loop.loopCount;
    out.loopPredictorState = p.encoded.loop.predictorState;
    return out;
}

std::optional<SamplePatchData> peekSamplePatch() {
    std::lock_guard<std::mutex> lock(sSamplePatchMutex);
    if (!sPendingSamplePatch.has_value()) {
        return std::nullopt;
    }
    return patchToData(sPendingSamplePatch.value());
}

std::optional<SamplePatchData> takeSamplePatch() {
    std::lock_guard<std::mutex> lock(sSamplePatchMutex);
    if (!sPendingSamplePatch.has_value()) {
        return std::nullopt;
    }
    SamplePatchData out = patchToData(sPendingSamplePatch.value());
    sPendingSamplePatch.reset();
    return out;
}

// Parse a JSON int array starting after '[', returning the values.
static std::vector<int32_t> parseJsonIntArray(const std::string& json, size_t& pos) {
    std::vector<int32_t> result;
    // Skip to '['
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    pos++; // skip '['
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',')) pos++;
        if (pos >= json.size() || json[pos] == ']') { pos++; break; }
        bool neg = json[pos] == '-';
        if (neg) pos++;
        int32_t v = 0;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            v = v * 10 + (json[pos++] - '0');
        }
        result.push_back(neg ? -v : v);
    }
    return result;
}

void updateSoundFontInfos(const std::string& json) {
    SoundFontState state;
    size_t pos = 0;

    // Parse "activeFonts":[...]
    size_t af = json.find("\"activeFonts\"");
    if (af != std::string::npos) {
        pos = af;
        state.activeFonts = parseJsonIntArray(json, pos);
    }

    // Parse fonts array: find "fonts":[{...},{...}]
    size_t fontsKey = json.find("\"fonts\"");
    if (fontsKey == std::string::npos) {
        std::lock_guard<std::mutex> lock(sSoundFontInfoMutex);
        sSoundFontState = std::move(state);
        return;
    }

    // Walk the fonts array — each element is a {...} object.
    size_t arrStart = json.find('[', fontsKey);
    if (arrStart == std::string::npos) {
        std::lock_guard<std::mutex> lock(sSoundFontInfoMutex);
        sSoundFontState = std::move(state);
        return;
    }
    pos = arrStart + 1;

    while (pos < json.size()) {
        while (pos < json.size() && json[pos] != '{' && json[pos] != ']') pos++;
        if (pos >= json.size() || json[pos] == ']') break;

        // Find matching '}'.  instRegions is an inner array, so we need depth tracking.
        size_t objStart = pos;
        int depth = 0;
        size_t objEnd = pos;
        for (size_t k = pos; k < json.size(); k++) {
            if (json[k] == '{') depth++;
            else if (json[k] == '}') { depth--; if (depth == 0) { objEnd = k; break; } }
            else if (json[k] == '[' && depth == 1) {
                // Skip inner array without incrementing object depth.
                size_t arrEnd = json.find(']', k);
                if (arrEnd != std::string::npos) k = arrEnd;
            }
        }
        std::string obj = json.substr(objStart, objEnd - objStart + 1);

        FontInfo fi;
        fi.numInstruments = jsonInt(obj, "numInstruments", 0);
        fi.numDrums       = jsonInt(obj, "numDrums", 0);
        fi.numSfx         = jsonInt(obj, "numSfx", 0);

        // Parse instRegions inner array.
        size_t irKey = obj.find("\"instRegions\"");
        if (irKey != std::string::npos) {
            size_t irPos = irKey;
            fi.instRegions = parseJsonIntArray(obj, irPos);
        }

        size_t itlKey = obj.find("\"instTuningsLo\"");
        if (itlKey != std::string::npos) {
            size_t itlPos = itlKey;
            fi.instTuningsLo = parseJsonIntArray(obj, itlPos);
        }

        size_t itmKey = obj.find("\"instTuningsMid\"");
        if (itmKey != std::string::npos) {
            size_t itmPos = itmKey;
            fi.instTuningsMid = parseJsonIntArray(obj, itmPos);
        }

        size_t ithKey = obj.find("\"instTuningsHi\"");
        if (ithKey != std::string::npos) {
            size_t ithPos = ithKey;
            fi.instTuningsHi = parseJsonIntArray(obj, ithPos);
        }

        size_t dtKey = obj.find("\"drumTunings\"");
        if (dtKey != std::string::npos) {
            size_t dtPos = dtKey;
            fi.drumTunings = parseJsonIntArray(obj, dtPos);
        }

        size_t stKey = obj.find("\"sfxTunings\"");
        if (stKey != std::string::npos) {
            size_t stPos = stKey;
            fi.sfxTunings = parseJsonIntArray(obj, stPos);
        }

        state.fonts.push_back(std::move(fi));
        pos = objEnd + 1;
    }

    {
        std::lock_guard<std::mutex> lock(sSoundFontInfoMutex);
        sSoundFontState = std::move(state);
    }
}

void setMixerOpen(bool open) {
    if (!open) {
        // Clear all overrides so the game stops applying them immediately.
        std::lock_guard<std::mutex> lock(sMixMutex);
        for (auto& player : sMixOverrides) {
            for (auto& ov : player) {
                ov = MixOverride{};
            }
        }
    }
    sMixerOpen.store(open, std::memory_order_release);
}

bool isMixerOpen() {
    return sMixerOpen.load(std::memory_order_acquire);
}

void setMixOverride(uint32_t playerIndex, uint32_t channelIndex, int32_t pan, int32_t volMilli, int32_t reverb, bool muted) {
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    auto& ov = sMixOverrides[playerIndex][channelIndex];
    ov.active   = true;
    ov.pan      = pan;
    ov.volMilli = volMilli;
    ov.reverb   = reverb;
    ov.muted    = muted;
}

void clearMixOverride(uint32_t playerIndex, uint32_t channelIndex) {
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    sMixOverrides[playerIndex][channelIndex] = MixOverride{};
}

MixOverride getMixOverride(uint32_t playerIndex, uint32_t channelIndex) {
    if (!sMixerOpen.load(std::memory_order_acquire)) {
        return MixOverride{};
    }
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return MixOverride{};
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    return sMixOverrides[playerIndex][channelIndex];
}

} // namespace Debug
