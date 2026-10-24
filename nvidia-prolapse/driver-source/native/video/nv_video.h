/* SPDX-License-Identifier: MIT
 * nv_video — hardware video decode on NVIDIA NVDEC through the Rust RM backend
 * (libnv_bionic_native / its GNU build). No NVIDIA userspace is loaded.
 *
 * Model
 * - The library parses the bitstream itself (our Rust parsers) and owns every
 *   engine-side resource: DPB surfaces (VRAM, engine-native block-linear layout),
 *   per-codec side buffers (colMV, segment maps, CDF/probability tables, filter
 *   buffers), descriptor rings. Callers never see the engine layout.
 * - Decoded pictures leave the library only through nv_video_copy_out(), a
 *   copy-engine conversion into a caller buffer (dma-buf, any supported layout).
 *   The copy is GPU work; its completion is a sync_file fd. No CPU untile.
 * - Output order is display order: hidden/alt-ref frames (VP9/AV1) never appear,
 *   show_existing_frame and B-frame reordering (H.264/HEVC) are handled inside.
 *
 * Threading: a session is single-threaded (caller serialises); different
 * sessions and the device may be used from different threads.
 * Errors: functions return 0 or a negative NV_VIDEO_E_*; nv_video_last_error()
 * gives a per-thread human-readable reason (also written to stderr / logcat tag
 * "nv-video"). Nothing falls back silently: an unsupported stream feature is a
 * NV_VIDEO_E_UNSUPPORTED with its name, never a wrong picture.
 */
#ifndef NV_VIDEO_H
#define NV_VIDEO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI 2 adds (all v1 entry points keep their v1 signature and struct layout):
 *  - nv_video_decode() returns the number of displayable pictures the unit produced, so
 *    success is rc >= 0 (a caller that tests rc != 0 treats every picture as an error);
 *  - NV_VIDEO_E_RESIZE + nv_video_required_size(): a random access point larger than the
 *    session (adaptive streaming), decoded by a new session;
 *  - struct NvVideoHdrMetadata / nv_video_picture_hdr() for HDR static metadata. */
#define NV_VIDEO_ABI_VERSION 2u

enum NvVideoError {
   NV_VIDEO_OK = 0,
   NV_VIDEO_E_INVALID = -1,      /* bad argument / call order */
   NV_VIDEO_E_UNSUPPORTED = -2,  /* codec, profile, size or stream feature not supported here */
   NV_VIDEO_E_BITSTREAM = -3,    /* malformed or truncated bitstream; session stays usable after a keyframe */
   NV_VIDEO_E_NO_MEMORY = -4,
   NV_VIDEO_E_DEVICE_LOST = -5,  /* channel error / engine fault; session must be destroyed */
   NV_VIDEO_E_AGAIN = -6,        /* no free DPB/output slot: release pictures, then retry */
   NV_VIDEO_E_RESIZE = -7,       /* ABI 2: the unit starts a stream section this session cannot
                                  * hold (see nv_video_required_size) */
};

enum NvVideoCodec {
   NV_VIDEO_CODEC_H264 = 1,
   NV_VIDEO_CODEC_HEVC = 2,
   NV_VIDEO_CODEC_VP9 = 3,
   NV_VIDEO_CODEC_AV1 = 4,
};

/* Bitstream framing per codec (one nv_video_decode() call = one unit):
 *   H264/HEVC: one access unit, Annex-B (start codes), parameter sets in-band or
 *              via nv_video_session_info.codec_config (Annex-B or avcC/hvcC).
 *   VP9:       one frame or one superframe (index parsed here).
 *   AV1:       one temporal unit (low-overhead OBU format, Section 5 of the spec);
 *              sequence header in-band or via codec_config (av1C).
 */

struct NvVideoCaps {
   uint32_t codec;              /* enum NvVideoCodec */
   uint32_t supported;          /* 0/1 for this GPU (NVDEC class + generation) */
   uint32_t max_width, max_height;
   uint32_t min_width, min_height;
   uint32_t bit_depth_mask;     /* bit n set => n-bit luma supported (8, 10, 12) */
   uint32_t chroma_420_only;    /* 1: only 4:2:0 */
   uint32_t max_level;          /* codec-specific level indication (e.g. H.264 level_idc*10), 0 = unspecified */
   uint32_t reserved[7];
};

struct NvVideoDeviceInfo {
   uint32_t abi_version;        /* NV_VIDEO_ABI_VERSION */
   uint32_t pci_vendor, pci_device, chipset;
   uint32_t nvdec_class;        /* e.g. 0xCFB0 */
   uint32_t nvdec_engines;      /* number of NVDEC engines usable */
   char name[64];
};

