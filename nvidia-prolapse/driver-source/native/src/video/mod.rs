//! nv_video (video/nv_video.h v1 entry points, v2 additions): NVDEC decode
//! sessions on the native RM backend. No NVIDIA userspace is loaded.
//!
//! Layering (see video/README.md for the codec plug-in contract):
//! - `channel`: one NVDEC GPFIFO channel + its completion timeline.
//! - `session`: codec-agnostic resources (DPB surface pool, 4-slot descriptor /
//!   bitstream ring, display queue, picture refcounts) and `copy_out`.
//! - `CodecDecoder`: per-codec parse → descriptor/method builder (`vp9`, `hevc`;
//!   AV1 / H.264 plug in the same way).
//! - `ffi`: the C ABI.
//!
//! Stock behaviour this follows was observed in libnvcuvid 615.71.09 (RM
//! traces, disassembly); ELF addresses are cited, no code is copied.
pub mod av1;
pub mod bits;
pub mod channel;
pub mod ffi;
pub mod h264;
pub mod hdr;
pub mod hevc;
#[cfg(test)]
pub mod pretend;
pub mod session;
pub mod vp9;

use crate::rm::{self, Client};
use std::rc::Rc;

/// ABI version reported in [`NvVideoDeviceInfo`]. v2 adds `nv_video_picture_hdr`
/// (HDR static metadata), the "displayable pictures this unit produced" return of
/// `nv_video_decode`, and `NV_VIDEO_E_RESIZE` + `nv_video_required_size` (a random
/// access point that needs a session of another size); every v1 entry point keeps its
/// v1 signature and layout.
pub const ABI_VERSION: u32 = 2;
pub const CODEC_H264: u32 = 1;
pub const CODEC_HEVC: u32 = 2;
pub const CODEC_VP9: u32 = 3;
pub const CODEC_AV1: u32 = 4;
pub const FORMAT_NV12: u32 = 1;
pub const FORMAT_P010: u32 = 2;
pub const FORMAT_YUV420P: u32 = 3;

/// nv_video error classes (NvVideoError). Every variant carries the reason
/// that nv_video_last_error() reports.
#[derive(Debug)]
pub enum VideoError {
    Invalid(String),
    Unsupported(String),
    Bitstream(String),
    NoMemory(String),
    DeviceLost(String),
    Again(String),
    /// A random access point (key frame / IDR) that this session cannot decode at its
    /// size: the caller drains the session, creates one of at least `width` x `height`
    /// and submits the same unit to it (NV_VIDEO_E_RESIZE).
    Resize {
        width: u32,
        height: u32,
        reason: String,
    },
}

impl VideoError {
    pub fn code(&self) -> i32 {
        match self {
            Self::Invalid(_) => -1,
            Self::Unsupported(_) => -2,
            Self::Bitstream(_) => -3,
            Self::NoMemory(_) => -4,
            Self::DeviceLost(_) => -5,
            Self::Again(_) => -6,
            Self::Resize { .. } => -7,
        }
    }
    pub fn message(&self) -> &str {
        match self {
            Self::Invalid(m)
            | Self::Unsupported(m)
            | Self::Bitstream(m)
            | Self::NoMemory(m)
            | Self::DeviceLost(m)
            | Self::Again(m)
            | Self::Resize { reason: m, .. } => m,
        }
    }
}

impl std::fmt::Display for VideoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message())
    }
}

/// RM/channel failures inside a session are device-level: the session must be
/// destroyed (NV_VIDEO_E_DEVICE_LOST).
impl From<rm::Error> for VideoError {
    fn from(e: rm::Error) -> Self {
        Self::DeviceLost(e.to_string())
    }
}

pub type VResult<T> = std::result::Result<T, VideoError>;

/// A picture larger than the session's maximum. At a random access point that starts a
/// new reference chain (H.264 IDR, HEVC IRAP, VP9 key frame, AV1 shown key frame) a
/// session of the new size decodes the stream from there on, so the caller re-creates
/// the session ([`VideoError::Resize`]) the way FFmpeg re-creates its hardware decoder
/// when the coded size changes (libavcodec/decode.c:1227 `ff_hwaccel_uninit` in
/// `ff_get_format`, reached from h264_slice.c:920, hevc/hevcdec.c:725, vp9.c:254,
/// av1dec.c:664). Anywhere else the references live in this session: refused.
pub(crate) fn too_large(
    codec: &str,
    width: u32,
    height: u32,
    max: (u32, u32),
    random_access: bool,
) -> VideoError {
    let what = format!(
        "{codec} picture {width}x{height} larger than the session max {}x{}",
        max.0, max.1
    );
    if random_access {
        VideoError::Resize {
            width,
            height,
            reason: format!("{what}: re-create the session at the new size"),
        }
    } else {
        VideoError::Unsupported(format!(
            "{what} on a picture that is not a random access point (references stay in this \
             session)"
        ))
    }
}

