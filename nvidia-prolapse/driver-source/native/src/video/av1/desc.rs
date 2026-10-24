//! NVDEC AV1 picture descriptor (`nvdec_av1_pic_s`, 572 bytes) and the side tables that follow it in the
//! per-picture descriptor block: tile info (+0x300), global motion (+0x1300), film grain (+0x1400).
//!
//! Field positions come from NVIDIA's MIT `nvdec_drv.h` (`nvdec_av1_pic_s`), computed with a C compiler
//! (bit offset, width). Values follow the AV1 spec semantics; where the value convention is NVDEC-specific
//! (constants, packing, enum remaps) it was matched against stock descriptors captured on an RTX 5090.

use super::header::{get_relative_dist, FrameHeader, SequenceHeader, TileRange, GM_IDENTITY};

pub const PIC_S_SIZE: usize = 572;
pub const DESC_TILE_INFO: usize = 0x300;
/// Word after `nvdec_av1_pic_s` (stock copies 0x300 bytes of its picture setup): the Blackwell 0x4dc
/// buffer size `(ALIGN_UP(w,64)/64 + 2) * 640` in 1280-byte units (6 at 640 wide, 16 at 1920; constant
/// per stream in every capture). Without it every inter frame reports error_status 0xd and decodes wrong.
pub const PIC_EXT_244: usize = 0x244;
pub const DESC_GLOBAL_MOTION: usize = 0x1300;
pub const DESC_FILM_GRAIN: usize = 0x1400;
pub const DESC_STATUS: usize = 0x4700;
pub const DESC_BLOCK_SIZE: usize = 0x5000;
pub const TILE_INFO_ENTRY: usize = 16;
pub const MAX_TILES: usize = 256;

