/* SPDX-License-Identifier: MIT
 * nvc2 decoder core — see nvc2_decoder.h for the contract.
 *
 * Unit bookkeeping (why "empty" outputs are exact):
 *  - VP9 and AV1 never reorder: pictures leave in the decode order of the units that show
 *    them (a hidden frame is shown later by show_existing_frame, i.e. by a later unit).
 *    So when the picture of unit N appears, every still-pending unit decoded before N can
 *    no longer produce one and is finished empty. This tolerates any library latency.
 *  - H.264 and HEVC reorder: a unit is finished when its picture appears, or at EOS/flush.
 *  - Every codec: nv_video_decode() returns the displayable pictures the unit produced
 *    (ABI 2). A unit that produced none (parameter sets, a VP9/AV1 hidden frame, an
 *    H.264/HEVC picture with pic_output_flag = 0 or a skipped RASL picture) can never show a
 *    picture and is finished empty as soon as the call returns.
 */
#include "nvc2_decoder.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <unistd.h>

namespace nvc2 {

uint32_t output_format_for_depth(uint32_t bit_depth)
{
    return bit_depth > 8 ? NV_VIDEO_FORMAT_P010 : NV_VIDEO_FORMAT_NV12;
}

HdrStatic hdr_static(uint32_t codec, const NvVideoHdrMetadata &m)
{
    HdrStatic h;
    if (m.has_mastering_display) {
        const NvVideoMasteringDisplay &d = m.mastering_display;
        h.present = true;
        float chroma, max_l, min_l;
        int r, g, b; /* index of each primary in the coded order */
        if (codec == NV_VIDEO_CODEC_AV1) {
            /* AV1 metadata_hdr_mdcv: 0.16 chromaticities, 24.8 / 18.14 luminances, R,G,B
             * (as FFmpeg av1dec.c:1053-1060) */
            chroma = 1.0f / 65536, max_l = 1.0f / 256, min_l = 1.0f / 16384;
            r = 0, g = 1, b = 2;
        } else {
            /* H.264/HEVC SEI: chromaticities 0.00002, luminances 0.0001 cd/m2, coded G,B,R
             * (as FFmpeg h2645_sei.c:608 mapping {2, 0, 1}) */
            chroma = 0.00002f, max_l = min_l = 0.0001f;
            r = 2, g = 0, b = 1;
        }
        h.red[0] = d.display_primaries_x[r] * chroma, h.red[1] = d.display_primaries_y[r] * chroma;
        h.green[0] = d.display_primaries_x[g] * chroma, h.green[1] = d.display_primaries_y[g] * chroma;
        h.blue[0] = d.display_primaries_x[b] * chroma, h.blue[1] = d.display_primaries_y[b] * chroma;
        h.white[0] = d.white_point_x * chroma, h.white[1] = d.white_point_y * chroma;
        h.max_luminance = d.max_luminance * max_l;
        h.min_luminance = d.min_luminance * min_l;
    }
    if (m.has_content_light_level) {
        h.present = true;
        h.max_cll = (float)m.max_content_light_level;
        h.max_fall = (float)m.max_pic_average_light_level;
    }
    return h;
}

static const char *err_name(int rc)
{
    switch (rc) {
    case NV_VIDEO_E_INVALID: return "INVALID";
    case NV_VIDEO_E_UNSUPPORTED: return "UNSUPPORTED";
    case NV_VIDEO_E_BITSTREAM: return "BITSTREAM";
    case NV_VIDEO_E_NO_MEMORY: return "NO_MEMORY";
    case NV_VIDEO_E_DEVICE_LOST: return "DEVICE_LOST";
    case NV_VIDEO_E_AGAIN: return "AGAIN";
    case NV_VIDEO_E_RESIZE: return "RESIZE";
    default: return "unknown";
    }
}

NvVideoSession *Decoder::open_session(NvVideoDevice *dev, const Config &cfg, std::string *error)
{
    NvVideoCaps caps{};
    int rc = nv_video_query_caps(dev, cfg.codec, &caps);
    if (rc != 0) {
        *error = std::string("nv_video_query_caps(codec ") + std::to_string(cfg.codec) + ") failed: " +
                 err_name(rc) + ": " + (nv_video_last_error() ? nv_video_last_error() : "");
        return nullptr;
    }
    if (!caps.supported) {
        *error = "codec " + std::to_string(cfg.codec) + " is not supported by this GPU (nv_video_query_caps)";
        return nullptr;
    }
    if (cfg.max_width > caps.max_width || cfg.max_height > caps.max_height ||
        cfg.max_width < caps.min_width || cfg.max_height < caps.min_height) {
        *error = "requested " + std::to_string(cfg.max_width) + "x" + std::to_string(cfg.max_height) +
                 " is outside the GPU's " + std::to_string(caps.min_width) + "x" + std::to_string(caps.min_height) +
                 ".." + std::to_string(caps.max_width) + "x" + std::to_string(caps.max_height);
        return nullptr;
    }
    if (!(caps.bit_depth_mask & (1u << cfg.bit_depth))) {
        *error = std::to_string(cfg.bit_depth) + "-bit is not supported for codec " + std::to_string(cfg.codec);
        return nullptr;
    }
    NvVideoSessionInfo info{};
    info.codec = cfg.codec;
    info.max_width = cfg.max_width;
    info.max_height = cfg.max_height;
    info.bit_depth = cfg.bit_depth;
    info.extra_output_slots = cfg.extra_output_slots;
    info.codec_config = cfg.codec_config.empty() ? nullptr : cfg.codec_config.data();
    info.codec_config_len = cfg.codec_config.size();
    NvVideoSession *s = nullptr;
    rc = nv_video_session_create(dev, &info, &s);
    if (rc != 0 || !s) {
        *error = std::string("nv_video_session_create failed: ") + err_name(rc) + ": " +
                 (nv_video_last_error() ? nv_video_last_error() : "");
        return nullptr;
    }
    return s;
}

Decoder *Decoder::create(NvVideoDevice *dev, const Config &cfg, std::string *error)
{
    NvVideoSession *s = open_session(dev, cfg, error);
    return s ? new Decoder(dev, s, cfg) : nullptr;
}

Decoder::~Decoder()
{
    if (have_held_)
        nv_video_picture_release(session_, held_.id);
    if (session_)
        nv_video_session_destroy(session_);
}

/* The old session got its END_OF_STREAM drain; copy out what it still holds, then replace it
 * by one of at least the required size. Again = waiting for an output buffer. */
Status Decoder::finish_resize(OutputPool *pool, std::vector<Output> *out)
{
    Status st = drain(pool, out);
    if (st != Status::Ok)
        return st;
    /* everything decoded before the keyframe has been output: the rest can never show */
    finish_all(out);
    nv_video_session_destroy(session_);
    session_ = nullptr;
    Config next = cfg_;
    next.max_width = std::max(cfg_.max_width, resize_w_);
    next.max_height = std::max(cfg_.max_height, resize_h_);
    std::string err;
    session_ = open_session(dev_, next, &err);
    if (!session_) {
        dead_ = true;
        error_ = "re-creating the session at " + std::to_string(next.max_width) + "x" +
                 std::to_string(next.max_height) + " for a size change: " + err;
        return Status::Error;
    }
    cfg_ = next;
    resizing_ = false;
    /* H.264/HEVC parameter sets sent in-band (csd) belong to the stream, not the session */
    for (const auto &u : csd_) {
        int rc = nv_video_decode(session_, u.data(), u.size(), 0, 0);
        if (rc < 0)
            return fail("nv_video_decode(codec config after a size change)", rc);
    }
    return Status::Ok;
}

Status Decoder::fail(const char *what, int rc)
{
    dead_ = true;
    const char *le = nv_video_last_error();
    error_ = std::string(what) + ": " + err_name(rc) + " (" + std::to_string(rc) + "): " + (le ? le : "");
    return Status::Error;
}

void Decoder::finish_older_than(uint64_t index, std::vector<Output> *out)
{
    while (!pending_.empty() && pending_.front() != index) {
        Output o;
        o.index = pending_.front();
        pending_.pop_front();
        out->push_back(o);
    }
}

void Decoder::finish_all(std::vector<Output> *out)
{
    for (uint64_t i : pending_) {
        if (eos_pending_ && i == eos_index_)
            continue; /* the EOS unit is returned last, with the EOS flag */
        Output o;
        o.index = i;
        out->push_back(o);
    }
    pending_.clear();
}

Status Decoder::emit_picture(const NvVideoPicture &pic, OutputPool *pool, std::vector<Output> *out)
{
    const uint32_t fmt = output_format_for_depth(pic.bit_depth);
    OutputBuffer buf;
    int rc = pool->fetch(pic.crop_width, pic.crop_height, fmt, &buf);
    if (rc == -EAGAIN) {
        held_ = pic;
        have_held_ = true;
        return Status::Again;
    }
    if (rc != 0) {
        nv_video_picture_release(session_, pic.id);
        have_held_ = false;
        dead_ = true;
        error_ = "output pool fetch failed (" + std::to_string(rc) + ") for " + std::to_string(pic.crop_width) +
                 "x" + std::to_string(pic.crop_height) + (fmt == NV_VIDEO_FORMAT_P010 ? " P010" : " NV12");
        return Status::Error;
    }
    have_held_ = false;
    /* A front that leaves the target format 0 gets the picture's format. It may instead lay an
     * 8-bit picture out planar (YUV420P) where its client expects that; any other format is a
     * front bug. */
    if (buf.target.format == 0)
        buf.target.format = fmt;
    if (buf.target.format != fmt && !(fmt == NV_VIDEO_FORMAT_NV12 && buf.target.format == NV_VIDEO_FORMAT_YUV420P)) {
        const uint32_t got = buf.target.format;
        pool->discard(buf);
        nv_video_picture_release(session_, pic.id);
        dead_ = true;
        error_ = "output pool returned target format " + std::to_string(got) + " for a " +
                 std::to_string(pic.bit_depth) + "-bit picture (wants " + std::to_string(fmt) + ")";
        return Status::Error;
    }
    NvVideoHdrMetadata hdr{};
    rc = nv_video_picture_hdr(session_, pic.id, &hdr);
    if (rc < 0) {
        pool->discard(buf);
        nv_video_picture_release(session_, pic.id);
        return fail("nv_video_picture_hdr", rc);
    }
    int fence = -1;
    rc = nv_video_copy_out(session_, pic.id, &buf.target, &fence);
    /* copy_out consumed (or ignored) the acquire fence; the front must not reuse it */
    if (buf.target.acquire_fence_fd >= 0) {
        close(buf.target.acquire_fence_fd);
        buf.target.acquire_fence_fd = -1;
    }
    if (rc != 0) {
        pool->discard(buf);
        nv_video_picture_release(session_, pic.id);
        return fail("nv_video_copy_out", rc);
    }
    rc = nv_video_picture_release(session_, pic.id);
    if (rc != 0) {
        if (fence >= 0)
            close(fence);
        pool->discard(buf);
        return fail("nv_video_picture_release", rc);
    }

    if (!reorders())
        finish_older_than(pic.timestamp, out);
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (*it == pic.timestamp) {
            pending_.erase(it);
            break;
        }

    Output o;
    o.index = pic.timestamp;
    o.empty = false;
    o.buffer = buf;
    o.release_fence = fence;
    o.picture = pic;
    o.hdr = hdr;
    o.size_changed = pic.crop_width != cur_w_ || pic.crop_height != cur_h_;
    cur_w_ = pic.crop_width;
    cur_h_ = pic.crop_height;
    if (eos_pending_ && pic.timestamp == eos_index_) {
        eos_out_ = o;
        have_eos_out_ = true;
    } else {
        out->push_back(o);
    }
    return Status::Ok;
}

Status Decoder::drain(OutputPool *pool, std::vector<Output> *out)
{
    if (dead_)
        return Status::Error;
    for (;;) {
        if (have_held_) {
            NvVideoPicture pic = held_;
            Status st = emit_picture(pic, pool, out);
            if (st != Status::Ok)
                return st;
            continue;
        }
        NvVideoPicture pic{};
        int rc = nv_video_next_picture(session_, &pic);
        if (rc < 0)
            return fail("nv_video_next_picture", rc);
        if (rc == 0)
            break;
        Status st = emit_picture(pic, pool, out);
        if (st != Status::Ok)
            return st;
    }
    if (eos_pending_ && eos_submitted_) {
        /* END_OF_STREAM drained everything: nothing pending can produce a picture now */
        finish_all(out);
        Output o = have_eos_out_ ? eos_out_ : Output{};
        o.index = eos_index_;
        o.eos = true;
        out->push_back(o);
        eos_pending_ = eos_submitted_ = have_eos_out_ = false;
    }
    return Status::Ok;
}

Status Decoder::queue(const uint8_t *data, size_t len, uint64_t index, bool eos, bool codec_config,
                      OutputPool *pool, std::vector<Output> *out)
{
    if (dead_)
        return Status::Error;
    if (resizing_) {
        Status st = finish_resize(pool, out);
        if (st != Status::Ok)
            return st;
    }
    if (codec_config && !eos) {
        /* Annex-B parameter sets (H.264/HEVC csd) go in-band; VP9/AV1 csd (vpcC/av1C) carry
         * nothing the bitstream does not repeat, and were handed to the session at create. */
        if (reorders() && len) {
            int rc = nv_video_decode(session_, data, len, index, 0);
            if (rc == NV_VIDEO_E_BITSTREAM) {
                Output o;
                o.index = index;
                o.corrupt = true;
                out->push_back(o);
                return Status::Ok;
            }
            if (rc < 0)
                return fail("nv_video_decode(codec config)", rc);
            /* csd-0/csd-1 (SPS, PPS) arrive as consecutive config units; a new run replaces */
            if (!last_was_csd_)
                csd_.clear();
            csd_.emplace_back(data, data + len);
            last_was_csd_ = true;
        }
        Output o;
        o.index = index;
        out->push_back(o);
        return Status::Ok;
    }

    last_was_csd_ = false;
    if (eos) {
        eos_pending_ = true;
        eos_index_ = index;
    }
    for (;;) {
        int rc = nv_video_decode(session_, len ? data : nullptr, len, index,
                                 eos ? NV_VIDEO_DECODE_END_OF_STREAM : 0);
        if (rc == NV_VIDEO_E_AGAIN) {
            /* no free DPB/output slot: copy out what is ready, then retry */
            size_t before = out->size();
            Status st = drain(pool, out);
            if (st == Status::Error)
                return st;
            if (st == Status::Again || out->size() == before)
                return Status::Again; /* front retries this unit after a buffer comes back */
            continue;
        }
        if (rc == NV_VIDEO_E_BITSTREAM) {
            Output o;
            o.index = index;
            o.corrupt = true;
            if (eos) {
                /* still drain what was decoded before */
                int drc = nv_video_decode(session_, nullptr, 0, index, NV_VIDEO_DECODE_END_OF_STREAM);
                if (drc < 0)
                    return fail("nv_video_decode(end of stream)", drc);
                eos_submitted_ = true;
                eos_out_ = o;
                have_eos_out_ = true;
                break;
            }
            out->push_back(o);
            break;
        }
        if (rc == NV_VIDEO_E_RESIZE) {
            if (nv_video_required_size(session_, &resize_w_, &resize_h_) < 0)
                return fail("nv_video_required_size", NV_VIDEO_E_INVALID);
            /* the unit was not consumed: drain what the old session decoded before it */
            int drc = nv_video_decode(session_, nullptr, 0, index, NV_VIDEO_DECODE_END_OF_STREAM);
            if (drc < 0)
                return fail("nv_video_decode(drain before a size change)", drc);
            resizing_ = true;
            Status st = finish_resize(pool, out);
            if (st != Status::Ok)
                return st; /* Again: the front re-calls queue() with this unit */
            continue;      /* decode the unit on the new session */
        }
        if (rc < 0)
            return fail("nv_video_decode", rc);
        if (len) {
            if (rc > 0 || eos) {
                pending_.push_back(index);
            } else {
                Output o; /* produced no displayable picture: finished now */
                o.index = index;
                out->push_back(o);
            }
        }
        if (eos)
            eos_submitted_ = true;
        break;
    }
    Status st = drain(pool, out);
    /* the unit itself was consumed; an empty pool only delays its picture */
    return st == Status::Error ? st : Status::Ok;
}

Status Decoder::flush(std::vector<Output> *out)
{
    if (dead_)
        return Status::Error;
    if (have_held_) {
        nv_video_picture_release(session_, held_.id);
        have_held_ = false;
    }
    /* a seek during a size change: the old session's drained pictures are dropped with the
     * rest; the next keyframe asks for the new size again */
    resizing_ = false;
    int rc = nv_video_flush(session_);
    bool was_eos = eos_pending_;
    eos_pending_ = false;
    finish_all(out);
    if (was_eos) {
        Output o = have_eos_out_ ? eos_out_ : Output{};
        o.index = eos_index_;
        o.eos = true;
        out->push_back(o);
    }
    eos_submitted_ = have_eos_out_ = false;
    if (rc != 0)
        return fail("nv_video_flush", rc);
    return Status::Ok;
}

} // namespace nvc2