pub(crate) fn unsupported<T>(m: impl Into<String>) -> VResult<T> {
    Err(VideoError::Unsupported(m.into()))
}
pub(crate) fn bitstream<T>(m: impl Into<String>) -> VResult<T> {
    Err(VideoError::Bitstream(m.into()))
}

/// NVDEC classes, newest first: (class, chip family, stock capability bit, tested on hardware by this
/// library). The bit is the generation bit stock libnvcuvid 615.71.09 sets in its decoder capability word
/// for that class (ELF 0x30970); every per-generation branch below tests that word with the masks of [`NvdecGen`]. Classes
/// with bit 0 (Volta and older) decode nothing in stock and are refused here too. CFB0 (GB202, local + rental),
/// C9B0 (AD107), C7B0 (GA106) and C4B0 (TU116) ran this library's decoder bit-exact against stock and FFmpeg
/// (rental run 20261015-nvdec-xgen3); the rest are refused unless `NV_VIDEO_UNTESTED_CLASSES=1`
/// ([`untested_classes_allowed`]).
pub const NVDEC_CLASSES: [(u32, &str, u32, bool); 11] = [
    (0xd2b0, "Rubin GR10x", 0x0200_0000, false),
    (0xceb0, "Blackwell GB10B/GB10C", 0x0100_0000, false),
    (0xcfb0, "Blackwell GB20x", 0x0080_0000, true),
    (0xd1b0, "Blackwell GB20B/GB20C", 0x0040_0000, false),
    (
        0xcdb0,
        "Blackwell GB10x/GB11x (data centre)",
        0x0020_0000,
        false,
    ),
    (0xc9b0, "Ada AD10x", 0x0010_0000, true),
    (0xc7b0, "Ampere GA10x", 0x0008_0000, true),
    (0xb8b0, "Hopper GH100", 0x0004_0000, false),
    (0xc6b0, "Ampere GA100", 0x0002_0000, false),
    (0xc4b0, "Turing", 0x0001_0000, true),
    (0xc3b0, "Volta", 0, false),
];

/// Per-generation gates of one NVDEC class: the stock capability bit of [`NVDEC_CLASSES`] tested with the
/// masks stock's decode paths use.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NvdecGen {
    pub class: u32,
    bit: u32,
}

impl NvdecGen {
    pub fn of(class: u32) -> Self {
        let bit = NVDEC_CLASSES
            .iter()
            .find(|c| c.0 == class)
            .map_or(0, |c| c.2);
        Self { class, bit }
    }
    /// Mask 0x1fff0000: Turing and newer (stock supports nothing below).
    pub fn turing_plus(self) -> bool {
        self.bit & 0x1fff_0000 != 0
    }
    /// Mask 0x1ff80000: GA10x, Ada, Blackwell, Rubin (not Turing, GA100, Hopper): AV1, method 0x4c8.
    pub fn ga10x_plus(self) -> bool {
        self.bit & 0x1ff8_0000 != 0
    }
    /// Mask 0x1fe00000: Blackwell and Rubin: H.264 NEW_H264 (application id 8).
    pub fn blackwell(self) -> bool {
        self.bit & 0x1fe0_0000 != 0
    }
    /// Mask 0x1ec00000: D1B0, CFB0, D2B0 (not CDB0, CEB0): method 0x4dc, the plane GOB-kind and
    /// surface-format fields, width tails (VP9 +0x108, HEVC +0x11c/+0x120, AV1 +0x244), H.264 NEW ext fields.
    pub fn blackwell_b(self) -> bool {
        self.bit & 0x1ec0_0000 != 0
    }
    /// Mask 0x1ff00000: Ada C9B0 and newer (stock honours BSP capsTbl[3] only here, ELF 0x4fcef).
    pub fn ada_plus(self) -> bool {
        self.bit & 0x1ff0_0000 != 0
    }
    /// Bit 0x400000: D1B0 only (stock writes an extra setup word from ELF 0x3bc60 for
    /// H.264/HEVC/VP9 whose value this library does not reproduce, so those codecs are refused there).
    pub fn d1b0(self) -> bool {
        self.bit & 0x0040_0000 != 0
    }
}

/// Explicit verification opt-in `NV_VIDEO_UNTESTED_CLASSES=1`: NVDEC classes this library never ran on
/// hardware are accepted (cross-generation bit-exactness runs on rented GPUs). Announced once on stderr;
/// never set in production.
pub fn untested_classes_allowed() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| {
        let on = std::env::var("NV_VIDEO_UNTESTED_CLASSES").is_ok_and(|v| v == "1");
        if on {
            eprintln!(
                "nv_video: NV_VIDEO_UNTESTED_CLASSES=1: NVDEC classes never run by this library are \
                 enabled (verification only, outputs unverified)"
            );
        }
        on
    })
}