/// (bit offset, width) inside `nvdec_av1_pic_s`.
type F = (usize, usize);
mod f {
    use super::F;
    pub const FG_APPLY: F = (512, 1);
    pub const FG_OVERLAP: F = (513, 1);
    pub const FG_CLIP: F = (514, 1);
    pub const FG_CSFL: F = (515, 1);
    pub const FG_NUM_Y_B: F = (516, 1);
    pub const FG_NUM_CB_B: F = (517, 1);
    pub const FG_NUM_CR_B: F = (518, 1);
    pub const FG_SCALING_SHIFT: F = (519, 4);
    pub const FG_SEED: F = (528, 16);
    pub const FG_CB_OFFSET: F = (544, 16);
    pub const FG_CR_OFFSET: F = (560, 16);
    pub const FG_CB_MULT: F = (576, 8);
    pub const FG_CB_LUMA_MULT: F = (584, 8);
    pub const FG_CR_MULT: F = (592, 8);
    pub const FG_CR_LUMA_MULT: F = (600, 8);
    pub const GPTIMER: F = (608, 32);
    pub const STREAM_LEN: F = (640, 32);
    pub const RESERVED12: F = (672, 32);
    pub const USE_128: F = (704, 1);
    pub const CHROMA_FORMAT: F = (705, 2);
    pub const BIT_DEPTH: F = (707, 4);
    pub const EN_FILTER_INTRA: F = (711, 1);
    pub const EN_INTRA_EDGE: F = (712, 1);
    pub const EN_INTERINTRA: F = (713, 1);
    pub const EN_MASKED: F = (714, 1);
    pub const EN_DUAL_FILTER: F = (715, 1);
    pub const EN_JNT: F = (720, 1);
    pub const EN_CDEF: F = (722, 1);
    pub const EN_FGS: F = (724, 1);
    pub const FRAME_TYPE: F = (736, 2);
    pub const SHOW_FRAME: F = (738, 1);
    pub const DISABLE_CDF_UPDATE: F = (740, 1);
    pub const SCREEN_CONTENT: F = (741, 1);
    pub const FORCE_INT_MV: F = (742, 1);
    pub const SCALE_DENOM_M9: F = (743, 3);
    pub const ALLOW_INTRABC: F = (746, 1);
    pub const ALLOW_HP_MV: F = (747, 1);
    pub const INTERP_FILTER: F = (748, 3);
    pub const SWITCHABLE_MOTION: F = (751, 1);
    pub const USE_REF_MVS: F = (752, 1);
    pub const REFRESH_CTX: F = (753, 1);
    pub const DQ_PRESENT: F = (754, 1);
    pub const DQ_RES: F = (755, 2);
    pub const DLF_PRESENT: F = (757, 1);
    pub const DLF_RES: F = (758, 2);
    pub const DLF_MULTI: F = (760, 1);
    pub const CODED_LOSSLESS: F = (762, 1);
    pub const TILE_ENABLED: F = (763, 1);
    pub const SUPERRES_SCALED: F = (766, 1);
    pub const TILE_COLS: F = (768, 8);
    pub const TILE_ROWS: F = (776, 8);
    pub const CTX_UPDATE_TILE: F = (784, 16);
    pub const CDEF_DAMPING_M3: F = (800, 2);
    pub const CDEF_BITS: F = (802, 2);
    pub const TX_MODE: F = (804, 3);
    pub const REF_MODE: F = (807, 2);
    pub const SKIP_MODE: F = (809, 1);
    pub const SKIP_REF0: F = (810, 4);
    pub const SKIP_REF1: F = (814, 4);
    pub const ALLOW_WARP: F = (818, 1);
    pub const REDUCED_TX: F = (819, 1);
    pub const REF_SCALING: F = (820, 1);
    pub const UPSCALED_W: F = (832, 16);
    pub const LUMA_STEP: F = (848, 16);
    pub const CHROMA_STEP: F = (864, 16);
    pub const LUMA_SUBPEL_X: F = (880, 16);
    pub const CHROMA_SUBPEL_X: F = (896, 16);
    pub const BASE_Q: F = (912, 8);
    pub const DQ_Y_DC: F = (920, 8);
    pub const DQ_U_DC: F = (928, 8);
    pub const DQ_V_DC: F = (936, 8);
    pub const DQ_U_AC: F = (944, 8);
    pub const DQ_V_AC: F = (952, 8);
    pub const QM_Y: F = (960, 8);
    pub const QM_U: F = (968, 8);
    pub const QM_V: F = (976, 8);
    pub const CDEF_Y_PRI: F = (992, 32);
    pub const CDEF_UV_PRI: F = (1024, 32);
    pub const CDEF_Y_SEC: F = (1056, 16);
    pub const CDEF_UV_SEC: F = (1072, 16);
    pub const SEG_ENABLED: F = (1088, 8);
    pub const SEG_UPDATE_MAP: F = (1096, 8);
    pub const SEG_TEMPORAL: F = (1112, 8);
    pub const SEG_DATA: usize = 1120; // short[8][8]
    pub const SEG_LAST_ACTIVE: F = (2144, 8);
    pub const SEG_PRESKIP: F = (2152, 8);
    pub const SEG_PREV_FLAG: F = (2160, 8);
    pub const SEG_QUANT_SIGN: F = (2168, 8);
    pub const LF_LEVEL0: F = (2176, 8);
    pub const LF_LEVEL1: F = (2184, 8);
    pub const LF_LEVEL_U: F = (2192, 8);
    pub const LF_LEVEL_V: F = (2200, 8);
    pub const LF_SHARPNESS: F = (2208, 8);
    pub const LF_REF_DELTAS: usize = 2216; // char[8]
    pub const LF_MODE_DELTAS: usize = 2280; // char[2]
    pub const LR_TYPE: F = (2296, 8);
    pub const LR_UNIT_SIZE: F = (2304, 8);
    pub const CUR_FRAME: usize = 2336; // frame_info_t
    pub const REF_FRAME: usize = 2432; // ref_frame_struct_s[7], 24 bytes each
    pub const REF_FRAME_BITS: usize = 192;
    pub const USE_TEMPORAL0: F = (3776, 1);
    pub const MF1_TYPE: F = (3779, 3);
    pub const MFMV_OFFSET: usize = 3808; // short[3][7]
    pub const MFMV_SIDE: usize = 4144; // char[3][7]
    pub const TILEFORMAT: F = (4312, 2);
    pub const GOB_HEIGHT: F = (4314, 3);
    pub const FLT_TOP: F = (4352, 32);
    pub const FLT_VERT: F = (4384, 32);
    pub const CDEF_VERT: F = (4416, 32);
    pub const LR_VERT: F = (4448, 32);
    pub const HUS_VERT: F = (4480, 32);
    pub const FGS_VERT: F = (4512, 32);
    pub const STAT_WORD: F = (4544, 32);
}

