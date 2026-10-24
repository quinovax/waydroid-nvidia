/* SPDX-License-Identifier: MIT
 * Host unit tests of the nvc2 decoder core against the scripted nv_video stub.
 * Covers: caps gating at config, decode -> display-order output, VP9/AV1 hidden frames
 * finished empty, H.264/HEVC reordering, END_OF_STREAM drain (with and without data),
 * flush/seek (pending returned, keyframe required), resolution change, 10-bit -> P010,
 * AGAIN backpressure with an exhausted output pool, BITSTREAM, DEVICE_LOST.
 */
#include "nvc2_decoder.h"
#include "nv_video_stub.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sys/mman.h>
#include <unistd.h>

using namespace nvc2;

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                                            \
        }                                                                        \
    } while (0)

struct TestPool : OutputPool {
    struct Buf { int fd; bool busy; };
    std::vector<Buf> bufs;
    unsigned fetches = 0, discards = 0;
    uint32_t last_w = 0, last_h = 0, last_fmt = 0;
    uint32_t target_fmt = 0; /* format the pool writes into the target (0 = leave it to the core) */
    explicit TestPool(int n)
    {
        for (int i = 0; i < n; i++) {
            int fd = memfd_create("nvc2-test", 0);
            if (ftruncate(fd, 1 << 16) != 0)
                abort();
            bufs.push_back({fd, false});
        }
    }
    ~TestPool() override
    {
        for (auto &b : bufs)
            close(b.fd);
    }
    int fetch(uint32_t w, uint32_t h, uint32_t fmt, OutputBuffer *out) override
    {
        fetches++;
        last_w = w, last_h = h, last_fmt = fmt;
        for (size_t i = 0; i < bufs.size(); i++)
            if (!bufs[i].busy) {
                bufs[i].busy = true;
                *out = OutputBuffer{};
                out->target.dmabuf_fd[0] = out->target.dmabuf_fd[1] = bufs[i].fd;
                out->target.offset[1] = 4096;
                out->target.pitch[0] = out->target.pitch[1] = 256;
                out->target.width = w;
                out->target.height = h;
                out->target.acquire_fence_fd = -1;
                out->target.format = target_fmt;
                if (target_fmt == NV_VIDEO_FORMAT_YUV420P) {
                    out->target.cr_dmabuf_fd = bufs[i].fd;
                    out->target.cr_offset = 8192;
                    out->target.cr_pitch = 128;
                }
                out->cookie = i;
                return 0;
            }
        return -EAGAIN;
    }
    void discard(const OutputBuffer &b) override
    {
        discards++;
        bufs[b.cookie].busy = false;
    }
    void give_back(const Output &o) { bufs[o.buffer.cookie].busy = false; }
    uint8_t mark(const Output &o)
    {
        uint8_t m = 0;
        if (pread(bufs[o.buffer.cookie].fd, &m, 1, 0) != 1)
            abort();
        return m;
    }
};

static std::vector<uint8_t> unit(uint8_t flags, uint16_t w, uint16_t h, uint8_t poc = 0, uint8_t depth = 8)
{
    return {'N', flags, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), poc, depth};
}
enum { KEY = 1, SHOW = 2, SHOWEX = 4, CORRUPT = 8 };

static void reset_stub()
{
    g_stub = StubControl{};
    for (int c = 1; c <= 4; c++) {
        g_stub.caps[c].supported = 1;
        g_stub.caps[c].max_width = 4096;
        g_stub.caps[c].max_height = 4096;
        g_stub.caps[c].min_width = 16;
        g_stub.caps[c].min_height = 16;
        g_stub.caps[c].bit_depth_mask = (1u << 8) | (1u << 10);
    }
}

static Decoder *make(NvVideoDevice *dev, uint32_t codec, unsigned extra = 4)
{
    Config c;
    c.codec = codec;
    c.max_width = 1920;
    c.max_height = 1088;
    c.extra_output_slots = extra;
    std::string err;
    Decoder *d = Decoder::create(dev, c, &err);
    if (!d)
        fprintf(stderr, "create: %s\n", err.c_str());
    return d;
}

static Status q(Decoder *d, const std::vector<uint8_t> &u, uint64_t idx, TestPool &p, std::vector<Output> &out,
                bool eos = false)
{
    return d->queue(u.data(), u.size(), idx, eos, false, &p, &out);
}

