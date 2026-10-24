//! VP9 profile 0 on the session layer. CPU parses headers and keeps the
//! probability contexts (`crate::vp9`), the engine decodes, its symbol counts
//! drive backward adaptation.
//!
//! Field derivations follow stock libnvcuvid 615.71.09: picture setup at ELF
//! 0x13f400 (bsd/flt above sizes, gptimer, word 0x108); method list and
//! seg/colMV slot rules at 0x141d50. Checked against FFmpeg/libvpx
//! (error-resilient sign bias, stride from surface pitch, scaled-reference
//! validity).
//!
//! Segment maps (libnvcuvid 615.78.08): the side buffer holding the two maps
//! and the colMV pair is zeroed once when the decoder is created, before the
//! first picture (ELF 0x3b7c0, a copy-engine fill of the whole buffer); here
//! the session zeroes every side buffer through its mapping at allocation,
//! before any submission. Per picture the pair is programmed READ = slot 0
//! (method 0x5c8), WRITE = slot 1 (0x5cc) and swapped after a picture whose
//! segmentation_update_map is set (ELF 0x3fbe1, swap at 0x437f3). Nothing is
//! cleared, copied or reselected at key, intra-only or error-resilient
//! pictures: the engine applies the VP9 reset (libvpx
//! `vp9_setup_past_independence`) from the picture flags (pic_s +0x68 bits 0,
//! 3 and 5). Engine work is ordered by submission, so the CPU never touches
//! the maps after creation.
use super::{
    BuildContext, CodecDecoder, DecodeJob, Job, NvdecGen, PictureInfo, VResult, VideoError,
    CODEC_VP9,
};
use crate::{
    gpu::method,
    nvdec_drv::{Pod, Vp9Dims, Vp9EntropyCounts, Vp9PicS},
    vp9::{split_superframe, tile_sizes, FrameHeader, Vp9State, KEY_FRAME},
};
use std::collections::VecDeque;

const TILES: usize = 0x200;
const PROBS: usize = 0x900;
const COUNTS: usize = 0x1800;
const STATUS: usize = 0x4c00;
// Filter buffer: INTRA_TOP (0x4c8) at 0, VP9 filter (0x5dc) at 0x10000, 0x4dc
// at 0xd5c800 (stock layout for max 8192x8192).
const FILTER_SIZE: usize = 0xe0_0000;
const FILTER_OFF: u64 = 0x1_0000;
const OFF_4DC: u64 = 0xd5_c800;
const SEG_SIZE: usize = 0x8_0000;
const COLMV_SIZE: usize = 0x100_0000;

fn bitstream_error(e: crate::rm::Error) -> VideoError {
    VideoError::Bitstream(format!("VP9: {e}"))
}

/// Per-job private data the session hands back in `build` / `finish`.
struct Pending {
    header: FrameHeader,
}

pub struct Vp9Decoder {
    st: Vp9State,
    frames: VecDeque<Vec<u8>>,
    current: Option<Pending>,
    seg: u64,
    colmv: u64,
    filter: u64,
    seg_flip: usize,
    mv_flip: usize,
    /// Coded size of the last picture that wrote a segment map into the side
    /// buffer (None: still the zeroed allocation). Survives `flush`, like the
    /// buffer contents.
    seg_extent: Option<(u32, u32)>,
    /// A coded size change on a picture that is not a key frame while a map is
    /// written: refused (see `next_job`).
    seg_extent_change: bool,
    prev_key: bool,
    prev_show: bool,
    last_decoded: Option<(u32, u32)>,
    max: (u32, u32),
    frame_len: usize,
    /// Session bit depth (8 = profile 0, 10 = profile 2); surfaces are sized for it.
    bit_depth: u32,
    /// Per-generation gates of the session's NVDEC class.
    gen: NvdecGen,
}

impl Vp9Decoder {
    pub fn new(decoder_class: u32, max_width: u32, max_height: u32, bit_depth: u32) -> Self {
        Self {
            st: Vp9State::default(),
            frames: VecDeque::new(),
            current: None,
            seg: 0,
            colmv: 0,
            filter: 0,
            seg_flip: 0,
            mv_flip: 0,
            seg_extent: None,
            seg_extent_change: false,
            prev_key: false,
            prev_show: false,
            last_decoded: None,
            max: (max_width, max_height),
            frame_len: 0,
            bit_depth,
            gen: NvdecGen::of(decoder_class),
        }
    }