fn put(b: &mut [u8], (bit, width): F, v: u64) {
    let v = if width == 64 {
        v
    } else {
        v & ((1u64 << width) - 1)
    };
    for i in 0..width {
        let p = bit + i;
        if (v >> i) & 1 != 0 {
            b[p / 8] |= 1 << (p % 8);
        } else {
            b[p / 8] &= !(1 << (p % 8));
        }
    }
}

/// Byte offsets (from the filter buffer base, in 256-byte units in pic_s) of the filter neighbour buffers;
/// sizes from the `AV1_*_BUF_SIZE` macros of nvdec_drv.h.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FilterLayout {
    pub flt_vert: u64,
    pub cdef_vert: u64,
    pub hus_vert: u64,
    pub lr_vert: u64,
    pub fgs_vert: u64,
    /// Blackwell extra buffer (method 0x4dc) after the FGS-left buffer.
    pub extra_4dc: u64,
    /// Intra-top buffer (method 0x4c8) after that.
    pub intra_top: u64,
    pub total: u64,
}

const fn align(v: u64, a: u64) -> u64 {
    v.div_ceil(a) * a
}

pub const fn filter_layout(w: u32, h: u32) -> FilterLayout {
    let (w, h) = (w as u64, h as u64);
    let sbw = align(w, 64) / 64 + 2;
    let sbh = align(h, 64) / 64 + 2;
    let flt_vert = align(sbw * align(1920, 128), 256);
    let cdef_vert = flt_vert + align(sbh * align(1536, 128), 256);
    let hus_vert = cdef_vert + align(sbh * align(1792, 128), 256);
    let lr_vert = hus_vert + 2 * align(sbh * align(12544, 128), 256);
    let fgs_vert = lr_vert + align(sbh * align(1920, 128), 256);
    let extra_4dc = fgs_vert + align(sbh * align(320, 128), 256);
    let intra_top = extra_4dc + align(sbw * 640, 256);
    FilterLayout {
        flt_vert,
        cdef_vert,
        hus_vert,
        lr_vert,
        fgs_vert,
        extra_4dc,
        intra_top,
        total: intra_top + 0x10000,
    }
}

/// Per reference (LAST..ALTREF) geometry handed to the descriptor.
#[derive(Debug, Clone, Copy, Default)]
pub struct RefGeom {
    pub width: u32,
    pub height: u32,
    pub order_hint: u32,
}

/// Motion-field projection inputs computed by `mf::select`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct MotionField {
    pub use_temporal: [bool; 3],
    pub mf_type: [u8; 3],
    pub mfmv_offset: [[i16; 7]; 3],
    pub mfmv_side: [[i8; 7]; 3],
}

pub struct PicInputs<'a> {
    pub seq: &'a SequenceHeader,
    pub hdr: &'a FrameHeader,
    pub stream_len: u32,
    /// LAST..ALTREF (spec order); for intra frames the entries are ignored.
    pub refs: [RefGeom; 7],
    pub mf: MotionField,
    pub prev_segment_ids: bool,
    /// Film grain applied to a separate display surface.
    pub apply_grain: bool,
    /// Luma pitch of the decode surface in bytes.
    pub pitch: u32,
    /// The NVDEC class has the stock `blackwell_b` gate (D1B0/CFB0/D2B0): only then stock writes the stat
    /// word's plane kinds / format code (and the `PIC_EXT_244` word; gens.rs DECODE_GATES 0x40977; Ada
    /// av1 8/10-bit traces: +0x238 = 0, +0x244 = 0).
    pub blackwell_b: bool,
}

/// gptimer_timeout_value = max(frame size in 16x16 macroblocks, 8100) * 10000 (stock rule shared by all
/// codecs; 81,000,000 at 640x360, 81,600,000 at 1920x1080).
fn gptimer(w: u32, h: u32) -> u64 {
    ((w.div_ceil(16) * h.div_ceil(16)).max(8100) as u64 * 10_000).min(u32::MAX as u64)
}

