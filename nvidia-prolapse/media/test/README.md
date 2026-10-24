# Real-GPU decode tests (Codec2 / nv_video)

The fast `make test` suite runs the decoder core against a scripted `nv_video` stub. The
tests here drive the real backend and the GPU and are opt-in.

`real_codec_test` links `nvc2::Decoder` against the GNU build of `libnv_bionic_native` and
decodes a stream on NVDEC. One invocation is one codec and one output layout:

```
make realtest CODEC=h264 STREAM=in.h264 REF=ref.nv12 W=1920 H=1080 MODE=nv12 \
     NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>"
```

* `CODEC` is `h264`, `hevc`, `vp9` or `av1`. H.264/HEVC input is Annex-B, one access unit
  per `nv_video_decode()` call (parameter sets in-band); VP9/AV1 input is IVF, one frame
  per call.
* `MODE` is `nv12` (default), `p010` (10-bit, 2-byte samples) or `yv12` (the planar
  Y/Cr/Cb target the Codec2 component builds for CPU/ByteBuffer clients, with CPU_READ).
* `REF` is the raw software-decoded reference (`ffmpeg -f rawvideo -pix_fmt nv12` or
  `p010le`); `-` together with `REAL_EXPECT=<file>` compares pinned FNV-1a hashes instead.
* The test requires `/dev/dma_heap/system` (root), returns every input unit exactly once,
  requires EOS last, and exits 77 when the GPU does not support the codec or bit depth.
  `REAL_EOS_ON_LAST=1` measures the alternative path that flags the last data unit as EOS.

Every codec and layout at once, with streams and FFmpeg references generated locally:

```
make realtest-all NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>"
```

Nine cases: H.264/HEVC/VP9/AV1 in NV12, H.264/HEVC/VP9 in YV12, HEVC Main10 and VP9
profile 2 in P010. Needs ffmpeg with libx264, libx265, libvpx-vp9 and libsvtav1.

The media gate runs the committed tiny streams with pinned per-frame hashes (no ffmpeg):

```
make smoke NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>" CODECS="h264 hevc vp9 av1"
```

`c2_smoke.cpp` is the guest-side client: it decodes an MP4 through the public MediaCodec
NDK on a ByteBuffer output (no Surface), i.e. through the shipped Codec2 service, the YV12
gralloc path and the CPU_READ pool flag. Build it against the NDK with `make c2-smoke`,
push it and the test MP4s to `/data/local/tmp`. `c2_smoke <in.mp4> [codec name]` prints every
output-format change (size, and `hdr-static-info` as the client receives it), so an adaptive
stream that switches size at a keyframe and an HDR stream's ST 2086 / CLL values can be checked
end to end. Run it while `strace -f -c` is attached
to `android.hardware.media.c2@1.2-service-nvprolapse` to re-measure the seccomp policy.