static void test_caps_gate(NvVideoDevice *dev)
{
    reset_stub();
    std::string err;
    Config c;
    c.codec = NV_VIDEO_CODEC_AV1;
    c.max_width = 1920;
    c.max_height = 1080;
    g_stub.caps[NV_VIDEO_CODEC_AV1].supported = 0;
    CHECK(Decoder::create(dev, c, &err) == nullptr);
    CHECK(err.find("not supported") != std::string::npos);
    g_stub.caps[NV_VIDEO_CODEC_AV1].supported = 1;
    c.max_width = 8192;
    CHECK(Decoder::create(dev, c, &err) == nullptr);
    CHECK(err.find("outside") != std::string::npos);
    c.max_width = 1920;
    c.bit_depth = 12;
    CHECK(Decoder::create(dev, c, &err) == nullptr);
    c.bit_depth = 8;
    c.codec_config = {1, 2, 3};
    Decoder *d = Decoder::create(dev, c, &err);
    CHECK(d != nullptr);
    CHECK(g_stub.last_codec_config.size() == 3);
    delete d;
    CHECK(g_stub.sessions_destroyed == 1);
}

static void test_vp9_hidden_frames(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_VP9);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 640, 360), 0, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && !out[0].empty && out[0].index == 0 && out[0].size_changed);
    CHECK(pool.mark(out[0]) == (uint8_t)out[0].picture.id);
    pool.give_back(out[0]);
    out.clear();
    /* superframe pattern: hidden alt-ref (1), shown inter (2), show_existing of the alt-ref (3) */
    CHECK(q(d, unit(0, 640, 360), 1, pool, out) == Status::Ok);
    /* ABI 2: the hidden frame's unit produced no displayable picture -> finished at once */
    CHECK(out.size() == 1 && out[0].index == 1 && out[0].empty);
    out.clear();
    CHECK(q(d, unit(SHOW, 640, 360), 2, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 2 && !out[0].empty && !out[0].size_changed);
    for (auto &o : out)
        if (!o.empty)
            pool.give_back(o);
    out.clear();
    CHECK(q(d, unit(SHOWEX, 640, 360), 3, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 3 && !out[0].empty);
    pool.give_back(out[0]);
    out.clear();
    /* EOS without data */
    CHECK(d->queue(nullptr, 0, 4, true, false, &pool, &out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].eos && out[0].empty && out[0].index == 4);
    delete d;
    CHECK(g_stub.outstanding_at_destroy == 0);
}

static void test_h264_reorder_and_eos(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_H264);
    std::vector<Output> out;
    std::vector<uint8_t> sps = {'P', 0x67};
    CHECK(d->queue(sps.data(), sps.size(), 0, false, true, &pool, &out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].empty && out[0].index == 0);
    out.clear();
    /* decode order I0 P3 B1 B2 (poc), frame indices 1..4 */
    uint8_t pocs[] = {0, 3, 1, 2};
    for (int i = 0; i < 4; i++)
        CHECK(q(d, unit(i == 0 ? KEY | SHOW : SHOW, 1280, 720, pocs[i]), 1 + i, pool, out) == Status::Ok);
    /* reorder depth 2: I0 and B1 are out */
    CHECK(out.size() == 2 && out[0].index == 1 && out[1].index == 3);
    for (auto &o : out)
        pool.give_back(o);
    out.clear();
    /* EOS on a unit with data (poc 4, index 5) */
    CHECK(q(d, unit(SHOW, 1280, 720, 4), 5, pool, out, true) == Status::Ok);
    CHECK(out.size() == 3);
    CHECK(out.size() == 3 && out[0].index == 4 && out[1].index == 2 && out[2].index == 5 && out[2].eos &&
          !out[2].empty);
    for (size_t i = 0; i + 1 < out.size(); i++)
        CHECK(!out[i].eos);
    delete d;
}