pub fn build_pic_s(i: &PicInputs) -> [u8; PIC_S_SIZE] {
    let (s, h) = (i.seq, i.hdr);
    let mut b = [0u8; PIC_S_SIZE];
    let p = &mut b;
    let g = &h.film_grain;
    if i.apply_grain {
        put(p, f::FG_APPLY, 1);
        put(p, f::FG_OVERLAP, g.overlap_flag as u64);
        put(p, f::FG_CLIP, g.clip_to_restricted_range as u64);
        put(p, f::FG_CSFL, g.chroma_scaling_from_luma as u64);
        put(p, f::FG_NUM_Y_B, (g.num_y_points > 0) as u64);
        put(p, f::FG_NUM_CB_B, (g.num_cb_points > 0) as u64);
        put(p, f::FG_NUM_CR_B, (g.num_cr_points > 0) as u64);
        put(p, f::FG_SCALING_SHIFT, g.grain_scaling_minus_8 as u64 + 8);
        put(p, f::FG_SEED, g.grain_seed as u64);
        put(p, f::FG_CB_OFFSET, (g.cb_offset as i64 - 256) as u64);
        put(p, f::FG_CR_OFFSET, (g.cr_offset as i64 - 256) as u64);
        put(p, f::FG_CB_MULT, (g.cb_mult as i64 - 128) as u64);
        put(p, f::FG_CB_LUMA_MULT, (g.cb_luma_mult as i64 - 128) as u64);
        put(p, f::FG_CR_MULT, (g.cr_mult as i64 - 128) as u64);
        put(p, f::FG_CR_LUMA_MULT, (g.cr_luma_mult as i64 - 128) as u64);
    }
    put(p, f::GPTIMER, gptimer(h.upscaled_width, h.frame_height));
    put(p, f::STREAM_LEN, i.stream_len as u64);
    // +0x54 = 0xffff only on blackwell_b classes: stock stores it at ELF 0x40a48 inside the
    // `caps & 0x1ec00000` block of the AV1 builder (objdump, 2026-10-15); Ada av1 traces: 0.
    if i.blackwell_b {
        put(p, f::RESERVED12, 0xffff);
    }
    put(p, f::USE_128, s.use_128x128_superblock as u64);
    put(p, f::CHROMA_FORMAT, 1);
    put(p, f::BIT_DEPTH, s.bit_depth as u64);
    put(p, f::EN_FILTER_INTRA, s.enable_filter_intra as u64);
    put(p, f::EN_INTRA_EDGE, s.enable_intra_edge_filter as u64);
    put(p, f::EN_INTERINTRA, s.enable_interintra_compound as u64);
    put(p, f::EN_MASKED, s.enable_masked_compound as u64);
    put(p, f::EN_DUAL_FILTER, s.enable_dual_filter as u64);
    put(p, f::EN_JNT, s.enable_jnt_comp as u64);
    // Stock sets enable_cdef only when the frame actually filters (some strength non-zero).
    let cdef_active = (0..1usize << h.cdef_bits)
        .any(|k| h.cdef_y_pri[k] | h.cdef_y_sec[k] | h.cdef_uv_pri[k] | h.cdef_uv_sec[k] != 0);
    put(p, f::EN_CDEF, (s.enable_cdef && cdef_active) as u64);
    put(p, f::EN_FGS, i.apply_grain as u64);
    put(p, f::FRAME_TYPE, h.frame_type as u64);
    put(p, f::SHOW_FRAME, h.show_frame as u64);
    put(p, f::DISABLE_CDF_UPDATE, h.disable_cdf_update as u64);
    put(p, f::SCREEN_CONTENT, h.allow_screen_content_tools as u64);
    put(p, f::FORCE_INT_MV, h.force_integer_mv as u64);
    if h.use_superres {
        put(p, f::SCALE_DENOM_M9, h.superres_denom as u64 - 9);
    }
    put(p, f::ALLOW_INTRABC, h.allow_intrabc as u64);
    put(p, f::ALLOW_HP_MV, h.allow_high_precision_mv as u64);
    put(p, f::INTERP_FILTER, h.interpolation_filter as u64);
    put(p, f::SWITCHABLE_MOTION, h.is_motion_mode_switchable as u64);
    put(p, f::USE_REF_MVS, h.use_ref_frame_mvs as u64);
    put(p, f::REFRESH_CTX, !h.disable_frame_end_update_cdf as u64);
    put(p, f::DQ_PRESENT, h.delta_q_present as u64);
    put(p, f::DQ_RES, h.delta_q_res as u64);
    put(p, f::DLF_PRESENT, h.delta_lf_present as u64);
    put(p, f::DLF_RES, h.delta_lf_res as u64);
    put(p, f::DLF_MULTI, h.delta_lf_multi as u64);
    put(p, f::CODED_LOSSLESS, h.coded_lossless as u64);
    put(
        p,
        f::TILE_ENABLED,
        (h.tile.tile_cols * h.tile.tile_rows > 1) as u64,
    );
    put(p, f::SUPERRES_SCALED, h.use_superres as u64);
    put(p, f::TILE_COLS, h.tile.tile_cols as u64);
    put(p, f::TILE_ROWS, h.tile.tile_rows as u64);
    put(p, f::CTX_UPDATE_TILE, h.tile.context_update_tile_id as u64);
    put(p, f::CDEF_DAMPING_M3, h.cdef_damping_minus_3 as u64);
    put(p, f::CDEF_BITS, h.cdef_bits as u64);
    // DRV enum: 0 ONLY_4X4, 3 LARGEST, 4 SELECT; reference mode 0 SINGLE, 2 SELECT.
    put(p, f::TX_MODE, [0u64, 3, 4][h.tx_mode as usize]);
    put(p, f::REF_MODE, if h.reference_select { 2 } else { 0 });
    if h.skip_mode_present {
        put(p, f::SKIP_MODE, 1);
        put(p, f::SKIP_REF0, h.skip_mode_frame[0] as u64);
        put(p, f::SKIP_REF1, h.skip_mode_frame[1] as u64);
    }
    put(p, f::ALLOW_WARP, h.allow_warped_motion as u64);
    put(p, f::REDUCED_TX, h.reduced_tx_set as u64);
    // Any reference whose size differs from the coded frame size is a scaled reference (spec 7.11.3.3).
    let scaled = !h.frame_is_intra
        && i.refs
            .iter()
            .any(|r| r.width != h.frame_width || r.height != h.frame_height);
    put(p, f::REF_SCALING, scaled as u64);
    put(p, f::UPSCALED_W, h.upscaled_width as u64);
    // Superres (spec 7.16): per-plane xStep and initialSubpelX; without superres stock writes 14 / 14 / 0 / 0.
    if h.use_superres {
        let (luma_step, luma_x) = superres_step(h.frame_width, h.upscaled_width, 0);
        let (chroma_step, chroma_x) =
            superres_step(h.frame_width, h.upscaled_width, s.subsampling_x as u32);
        put(p, f::LUMA_STEP, luma_step as u64);
        put(p, f::CHROMA_STEP, chroma_step as u64);
        put(p, f::LUMA_SUBPEL_X, luma_x as u64);
        put(p, f::CHROMA_SUBPEL_X, chroma_x as u64);
    } else {
        put(p, f::LUMA_STEP, 14);
        put(p, f::CHROMA_STEP, 14);
    }
    put(p, f::BASE_Q, h.base_q_idx as u64);
    put(p, f::DQ_Y_DC, h.delta_q_y_dc as u8 as u64);
    put(p, f::DQ_U_DC, h.delta_q_u_dc as u8 as u64);
    put(p, f::DQ_V_DC, h.delta_q_v_dc as u8 as u64);
    put(p, f::DQ_U_AC, h.delta_q_u_ac as u8 as u64);
    put(p, f::DQ_V_AC, h.delta_q_v_ac as u8 as u64);
    let (qy, qu, qv) = if h.using_qmatrix {
        (h.qm_y, h.qm_u, h.qm_v)
    } else {
        (15, 15, 15)
    };
    put(p, f::QM_Y, qy as u64);
    put(p, f::QM_U, qu as u64);
    put(p, f::QM_V, qv as u64);
    let (mut yp, mut up, mut ys, mut us) = (0u64, 0u64, 0u64, 0u64);
    for k in 0..8 {
        yp |= (h.cdef_y_pri[k] as u64) << (4 * k);
        up |= (h.cdef_uv_pri[k] as u64) << (4 * k);
        ys |= (h.cdef_y_sec[k] as u64) << (2 * k);
        us |= (h.cdef_uv_sec[k] as u64) << (2 * k);
    }
    put(p, f::CDEF_Y_PRI, yp);
    put(p, f::CDEF_UV_PRI, up);
    put(p, f::CDEF_Y_SEC, ys);
    put(p, f::CDEF_UV_SEC, us);
    if h.segmentation_enabled {
        put(p, f::SEG_ENABLED, 1);
        put(p, f::SEG_UPDATE_MAP, h.segmentation_update_map as u64);
        put(p, f::SEG_TEMPORAL, h.segmentation_temporal_update as u64);
        let mut sign = 0u64;
        for seg in 0..8 {
            for feat in 0..8 {
                let mut v = if h.feature_enabled[seg][feat] {
                    h.feature_data[seg][feat]
                } else {
                    0
                };
                // The alternative-quantizer feature is stored as magnitude, its sign in segment_quant_sign.
                // UNVERIFIED for the other features (no capture uses them).
                if feat == 0 {
                    v = v.abs();
                }
                put(
                    p,
                    (f::SEG_DATA + 16 * (seg * 8 + feat), 16),
                    v as u16 as u64,
                );
            }
            if h.feature_enabled[seg][0] && h.feature_data[seg][0] < 0 {
                sign |= 1 << seg;
            }
        }
        put(p, f::SEG_LAST_ACTIVE, h.last_active_seg_id as u64);
        put(p, f::SEG_PRESKIP, h.seg_id_pre_skip as u64);
        put(p, f::SEG_QUANT_SIGN, sign);
    }
    put(p, f::SEG_PREV_FLAG, i.prev_segment_ids as u64);
    put(p, f::LF_LEVEL0, h.loop_filter_level[0] as u64);
    put(p, f::LF_LEVEL1, h.loop_filter_level[1] as u64);
    put(p, f::LF_LEVEL_U, h.loop_filter_level[2] as u64);
    put(p, f::LF_LEVEL_V, h.loop_filter_level[3] as u64);
    put(p, f::LF_SHARPNESS, h.loop_filter_sharpness as u64);
    // pic_s has no loop_filter_delta_enabled bit: the deltas are passed only when enabled (spec 7.14.4).
    if h.loop_filter_delta_enabled {
        for k in 0..8 {
            put(
                p,
                (f::LF_REF_DELTAS + 8 * k, 8),
                h.loop_filter_ref_deltas[k] as u8 as u64,
            );
        }
        for k in 0..2 {
            put(
                p,
                (f::LF_MODE_DELTAS + 8 * k, 8),
                h.loop_filter_mode_deltas[k] as u8 as u64,
            );
        }
    }
    let rt = &h.frame_restoration_type;
    put(
        p,
        f::LR_TYPE,
        (rt[0] & 3) as u64 | ((rt[1] & 3) as u64) << 2 | ((rt[2] & 3) as u64) << 4,
    );
    let uses_lr = rt.iter().any(|&t| t != 0);
    let lr_size = if uses_lr {
        let y = 1 + h.lr_unit_shift as u64;
        let uv = y - h.lr_uv_shift as u64;
        y | uv << 2 | uv << 4
    } else {
        0x3f
    };
    put(p, f::LR_UNIT_SIZE, lr_size);
    let stride = (i.pitch / 16) as u64;
    // Current frame: the CUVID width is the upscaled width when superres is on, but the picture
    // descriptor carries the coded (pre-superres) width — stock libnvcuvid 615.78.08 on a
    // superres inter frame writes 213 for a 320-wide upscaled 213-coded picture, and the
    // motion-field size gate compares references against this same width.
    put_frame_info(p, f::CUR_FRAME, h.frame_width, h.frame_height, stride);
    for k in 0..7 {
        let base = f::REF_FRAME + f::REF_FRAME_BITS * k;
        if h.frame_is_intra {
            continue;
        }
        let r = &i.refs[k];
        put_frame_info(p, base, r.width, r.height, stride);
        let xs = (((r.width as u64) << 14) + (h.frame_width as u64 / 2)) / h.frame_width as u64;
        let ys = (((r.height as u64) << 14) + (h.frame_height as u64 / 2)) / h.frame_height as u64;
        put(p, (base + 96, 16), xs);
        put(p, (base + 112, 16), ys);
        put(p, (base + 128, 1), h.ref_frame_sign_bias[k] as u64);
        put(p, (base + 129, 2), h.gm_type[k + 1] as u64);
        let off = get_relative_dist(i.seq, h.order_hint, r.order_hint);
        put(p, (base + 144, 16), off as i16 as u16 as u64);
        put(p, (base + 160, 16), (-off) as i16 as u16 as u64);
    }
    for j in 0..3 {
        put(p, (f::USE_TEMPORAL0.0 + j, 1), i.mf.use_temporal[j] as u64);
        put(p, (f::MF1_TYPE.0 + 3 * j, 3), i.mf.mf_type[j] as u64);
        for k in 0..7 {
            put(
                p,
                (f::MFMV_OFFSET + 16 * (j * 7 + k), 16),
                i.mf.mfmv_offset[j][k] as u16 as u64,
            );
            put(
                p,
                (f::MFMV_SIDE + 8 * (j * 7 + k), 8),
                i.mf.mfmv_side[j][k] as u8 as u64,
            );
        }
    }
    put(p, f::TILEFORMAT, 1);
    put(p, f::GOB_HEIGHT, 0);
    let fl = filter_layout(h.upscaled_width, h.frame_height);
    put(p, f::FLT_TOP, 0);
    put(p, f::FLT_VERT, fl.flt_vert / 256);
    put(p, f::CDEF_VERT, fl.cdef_vert / 256);
    put(p, f::LR_VERT, fl.lr_vert / 256);
    put(p, f::HUS_VERT, fl.hus_vert / 256);
    put(p, f::FGS_VERT, fl.fgs_vert / 256);
    // enable_histogram 0, sw_skip_start_length 0, then (stock 8- and 10-bit av1aom traces) bits 15..18 =
    // plane GOB kinds `luma | chroma << 2` (8-bit 9, 10-bit 2) and bits 19.. = the surface-format code
    // (0 / 3), the same pair VP9 and HEVC carry; see `crate::video::plane_kind_bits`.
    if i.blackwell_b {
        let bd = s.bit_depth as u32;
        let kinds = crate::video::plane_kind_bits(crate::video::bytes_per_sample(bd));
        put(
            p,
            f::STAT_WORD,
            u64::from(kinds << 15 | crate::video::surface_format_code(bd) << 19),
        );
    }
    b
}

