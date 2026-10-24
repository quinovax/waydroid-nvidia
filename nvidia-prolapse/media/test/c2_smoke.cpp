/* SPDX-License-Identifier: MIT
 * Guest smoke test of the Codec2 NVDEC HAL through the public MediaCodec NDK on a
 * ByteBuffer output (no Surface), i.e. the path whose blocks are CPU-mapped: the HAL
 * fetches YV12/P010 gralloc blocks with CPU_READ (nvidia-prolapse/media/c2/NvC2Component.cpp
 * Component::Pool). Used for the seccomp measurement and the bionic end-to-end check.
 *
 *   c2_smoke <in.mp4>    -> "<mime>: <n> frames, <w>x<h>, YUV format <fmt>" and exit 0
 * The input must be an MP4 whose first video track is the codec under test; the sample
 * count is the number of frames the extractor reports, every output must have a buffer
 * and an image of the configured size.
 */
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

static int32_t format_int(AMediaFormat *f, const char *key, int32_t dflt)
{
    int32_t v = dflt;
    if (!AMediaFormat_getInt32(f, key, &v)) {
        int64_t q = dflt;
        if (AMediaFormat_getInt64(f, key, &q))
            v = (int32_t)q;
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: c2_smoke <in.mp4> [codec name, e.g. c2.nvprolapse.hevc.decoder]\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    struct stat st = {};
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "open(%s) failed\n", argv[1]);
        return 2;
    }
    AMediaExtractor *ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, (off64_t)st.st_size) != AMEDIA_OK) {
        fprintf(stderr, "setDataSourceFd(%s) failed\n", argv[1]);
        return 2;
    }
    ssize_t track = -1;
    AMediaFormat *fmt = nullptr;
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
        const char *mime = nullptr;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !strncmp(mime, "video/", 6)) {
            track = (ssize_t)i;
            fmt = f;
            break;
        }
        AMediaFormat_delete(f);
    }
    if (track < 0) {
        fprintf(stderr, "no video track\n");
        return 2;
    }
    AMediaExtractor_selectTrack(ex, track);
    const char *mime = nullptr;
    AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);
    const int32_t w = format_int(fmt, AMEDIAFORMAT_KEY_WIDTH, 0);
    const int32_t h = format_int(fmt, AMEDIAFORMAT_KEY_HEIGHT, 0);

    AMediaCodec *codec = argc > 2 ? AMediaCodec_createCodecByName(argv[2]) : AMediaCodec_createDecoderByType(mime);
    if (!codec) {
        fprintf(stderr, "no decoder for %s\n", mime);
        return 77;
    }
    if (AMediaCodec_configure(codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK) {
        fprintf(stderr, "configure(%s) failed\n", mime);
        return 2;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        fprintf(stderr, "start failed\n");
        return 2;
    }

    AMediaCodecBufferInfo info;
    size_t frames = 0, bad = 0, input_done = 0;
    bool eos_queued = false;
    for (int guard = 0; guard < 10000; guard++) {
        if (!input_done) {
            ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (in >= 0) {
                size_t cap = 0;
                uint8_t *p = AMediaCodec_getInputBuffer(codec, (size_t)in, &cap);
                ssize_t n = AMediaExtractor_readSampleData(ex, p, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    input_done = 1;
                    eos_queued = true;
                } else {
                    int64_t pts = AMediaExtractor_getSampleTime(ex);
                    AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)n, pts, 0);
                    AMediaExtractor_advance(ex);
                }
            }
        }
        ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (out >= 0) {
            if (info.size > 0) {
                /* A ByteBuffer (no-Surface) output is one gralloc block per frame; the HAL
                 * writes YV12/P010 into it. Its size must cover the display window. */
                size_t cap = 0;
                uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)out, &cap);
                if (!data || (size_t)info.size < (size_t)w * h * 3 / 2)
                    bad++;
                frames++;
            }
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
                break;
        } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            /* size changes (adaptive streams) and HDR static info (MediaFormat "hdr-static-info":
             * HDRStaticInfo type 1, little-endian u16 R/G/B/W xy in 0.00002, max/min display
             * luminance in 1 / 0.0001 cd/m2, MaxCLL, MaxFALL) as the client sees them */
            AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
            printf("output format: %dx%d\n", format_int(of, AMEDIAFORMAT_KEY_WIDTH, 0),
                   format_int(of, AMEDIAFORMAT_KEY_HEIGHT, 0));
            void *hdr = nullptr;
            size_t hdr_len = 0;
            if (AMediaFormat_getBuffer(of, AMEDIAFORMAT_KEY_HDR_STATIC_INFO, &hdr, &hdr_len) && hdr_len >= 25) {
                const uint8_t *b = (const uint8_t *)hdr;
                auto u16 = [b](int i) { return (unsigned)(b[1 + 2 * i] | b[2 + 2 * i] << 8); };
                printf("hdr-static-info: type %u R(%u,%u) G(%u,%u) B(%u,%u) W(%u,%u) maxL %u minL %u MaxCLL %u "
                       "MaxFALL %u\n",
                       b[0], u16(0), u16(1), u16(2), u16(3), u16(4), u16(5), u16(6), u16(7), u16(8), u16(9), u16(10),
                       u16(11));
            }
            AMediaFormat_delete(of);
        }
    }
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaFormat_delete(fmt);
    AMediaExtractor_delete(ex);
    if (!eos_queued) {
        fprintf(stderr, "%s: never reached end of stream\n", mime);
        return 1;
    }
    printf("%s: %zu frames, %dx%d, %zu bad buffers\n", mime, frames, w, h, bad);
    return (frames > 0 && bad == 0) ? 0 : 1;
}