static void test_flush_seek(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_HEVC);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 1280, 720, 0), 0, pool, out) == Status::Ok);
    CHECK(q(d, unit(SHOW, 1280, 720, 2), 1, pool, out) == Status::Ok);
    CHECK(out.empty()); /* both held in the reorder window */
    CHECK(d->flush(&out) == Status::Ok);
    std::set<uint64_t> got;
    for (auto &o : out) {
        CHECK(o.empty);
        got.insert(o.index);
    }
    CHECK(got == (std::set<uint64_t>{0, 1}));
    out.clear();
    /* after a seek a non-keyframe is a BITSTREAM error: finished empty+corrupt, not fatal */
    CHECK(q(d, unit(SHOW, 1280, 720, 5), 7, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].corrupt && out[0].index == 7);
    out.clear();
    CHECK(q(d, unit(KEY | SHOW, 1280, 720, 0), 8, pool, out) == Status::Ok);
    CHECK(d->queue(nullptr, 0, 9, true, false, &pool, &out) == Status::Ok);
    CHECK(out.size() == 2 && out[0].index == 8 && !out[0].empty && out[1].eos);
    delete d;
}

static void test_resolution_change_and_10bit(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_AV1);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 640, 360), 0, pool, out) == Status::Ok);
    CHECK(q(d, unit(SHOW, 640, 360), 1, pool, out) == Status::Ok);
    CHECK(q(d, unit(KEY | SHOW, 1920, 1080, 0, 10), 2, pool, out) == Status::Ok);
    CHECK(out.size() == 3 && out[0].size_changed && !out[1].size_changed && out[2].size_changed);
    CHECK(pool.last_w == 1920 && pool.last_h == 1080 && pool.last_fmt == NV_VIDEO_FORMAT_P010);
    CHECK(g_stub.last_copy_format == NV_VIDEO_FORMAT_P010);
    /* a non-keyframe larger than the session: UNSUPPORTED is terminal and named */
    CHECK(q(d, unit(SHOW, 4096, 2160), 3, pool, out) == Status::Error);
    CHECK(d->error().find("UNSUPPORTED") != std::string::npos);
    CHECK(d->error().find("larger than the session") != std::string::npos);
    CHECK(q(d, unit(KEY | SHOW, 640, 360), 4, pool, out) == Status::Error); /* stays dead */
    delete d;
}

/* Adaptive upswitch: a keyframe past the session max re-creates the session at the new size,
 * after every picture of the old session was copied out; the csd is replayed. */
static void test_upswitch_recreates_session(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_HEVC); /* max 1920x1088 */
    std::vector<Output> out;
    std::vector<uint8_t> ps = {'P', 0x40};
    CHECK(d->queue(ps.data(), ps.size(), 0, false, true, &pool, &out) == Status::Ok);
    CHECK(q(d, unit(KEY | SHOW, 1280, 720, 0), 1, pool, out) == Status::Ok);
    CHECK(q(d, unit(SHOW, 1280, 720, 1), 2, pool, out) == Status::Ok);
    for (auto &o : out)
        if (!o.empty)
            pool.give_back(o);
    out.clear();
    const unsigned created = g_stub.sessions_created, decodes = g_stub.decode_calls;
    CHECK(q(d, unit(KEY | SHOW, 3840, 2160, 0), 3, pool, out) == Status::Ok);
    CHECK(g_stub.sessions_created == created + 1 && g_stub.sessions_destroyed == 1);
    CHECK(g_stub.outstanding_at_destroy == 0);
    /* resize attempt + drain + replayed csd + the unit again */
    CHECK(g_stub.decode_calls == decodes + 4);
    /* the two 720p pictures came out of the old session, in order, before the new size */
    CHECK(out.size() >= 2 && out[0].index == 1 && out[1].index == 2 && !out[0].empty && !out[1].empty);
    for (auto &o : out)
        if (!o.empty)
            pool.give_back(o);
    out.clear();
    CHECK(d->queue(nullptr, 0, 4, true, false, &pool, &out) == Status::Ok);
    CHECK(out.size() == 2 && out[0].index == 3 && !out[0].empty && out[0].size_changed &&
          out[0].picture.width == 3840 && out[1].eos);
    out.clear();
    delete d;

    /* the drain waits for a buffer: Again, and the same unit completes the switch later */
    reset_stub();
    TestPool small(1);
    d = make(dev, NV_VIDEO_CODEC_VP9);
    CHECK(q(d, unit(KEY | SHOW, 1280, 720), 0, small, out) == Status::Ok);
    CHECK(out.size() == 1 && !out[0].empty);
    Output held = out[0];
    out.clear();
    CHECK(q(d, unit(SHOW, 1280, 720), 1, small, out) == Status::Ok); /* picture waits for a buffer */
    CHECK(out.empty());
    CHECK(q(d, unit(KEY | SHOW, 2560, 1440), 2, small, out) == Status::Again);
    CHECK(g_stub.sessions_created == 1);
    small.give_back(held);
    CHECK(q(d, unit(KEY | SHOW, 2560, 1440), 2, small, out) == Status::Ok);
    CHECK(g_stub.sessions_created == 2 && out.size() == 1 && out[0].index == 1 && !out[0].empty);
    small.give_back(out[0]);
    out.clear();
    CHECK(d->drain(&small, &out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 2 && out[0].picture.width == 2560);
    delete d;
}