fn put_frame_info(p: &mut [u8], base: usize, w: u32, h: u32, stride: u64) {
    put(p, (base, 16), w as u64);
    put(p, (base + 16, 16), h as u64);
    put(p, (base + 32, 16), stride);
    put(p, (base + 48, 16), stride);
}

/// `AV1TileInfo_OLD` entries: width/height in superblocks, then [start, end) of the tile data.
pub fn build_tile_info(h: &FrameHeader, tiles: &[TileRange], out: &mut [u8]) {
    let t = &h.tile;
    for (n, tr) in tiles.iter().enumerate().take(MAX_TILES) {
        let (row, col) = (n / t.tile_cols as usize, n % t.tile_cols as usize);
        let e = &mut out[n * TILE_INFO_ENTRY..(n + 1) * TILE_INFO_ENTRY];
        e.fill(0);
        e[0] = (t.col_start_sb[col + 1] - t.col_start_sb[col]) as u8;
        e[1] = (t.row_start_sb[row + 1] - t.row_start_sb[row]) as u8;
        e[2..6].copy_from_slice(&tr.start.to_le_bytes());
        e[6..10].copy_from_slice(&tr.end.to_le_bytes());
    }
}

/// Global motion: 7 x 32 bytes, wmmat[6] as int32 followed by the four int16 shear parameters (spec
/// 7.11.3.6). Intra frames carry 0 and identity models 0x8000 in the shear words (stock); valid non-identity models carry
/// alpha/beta/gamma/delta. UNVERIFIED for non-identity models (no capture with global motion).
pub fn build_global_motion(h: &FrameHeader, out: &mut [u8]) {
    for k in 0..7 {
        let e = &mut out[32 * k..32 * (k + 1)];
        let m = &h.gm_params[k + 1];
        for (j, v) in m.iter().enumerate() {
            e[4 * j..4 * j + 4].copy_from_slice(&v.to_le_bytes());
        }
        let shear: [i16; 4] = if h.frame_is_intra {
            [0; 4]
        } else if h.gm_type[k + 1] == GM_IDENTITY {
            [i16::MIN; 4]
        } else {
            setup_shear(m).unwrap_or([i16::MIN; 4])
        };
        for (j, v) in shear.iter().enumerate() {
            e[24 + 2 * j..26 + 2 * j].copy_from_slice(&v.to_le_bytes());
        }
    }
}

