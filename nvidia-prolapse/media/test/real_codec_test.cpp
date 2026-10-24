/* SPDX-License-Identifier: MIT
 * Host end-to-end test of the decoder core on the REAL nv_video backend (GNU build of
 * libnv_bionic_native) and the GPU, for every codec the Codec2 HAL advertises.
 *
 *   real_codec_test <h264|hevc|vp9|av1> <stream> <ref.raw|-> <w> <h> [pool buffers]
 *
 * Input framing is the nv_video contract: one IVF frame = one unit for VP9/AV1 (superframes
 * included; an IVF frame is an AV1 temporal unit), one Annex-B access unit = one unit for
 * H.264/HEVC (7.4.1.2.3 / 7.4.2.4.4; parameter sets in-band).
 *
 * Output targets, selected with REAL_MODE (default nv12):
 *   nv12  LINEAR NV12 dma-buf, pitch ALIGN(width, 256), CbCr at pitch*even(height)
 *   p010  LINEAR P010 dma-buf, same layout at 2 bytes/sample (10-bit pictures only)
 *   yv12  Android YV12 blocks, exactly the target the Codec2 component's CPU/ByteBuffer
 *         pool builds (nvidia-prolapse/media/c2/NvC2Component.cpp Component::Pool):
 *         Y stride ALIGN(width,16), Cr at Y stride*height, Cb after Cr, one dma-buf.
 *         The pool answers an NV12 picture with NV_VIDEO_FORMAT_YUV420P, as the component does.
 *
 * The picture is compared with an FFmpeg software-decoder reference (raw nv12 or p010le,
 * display order) in the display window; every input unit must be returned exactly once
 * (hidden/non-output units as empty), EOS last. REAL_EXPECT=<file> instead compares the
 * FNV-1a hash of every tightly packed output frame with one hex hash per line (the pinned
 * smoke reference for the release gate; produced from a run that passed the FFmpeg
 * comparison). Buffers come from /dev/dma_heap/system (sysmem, a foreign dma-buf).
 *
 * Exit: 0 PASS, 1 FAIL, 2 usage/environment, 77 codec or bit depth not supported by this GPU.
 */
#include "nvc2_decoder.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace nvc2;

enum class Mode { NV12, P010, YV12 };

static std::vector<uint8_t> slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(2);
    }
    std::vector<uint8_t> v;
    uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

/* NAL boundaries exactly as the backend's parser sees them (native/src/video/bits.rs
 * split_annexb): start code at `start` (including a leading zero when 00 00 00 01),
 * payload from `nal`. */
struct Nal {
    size_t start, payload, end;
};
static std::vector<Nal> split_annexb(const std::vector<uint8_t> &d)
{
    std::vector<std::pair<size_t, size_t>> starts;
    for (size_t i = 0; i + 3 <= d.size();) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            starts.push_back({i > 0 && d[i - 1] == 0 ? i - 1 : i, i + 3});
            i += 3;
        } else {
            i++;
        }
    }
    std::vector<Nal> out;
    for (size_t k = 0; k < starts.size(); k++) {
        size_t end = k + 1 < starts.size() ? starts[k + 1].first : d.size();
        while (end > starts[k].second && d[end - 1] == 0)
            end--;
        if (end > starts[k].second)
            out.push_back({starts[k].first, starts[k].second, end});
    }
    return out;
}

/* Access units per H.264 7.4.1.2.3 / H.265 7.4.2.4.4, same rule as native/src/video/bits.rs
 * access_units(): a unit starts at a parameter set / AUD / prefix SEI after a slice, or at
 * the first slice of a new picture. */
static std::vector<std::pair<size_t, size_t>> access_units(const std::vector<uint8_t> &d, bool hevc)
{
    std::vector<std::pair<size_t, size_t>> out;
    size_t start = 0;
    bool has_slice = false;
    for (const auto &n : split_annexb(d)) {
        unsigned type = hevc ? (d[n.payload] >> 1) & 0x3f : d[n.payload] & 0x1f;
        bool vcl = hevc ? type < 32 : (type == 1 || type == 5);
        bool first = vcl && n.end - n.payload > (hevc ? 2u : 1u) &&
                     (d[n.payload + (hevc ? 2 : 1)] & 0x80) != 0;
        bool prefix = hevc ? (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) ||
                                 (type >= 48 && type <= 55)
                           : (type >= 6 && type <= 9) || (type >= 14 && type <= 18);
        if (has_slice && (prefix || first)) {
            out.push_back({start, n.start});
            start = n.start;
            has_slice = false;
        }
        has_slice |= vcl;
    }
    if (start < d.size())
        out.push_back({start, d.size()});
    return out;
}