    /// Profile from the first byte (frame_marker 2, profile_low, profile_high).
    fn profile(frame: &[u8]) -> VResult<u8> {
        let b = *frame
            .first()
            .ok_or_else(|| VideoError::Bitstream("VP9: empty frame".into()))?;
        if b >> 6 != 2 {
            return Err(VideoError::Bitstream("VP9: frame marker".into()));
        }
        Ok((b >> 5 & 1) | (b >> 4 & 1) << 1)
    }

    fn info(&self, h: &FrameHeader, w: u32, hh: u32) -> PictureInfo {
        // VP9 color_space -> ISO/IEC 23091-2 matrix (spec 7.2 color_space).
        let (cs, full) = self.st.color;
        let (prim, xfer, matrix) = match cs {
            1 => (6, 6, 6),  // BT.601
            2 => (1, 1, 1),  // BT.709
            3 => (6, 6, 6),  // SMPTE 170
            4 => (7, 7, 7),  // SMPTE 240
            5 => (9, 14, 9), // BT.2020
            _ => (2, 2, 2),  // unknown / reserved
        };
        PictureInfo {
            width: w,
            height: hh,
            crop: [0, 0, w, hh],
            bit_depth: self.st.bit_depth,
            color_primaries: prim,
            transfer: xfer,
            matrix,
            full_range: full,
            keyframe: h.frame_type == KEY_FRAME,
        }
    }