struct NvVideoDevice;
struct NvVideoSession;

/* Opens an NVIDIA GPU by PCI identity: NV_VIDEO_PCI="dddd:bb:dd.f" selects one
 * (an absent or inaccessible BDF fails, never substituted); otherwise the first
 * GPU RM reports whose /dev/nvidiaN this process may open (GPUs a container
 * does not grant are skipped). No accessible GPU -> UNSUPPORTED, reasons in
 * the message. */
int nv_video_device_open(struct NvVideoDevice **out, struct NvVideoDeviceInfo *info);
void nv_video_device_close(struct NvVideoDevice *dev);
int nv_video_query_caps(struct NvVideoDevice *dev, uint32_t codec, struct NvVideoCaps *caps);

struct NvVideoSessionInfo {
   uint32_t codec;              /* enum NvVideoCodec */
   uint32_t max_width, max_height; /* resources are sized for these; a larger random access
                                    * point -> NV_VIDEO_E_RESIZE, any other larger picture ->
                                    * NV_VIDEO_E_UNSUPPORTED */
   uint32_t bit_depth;          /* expected luma bit depth (8/10); stream changes within caps are handled */
   uint32_t extra_output_slots; /* decoded pictures the caller may hold (not yet released) beyond the DPB */
   const uint8_t *codec_config; /* optional out-of-band config (avcC/hvcC/av1C/Annex-B PS), may be NULL */
   size_t codec_config_len;
   uint32_t reserved[8];
};

int nv_video_session_create(struct NvVideoDevice *dev, const struct NvVideoSessionInfo *info,
                            struct NvVideoSession **out);
void nv_video_session_destroy(struct NvVideoSession *s);

/* A decoded picture ready for display (display order). Valid until
 * nv_video_picture_release(). */
struct NvVideoPicture {
   uint64_t id;                 /* opaque, for copy_out/release */
   uint64_t timestamp;          /* the timestamp passed with the unit that produced it */
   uint32_t width, height;      /* coded size of this picture */
   uint32_t crop_left, crop_top, crop_width, crop_height; /* display window */
   uint32_t bit_depth;          /* 8/10 */
   uint32_t color_primaries, transfer, matrix, full_range; /* ISO/IEC 23091-2 code points */
   uint32_t keyframe;           /* 1 for IDR/IRAP/key frames */
   uint32_t reserved[5];
};

/* HDR static metadata (ABI 2). The library reads it from the bitstream: H.264/HEVC SEI
 * mastering_display_colour_volume (137) / content_light_level_info (144), AV1 metadata OBUs
 * HDR_MDCV / HDR_CLL. Values and primary order are the coded ones of the stream's codec:
 *  - H.264/HEVC: chromaticities in units of 0.00002, luminances in units of 0.0001 cd/m2,
 *    primaries in SEI order (H.265 D.3.28: index 0 green, 1 blue, 2 red);
 *  - AV1: chromaticities 0.16 fixed point, max luminance 24.8 and min luminance 18.14 fixed
 *    point cd/m2, primaries red, green, blue (AV1 spec metadata_hdr_mdcv semantics).
 * CLL values are cd/m2 for every codec. A field group whose has_* flag is 0 carries no
 * values. An AV1 temporal unit with several frames reports the unit's metadata for each
 * picture it produces. Declarations persist: every picture after one carries it until the
 * stream declares new values or the session is flushed. */
struct NvVideoMasteringDisplay {
   uint16_t display_primaries_x[3];
   uint16_t display_primaries_y[3];
   uint16_t white_point_x, white_point_y;
   uint32_t max_luminance, min_luminance;
};

struct NvVideoHdrMetadata {
   uint32_t has_mastering_display;
   struct NvVideoMasteringDisplay mastering_display;
   uint32_t has_content_light_level;
   uint32_t max_content_light_level, max_pic_average_light_level;
   uint32_t reserved[6];
};

/* Submits one unit (see framing above). Decoding is asynchronous on the GPU;
 * pictures become available through nv_video_next_picture() in display order.
 * flags: NV_VIDEO_DECODE_END_OF_STREAM drains all remaining pictures.
 *
 * Returns 0 or more = the number of displayable pictures this unit produced (ABI 2):
 * 0 when the unit can never produce one (parameter sets only, a skipped RASL picture,
 * pic_output_flag = 0) and for a drain call, so a caller can retire the input as soon
 * as the call returns 0 instead of holding it until EOS/flush. The pictures themselves
 * still come out of nv_video_next_picture() in display order. */