/* Units of an IVF file: 32-byte DKIF header, then 12-byte frame headers. */
static std::vector<std::pair<size_t, size_t>> ivf_units(const std::vector<uint8_t> &d)
{
    std::vector<std::pair<size_t, size_t>> out;
    size_t off = 32;
    while (off + 12 <= d.size()) {
        uint32_t len = d[off] | d[off + 1] << 8 | d[off + 2] << 16 | (uint32_t)d[off + 3] << 24;
        if (off + 12 + len > d.size())
            break;
        out.push_back({off + 12, off + 12 + len});
        off += 12 + len;
    }
    return out;
}

struct HeapPool : OutputPool {
    struct Buf {
        int fd;
        size_t size;
        bool busy;
    };
    std::vector<Buf> bufs;
    Mode mode;
    uint32_t w, h;          /* display size */
    uint32_t pitch, luma_rows, ys, cs, cr_off, cb_off;
    uint32_t block_w, block_h;
    size_t size;

    HeapPool(int n, Mode m, uint32_t w_, uint32_t h_) : mode(m), w(w_), h(h_)
    {
        block_w = mode == Mode::YV12 ? align_up(w, 128) : w;
        block_h = (h + 1) & ~1u;
        if (mode == Mode::P010) {
            pitch = align_up(w * 2, 256);
            luma_rows = block_h;
            size = (size_t)pitch * luma_rows + (size_t)pitch * (block_h / 2);
        } else if (mode == Mode::YV12) {
            ys = align_up(block_w, 16);
            cs = align_up(ys / 2, 16);
            cr_off = ys * block_h;
            cb_off = cr_off + cs * (block_h / 2);
            size = (size_t)cb_off + (size_t)cs * (block_h / 2);
        } else {
            pitch = align_up(w, 256);
            luma_rows = block_h;
            size = (size_t)pitch * luma_rows * 3 / 2;
        }
        int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        if (heap < 0) {
            perror("/dev/dma_heap/system");
            exit(2);
        }
        for (int i = 0; i < n; i++) {
            struct dma_heap_allocation_data a = {};
            a.len = (size + 4095) & ~4095ul;
            a.fd_flags = O_RDWR | O_CLOEXEC;
            if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) != 0) {
                perror("DMA_HEAP_IOCTL_ALLOC");
                exit(2);
            }
            bufs.push_back({(int)a.fd, (size_t)a.len, false});
        }
        close(heap);
    }
    int fetch(uint32_t fw, uint32_t fh, uint32_t format, OutputBuffer *out) override
    {
        const uint32_t want = mode == Mode::P010 ? NV_VIDEO_FORMAT_P010 : NV_VIDEO_FORMAT_NV12;
        if (format != want || fw > w || fh > h)
            return -EINVAL;
        for (size_t i = 0; i < bufs.size(); i++)
            if (!bufs[i].busy) {
                bufs[i].busy = true;
                *out = OutputBuffer{};
                out->target.dmabuf_fd[0] = bufs[i].fd;
                out->target.dmabuf_fd[1] = bufs[i].fd;
                out->target.modifier = 0;
                out->target.acquire_fence_fd = -1;
                out->cookie = i;
                if (mode == Mode::YV12) {
                    /* The component's target for a CPU/ByteBuffer client: three planes in one
                     * dma-buf, YV12 order Y/Cr/Cb, NV_VIDEO_FORMAT_YUV420P. */
                    out->target.cr_dmabuf_fd = bufs[i].fd;
                    out->target.offset[0] = 0;
                    out->target.offset[1] = cb_off;
                    out->target.cr_offset = cr_off;
                    out->target.pitch[0] = ys;
                    out->target.pitch[1] = cs;
                    out->target.cr_pitch = cs;
                    out->target.format = NV_VIDEO_FORMAT_YUV420P;
                    out->target.width = block_w;
                    out->target.height = block_h;
                } else {
                    out->target.offset[0] = 0;
                    out->target.offset[1] = (uint64_t)pitch * luma_rows;
                    out->target.pitch[0] = out->target.pitch[1] = pitch;
                    out->target.format = mode == Mode::P010 ? NV_VIDEO_FORMAT_P010 : NV_VIDEO_FORMAT_NV12;
                    out->target.width = w;
                    out->target.height = h;
                }
                return 0;
            }
        return -EAGAIN;
    }
    void discard(const OutputBuffer &b) override { bufs[b.cookie].busy = false; }
};

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: real_codec_test <h264|hevc|vp9|av1> <stream> <ref.raw|-> <w> <h> [pool]\n"
                        "       REAL_MODE=nv12|p010|yv12 (default nv12), REAL_EXPECT=<hashes>\n");
        return 2;
    }
    const std::string codec_name = argv[1];
    const std::vector<uint8_t> stream = slurp(argv[2]);
    const auto ref = std::string(argv[3]) == "-" ? std::vector<uint8_t>{} : slurp(argv[3]);
    const uint32_t w = atoi(argv[4]), h = atoi(argv[5]);
    const int npool = argc > 6 ? atoi(argv[6]) : 3;
    const char *mode_env = getenv("REAL_MODE") ? getenv("REAL_MODE") : "nv12";
    Mode mode;
    if (!strcmp(mode_env, "nv12"))
        mode = Mode::NV12;
    else if (!strcmp(mode_env, "p010"))
        mode = Mode::P010;
    else if (!strcmp(mode_env, "yv12"))
        mode = Mode::YV12;
    else {
        fprintf(stderr, "REAL_MODE=%s: nv12, p010 or yv12\n", mode_env);
        return 2;
    }
    const uint32_t bit_depth = mode == Mode::P010 ? 10 : 8;
    const size_t sample = bit_depth > 8 ? 2 : 1;

    uint32_t codec = 0;
    if (codec_name == "h264")
        codec = NV_VIDEO_CODEC_H264;
    else if (codec_name == "hevc")
        codec = NV_VIDEO_CODEC_HEVC;
    else if (codec_name == "vp9")
        codec = NV_VIDEO_CODEC_VP9;
    else if (codec_name == "av1")
        codec = NV_VIDEO_CODEC_AV1;
    else {
        fprintf(stderr, "unknown codec %s\n", codec_name.c_str());
        return 2;
    }

    NvVideoDevice *dev = nullptr;
    NvVideoDeviceInfo info{};
    if (nv_video_device_open(&dev, &info) != 0) {
        fprintf(stderr, "device_open: %s\n", nv_video_last_error());
        return 2;
    }
    NvVideoCaps caps{};
    int crc = nv_video_query_caps(dev, codec, &caps);
    if (crc != 0) {
        fprintf(stderr, "query_caps(%s): %s\n", codec_name.c_str(), nv_video_last_error());
        return 2;
    }
    if (!caps.supported) {
        printf("SKIP %s: not supported by this GPU (nvidia-smi/query_caps)\n", codec_name.c_str());
        return 77;
    }
    if (!(caps.bit_depth_mask & (1u << bit_depth))) {
        printf("SKIP %s %u-bit: not supported by this GPU (bit_depth_mask 0x%x)\n", codec_name.c_str(),
               bit_depth, caps.bit_depth_mask);
        return 77;
    }

    Config cfg;
    cfg.codec = codec;
    /* H.264/HEVC are coded in macroblock/CTU units: the session is sized for the padded size. */
    const bool annexb = codec == NV_VIDEO_CODEC_H264 || codec == NV_VIDEO_CODEC_HEVC;
    cfg.max_width = annexb ? align_up(w, 16) : w;
    cfg.max_height = annexb ? align_up(h, 16) : h;
    cfg.bit_depth = bit_depth;
    cfg.extra_output_slots = 0;
    std::string err;
    Decoder *d = Decoder::create(dev, cfg, &err);
    if (!d) {
        fprintf(stderr, "create: %s\n", err.c_str());
        return 2;
    }
    HeapPool pool(npool, mode, w, h);

    std::vector<std::pair<size_t, size_t>> units;
    if (codec == NV_VIDEO_CODEC_VP9 || codec == NV_VIDEO_CODEC_AV1) {
        if (stream.size() < 32 || memcmp(stream.data(), "DKIF", 4)) {
            fprintf(stderr, "%s needs an IVF stream\n", codec_name.c_str());
            return 2;
        }
        units = ivf_units(stream);
    } else {
        units = access_units(stream, codec == NV_VIDEO_CODEC_HEVC);
    }
    if (units.empty()) {
        fprintf(stderr, "no input units in %s\n", argv[2]);
        return 2;
    }

    const size_t ref_frame = (size_t)w * h * 3 / 2 * sample;
    const bool have_ref = !ref.empty();
    const size_t ref_frames = have_ref ? ref.size() / ref_frame : 0;
    std::vector<uint64_t> expect;
    if (const char *ep = getenv("REAL_EXPECT")) {
        auto text = slurp(ep);
        std::string s(text.begin(), text.end());
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find('\n', p);
            if (e == std::string::npos)
                e = s.size();
            std::string line = s.substr(p, e - p);
            p = e + 1;
            if (line.empty() || line[0] == '#')
                continue;
            expect.push_back(strtoull(line.c_str(), nullptr, 16));
        }
    }

    size_t shown = 0, empties = 0, bad_frames = 0, agains = 0;
    std::set<uint64_t> returned;
    bool eos_seen = false, order_ok = true;
    std::vector<uint8_t> packed(ref_frame);

    auto consume = [&](std::vector<Output> &outs) {
        for (auto &o : outs) {
            if (returned.count(o.index) || eos_seen)
                order_ok = false;
            returned.insert(o.index);
            if (o.eos)
                eos_seen = true;
            if (o.empty) {
                if (!o.eos)
                    empties++;
                continue;
            }
            if (o.release_fence >= 0) {
                struct pollfd p = {o.release_fence, POLLIN, 0};
                if (poll(&p, 1, 2000) != 1) {
                    fprintf(stderr, "copy fence timeout\n");
                    exit(1);
                }
                close(o.release_fence);
            }
            auto &b = pool.bufs[o.buffer.cookie];
            uint8_t *m = (uint8_t *)mmap(nullptr, b.size, PROT_READ, MAP_SHARED, b.fd, 0);
            if (m == MAP_FAILED) {
                perror("mmap");
                exit(1);
            }
            /* Repack the display window into tightly packed nv12 (or p010le) for the
             * comparison and the pinned hash. */
            uint8_t *pk = packed.data();
            if (mode == Mode::YV12) {
                for (uint32_t y = 0; y < h; y++)
                    memcpy(pk + (size_t)y * w, m + (size_t)y * pool.ys, w);
                uint8_t *pc = pk + (size_t)w * h;
                const uint8_t *cb = m + pool.cb_off, *cr = m + pool.cr_off;
                for (uint32_t cy = 0; cy < h / 2; cy++)
                    for (uint32_t cx = 0; cx < w / 2; cx++) {
                        pc[((size_t)cy * w) + cx * 2] = cb[(size_t)cy * pool.cs + cx];
                        pc[((size_t)cy * w) + cx * 2 + 1] = cr[(size_t)cy * pool.cs + cx];
                    }
            } else {
                const size_t rb = (size_t)w * sample;
                for (uint32_t y = 0; y < h; y++)
                    memcpy(pk + (size_t)y * rb, m + (size_t)y * pool.pitch, rb);
                const uint8_t *mc = m + (size_t)pool.pitch * pool.luma_rows;
                uint8_t *pc = pk + (size_t)w * h * sample;
                for (uint32_t y = 0; y < h / 2; y++)
                    memcpy(pc + (size_t)y * rb, mc + (size_t)y * pool.pitch, rb);
            }
            munmap(m, b.size);

            if (have_ref && getenv("REAL_PRINT_HASHES"))
                printf("%016llx\n", (unsigned long long)fnv1a(pk, ref_frame));
            if (have_ref) {
                if (shown >= ref_frames) {
                    fprintf(stderr, "more pictures than reference frames\n");
                    exit(1);
                }
                size_t diff = 0;
                const uint8_t *r = ref.data() + shown * ref_frame;
                for (size_t i = 0; i < ref_frame; i++)
                    diff += pk[i] != r[i];
                if (diff) {
                    bad_frames++;
                    fprintf(stderr, "picture %zu (unit %llu): %zu bytes differ\n", shown,
                            (unsigned long long)o.index, diff);
                }
            } else if (!expect.empty() || getenv("REAL_PRINT_HASHES")) {
                const uint64_t got = fnv1a(pk, ref_frame);
                if (getenv("REAL_PRINT_HASHES"))
                    printf("%016llx\n", (unsigned long long)got);
                if (!expect.empty()) {
                    if (shown >= expect.size()) {
                        fprintf(stderr, "more pictures than expected hashes\n");
                        exit(1);
                    }
                    if (got != expect[shown]) {
                        bad_frames++;
                        fprintf(stderr, "picture %zu (unit %llu): hash %016llx != %016llx\n", shown,
                                (unsigned long long)o.index, (unsigned long long)got,
                                (unsigned long long)expect[shown]);
                    }
                }
            }
            shown++;
            b.busy = false; /* the "consumer" returns the buffer at once */
        }
        outs.clear();
    };

    std::vector<Output> outs;
    /* EOS is its own empty input unit by default, as the Codec2 front sends it (a work item
     * with the END_OF_STREAM flag and no buffer). REAL_EOS_ON_LAST=1 instead sets the flag on
     * the last data unit, a separate backend path kept measurable. */
    const bool combined_eos = getenv("REAL_EOS_ON_LAST") != nullptr;
    for (size_t u = 0; u < units.size(); u++) {
        const uint8_t *p = stream.data() + units[u].first;
        const size_t len = units[u].second - units[u].first;
        const bool last = combined_eos && u + 1 == units.size();
        for (;;) {
            Status st = d->queue(p, len, u, last, false, &pool, &outs);
            consume(outs);
            if (st == Status::Error) {
                fprintf(stderr, "queue(unit %zu): %s\n", u, d->error().c_str());
                return 2;
            }
            if (st == Status::Ok)
                break;
            agains++;
            if (d->drain(&pool, &outs) == Status::Error) {
                fprintf(stderr, "drain: %s\n", d->error().c_str());
                return 2;
            }
            consume(outs);
        }
    }
    if (!combined_eos) {
        Status st = d->queue(nullptr, 0, units.size(), true, false, &pool, &outs);
        consume(outs);
        if (st == Status::Error) {
            fprintf(stderr, "queue(EOS): %s\n", d->error().c_str());
            return 2;
        }
        if (st == Status::Again)
            agains++;
    }
    while (!eos_seen) {
        if (d->drain(&pool, &outs) == Status::Error) {
            fprintf(stderr, "drain: %s\n", d->error().c_str());
            return 2;
        }
        bool none = outs.empty();
        consume(outs);
        if (none && !eos_seen) {
            fprintf(stderr, "stuck: EOS never returned\n");
            return 2;
        }
    }
    delete d;
    nv_video_device_close(dev);
    const char *mode_name = mode == Mode::YV12 ? "yv12" : mode == Mode::P010 ? "p010" : "nv12";
    const size_t want_frames = have_ref ? ref_frames : expect.size();
    const size_t want_returned = units.size() + (combined_eos ? 0 : 1);
    printf("real_codec_test %s %ux%u %s on %s: %zu units, %zu pictures (ref %zu), %zu empty, %zu AGAIN "
           "retries, %zu returned, order %s, %zu mismatching pictures\n",
           codec_name.c_str(), w, h, mode_name, info.name, units.size(), shown, want_frames, empties, agains,
           returned.size(), order_ok ? "ok" : "BAD", bad_frames);
    const bool ok = shown == want_frames && returned.size() == want_returned && order_ok && !bad_frames && eos_seen;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