    /// nvdec_vp9_pic_s from a parsed frame (stock build_pic_13f400 rules).
    fn pic(&mut self, h: &FrameHeader, ctx: &BuildContext, stream_len: usize) -> VResult<Vp9PicS> {
        // Stride in samples: stock writes the surface pitch >> 1 for its 16-bit surface format
        // (build_pic_13f400 `shift = surface_format_1dc == 8`; vp9p2 trace: 640 for a 1280-byte pitch).
        let pitch = (ctx.layout.pitch / ctx.layout.bytes_per_sample) as u16;
        let dims = |w: u32, hh: u32| Vp9Dims {
            width: w as u16,
            height: hh as u16,
            // Stride = the real surface pitch (FFmpeg audit item 4).
            stride: [pitch; 2],
        };
        let key = h.frame_type == KEY_FRAME;
        let mbs = |v: u32| v.div_ceil(16);
        let mut p = Vp9PicS {
            stream_len: stream_len as u32,
            // gptimer: no cap read from the engine caps here (stock +0x65f8 is
            // the class's max, larger than any 8K value) -> uncapped formula.
            gptimer_timeout_value: stock_gptimer(mbs(h.width), mbs(h.height)),
            surface: 1, // tileformat 1, gob_height 0 (GOB_2) as stock
            ..Default::default()
        };
        let (bsd, flt) = stock_bsd_flt(h.height, h.log2_tile_cols);
        p.vp9_bsd_ctrl_offset = bsd;
        p.vp9_flt_above_offset = flt;
        // v1 bit_depth (stock +0xf0 = (bitDepthMinus8·2 + 0x10) & 0x1e) and the surface-format
        // code +0x43 (stock surface_format_code_13bb30(1, bit_depth): 0 / 3), vp9p2 trace.
        // Stock writes the format code (and +0xeb, +0x108 below) only on blackwell_b classes
        // (gens.rs DECODE_GATES 0x3f977; Ada vp9p2 trace: +0x43 = 0).
        p.v1_bits = self.bit_depth << 1;
        if self.gen.blackwell_b() {
            p.reserved1[2] = super::surface_format_code(self.bit_depth) as u8;
        }
        if !key {
            // Stock fills ref dims for every non-key frame, intra-only included.
            for i in 0..3 {
                let (w, hh) = self.st.slot_size(h.ref_frame_idx[i]);
                p.refs[i] = dims(w, hh);
            }
        }
        if !h.is_intra() {
            // Scaled-reference validity (libvpx vp9_scale.h valid_ref_frame_size;
            // vp9_decodeframe.c:1584-1590): every reference must satisfy it for
            // the engine, which has no per-ref scale fallback here.
            for i in 0..3 {
                let (rw, rh) = self.st.slot_size(h.ref_frame_idx[i]);
                if 2 * h.width < rw || 2 * h.height < rh || h.width > 16 * rw || h.height > 16 * rh
                {
                    return Err(VideoError::Unsupported(format!(
                        "VP9 reference {rw}x{rh} outside the 2x/16x scale range of {}x{}",
                        h.width, h.height
                    )));
                }
            }
        }
        p.current = dims(h.width, h.height);
        let res_change = !key && self.last_decoded.is_some_and(|s| s != (h.width, h.height));
        p.flags = key as u8
            | (self.prev_key as u8) << 1
            | (res_change as u8) << 2
            | (h.error_resilient as u8) << 3
            | (self.prev_show as u8) << 4
            | (h.intra_only as u8) << 5;
        // Sign bias / compound refs only when not key|error-resilient|intra-only
        // (stock flags & 0x29; libvpx zeroes sign bias there as well).
        if p.flags & 0x29 == 0 {
            p.ref_frame_sign_bias = h.sign_bias;
            let s = h.sign_bias;
            if s[2] != s[1] || s[3] != s[1] {
                p.comp_fixed_ref = h.comp_fixed_ref as i8;
                p.comp_var_ref = [h.comp_var_ref[0] as i8, h.comp_var_ref[1] as i8];
            }
        }
        p.loop_filter_level = h.filter_level as i8;
        p.loop_filter_sharpness = h.sharpness as i8;
        p.qp_y_ac = h.base_q_idx;
        p.qp_y_dc = h.delta_q_y_dc;
        p.qp_ch_ac = h.delta_q_uv_ac;
        p.qp_ch_dc = h.delta_q_uv_dc;
        p.lossless = h.lossless as i8;
        p.transform_mode = h.tx_mode as i8;
        p.allow_high_precision_mv = h.allow_high_precision_mv as i8;
        p.mcomp_filter_type = engine_interp_filter(h);
        p.comp_pred_mode = h.reference_mode as i8;
        p.log2_tile_columns = h.log2_tile_cols as i8;
        p.log2_tile_rows = h.log2_tile_rows as i8;
        let seg = &self.st.seg;
        p.segment_enabled = seg.enabled as u8;
        // FFmpeg vp9.c:733-738: update/temporal cleared when segmentation off.
        p.segment_map_update = (seg.enabled && seg.update_map) as u8;
        p.segment_map_temporal_update = (seg.enabled && seg.temporal_update) as u8;
        p.segment_feature_mode = seg.abs_delta as u8;
        for i in 0..8 {
            for j in 0..4 {
                p.segment_feature_enable[i][j] = seg.feature_enabled[i][j] as u8;
                p.segment_feature_data[i][j] = seg.feature_data[i][j];
            }
        }
        p.mode_ref_lf_enabled = h.mode_ref_delta_enabled as i8;
        p.mb_ref_lf_delta = self.st.ref_deltas;
        p.mb_mode_lf_delta = self.st.mode_deltas;
        // Stock +0xeb: plane GOB kinds `luma | chroma << 2` (stock `plane_bits_90`): 9 for 8-bit,
        // 2 for 10-bit (vp9p0 / vp9p2 traces), see `super::plane_kind_bits`; blackwell_b only.
        if self.gen.blackwell_b() {
            p.reserved5 = super::plane_kind_bits(ctx.layout.bytes_per_sample) as i8;
        }
        Ok(p)
    }
}

/// pic_s +0x79 mcomp_filter_type. The engine takes the bitstream literal
/// order (EIGHTTAP_SMOOTH 0, EIGHTTAP 1, EIGHTTAP_SHARP 2, BILINEAR 3,
/// SWITCHABLE 4): stock `build_pic_13f400` swaps 0<->1 of the CUVID value,
/// which FFmpeg nvdec_vp9.c:101 fills in libvpx order (EIGHTTAP 0,
/// SMOOTH 1), the order `h.interp_filter` uses. Stock writes 0 on every key
/// frame captured (7 of 7); intra frames have no inter prediction, so 0 there.
fn engine_interp_filter(h: &FrameHeader) -> i8 {
    if h.is_intra() {
        return 0;
    }
    let f = h.interp_filter;
    (if f < 2 { f ^ 1 } else { f }) as i8
}

/// stock `gptimer` (uncapped): max(mbs, 0x1fa4) * 10000.
fn stock_gptimer(mbw: u32, mbh: u32) -> u32 {
    (mbw * mbh).max(0x1fa4).saturating_mul(10000)
}

