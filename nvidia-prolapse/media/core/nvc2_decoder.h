/* SPDX-License-Identifier: MIT
 * nvc2 decoder core: the codec-agnostic state machine between a framework front
 * (the Codec2 component in ../c2) and the nv_video decode ABI
 * (driver-source/native/video/nv_video.h). No Android dependency, so it is unit-tested
 * on the host against a stubbed nv_video (../test).
 *
 * Contract with the front:
 *  - queue():  one input unit (frame index = the framework's C2 frameIndex). The unit is
 *              handed to nv_video_decode() with the frame index as its timestamp.
 *  - drain():  pictures in display order are copied (GPU copy engine) into buffers the
 *              OutputPool supplies and reported as Output{index, buffer, release fence}.
 *              Inputs that can no longer produce a picture are reported as "empty" outputs
 *              so the front can finish their work items.
 *  - Backpressure: nv_video_decode() returning NV_VIDEO_E_AGAIN means no free DPB/output
 *              slot. The core then drains; if the pool has no free buffer either, queue()
 *              returns Status::Again and keeps the unit — the front re-calls queue() with
 *              the same unit after it got a buffer back.
 *  - Errors are never swallowed: DEVICE_LOST and UNSUPPORTED are terminal for the session
 *              and carry nv_video_last_error(); BITSTREAM finishes that unit empty with the
 *              corrupt flag (the library stays usable after the next keyframe).
 *  - Size changes: NV_VIDEO_E_RESIZE (a keyframe larger than the session, adaptive streaming)
 *              drains the old session's pictures, re-creates the session at the new size
 *              with the parameter sets seen so far and decodes the unit again (AOSP
 *              C2SoftHevcDec.cpp:881-890 on IVD_RES_CHANGED: drainInternal, resetDecoder,
 *              re-decode). If the drain waits for a buffer, queue() returns Again and the
 *              front re-calls it with the same unit.
 */
#ifndef NVC2_DECODER_H
#define NVC2_DECODER_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "nv_video.h"

namespace nvc2 {

enum class Status { Ok, Again, Error };

/* A destination buffer, already allocated by the front (gralloc). */
struct OutputBuffer {
    NvVideoTarget target{};   /* fds stay owned by the front; acquire fence is consumed */
    uint64_t cookie = 0;      /* front's handle for the block */
};

class OutputPool {
public:
    virtual ~OutputPool() = default;
    /* Returns 0 and a buffer of at least width x height in `format`, -EAGAIN when no buffer
     * is free right now (the front calls queue()/drain() again later), other <0 = fatal. */
    virtual int fetch(uint32_t width, uint32_t height, uint32_t format, OutputBuffer *out) = 0;
    /* Buffer was fetched but not used (error path); give it back. */
    virtual void discard(const OutputBuffer &buf) = 0;
};

struct Output {
    uint64_t index = 0;        /* frame index of the input unit */
    bool empty = true;         /* no picture for this unit (hidden frame, config, corrupt, flushed) */
    bool corrupt = false;
    bool eos = false;          /* this is the end-of-stream marker work */
    OutputBuffer buffer{};     /* valid if !empty */
    int release_fence = -1;    /* sync_file fd owned by the receiver, -1 = already complete */
    NvVideoPicture picture{};  /* valid if !empty */
    NvVideoHdrMetadata hdr{};  /* valid if !empty: HDR static metadata the stream declared */
    bool size_changed = false; /* first picture with a new display size */
};

struct Config {
    uint32_t codec = 0;                /* enum NvVideoCodec */
    uint32_t max_width = 0, max_height = 0;
    uint32_t bit_depth = 8;
    uint32_t extra_output_slots = 4;
    std::vector<uint8_t> codec_config; /* csd, may be empty */
};

class Decoder {
public:
    /* Checks `cfg` against nv_video_query_caps() and creates the session. On failure
     * returns nullptr and sets *error to the named reason. */
    static Decoder *create(NvVideoDevice *dev, const Config &cfg, std::string *error);
    ~Decoder();

    Status queue(const uint8_t *data, size_t len, uint64_t index, bool eos, bool codec_config,
                 OutputPool *pool, std::vector<Output> *out);
    Status drain(OutputPool *pool, std::vector<Output> *out);
    /* Seek/flush: discards pending pictures; every pending unit is returned empty. */
    Status flush(std::vector<Output> *out);

    const std::string &error() const { return error_; }
    bool reorders() const { return codec_ == NV_VIDEO_CODEC_H264 || codec_ == NV_VIDEO_CODEC_HEVC; }

private:
    Decoder(NvVideoDevice *dev, NvVideoSession *s, const Config &cfg)
        : dev_(dev), session_(s), codec_(cfg.codec), cfg_(cfg) {}
    static NvVideoSession *open_session(NvVideoDevice *dev, const Config &cfg, std::string *error);
    Status fail(const char *what, int rc);
    Status finish_resize(OutputPool *pool, std::vector<Output> *out);
    Status emit_picture(const NvVideoPicture &pic, OutputPool *pool, std::vector<Output> *out);
    void finish_older_than(uint64_t index, std::vector<Output> *out);
    void finish_all(std::vector<Output> *out);

    NvVideoDevice *dev_;
    NvVideoSession *session_;
    uint32_t codec_;
    Config cfg_;                               /* the current session's config (max size grows) */
    std::vector<std::vector<uint8_t>> csd_;    /* in-band parameter-set units, replayed after a resize */
    bool last_was_csd_ = false;
    bool resizing_ = false;                    /* old session drained up to here, not yet replaced */
    uint32_t resize_w_ = 0, resize_h_ = 0;
    std::deque<uint64_t> pending_;   /* decode order of units without a picture yet */
    bool have_held_ = false;          /* a picture fetched from the library but not yet copied */
    NvVideoPicture held_{};
    uint32_t cur_w_ = 0, cur_h_ = 0;
    bool eos_pending_ = false;   /* an END_OF_STREAM unit was queued */
    bool eos_submitted_ = false; /* ... and accepted by nv_video_decode */
    uint64_t eos_index_ = 0;
    bool have_eos_out_ = false;  /* the EOS unit's own output, returned last */
    Output eos_out_{};
    bool dead_ = false;
    std::string error_;
};

uint32_t output_format_for_depth(uint32_t bit_depth);

/* HDR static metadata in Codec2's units (C2HdrStaticMetadataStruct: CIE 1931 xy
 * chromaticities, luminances and light levels in cd/m2). A group that the stream did not
 * declare stays 0, which Codec2 reads as "not specified". */
struct HdrStatic {
    bool present = false;
    float red[2]{}, green[2]{}, blue[2]{}, white[2]{};
    float max_luminance = 0, min_luminance = 0;
    float max_cll = 0, max_fall = 0;
};
/* Converts nv_video's coded values (per-codec units and primary order, nv_video.h) */
HdrStatic hdr_static(uint32_t codec, const NvVideoHdrMetadata &m);

} // namespace nvc2

#endif