#define NV_VIDEO_DECODE_END_OF_STREAM 0x1u
int nv_video_decode(struct NvVideoSession *s, const uint8_t *data, size_t len, uint64_t timestamp,
                    uint32_t flags);

/* After nv_video_decode() returned NV_VIDEO_E_RESIZE (ABI 2): 0 and the coded size the unit
 * needs. The unit was a random access point (H.264 IDR, HEVC IRAP, VP9 key frame, AV1 shown
 * key frame) that starts a new reference chain, so a new session decodes the stream from it:
 * drain this one (nv_video_decode with NV_VIDEO_DECODE_END_OF_STREAM and no data, then
 * nv_video_next_picture until 0), destroy it, create one with max_width/max_height at least
 * this size and submit the same unit (with any out-of-band parameter sets) to it. Until then
 * the session accepts only that drain. VP9 also returns it at a key frame that changes the
 * coded size after segment maps were written (a new session starts from zeroed maps).
 * <0 if no unit returned NV_VIDEO_E_RESIZE. */
int nv_video_required_size(struct NvVideoSession *s, uint32_t *width, uint32_t *height);

/* Returns 1 and fills *pic if a picture is ready (its decode may still be
 * running on the GPU: copy_out orders after it), 0 if none, <0 on error. */
int nv_video_next_picture(struct NvVideoSession *s, struct NvVideoPicture *pic);

/* Discard pending pictures and reset reference state (seek / flush). The next
 * unit must be a keyframe / random access point. */
int nv_video_flush(struct NvVideoSession *s);

enum NvVideoFormat {
   NV_VIDEO_FORMAT_NV12 = 1,    /* 8-bit Y plane + interleaved CbCr plane */
   NV_VIDEO_FORMAT_P010 = 2,    /* 16-bit containers, 10 MSBs */
   NV_VIDEO_FORMAT_YUV420P = 3, /* 8-bit Y, Cb and Cr planes (planar 4:2:0), LINEAR only; plane 1
                                 * is Cb, the Cr plane is cr_dmabuf_fd/cr_offset/cr_pitch (an
                                 * Android YV12 buffer passes its third plane as plane 1) */
};

/* Destination buffer of copy_out. Planes may share one dma-buf. modifier is a
 * DRM format modifier: DRM_FORMAT_MOD_LINEAR (0) or an NVIDIA block-linear
 * modifier (fourcc_mod_code(NVIDIA, ...)) that the copy engine can write.
 * Block-linear carries NV12 (1-byte samples) only: a P010 block-linear target is
 * refused with NV_VIDEO_E_UNSUPPORTED because the 10-bit block-linear write is not
 * verified on hardware; P010 is copied to LINEAR targets. */
struct NvVideoTarget {
   int dmabuf_fd[2];            /* plane 0 (Y), plane 1 (CbCr); [1] may equal [0] */
   uint64_t offset[2];
   uint32_t pitch[2];           /* bytes per row (LINEAR) — ignored for block-linear */
   uint64_t modifier;
   uint32_t format;             /* enum NvVideoFormat */
   uint32_t width, height;      /* buffer size; the picture's display window is written at (0,0) */
   int acquire_fence_fd;        /* -1 or a sync_file the copy must wait for (buffer free) */
   /* NV_VIDEO_FORMAT_YUV420P only (ignored otherwise): the Cr plane; may share a dma-buf */
   int cr_dmabuf_fd;
   uint32_t cr_pitch;
   uint64_t cr_offset;
   uint32_t reserved[2];
};

/* Copies picture `id` (display window, film grain applied for AV1) into
 * `target` on the copy engine. Returns 0 and a sync_file fd in *release_fence_fd
 * that signals when the copy is complete (-1 if already complete). The library
 * does not keep target fds open after the call returns. */
int nv_video_copy_out(struct NvVideoSession *s, uint64_t id, const struct NvVideoTarget *target,
                      int *release_fence_fd);

/* HDR static metadata of picture `id` (ABI 2): 0 and *metadata filled (flags 0 when the
 * stream declared none), <0 on an unknown id or invalid argument. The values belong to the
 * picture, not to the caller's buffer, and stay valid until nv_video_picture_release(). */
int nv_video_picture_hdr(struct NvVideoSession *s, uint64_t id,
                         struct NvVideoHdrMetadata *metadata);

/* The caller is done with picture `id` (copied out or dropped). */
int nv_video_picture_release(struct NvVideoSession *s, uint64_t id);

const char *nv_video_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* NV_VIDEO_H */