/// stock bsd/flt above sizes: a = align64(h), n = tile columns;
/// u = a (n == 1) else a*(n-1) with n := n-1; BSD = u*48 >> 8,
/// FltAbove = ((a/4)*n*16 + u*48) >> 8.
fn stock_bsd_flt(height: u32, log2_cols: u32) -> (u32, u32) {
    let a = height.next_multiple_of(64);
    let mut n = 1u32 << log2_cols;
    let mut u = a;
    if n != 1 {
        n -= 1;
        u = a * n;
    }
    ((u * 0x30) >> 8, ((a >> 2) * n * 0x10 + u * 0x30) >> 8)
}

/// pic_s +0x108 (outside the public struct), stock `ext_108`.
fn stock_ext_108(width: u32) -> u32 {
    (((2 * width + 0x7e) & 0x3ff80) + 0x1ff) >> 8
}

/// Whether the segment map pair swaps after this picture: stock sets its swap flag iff
/// pic_s +0x81 (segment_map_update) is non-zero, which `pic` writes as
/// `enabled && update_map`.
fn seg_swaps_after(seg: &crate::vp9::Segmentation) -> bool {
    seg.enabled && seg.update_map
}

impl CodecDecoder for Vp9Decoder {
    fn codec(&self) -> u32 {
        CODEC_VP9
    }
    fn bit_depth(&self) -> u32 {
        self.bit_depth
    }
    fn dpb_size(&self) -> usize {
        8
    }
    fn allocate(
        &mut self,
        alloc: &mut dyn super::Allocator,
        _max_width: u32,
        _max_height: u32,
    ) -> VResult<()> {
        // Sized for 8192x8192 like stock (ffmpeg's max). The allocator zeroes
        // each buffer before the session submits anything: stock zeroes the
        // whole seg + colMV side buffer once at create (ELF 0x3b7c0) and never
        // again.
        self.seg = alloc.vram(2 * SEG_SIZE)?;
        self.colmv = alloc.vram(2 * COLMV_SIZE)?;
        self.filter = alloc.vram(FILTER_SIZE)?;
        Ok(())
    }
    fn surfaces_needed(&self, data: &[u8]) -> VResult<usize> {
        let frames = split_superframe(data).map_err(bitstream_error)?;
        let mut n = 0;
        for f in frames {
            let profile = Self::profile(f)?;
            let session_profile = if self.bit_depth == 8 { 0 } else { 2 };
            if profile != session_profile {
                return Err(VideoError::Unsupported(format!(
                    "VP9 profile {profile} in a {}-bit session (profile 0 = 8-bit, profile 2 = 10-bit \
                     4:2:0; create the session with the stream's bit depth)",
                    self.bit_depth
                )));
            }
            // show_existing_frame bit follows the profile bits (profiles 0-2).
            if f[0] >> 3 & 1 == 0 {
                n += 1;
            }
        }
        Ok(n)
    }
    fn push_unit(&mut self, data: &[u8]) -> VResult<()> {
        for f in split_superframe(data).map_err(bitstream_error)? {
            self.frames.push_back(f.to_vec());
        }
        Ok(())
    }
    fn next_job(&mut self) -> VResult<Option<Job>> {
        let Some(frame) = self.frames.pop_front() else {
            return Ok(None);
        };
        if frame.len() == 1 && frame[0] == 0 {
            return self.next_job(); // libvpx skips 1-byte zero frames
        }
        let h = self.st.parse(&frame).map_err(bitstream_error)?;
        if self.st.bit_depth != self.bit_depth {
            return Err(VideoError::Unsupported(format!(
                "VP9 {}-bit frame in a {}-bit session",
                self.st.bit_depth, self.bit_depth
            )));
        }
        if h.show_existing_frame {
            let surface = self.st.slot_surface(h.frame_to_show).ok_or_else(|| {
                VideoError::Bitstream("VP9: show_existing of an empty slot".into())
            })?;
            let (w, hh) = self.st.slot_size(h.frame_to_show);
            let mut info = self.info(&h, w, hh);
            info.keyframe = false;
            return Ok(Some(Job::ShowExisting { surface, info }));
        }
        let key = h.frame_type == KEY_FRAME;
        if h.width > self.max.0 || h.height > self.max.1 {
            // A key frame refreshes every reference slot.
            return Err(super::too_large("VP9", h.width, h.height, self.max, key));
        }
        // A coded size change also changes the segment map extent: libvpx zeroes the maps
        // on every size change (vp9_alloccommon.c vp9_init_context_buffers via
        // resize_context_buffers). With no map written yet the side buffer is still the
        // zeroed allocation. Otherwise a key frame starts a fresh session (zeroed side
        // buffer, as FFmpeg re-creates its decoder at every VP9 size change, libavcodec/vp9.c:182-254), and any other picture is refused:
        // what the engine does with a written map of the old extent is not established.
        let extent_changed = self.seg_extent.is_some_and(|e| e != (h.width, h.height));
        self.seg_extent_change = extent_changed && !key;
        if extent_changed && key {
            return Err(VideoError::Resize {
                width: h.width,
                height: h.height,
                reason: format!(
                    "VP9 key frame changes the coded size to {}x{} after segment maps were \
                     written: a new session starts from zeroed maps",
                    h.width, h.height
                ),
            });
        }
        let info = self.info(&h, h.width, h.height);
        let tiles = frame
            .get(h.uncompressed_size + h.header_size..)
            .ok_or_else(|| VideoError::Bitstream("VP9: headers overrun frame".into()))?
            .to_vec();
        let readback = (!h.error_resilient && !h.frame_parallel)
            .then(|| COUNTS..COUNTS + std::mem::size_of::<Vp9EntropyCounts>());
        let show = h.show_frame;
        // stream_len is the whole frame (stock), the engine reads from the
        // first tile (frameTagSize + offsetToDctParts).
        let job = DecodeJob {
            info,
            show,
            display_key: None,
            bitstream: tiles,
            readback,
        };
        self.current = Some(Pending { header: h });
        self.frame_len = frame.len();
        Ok(Some(Job::Decode(job)))
    }
    fn build(&mut self, _job: &DecodeJob, ctx: &BuildContext, d: &mut [u8]) -> VResult<Vec<u32>> {
        let h = self
            .current
            .as_ref()
            .ok_or_else(|| VideoError::Invalid("VP9 build without a parsed frame".into()))?
            .header
            .clone();
        let pic = self.pic(&h, ctx, self.frame_len)?;
        d[..256].copy_from_slice(pic.as_bytes());
        if self.gen.blackwell_b() {
            d[0x108..0x10c].copy_from_slice(&stock_ext_108(h.width).to_le_bytes());
        }
        for (k, (tw, th)) in tile_sizes(&h).iter().enumerate() {
            d[TILES + k * 4..TILES + k * 4 + 2].copy_from_slice(&tw.to_le_bytes());
            d[TILES + k * 4 + 2..TILES + k * 4 + 4].copy_from_slice(&th.to_le_bytes());
        }
        let probs = self.st.entropy_table();
        d[PROBS..PROBS + probs.as_bytes().len()].copy_from_slice(probs.as_bytes());
        if self.seg_extent_change {
            return Err(VideoError::Unsupported(
                "VP9 coded size change on a non-key picture after segment maps were written \
                 (libvpx zeroes both maps at every size change): the engine's handling of a \
                 map written at the old extent is not established"
                    .into(),
            ));
        }
        let da = ctx.descriptor;
        let mut w = Vec::new();
        // Relocation encoding per host class (stock reloc_4a8a0, `super::reloc`).
        let r = |w: &mut Vec<u32>, m: u32, va: u64| ctx.reloc(w, m, va);
        let cur = ctx.current;
        let s = |i: usize| ctx.surfaces[i];
        method(&mut w, 4, 0x200, &[9]); // SET_APPLICATION_ID VP9
        method(&mut w, 4, 0x400, &[0x52059]); // SET_CONTROL_PARAMS as stock
        r(&mut w, 0x404, da);
        r(&mut w, 0x408, ctx.bitstream);
        for i in 0..3 {
            let si = if h.frame_type == KEY_FRAME {
                cur
            } else {
                self.st.slot_surface(h.ref_frame_idx[i]).unwrap_or(cur)
            };
            r(&mut w, 0x430 + 4 * i as u32, s(si).luma);
            r(&mut w, 0x474 + 4 * i as u32, s(si).chroma);
        }
        r(&mut w, 0x424, da + STATUS as u64);
        method(&mut w, 4, 0x40c, &[ctx.picture_index]); // PICTURE_INDEX
        r(&mut w, 0x5d0, da + TILES as u64);
        r(&mut w, 0x5c0, da + PROBS as u64);
        r(&mut w, 0x5c4, da + COUNTS as u64);
        let sega = |k: usize| self.seg + (k * SEG_SIZE) as u64;
        let mva = |k: usize| self.colmv + (k * COLMV_SIZE) as u64;
        r(&mut w, 0x5c8, sega(self.seg_flip));
        r(&mut w, 0x5cc, sega(1 - self.seg_flip));
        // colMV parity by picture counter (stock method 0x40c parity).
        r(&mut w, 0x5d8, mva(self.mv_flip));
        r(&mut w, 0x5d4, mva(1 - self.mv_flip));
        // Stock vp9_layout_13a8d0 / decode_picture_141d50: the 0x5dc region sits at 0x10000 and 0x4c8
        // (filter + 0) exists on GA10x+ only, else 0x5dc is at offset 0; 0x4dc on blackwell_b only.
        let g = self.gen;
        r(
            &mut w,
            0x5dc,
            self.filter + if g.ga10x_plus() { FILTER_OFF } else { 0 },
        );
        if g.ga10x_plus() {
            r(&mut w, 0x4c8, self.filter);
        }
        if g.blackwell_b() {
            r(&mut w, 0x4dc, self.filter + OFF_4DC);
        }
        r(&mut w, 0x43c, s(cur).luma);
        r(&mut w, 0x480, s(cur).chroma);
        method(&mut w, 4, 0x300, &[0]); // EXECUTE
        Ok(w)
    }
    fn finish(&mut self, _job: &DecodeJob, current: usize, readback: Option<&[u8]>) -> VResult<()> {
        let p = self
            .current
            .take()
            .ok_or_else(|| VideoError::Invalid("VP9 finish without a frame".into()))?;
        let h = p.header;
        let counts = readback.and_then(Vp9EntropyCounts::from_bytes);
        self.mv_flip ^= 1;
        // Stock swaps the read/write pair after a picture whose
        // segmentation_update_map is set (libnvcuvid ELF 0x3fbd7/0x3fbe1, the swap at
        // 0x437f3). The update flag only exists with segmentation enabled.
        if seg_swaps_after(&self.st.seg) {
            self.seg_flip ^= 1;
            self.seg_extent = Some((h.width, h.height));
        }
        self.prev_key = h.frame_type == KEY_FRAME;
        self.prev_show = h.show_frame;
        self.last_decoded = Some((h.width, h.height));
        self.st
            .finish_frame(&h, counts.as_ref(), current)
            .map_err(bitstream_error)
    }
    fn referenced(&self) -> Vec<usize> {
        self.st.referenced().collect()
    }
    fn flush(&mut self) {
        let max = self.max;
        *self = Self {
            seg: self.seg,
            colmv: self.colmv,
            filter: self.filter,
            // The maps keep their contents across a flush.
            seg_extent: self.seg_extent,
            ..Self::new(self.gen.class, max.0, max.1, self.bit_depth)
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn stock_derived_fields() {
        // Stock: 1080p gptimer 81.6e6, 4K 324e6; ext_108 widths.
        assert_eq!(stock_gptimer(120, 68), 81_600_000);
        assert_eq!(stock_gptimer(240, 135), 324_000_000);
        assert_eq!(stock_gptimer(40, 23), 0x1fa4 * 10000);
        for (w, v) in [(640, 6), (1920, 16), (1928, 17), (1000, 9), (3840, 31)] {
            assert_eq!(stock_ext_108(w), v, "width {w}");
        }
        // Single tile column: n = 1 -> 12 * sb_rows / 13 * sb_rows (item 2).
        assert_eq!(stock_bsd_flt(1080, 0), (12 * 17, 13 * 17));
        assert_eq!(
            stock_bsd_flt(2160, 2),
            ((3 * 2176 * 48) >> 8, (544 * 3 * 16 + 3 * 2176 * 48) >> 8)
        );
    }
    #[test]
    fn segment_map_pair_swaps_on_update_map_only() {
        use crate::vp9::Segmentation;
        let seg = |enabled, update_map, temporal_update| Segmentation {
            enabled,
            update_map,
            temporal_update,
            ..Default::default()
        };
        // Stock swaps after a picture with segment_map_update set, temporal or not.
        assert!(seg_swaps_after(&seg(true, true, false)));
        assert!(seg_swaps_after(&seg(true, true, true)));
        // Segmentation on without a map update reads the map in place: no swap
        // (libvpx would swap here; stock does not).
        assert!(!seg_swaps_after(&seg(true, false, false)));
        // Segmentation off: no map write, no swap.
        assert!(!seg_swaps_after(&seg(false, false, false)));
    }
    #[test]
    fn key_frame_past_the_session_max_asks_for_a_new_session() {
        let ivf = include_bytes!("testdata/vp9_160x144.ivf");
        let frames = crate::vp9::ivf_frames(ivf).unwrap();
        // The first frame is a 160x144 key frame: a 128x128 session cannot hold it, a new
        // session of the reported size can.
        let mut d = Vp9Decoder::new(0xcfb0, 128, 128, 8);
        d.push_unit(frames[0]).unwrap();
        match d.next_job() {
            Err(VideoError::Resize { width, height, .. }) => {
                assert_eq!((width, height), (160, 144))
            }
            Err(e) => panic!("expected Resize, got {e}"),
            Ok(_) => panic!("expected Resize, got a job"),
        }
        let mut d = Vp9Decoder::new(0xcfb0, 160, 144, 8);
        d.push_unit(frames[0]).unwrap();
        assert!(matches!(d.next_job(), Ok(Some(Job::Decode(_)))));
    }
    #[test]
    fn interp_filter_is_the_bitstream_literal() {
        // Stock pic_s +0x79 on vp9_1080 (gallery, libvpx two-pass alt-ref):
        // the 65/68/67/69-byte alt-ref overlay frames code
        // is_filter_switchable 0, literal 1 (EIGHTTAP) and stock writes 1;
        // writing the libvpx enum (0 = EIGHTTAP) made the engine use
        // EIGHTTAP_SMOOTH and broke bit-exactness from output frame 23 on.
        // Key frames: stock 0. Switchable: 4.
        let mut h = FrameHeader {
            frame_type: 1,
            ..Default::default()
        };
        for lit in 0..4u8 {
            h.interp_filter = crate::vp9::LITERAL_TO_FILTER[lit as usize];
            assert_eq!(engine_interp_filter(&h), lit as i8, "literal {lit}");
        }
        h.interp_filter = crate::vp9::SWITCHABLE;
        assert_eq!(engine_interp_filter(&h), 4);
        h.frame_type = KEY_FRAME;
        assert_eq!(engine_interp_filter(&h), 0);
    }
    #[test]
    fn profile_and_show_existing_bits() {
        assert_eq!(Vp9Decoder::profile(&[0x82]).unwrap(), 0);
        assert_eq!(Vp9Decoder::profile(&[0xa2]).unwrap(), 1);
        assert_eq!(Vp9Decoder::profile(&[0x92]).unwrap(), 2);
        assert!(Vp9Decoder::profile(&[0x02]).is_err());
        let d = Vp9Decoder::new(0xcfb0, 640, 480, 8);
        assert!(matches!(
            d.surfaces_needed(&[0x92, 0, 0]),
            Err(VideoError::Unsupported(_))
        ));
        // Profile 1 (4:4:4) is refused in every session: NVDEC's VP9 decode
        // supports profiles 0 and 2 only (the engine input is NV12/P010;
        // 4:2:0 is a session surface contract), so there is no 4:4:4 path to
        // size a surface for.
        assert!(matches!(
            d.surfaces_needed(&[0xa2, 0, 0]),
            Err(VideoError::Unsupported(_))
        ));
        assert_eq!(d.surfaces_needed(&[0x88]).unwrap(), 0); // show_existing
                                                            // Profile 2 (10-bit) only in a 10-bit session; profile 1 (4:4:4) never.
        let d10 = Vp9Decoder::new(0xcfb0, 640, 480, 10);
        assert_eq!(d10.surfaces_needed(&[0x92, 0, 0]).unwrap(), 1);
        assert_eq!(d10.surfaces_needed(&[0x98]).unwrap(), 0); // show_existing
        assert!(matches!(
            d10.surfaces_needed(&[0x82, 0, 0]),
            Err(VideoError::Unsupported(_))
        ));
        assert!(matches!(
            d10.surfaces_needed(&[0xa2, 0, 0]),
            Err(VideoError::Unsupported(_))
        ));
    }
}