/// Append one buffer-address method the way stock's relocation emitter encodes it (ELF
/// 0x4a8a0 / 0x4acd0): a NONINC pair (`va >> 8`, `va >> 40`) when the host GPFIFO class
/// is newer than C86F (Blackwell) or the address needs more than 40 bits, else one INC dword `va >> 8`
/// (Turing/Ampere/Ada/Hopper; Ada stock traces: every buffer method INC count 1).
pub fn reloc(w: &mut Vec<u32>, host_class: u32, m: u32, va: u64) {
    if host_class > 0xc86f || va >> 40 != 0 {
        w.push(3 << 29 | 2 << 16 | 4 << 13 | m >> 2);
        w.extend_from_slice(&[(va >> 8) as u32, (va >> 40) as u32]);
    } else {
        crate::gpu::method(w, 4, m, &[(va >> 8) as u32]);
    }
}

/// Picture metadata (NvVideoPicture without id/timestamp).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PictureInfo {
    pub width: u32,
    pub height: u32,
    pub crop: [u32; 4],
    pub bit_depth: u32,
    pub color_primaries: u32,
    pub transfer: u32,
    pub matrix: u32,
    pub full_range: bool,
    pub keyframe: bool,
}

/// What `CodecDecoder::next_job` hands the session, in decode order.
pub enum Job {
    /// Decode one picture into a session-chosen surface.
    Decode(DecodeJob),
    /// Output an already decoded surface again (VP9 show_existing_frame, AV1
    /// show_existing_frame). `surface` is the session surface index.
    ShowExisting { surface: usize, info: PictureInfo },
}

pub struct DecodeJob {
    pub info: PictureInfo,
    /// Goes to the display queue once decoded (hidden frames: false).
    pub show: bool,
    /// Display order key for codecs with reordering (POC); `None` = output in
    /// decode order. The session releases pictures in key order once the
    /// codec reports them bumpable (see `CodecDecoder::bump_ready`).
    pub display_key: Option<i64>,
    /// Bytes to stage in the bitstream buffer (exactly what the engine reads).
    pub bitstream: Vec<u8>,
    /// Descriptor byte range the codec needs back after the GPU finished this
    /// picture (VP9 symbol counts). `Some` makes the session wait for this
    /// picture before the next `next_job` (a codec-semantic CPU wait).
    pub readback: Option<std::ops::Range<usize>>,
}

/// GPU addresses of one engine-layout surface.
#[derive(Clone, Copy, Debug)]
pub struct SurfaceRef {
    pub luma: u64,
    pub chroma: u64,
}

/// Engine-layout surface geometry, identical for every surface of a session
/// (sized from the session's max dimensions, so pitch never changes on a
/// resolution change).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SurfaceLayout {
    /// Bytes per row, multiple of 64 (one GOB).
    pub pitch: u32,
    /// Luma rows allocated (multiple of 16: 2-GOB blocks).
    pub luma_rows: u32,
    /// Chroma rows allocated (multiple of 16).
    pub chroma_rows: u32,
    pub chroma_offset: u64,
    pub size: usize,
    /// Bytes per luma sample (1 = 8-bit, 2 = 16-bit containers).
    pub bytes_per_sample: u32,
}

impl SurfaceLayout {
    pub fn new(max_width: u32, max_height: u32, bytes_per_sample: u32) -> Self {
        let pitch = (max_width * bytes_per_sample).next_multiple_of(64);
        let luma_rows = max_height.next_multiple_of(64);
        let chroma_rows = max_height.div_ceil(2).next_multiple_of(64);
        let chroma_offset = u64::from(pitch) * u64::from(luma_rows);
        let size = (chroma_offset as usize + pitch as usize * chroma_rows as usize)
            .next_multiple_of(1 << 16);
        Self {
            pitch,
            luma_rows,
            chroma_rows,
            chroma_offset,
            size,
            bytes_per_sample,
        }
    }
}

/// Surface-format code of the engine output (stock `surface_format_code_13bb30`, chroma 4:2:0):
/// 0 = 8-bit, 3 = 10-bit, 6 = 12-bit. Written to VP9 pic_s +0x43, HEVC +0xba, AV1 stat word bits 19.. .
pub fn surface_format_code(bit_depth: u32) -> u32 {
    match bit_depth {
        8 => 0,
        10 => 3,
        _ => 6,
    }
}

/// Copy-engine GOB kinds (clc9b5.h SET_*_BLOCK_SIZE.GOB_KIND: 0 = 32-bit / Turing layout, 1 = 8-bit,
/// 2 = 16-bit) of a Blackwell-GOB surface whose planes hold `bytes_per_sample` luma / twice that CbCr:
/// 1-byte luma / 2-byte CbCr = (1, 2), 2-byte luma / 4-byte CbCr = (2, 0).
pub fn blackwell_gob_kinds(bytes_per_sample: u32) -> [u32; 2] {
    if bytes_per_sample == 1 {
        [1, 2]
    } else {
        [2, 0]
    }
}