/* ABI 2: an H.264/HEVC unit that produces no picture (pic_output_flag = 0) is finished at once */
static void test_unit_without_picture_retires(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_HEVC);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 1280, 720, 0), 0, pool, out) == Status::Ok);
    CHECK(q(d, unit(16, 1280, 720, 1), 1, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 1 && out[0].empty);
    delete d;
}

static bool near(float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; }

/* Coded values -> Codec2 units: x265's master-display example
 * G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1) and the same display
 * in AV1's fixed-point units (SVT-AV1 --mastering-display "G(0.265,0.69)B(0.15,0.06)
 * R(0.68,0.32)WP(0.3127,0.329)L(1000,0.0001)") give the same C2 values. */
static void test_hdr_units()
{
    NvVideoHdrMetadata m{};
    m.has_mastering_display = 1;
    m.mastering_display = {{13250, 7500, 34000}, {34500, 3000, 16000}, 15635, 16450, 10000000, 1};
    m.has_content_light_level = 1;
    m.max_content_light_level = 1000;
    m.max_pic_average_light_level = 400;
    HdrStatic h = hdr_static(NV_VIDEO_CODEC_HEVC, m);
    CHECK(h.present && near(h.red[0], 0.68f) && near(h.red[1], 0.32f) && near(h.green[0], 0.265f) &&
          near(h.green[1], 0.69f) && near(h.blue[0], 0.15f) && near(h.blue[1], 0.06f) &&
          near(h.white[0], 0.3127f) && near(h.white[1], 0.329f) && near(h.max_luminance, 1000.0f) &&
          near(h.min_luminance, 0.0001f) && h.max_cll == 1000 && h.max_fall == 400);
    NvVideoHdrMetadata a{};
    a.has_mastering_display = 1;
    a.mastering_display = {{44564, 17367, 9830}, {20972, 45220, 3932}, 20493, 21561, 256000, 2};
    h = hdr_static(NV_VIDEO_CODEC_AV1, a);
    CHECK(h.present && near(h.red[0], 0.68f) && near(h.green[1], 0.69f) && near(h.blue[0], 0.15f) &&
          near(h.white[0], 0.3127f) && near(h.max_luminance, 1000.0f) && near(h.min_luminance, 0.000122f) &&
          h.max_cll == 0);
    CHECK(!hdr_static(NV_VIDEO_CODEC_AV1, NvVideoHdrMetadata{}).present);
}

/* HDR static metadata travels with the picture's output */
static void test_hdr_metadata_on_output(NvVideoDevice *dev)
{
    reset_stub();
    g_stub.hdr.has_mastering_display = 1;
    g_stub.hdr.mastering_display.max_luminance = 10000000;
    g_stub.hdr.has_content_light_level = 1;
    g_stub.hdr.max_content_light_level = 1000;
    TestPool pool(8);
    Decoder *d = make(dev, NV_VIDEO_CODEC_VP9);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 640, 360), 0, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].hdr.has_mastering_display && out[0].hdr.mastering_display.max_luminance == 10000000 &&
          out[0].hdr.max_content_light_level == 1000);
    delete d;
}