fn resolve_divisor(d: i64) -> (i64, i64) {
    let n = 63 - (d.unsigned_abs()).leading_zeros() as i64; // FloorLog2(|d|)
    let e = d.abs() - (1i64 << n);
    let f = if n > 8 {
        (e + (1i64 << (n - 9))) >> (n - 8)
    } else {
        e << (8 - n)
    };
    let div = ((1i64 << 22) + (256 + f) / 2) / (256 + f); // Div_Lut[f]
    let shift = n + 14;
    (if d < 0 { -div } else { div }, shift)
}

fn round2signed(x: i64, n: i64) -> i64 {
    if n == 0 {
        x
    } else if x >= 0 {
        (x + (1i64 << (n - 1))) >> n
    } else {
        -((-x + (1i64 << (n - 1))) >> n)
    }
}

/// Spec 7.11.3.6 setup_shear: returns (alpha, beta, gamma, delta) or None when the model is not valid.
pub fn setup_shear(m: &[i32; 6]) -> Option<[i16; 4]> {
    let m: [i64; 6] = m.map(|v| v as i64);
    if m[2] <= 0 {
        return None;
    }
    let alpha0 = (m[2] - (1 << 16)).clamp(-32768, 32767);
    let beta0 = m[3].clamp(-32768, 32767);
    let (div, shift) = resolve_divisor(m[2]);
    let v = m[4] << 16;
    let gamma0 = round2signed(v * div, shift).clamp(-32768, 32767);
    let w = m[3] * m[4];
    let delta0 = (m[5] - round2signed(w * div, shift) - (1 << 16)).clamp(-32768, 32767);
    let r = |x: i64| round2signed(x, 6) << 6;
    let (a, b, g, d) = (r(alpha0), r(beta0), r(gamma0), r(delta0));
    if 4 * a.abs() + 7 * b.abs() >= (1 << 16) || 4 * g.abs() + 4 * d.abs() >= (1 << 16) {
        return None;
    }
    Some([a as i16, b as i16, g as i16, d as i16])
}

/// Value of the word at `PIC_EXT_244` (rounding for an odd superblock count UNVERIFIED: no capture).
pub fn pic_ext_244(width: u32) -> u32 {
    (width.div_ceil(64) + 2).div_ceil(2)
}

/// Spec 7.16 upscaling: (stepX, initialSubpelX) for one plane.
fn superres_step(frame_width: u32, upscaled_width: u32, sub_x: u32) -> (i64, i64) {
    let round2 = |x: u32, n: u32| if n == 0 { x } else { (x + (1 << (n - 1))) >> n };
    let down = round2(frame_width, sub_x) as i64;
    let up = round2(upscaled_width, sub_x) as i64;
    let step = ((down << 14) + up / 2) / up;
    let err = up * step - (down << 14);
    let init = ((-((up - down) << 13)) + up / 2) / up + (1 << 7) - err / 2;
    (step, init & ((1 << 14) - 1))
}