/// GOB kinds of the engine output. Stock writes the plane kinds into the picture setup (VP9 +0xeb, HEVC
/// +0x71, AV1 stat word bits 15..18, as `luma | chroma << 2`: 8-bit 4:2:0 = 9, 10-bit = 2) only on classes
/// with [`NvdecGen::blackwell_b`] (stock 615 traces on GB202 and Ada); every other class gets 0 there, which we take as the standard
/// (pre-Blackwell, GOB kind 0) block-linear layout. Measured on CFB0 locally and on C4B0/C7B0/C9B0
/// in the cross-generation run (copy_out bit-exact); the layout older engines write for
/// tileformat 1 is not established (stock untiles with CUDA), see [`output_layout_verified`].
pub fn engine_gob_kinds(decoder_class: u32, bytes_per_sample: u32) -> [u32; 2] {
    if NvdecGen::of(decoder_class).blackwell_b() {
        blackwell_gob_kinds(bytes_per_sample)
    } else {
        [0, 0]
    }
}

/// Whether the engine output layout copy_out assumes for this class was verified bit-exact
/// (copy of a decoded surface against a software decoder): CFB0 locally (2026-10-14) and
/// C9B0/C7B0/C4B0 in the cross-generation run 20261015-nvdec-xgen3 (31/31 bit-exact on each).
/// On these classes the pre-Blackwell standard block-linear output (GOB kind 0) and the
/// Blackwell GOB kinds 1/2 are measured, not assumed.
pub fn output_layout_verified(decoder_class: u32) -> bool {
    matches!(decoder_class, 0xcfb0 | 0xc9b0 | 0xc7b0 | 0xc4b0)
}

/// `luma | chroma << 2` of [`engine_gob_kinds`] as stock writes it into the picture setup of a
/// [`NvdecGen::blackwell_b`] class (callers write it only there).
pub fn plane_kind_bits(bytes_per_sample: u32) -> u32 {
    let [l, c] = blackwell_gob_kinds(bytes_per_sample);
    l | c << 2
}

/// Bytes per sample container of the engine output for a luma bit depth.
pub fn bytes_per_sample(bit_depth: u32) -> u32 {
    if bit_depth > 8 {
        2
    } else {
        1
    }
}

impl BuildContext<'_> {
    pub fn gen(&self) -> NvdecGen {
        NvdecGen::of(self.decoder_class)
    }
    /// One buffer-address method in this channel's relocation encoding.
    pub fn reloc(&self, w: &mut Vec<u32>, m: u32, va: u64) {
        reloc(w, self.host_class, m, va)
    }
}

/// Addresses the codec may use while building one picture.
pub struct BuildContext<'a> {
    pub decoder_class: u32,
    /// Host GPFIFO class of the NVDEC channel (selects the relocation encoding, [`reloc`]).
    pub host_class: u32,
    /// GPU address of this picture's descriptor slot (0x5000 bytes).
    pub descriptor: u64,
    pub bitstream: u64,
    /// Bytes staged at `bitstream` (the job's bitstream length).
    pub bitstream_len: usize,
    /// Monotonic picture counter (decoded pictures since session start).
    pub picture_index: u32,
    pub current: usize,
    pub surfaces: &'a [SurfaceRef],
    pub layout: SurfaceLayout,
}

/// GPU memory a codec allocates once per session (side buffers: colMV, seg
/// maps, filter buffers). Zeroed; VRAM; 256-byte aligned 40-bit VAs.
pub trait Allocator {
    fn vram(&mut self, size: usize) -> VResult<u64>;
    /// CPU write into a buffer returned by `vram` (offset relative to it).
    fn write(&mut self, va: u64, offset: usize, data: &[u8]) -> VResult<()>;
}