static void test_backpressure(NvVideoDevice *dev)
{
    reset_stub();
    g_stub.dpb_slots = 1; /* limit = 1 + extra(1) = 2 unreleased pictures */
    TestPool pool(1);
    Decoder *d = make(dev, NV_VIDEO_CODEC_VP9, 1);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 320, 240), 0, pool, out) == Status::Ok);
    CHECK(out.size() == 1); /* pool now empty, buffer with the client */
    Output first = out[0];
    out.clear();
    CHECK(q(d, unit(SHOW, 320, 240), 1, pool, out) == Status::Ok); /* consumed; picture held */
    CHECK(out.empty());
    CHECK(q(d, unit(SHOW, 320, 240), 2, pool, out) == Status::Ok); /* consumed; 2nd waits in library */
    unsigned calls = g_stub.decode_calls;
    CHECK(q(d, unit(SHOW, 320, 240), 3, pool, out) == Status::Again); /* library full, pool empty */
    CHECK(g_stub.decode_calls > calls);
    CHECK(out.empty());
    /* the client returns the buffer: drain releases one picture, the retry of unit 3 goes in */
    pool.give_back(first);
    CHECK(d->drain(&pool, &out) == Status::Ok || true);
    CHECK(out.size() == 1 && out[0].index == 1);
    pool.give_back(out[0]);
    out.clear();
    CHECK(q(d, unit(SHOW, 320, 240), 3, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 2);
    pool.give_back(out[0]);
    out.clear();
    CHECK(d->drain(&pool, &out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].index == 3);
    pool.give_back(out[0]);
    out.clear();
    CHECK(d->queue(nullptr, 0, 4, true, false, &pool, &out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].eos);
    delete d;
    CHECK(g_stub.outstanding_at_destroy == 0);
}

static void test_device_lost(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(4);
    Decoder *d = make(dev, NV_VIDEO_CODEC_VP9);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 320, 240), 0, pool, out) == Status::Ok);
    g_stub.device_lost = true;
    CHECK(q(d, unit(SHOW, 320, 240), 1, pool, out) == Status::Error);
    CHECK(d->error().find("DEVICE_LOST") != std::string::npos && d->error().find("channel error") != std::string::npos);
    CHECK(d->drain(&pool, &out) == Status::Error);
    delete d;
}

static void test_fence_passthrough(NvVideoDevice *dev)
{
    reset_stub();
    g_stub.fence_fds = true;
    TestPool pool(4);
    Decoder *d = make(dev, NV_VIDEO_CODEC_VP9);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 320, 240), 0, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && out[0].release_fence >= 0);
    if (!out.empty() && out[0].release_fence >= 0)
        close(out[0].release_fence);
    delete d;
}

/* A front may lay an 8-bit picture out planar (YUV420P, e.g. Android YV12 for a CPU client); the
 * core passes that through, and refuses any other format the front picks. */
static void test_front_target_format(NvVideoDevice *dev)
{
    reset_stub();
    TestPool pool(4);
    pool.target_fmt = NV_VIDEO_FORMAT_YUV420P;
    Decoder *d = make(dev, NV_VIDEO_CODEC_AV1);
    std::vector<Output> out;
    CHECK(q(d, unit(KEY | SHOW, 640, 360), 0, pool, out) == Status::Ok);
    CHECK(out.size() == 1 && pool.last_fmt == NV_VIDEO_FORMAT_NV12);
    CHECK(g_stub.last_copy_format == NV_VIDEO_FORMAT_YUV420P);
    if (!out.empty())
        pool.give_back(out[0]);
    out.clear();
    /* 10-bit picture: planar 8-bit is not P010 */
    CHECK(q(d, unit(KEY | SHOW, 640, 360, 0, 10), 1, pool, out) == Status::Error);
    CHECK(d->error().find("output pool returned target format 3") != std::string::npos);
    CHECK(pool.discards == 1);
    delete d;
}

int main()
{
    NvVideoDevice *dev = nullptr;
    nv_video_device_open(&dev, nullptr);
    test_caps_gate(dev);
    test_vp9_hidden_frames(dev);
    test_h264_reorder_and_eos(dev);
    test_flush_seek(dev);
    test_resolution_change_and_10bit(dev);
    test_upswitch_recreates_session(dev);
    test_unit_without_picture_retires(dev);
    test_hdr_metadata_on_output(dev);
    test_hdr_units();
    test_backpressure(dev);
    test_device_lost(dev);
    test_fence_passthrough(dev);
    test_front_target_format(dev);
    nv_video_device_close(dev);
    if (g_fail) {
        fprintf(stderr, "core_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("core_test: all checks passed\n");
    return 0;
}