/// The codec plug-in contract (documented in video/README.md).
pub trait CodecDecoder {
    fn codec(&self) -> u32;
    /// Luma bit depth the codec was created for (the session's); a stream of another depth is
    /// `Unsupported` (surfaces are sized for one sample container).
    fn bit_depth(&self) -> u32;
    /// Engine surface layout for the session's max size (16-bit containers above 8 bits, stock
    /// `vp9_layout_13a8d0`: pitch align64(2w) for 16-bit formats).
    fn surface_layout(&self, max_width: u32, max_height: u32) -> SurfaceLayout {
        SurfaceLayout::new(max_width, max_height, bytes_per_sample(self.bit_depth()))
    }
    /// Copy-engine GOB kinds of the engine's output (luma, chroma) on
    /// Blackwell copy classes (clc9b5.h SET_SRC_BLOCK_SIZE_GOB_KIND: 0 = 32-bit
    /// /Turing layout, 1 = 8-bit, 2 = 16-bit). Ignored on older copy classes.
    fn gob_kinds(&self, decoder_class: u32) -> [u32; 2] {
        engine_gob_kinds(decoder_class, bytes_per_sample(self.bit_depth()))
    }
    /// Reference pictures the codec may hold (DPB size).
    fn dpb_size(&self) -> usize;
    /// Side buffers, once at session creation.
    fn allocate(
        &mut self,
        alloc: &mut dyn Allocator,
        max_width: u32,
        max_height: u32,
    ) -> VResult<()>;
    /// Upper bound of new surfaces `data` will need (for NV_VIDEO_E_AGAIN
    /// before any state changes).
    fn surfaces_needed(&self, data: &[u8]) -> VResult<usize>;
    /// Accept one nv_video_decode() unit; pictures come out of `next_job`.
    fn push_unit(&mut self, data: &[u8]) -> VResult<()>;
    /// Next picture of the pushed unit(s) in decode order. May parse lazily
    /// (VP9 superframes need the previous frame's adaptation first).
    fn next_job(&mut self) -> VResult<Option<Job>>;
    /// Write the descriptor slot (`desc`, 0x5000 bytes, pre-zeroed) and return
    /// the decoder-class methods (subchannel 4, ending in EXECUTE).
    fn build(&mut self, job: &DecodeJob, ctx: &BuildContext, desc: &mut [u8]) -> VResult<Vec<u32>>;
    /// After submission (and, when `job.readback` is set, after completion
    /// with the requested descriptor bytes): update reference state.
    fn finish(&mut self, job: &DecodeJob, current: usize, readback: Option<&[u8]>) -> VResult<()>;
    /// Surfaces the codec's DPB still references.
    fn referenced(&self) -> Vec<usize>;
    /// Reorder bound: true once the oldest pending display key may be output
    /// (codecs without reordering never set `display_key`).
    fn bump_ready(&self, _pending: usize) -> bool {
        true
    }
    /// Drop reference state (seek); the next unit must be a random access point.
    fn flush(&mut self);
    /// Most distinct surfaces `referenced()` can return between two pictures. `Some(n)` lets a unit
    /// with more pictures than free surfaces decode when `n` + 1 (the picture being decoded) + the
    /// surfaces of all pictures not yet released fit the pool: reference refreshes inside the unit then
    /// always free a surface in time (AV1 temporal units with several hidden frames). `None`: only the
    /// `surfaces_needed` check.
    fn max_referenced(&self) -> Option<usize> {
        None
    }
    /// Descriptor offset of the engine's `nvdec_status_s` (nvdec_drv.h: word 1 mbs_in_error, word 3
    /// error_status). `Some` makes the session check every completed picture and fail loudly.
    fn status_offset(&self) -> Option<usize> {
        None
    }
}

/// Why this library refuses `codec` on `class` although stock decodes it there, or `None`.
/// D1B0 (GB20B/C): stock writes an extra setup word from ELF 0x3bc60 for H.264, HEVC and VP9 (gates
/// at 0x3cd42 / 0x3e873); not implemented.
pub fn class_gap(codec: u32, class: u32) -> Option<&'static str> {
    let g = NvdecGen::of(class);
    (g.d1b0() && matches!(codec, CODEC_H264 | CODEC_HEVC | CODEC_VP9)).then_some(
        "NVDEC class 0xd1b0 needs the D1B0-only setup word stock writes from ELF 0x3bc60 (not implemented)",
    )
}

/// NV0080_CTRL_CMD_BSP_GET_CAPS_V2 (0x801c02, device object) parameters: one NVDEC instance's capsTbl.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct BspCapsV2 {
    pub caps_tbl: [u8; 8],
    pub instance_id: u32,
}

/// What stock does with one NVDEC engine's BSP capsTbl (ELF 0x4fd02; read by the per-engine predicates 0x2c360 / 0x2c470). The bits are reported by GSP per engine at
/// runtime, they are not a property of the NVDEC class; the class only decides whether capsTbl[3] counts
/// (C9B0 and newer). Unmeasured bit meanings beyond that use are not assumed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NvdecEngineCaps {
    pub vp8: bool,
    pub vp9_high_bit_depth: bool,
    pub hevc_off: bool,
    pub h264_off: bool,
    pub vp9_off: bool,
    pub av1_off: bool,
}

impl NvdecEngineCaps {
    /// Every codec on, VP9 high bit depth on (tests of the class gates alone).
    pub const ALL: Self = Self {
        vp8: true,
        vp9_high_bit_depth: true,
        hevc_off: false,
        h264_off: false,
        vp9_off: false,
        av1_off: false,
    };

    pub fn from_caps_table(t: [u8; 8], class: u32) -> Self {
        let ada_plus = NvdecGen::of(class).ada_plus();
        let vp9_base = t[0] & 1 != 0;
        let vp9_off = !vp9_base || (ada_plus && t[3] & 2 != 0);
        Self {
            vp8: vp9_base && t[4] & 0x04 != 0,
            vp9_high_bit_depth: !vp9_off && t[4] & 0x10 != 0,
            hevc_off: t[1] & 1 != 0,
            h264_off: t[2] & 1 != 0,
            vp9_off,
            av1_off: ada_plus && t[3] & 1 != 0,
        }
    }

    /// Stock's per-engine codec predicate (gens.rs `engine_codec_2c360`) for the codecs this library has.
    pub fn decodes(&self, codec: u32) -> bool {
        match codec {
            CODEC_H264 => !self.h264_off,
            CODEC_HEVC => !self.hevc_off,
            CODEC_VP9 => !self.vp9_off,
            CODEC_AV1 => !self.av1_off,
            _ => false,
        }
    }
}

/// Static capabilities per codec and NVDEC class, as stock's decoder-caps query decides them (ELF
/// 0x2c520) intersected with what this library implements (8-bit H.264
/// 4:2:0 up to 4096², HEVC Main/Main 10, VP9 profiles 0/2, AV1 Main 8/10-bit). `engine` is the BSP caps of
/// the engine sessions submit to (NVDEC0): stock asks whether *any* present engine qualifies (0x2c420,
/// 0x2c470); this library drives NVDEC0 only, so it asks that engine. `supported` also needs a class this
/// library ran on hardware, or the `NV_VIDEO_UNTESTED_CLASSES=1` opt-in.
pub fn caps(codec: u32, class: u32, engine: &NvdecEngineCaps) -> NvVideoCaps {
    let g = NvdecGen::of(class);
    let tested = NVDEC_CLASSES
        .iter()
        .find(|c| c.0 == class)
        .is_some_and(|c| c.3 || untested_classes_allowed());
    let mut c = NvVideoCaps {
        codec,
        ..Default::default()
    };
    let mut stock = g.turing_plus() && class_gap(codec, class).is_none() && engine.decodes(codec);
    match codec {
        // Stock: 4:2:0, 128..8192; > 8 bits only on an engine whose BSP capsTbl[4] bit 4 is set (and VP9 not
        // vetoed by capsTbl[3] bit 1 on C9B0+): gens.rs `vp9_high_bit_depth_2c470`, ELF 0x2c927.
        CODEC_VP9 => {
            (c.min_width, c.min_height, c.max_width, c.max_height) = (128, 128, 8192, 8192);
            c.bit_depth_mask = 1 << 8;
            if engine.vp9_high_bit_depth {
                c.bit_depth_mask |= 1 << 10;
            }
            c.chroma_420_only = 1;
        }
        // Stock: Blackwell 4:2:0/4:2:2 8..10-bit up to 8192² (min 48x64), older 8-bit 4:2:0 up to 4096²
        // (min 48x16). This library: 8-bit 4:2:0 up to 4096² on every class (NEW_H264 on Blackwell, the
        // legacy setup elsewhere and for interlaced streams).
        CODEC_H264 => {
            let min_h = if g.blackwell() { 64 } else { 16 };
            (c.min_width, c.min_height, c.max_width, c.max_height) = (48, min_h, 4096, 4096);
            c.bit_depth_mask = 1 << 8;
            c.chroma_420_only = 1;
        }
        // Stock: 4:2:0 (and 4:4:4; 4:2:2 on Blackwell) 8..12-bit, 144..8192 on every Turing+ class.
        // This library: Main and Main 10 (4:2:0).
        CODEC_HEVC => {
            (c.min_width, c.min_height, c.max_width, c.max_height) = (144, 144, 8192, 8192);
            c.bit_depth_mask = 1 << 8 | 1 << 10;
            c.chroma_420_only = 1;
        }
        // Stock: GA10x and newer only (not Turing, GA100, Hopper), 4:2:0 8/10-bit, 128..8192.
        CODEC_AV1 => {
            stock &= g.ga10x_plus();
            (c.min_width, c.min_height, c.max_width, c.max_height) = (128, 128, 8192, 8192);
            c.bit_depth_mask = 1 << 8 | 1 << 10;
            c.chroma_420_only = 1;
        }
        _ => stock = false,
    }
    c.supported = u32::from(stock && tested);
    c
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct NvVideoCaps {
    pub codec: u32,
    pub supported: u32,
    pub max_width: u32,
    pub max_height: u32,
    pub min_width: u32,
    pub min_height: u32,
    pub bit_depth_mask: u32,
    pub chroma_420_only: u32,
    pub max_level: u32,
    pub reserved: [u32; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct NvVideoDeviceInfo {
    pub abi_version: u32,
    pub pci_vendor: u32,
    pub pci_device: u32,
    pub chipset: u32,
    pub nvdec_class: u32,
    pub nvdec_engines: u32,
    pub name: [u8; 64],
}

/// The device handle: PCI identity + NVDEC class only. Sessions open their
/// own RM client so that a session can live on any thread (RM objects are
/// thread-confined `Rc`s).
#[derive(Clone, Debug)]
pub struct VideoDevice {
    pub pci: [u32; 4],
    pub info: NvVideoDeviceInfo,
    /// BSP caps of NVDEC0, the engine sessions submit to, and the raw capsTbl they were read from.
    pub engine0: NvdecEngineCaps,
    pub engine0_caps_tbl: [u8; 8],
}

fn pci_string(p: [u32; 4]) -> String {
    rm::bdf_string(p)
}

/// Open the RM device of `pci` (or of NV_VIDEO_PCI, or else the first NVIDIA
/// GPU whose /dev/nvidiaN this process may open), through the RM backend's
/// selection (`Client::select_card`): inaccessible GPUs of a multi-GPU host
/// are skipped; a requested BDF is never substituted.
pub(crate) fn open_rm(pci: Option<[u32; 4]>) -> VResult<(Rc<rm::Device>, [u32; 4])> {
    let client = Client::open().map_err(|e| VideoError::Unsupported(format!("RM open: {e}")))?;
    let want = match pci {
        Some(p) => Some(p),
        None => match std::env::var("NV_VIDEO_PCI") {
            Ok(s) => Some(parse_pci(&s)?),
            Err(_) => None,
        },
    };
    let card = client
        .select_card(want)
        .map_err(|e| VideoError::Unsupported(format!("select NVIDIA GPU: {e}")))?;
    Ok((client.device(card)?, rm::card_bdf(&card)))
}

fn parse_pci(s: &str) -> VResult<[u32; 4]> {
    let bad = || VideoError::Invalid(format!("NV_VIDEO_PCI={s:?}: expected dddd:bb:dd.f"));
    let (dom, rest) = s.split_once(':').ok_or_else(bad)?;
    let (bus, rest) = rest.split_once(':').ok_or_else(bad)?;
    let (dev, fun) = rest.split_once('.').ok_or_else(bad)?;
    let h = |v: &str| u32::from_str_radix(v, 16).map_err(|_| bad());
    Ok([h(dom)?, h(bus)?, h(dev)?, h(fun)?])
}

impl VideoDevice {
    pub fn open() -> VResult<Self> {
        let (device, pci) = open_rm(None)?;
        let classes = device.classes()?;
        let nvdec_class = NVDEC_CLASSES
            .iter()
            .map(|c| c.0)
            .find(|c| classes.contains(c))
            .unwrap_or(0);
        let mut raw = [0u32; 17];
        // NV2080_CTRL_CMD_GPU_GET_NAME_STRING (ASCII flavour), as mesa.rs query_info.
        device
            .client
            .control(device.subdevice, 0x20800110, &mut raw)?;
        let mut name = [0u8; 64];
        for (i, w) in raw[1..].iter().enumerate() {
            name[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        name[63] = 0;
        let card_device = device.card.pci.device;
        let mut arch = [0u32; 4];
        // NV2080_CTRL_CMD_MC_GET_ARCH_INFO: architecture | implementation.
        device
            .client
            .control(device.subdevice, 0x20801701, &mut arch)?;
        // Stock reads every present engine's caps at device init and fails the device when the control
        // fails (gens.rs `engine_records_4fc88`, ELF 0x4fc88); sessions here use NVDEC0 only.
        let mut p = BspCapsV2::default();
        let engine0 = if nvdec_class != 0 {
            device
                .client
                .control(device.handle, 0x801c02, &mut p)
                .map_err(|e| {
                    VideoError::Unsupported(format!(
                        "NV0080_CTRL_CMD_BSP_GET_CAPS_V2 (NVDEC0) failed: {e}"
                    ))
                })?;
            NvdecEngineCaps::from_caps_table(p.caps_tbl, nvdec_class)
        } else {
            NvdecEngineCaps::default()
        };
        Ok(Self {
            pci,
            engine0,
            engine0_caps_tbl: p.caps_tbl,
            info: NvVideoDeviceInfo {
                abi_version: ABI_VERSION,
                pci_vendor: 0x10de,
                pci_device: u32::from(card_device),
                chipset: arch[0] | arch[1],
                nvdec_class,
                // Sessions drive NVDEC0 only (the stock 0x13/0x16 alternation is
                // optional).
                nvdec_engines: u32::from(nvdec_class != 0),
                name,
            },
        })
    }
    pub fn caps(&self, codec: u32) -> VResult<NvVideoCaps> {
        if !(CODEC_H264..=CODEC_AV1).contains(&codec) {
            return Err(VideoError::Invalid(format!("unknown codec {codec}")));
        }
        Ok(caps(codec, self.info.nvdec_class, &self.engine0))
    }
}

/// Open the nvidia-drm render node of `pci` (sync_file export, dma-buf import).
pub(crate) fn open_render_node(pci: [u32; 4]) -> VResult<std::fs::File> {
    let want = pci_string(pci);
    let entries = std::fs::read_dir("/sys/class/drm")
        .map_err(|e| VideoError::Unsupported(format!("/sys/class/drm: {e}")))?;
    for e in entries.flatten() {
        if !e.file_name().to_string_lossy().starts_with("renderD") {
            continue;
        }
        let Ok(dev) = std::fs::canonicalize(e.path().join("device")) else {
            continue;
        };
        if dev.file_name().is_some_and(|n| n.to_string_lossy() == want) {
            return std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open(std::path::Path::new("/dev/dri").join(e.file_name()))
                .map_err(|err| {
                    VideoError::Unsupported(format!("open render node of {want}: {err}"))
                });
        }
    }
    Err(VideoError::Unsupported(format!(
        "no DRM render node for NVIDIA GPU {want} (nvidia-drm loaded with modeset?)"
    )))
}

/// Test support: a dma-buf exported by nvidia-drm from our own VRAM
/// allocation, readable through the CPU, standing in for a gralloc buffer.
pub struct TestDmaBuf {
    pub fd: std::os::fd::OwnedFd,
    memory: Rc<rm::Memory>,
    _device: Rc<rm::Device>,
}

impl TestDmaBuf {
    pub fn new(dev: &VideoDevice, size: usize) -> VResult<Self> {
        use std::os::fd::AsFd;
        let (device, _) = open_rm(Some(dev.pci))?;
        let drm = open_render_node(dev.pci)?;
        let memory = device.allocate_vram(size.next_multiple_of(1 << 16))?;
        let fd = crate::external::export(&memory, drm.as_fd())?;
        Ok(Self {
            fd,
            memory,
            _device: device,
        })
    }
    pub fn read(&self, offset: usize, out: &mut [u8]) -> VResult<()> {
        self.memory.map()?.read_bytes(offset, out)?;
        Ok(())
    }
    pub fn fill(&self, byte: u8) -> VResult<()> {
        let v = vec![byte; self.memory.size];
        self.memory.map()?.write_bytes(0, &v)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 10-bit engine contract values against the stock 615 captures (hevc8/hevc10 352x288 strides
    /// 384 / 352; vp9p0/vp9p2 and av1aom8/av1aom10 640x360 plane bits 9 / 2, format code 0 / 3).
    #[test]
    fn high_bit_depth_contract() {
        let l8 = SurfaceLayout::new(352, 288, bytes_per_sample(8));
        let l10 = SurfaceLayout::new(352, 288, bytes_per_sample(10));
        assert_eq!(l8.pitch / l8.bytes_per_sample, 384);
        assert_eq!(l10.pitch / l10.bytes_per_sample, 352);
        assert_eq!(SurfaceLayout::new(640, 360, 2).pitch / 16, 0x50); // AV1 stride field
        assert_eq!((plane_kind_bits(1), plane_kind_bits(2)), (9, 2));
        assert_eq!((surface_format_code(8), surface_format_code(10)), (0, 3));
        assert_eq!(engine_gob_kinds(0xcfb0, 2), [2, 0]);
        assert_eq!(engine_gob_kinds(0xc9b0, 2), [0, 0]);
        // AV1 stat word as stock wrote it (+0x238): 0x48000 for 8-bit, 0x190000 for 10-bit.
        let w = |bd| plane_kind_bits(bytes_per_sample(bd)) << 15 | surface_format_code(bd) << 19;
        assert_eq!((w(8), w(10)), (0x48000, 0x190000));
    }

    /// Live: NVDEC0's BSP capsTbl on this host and what it enables (`cargo test -- --ignored engine0`).
    #[test]
    #[ignore = "needs an NVIDIA GPU"]
    fn engine0_bsp_caps_live() {
        let d = VideoDevice::open().unwrap();
        println!(
            "class {:#x} capsTbl {:02x?} -> {:?}",
            d.info.nvdec_class, d.engine0_caps_tbl, d.engine0
        );
    }

    /// The copy_out layout warning fires only on classes whose engine output was never
    /// measured: the four classes of the decoding runs (local CFB0, cross-generation
    /// C9B0/C7B0/C4B0) do not warn, every other class does.
    #[test]
    fn output_layout_warning_matches_measured_classes() {
        for c in [0xcfb0, 0xc9b0, 0xc7b0, 0xc4b0] {
            assert!(output_layout_verified(c), "{c:#x}");
        }
        for c in [0xd2b0, 0xceb0, 0xd1b0, 0xcdb0, 0xb8b0, 0xc6b0, 0xc3b0] {
            assert!(!output_layout_verified(c), "{c:#x}");
        }
    }

    #[test]
    fn caps_bit_depths() {
        let m = |codec, class| caps(codec, class, &NvdecEngineCaps::ALL).bit_depth_mask;
        assert_eq!(m(CODEC_HEVC, 0xcfb0), 1 << 8 | 1 << 10);
        assert_eq!(m(CODEC_VP9, 0xcfb0), 1 << 8 | 1 << 10);
        assert_eq!(m(CODEC_AV1, 0xcfb0), 1 << 8 | 1 << 10);
        assert_eq!(m(CODEC_H264, 0xcfb0), 1 << 8);
    }
}
