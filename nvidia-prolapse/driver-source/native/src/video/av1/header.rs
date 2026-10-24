//! AV1 sequence header, uncompressed frame header and tile group parsing, plus the reference state the
//! frame header depends on (spec 5.5, 5.9, 5.11.1, 7.20, 7.21). Written from the AV1 specification.

use super::obu::{Av1Error, BitReader, Result};

pub const NUM_REF_FRAMES: usize = 8;
pub const REFS_PER_FRAME: usize = 7;
pub const PRIMARY_REF_NONE: u8 = 7;
pub const KEY_FRAME: u8 = 0;
pub const INTER_FRAME: u8 = 1;
pub const INTRA_ONLY_FRAME: u8 = 2;
pub const SWITCH_FRAME: u8 = 3;
pub const SELECT_SCREEN_CONTENT_TOOLS: u32 = 2;
pub const SELECT_INTEGER_MV: u32 = 2;
pub const WARPEDMODEL_PREC_BITS: u32 = 16;
pub const GM_IDENTITY: u8 = 0;
pub const GM_TRANSLATION: u8 = 1;
pub const GM_ROTZOOM: u8 = 2;
pub const GM_AFFINE: u8 = 3;

const SEG_FEATURE_BITS: [u32; 8] = [8, 6, 6, 6, 6, 3, 0, 0];
const SEG_FEATURE_SIGNED: [bool; 8] = [true, true, true, true, true, false, false, false];
const SEG_FEATURE_MAX: [i32; 8] = [255, 63, 63, 63, 63, 7, 0, 0];
pub const DEFAULT_LF_REF_DELTAS: [i8; 8] = [1, 0, 0, 0, -1, 0, -1, -1];

#[derive(Debug, Clone, Default)]
pub struct SequenceHeader {
    pub seq_profile: u8,
    pub still_picture: bool,
    pub reduced_still_picture_header: bool,
    pub timing_info_present: bool,
    pub equal_picture_interval: bool,
    pub decoder_model_info_present: bool,
    pub buffer_delay_length_minus_1: u32,
    pub buffer_removal_time_length_minus_1: u32,
    pub frame_presentation_time_length_minus_1: u32,
    pub operating_points: Vec<OperatingPoint>,
    pub frame_width_bits_minus_1: u32,
    pub frame_height_bits_minus_1: u32,
    pub max_frame_width_minus_1: u32,
    pub max_frame_height_minus_1: u32,
    pub frame_id_numbers_present: bool,
    pub delta_frame_id_length_minus_2: u32,
    pub additional_frame_id_length_minus_1: u32,
    pub use_128x128_superblock: bool,
    pub enable_filter_intra: bool,
    pub enable_intra_edge_filter: bool,
    pub enable_interintra_compound: bool,
    pub enable_masked_compound: bool,
    pub enable_warped_motion: bool,
    pub enable_dual_filter: bool,
    pub enable_order_hint: bool,
    pub enable_jnt_comp: bool,
    pub enable_ref_frame_mvs: bool,
    pub seq_force_screen_content_tools: u32,
    pub seq_force_integer_mv: u32,
    pub order_hint_bits: u32,
    pub enable_superres: bool,
    pub enable_cdef: bool,
    pub enable_restoration: bool,
    pub bit_depth: u8,
    pub mono_chrome: bool,
    pub subsampling_x: bool,
    pub subsampling_y: bool,
    pub color_range: bool,
    pub separate_uv_delta_q: bool,
    pub film_grain_params_present: bool,
}

#[derive(Debug, Clone, Default)]
pub struct OperatingPoint {
    pub idc: u32,
    pub seq_level_idx: u32,
    pub seq_tier: u32,
    pub decoder_model_present: bool,
}

impl SequenceHeader {
    pub fn num_planes(&self) -> usize {
        if self.mono_chrome {
            1
        } else {
            3
        }
    }

    pub fn parse(data: &[u8]) -> Result<Self> {
        let mut r = BitReader::new(data);
        let mut s = SequenceHeader {
            seq_profile: r.f(3)? as u8,
            ..Default::default()
        };
        s.still_picture = r.flag()?;
        s.reduced_still_picture_header = r.flag()?;
        if s.reduced_still_picture_header {
            s.operating_points.push(OperatingPoint {
                idc: 0,
                seq_level_idx: r.f(5)?,
                ..Default::default()
            });
        } else {
            s.timing_info_present = r.flag()?;
            if s.timing_info_present {
                r.f(32)?;
                r.f(32)?;
                s.equal_picture_interval = r.flag()?;
                if s.equal_picture_interval {
                    r.uvlc()?;
                }
                s.decoder_model_info_present = r.flag()?;
                if s.decoder_model_info_present {
                    s.buffer_delay_length_minus_1 = r.f(5)?;
                    r.f(32)?;
                    s.buffer_removal_time_length_minus_1 = r.f(5)?;
                    s.frame_presentation_time_length_minus_1 = r.f(5)?;
                }
            }
            let initial_display_delay_present = r.flag()?;
            let cnt = r.f(5)? + 1;
            for _ in 0..cnt {
                let mut op = OperatingPoint {
                    idc: r.f(12)?,
                    seq_level_idx: r.f(5)?,
                    ..Default::default()
                };
                if op.seq_level_idx > 7 {
                    op.seq_tier = r.f(1)?;
                }
                if s.decoder_model_info_present {
                    op.decoder_model_present = r.flag()?;
                    if op.decoder_model_present {
                        let n = s.buffer_delay_length_minus_1 + 1;
                        r.f(n)?;
                        r.f(n)?;
                        r.f(1)?;
                    }
                }
                if initial_display_delay_present && r.flag()? {
                    r.f(4)?;
                }
                s.operating_points.push(op);
            }
        }
        s.frame_width_bits_minus_1 = r.f(4)?;
        s.frame_height_bits_minus_1 = r.f(4)?;
        s.max_frame_width_minus_1 = r.f(s.frame_width_bits_minus_1 + 1)?;
        s.max_frame_height_minus_1 = r.f(s.frame_height_bits_minus_1 + 1)?;
        if !s.reduced_still_picture_header {
            s.frame_id_numbers_present = r.flag()?;
        }
        if s.frame_id_numbers_present {
            s.delta_frame_id_length_minus_2 = r.f(4)?;
            s.additional_frame_id_length_minus_1 = r.f(3)?;
        }
        s.use_128x128_superblock = r.flag()?;
        s.enable_filter_intra = r.flag()?;
        s.enable_intra_edge_filter = r.flag()?;
        if s.reduced_still_picture_header {
            s.seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
            s.seq_force_integer_mv = SELECT_INTEGER_MV;
        } else {
            s.enable_interintra_compound = r.flag()?;
            s.enable_masked_compound = r.flag()?;
            s.enable_warped_motion = r.flag()?;
            s.enable_dual_filter = r.flag()?;
            s.enable_order_hint = r.flag()?;
            if s.enable_order_hint {
                s.enable_jnt_comp = r.flag()?;
                s.enable_ref_frame_mvs = r.flag()?;
            }
            s.seq_force_screen_content_tools = if r.flag()? {
                SELECT_SCREEN_CONTENT_TOOLS
            } else {
                r.f(1)?
            };
            s.seq_force_integer_mv = if s.seq_force_screen_content_tools > 0 {
                if r.flag()? {
                    SELECT_INTEGER_MV
                } else {
                    r.f(1)?
                }
            } else {
                SELECT_INTEGER_MV
            };
            if s.enable_order_hint {
                s.order_hint_bits = r.f(3)? + 1;
            }
        }
        s.enable_superres = r.flag()?;
        s.enable_cdef = r.flag()?;
        s.enable_restoration = r.flag()?;
        // color_config()
        let high_bitdepth = r.flag()?;
        s.bit_depth = if s.seq_profile == 2 && high_bitdepth {
            if r.flag()? {
                12
            } else {
                10
            }
        } else if high_bitdepth {
            10
        } else {
            8
        };
        s.mono_chrome = if s.seq_profile == 1 { false } else { r.flag()? };
        let (mut cp, mut tc, mut mc) = (2, 2, 2);
        if r.flag()? {
            cp = r.f(8)?;
            tc = r.f(8)?;
            mc = r.f(8)?;
        }
        if s.mono_chrome {
            s.color_range = r.flag()?;
            s.subsampling_x = true;
            s.subsampling_y = true;
        } else if cp == 1 && tc == 13 && mc == 0 {
            s.color_range = true;
        } else {
            s.color_range = r.flag()?;
            match s.seq_profile {
                0 => {
                    s.subsampling_x = true;
                    s.subsampling_y = true;
                }
                1 => {}
                _ => {
                    if s.bit_depth == 12 {
                        s.subsampling_x = r.flag()?;
                        if s.subsampling_x {
                            s.subsampling_y = r.flag()?;
                        }
                    } else {
                        s.subsampling_x = true;
                    }
                }
            }
            if s.subsampling_x && s.subsampling_y {
                r.f(2)?; // chroma_sample_position
            }
        }
        if !s.mono_chrome {
            s.separate_uv_delta_q = r.flag()?;
        }
        s.film_grain_params_present = r.flag()?;
        Ok(s)
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct FilmGrainParams {
    pub apply_grain: bool,
    pub grain_seed: u16,
    pub update_grain: bool,
    pub num_y_points: u8,
    pub point_y_value: [u8; 14],
    pub point_y_scaling: [u8; 14],
    pub chroma_scaling_from_luma: bool,
    pub num_cb_points: u8,
    pub point_cb_value: [u8; 10],
    pub point_cb_scaling: [u8; 10],
    pub num_cr_points: u8,
    pub point_cr_value: [u8; 10],
    pub point_cr_scaling: [u8; 10],
    pub grain_scaling_minus_8: u8,
    pub ar_coeff_lag: u8,
    pub ar_coeffs_y_plus_128: [u8; 24],
    pub ar_coeffs_cb_plus_128: [u8; 25],
    pub ar_coeffs_cr_plus_128: [u8; 25],
    pub ar_coeff_shift_minus_6: u8,
    pub grain_scale_shift: u8,
    pub cb_mult: u8,
    pub cb_luma_mult: u8,
    pub cb_offset: u16,
    pub cr_mult: u8,
    pub cr_luma_mult: u8,
    pub cr_offset: u16,
    pub overlap_flag: bool,
    pub clip_to_restricted_range: bool,
}

/// State saved per reference slot by the reference frame update process (spec 7.20).
#[derive(Debug, Clone, Copy)]
pub struct RefSlotState {
    pub valid: bool,
    pub frame_type: u8,
    pub upscaled_width: u32,
    pub frame_width: u32,
    pub frame_height: u32,
    pub render_width: u32,
    pub render_height: u32,
    pub mi_cols: u32,
    pub mi_rows: u32,
    pub order_hint: u32,
    pub saved_order_hints: [u32; 8],
    pub gm_params: [[i32; 6]; 8],
    pub lf_ref_deltas: [i8; 8],
    pub lf_mode_deltas: [i8; 2],
    pub feature_enabled: [[bool; 8]; 8],
    pub feature_data: [[i16; 8]; 8],
    pub film_grain: FilmGrainParams,
    pub showable_frame: bool,
}

impl Default for RefSlotState {
    fn default() -> Self {
        Self {
            valid: false,
            frame_type: 0,
            upscaled_width: 0,
            frame_width: 0,
            frame_height: 0,
            render_width: 0,
            render_height: 0,
            mi_cols: 0,
            mi_rows: 0,
            order_hint: 0,
            saved_order_hints: [0; 8],
            gm_params: [identity_gm(); 8],
            lf_ref_deltas: DEFAULT_LF_REF_DELTAS,
            lf_mode_deltas: [0; 2],
            feature_enabled: [[false; 8]; 8],
            feature_data: [[0; 8]; 8],
            film_grain: FilmGrainParams::default(),
            showable_frame: false,
        }
    }
}

pub const fn identity_gm() -> [i32; 6] {
    [
        0,
        0,
        1 << WARPEDMODEL_PREC_BITS,
        0,
        0,
        1 << WARPEDMODEL_PREC_BITS,
    ]
}

#[derive(Debug, Clone, Default)]
pub struct TileInfo {
    pub tile_cols: u32,
    pub tile_rows: u32,
    pub tile_cols_log2: u32,
    pub tile_rows_log2: u32,
    /// In superblock units, `tile_cols + 1` / `tile_rows + 1` entries.
    pub col_start_sb: Vec<u32>,
    pub row_start_sb: Vec<u32>,
    pub context_update_tile_id: u32,
    pub tile_size_bytes: u32,
}

#[derive(Debug, Clone, Default)]
pub struct FrameHeader {
    pub show_existing_frame: bool,
    pub frame_to_show_map_idx: u8,
    pub frame_type: u8,
    pub frame_is_intra: bool,
    pub show_frame: bool,
    pub showable_frame: bool,
    pub error_resilient_mode: bool,
    pub disable_cdf_update: bool,
    pub allow_screen_content_tools: bool,
    pub force_integer_mv: bool,
    pub frame_size_override_flag: bool,
    pub order_hint: u32,
    pub primary_ref_frame: u8,
    pub refresh_frame_flags: u8,
    pub ref_order_hint: [u32; 8],
    pub ref_frame_idx: [u8; REFS_PER_FRAME],
    pub frame_width: u32,
    pub frame_height: u32,
    pub upscaled_width: u32,
    pub render_width: u32,
    pub render_height: u32,
    pub use_superres: bool,
    pub superres_denom: u32,
    pub mi_cols: u32,
    pub mi_rows: u32,
    pub allow_intrabc: bool,
    pub allow_high_precision_mv: bool,
    pub is_filter_switchable: bool,
    /// 0..3 = EIGHTTAP, SMOOTH, SHARP, BILINEAR; 4 = SWITCHABLE.
    pub interpolation_filter: u8,
    pub is_motion_mode_switchable: bool,
    pub use_ref_frame_mvs: bool,
    /// OrderHints[LAST..=ALTREF] (index 0 = LAST).
    pub order_hints: [u32; REFS_PER_FRAME],
    pub ref_frame_sign_bias: [bool; REFS_PER_FRAME],
    pub disable_frame_end_update_cdf: bool,
    pub tile: TileInfo,
    pub base_q_idx: u8,
    pub delta_q_y_dc: i8,
    pub delta_q_u_dc: i8,
    pub delta_q_u_ac: i8,
    pub delta_q_v_dc: i8,
    pub delta_q_v_ac: i8,
    pub using_qmatrix: bool,
    pub qm_y: u8,
    pub qm_u: u8,
    pub qm_v: u8,
    pub segmentation_enabled: bool,
    pub segmentation_update_map: bool,
    pub segmentation_temporal_update: bool,
    pub segmentation_update_data: bool,
    pub feature_enabled: [[bool; 8]; 8],
    pub feature_data: [[i16; 8]; 8],
    pub seg_id_pre_skip: bool,
    pub last_active_seg_id: u8,
    pub delta_q_present: bool,
    pub delta_q_res: u8,
    pub delta_lf_present: bool,
    pub delta_lf_res: u8,
    pub delta_lf_multi: bool,
    pub coded_lossless: bool,
    pub all_lossless: bool,
    pub loop_filter_level: [u8; 4],
    pub loop_filter_sharpness: u8,
    pub loop_filter_delta_enabled: bool,
    pub loop_filter_ref_deltas: [i8; 8],
    pub loop_filter_mode_deltas: [i8; 2],
    pub cdef_damping_minus_3: u8,
    pub cdef_bits: u8,
    /// Coded values (secondary strengths as coded, 0..3).
    pub cdef_y_pri: [u8; 8],
    pub cdef_y_sec: [u8; 8],
    pub cdef_uv_pri: [u8; 8],
    pub cdef_uv_sec: [u8; 8],
    /// FrameRestorationType: 0 NONE, 1 WIENER, 2 SGRPROJ, 3 SWITCHABLE.
    pub frame_restoration_type: [u8; 3],
    pub lr_unit_shift: u8,
    pub lr_uv_shift: u8,
    /// 0 ONLY_4X4, 1 LARGEST, 2 SELECT.
    pub tx_mode: u8,
    pub reference_select: bool,
    pub skip_mode_present: bool,
    /// SkipModeFrame[0..2] as reference frame numbers (LAST = 1).
    pub skip_mode_frame: [u8; 2],
    pub allow_warped_motion: bool,
    pub reduced_tx_set: bool,
    pub gm_type: [u8; 8],
    pub gm_params: [[i32; 6]; 8],
    pub film_grain: FilmGrainParams,
    /// Bytes of the frame header OBU payload consumed (for OBU_FRAME).
    pub header_bytes: usize,
}

pub fn get_relative_dist(seq: &SequenceHeader, a: u32, b: u32) -> i32 {
    if !seq.enable_order_hint {
        return 0;
    }
    let bits = seq.order_hint_bits;
    let diff = a as i32 - b as i32;
    let m = 1i32 << (bits - 1);
    (diff & (m - 1)) - (diff & m)
}

fn tile_log2(blk: u32, target: u32) -> u32 {
    let mut k = 0;
    while (blk << k) < target {
        k += 1;
    }
    k
}

fn inverse_recenter(r: i32, v: i32) -> i32 {
    if v > 2 * r {
        v
    } else if v & 1 != 0 {
        r - ((v + 1) >> 1)
    } else {
        r + (v >> 1)
    }
}

fn decode_subexp(r: &mut BitReader, num_syms: i32) -> Result<i32> {
    let (mut i, mut mk, k) = (0, 0i32, 3);
    loop {
        let b2 = if i != 0 { k + i - 1 } else { k };
        let a = 1i32 << b2;
        if num_syms <= mk + 3 * a {
            return Ok(r.ns((num_syms - mk) as u32)? as i32 + mk);
        }
        if r.flag()? {
            i += 1;
            mk += a;
        } else {
            return Ok(r.f(b2 as u32)? as i32 + mk);
        }
    }
}

fn decode_signed_subexp_with_ref(r: &mut BitReader, low: i32, high: i32, rf: i32) -> Result<i32> {
    let mx = high - low;
    let rr = rf - low;
    let v = decode_subexp(r, mx)?;
    let x = if (rr << 1) <= mx {
        inverse_recenter(rr, v)
    } else {
        mx - 1 - inverse_recenter(mx - 1 - rr, v)
    };
    Ok(x + low)
}

/// Decoder-side parser state: sequence header plus the 8 reference slots.
#[derive(Debug, Clone, Default)]
pub struct ParserState {
    pub seq: Option<SequenceHeader>,
    pub refs: [RefSlotState; NUM_REF_FRAMES],
}

impl ParserState {
    /// Spec 7.8 `set_frame_refs()`: derive the reference slots for
    /// LAST..ALTREF from the reference slots' order hints when
    /// `frame_refs_short_signaling` is set. Control flow follows FFmpeg's
    /// cbs_av1_syntax_template.c:348-455 (a transcription of the spec
    /// pseudocode): pick ALTREF (latest at/after the current frame), BWDREF
    /// (earliest at/after), ALTREF2 (the next one), LAST2/LAST3 (latest before),
    /// then fill anything left with the earliest slot overall.
    pub(super) fn set_frame_refs(
        &self,
        last_frame_idx: u8,
        gold_frame_idx: u8,
        cur_order_hint: u32,
    ) -> Result<[u8; REFS_PER_FRAME]> {
        let seq = self.seq.as_ref().ok_or(Av1Error::NoSequenceHeader)?;
        // 0 = LAST, 1 = LAST2, 2 = LAST3, 3 = GOLDEN, 4 = BWDREF, 5 = ALTREF2,
        // 6 = ALTREF. 0xff = AV1_REF_FRAME_NONE.
        let mut idx = [0xffu8; REFS_PER_FRAME];
        idx[0] = last_frame_idx;
        idx[3] = gold_frame_idx;
        let mut used = [false; NUM_REF_FRAMES];
        used[last_frame_idx as usize] = true;
        used[gold_frame_idx as usize] = true;

        let cur = 1i32 << (seq.order_hint_bits - 1);
        let mut shifted = [0i32; NUM_REF_FRAMES];
        for i in 0..NUM_REF_FRAMES {
            shifted[i] = cur + get_relative_dist(seq, self.refs[i].order_hint, cur_order_hint);
        }
        let mut latest = shifted[last_frame_idx as usize];
        let mut earliest = shifted[gold_frame_idx as usize];

        // ALTREF
        let mut r: i8 = -1;
        for i in 0..NUM_REF_FRAMES {
            let hint = shifted[i];
            if !used[i] && hint >= cur && (r < 0 || hint >= latest) {
                r = i as i8;
                latest = hint;
            }
        }
        if r >= 0 {
            idx[6] = r as u8;
            used[r as usize] = true;
        }
        // BWDREF
        let mut r: i8 = -1;
        for i in 0..NUM_REF_FRAMES {
            let hint = shifted[i];
            if !used[i] && hint >= cur && (r < 0 || hint < earliest) {
                r = i as i8;
                earliest = hint;
            }
        }
        if r >= 0 {
            idx[4] = r as u8;
            used[r as usize] = true;
        }
        // ALTREF2
        let mut r: i8 = -1;
        for i in 0..NUM_REF_FRAMES {
            let hint = shifted[i];
            if !used[i] && hint >= cur && (r < 0 || hint < earliest) {
                r = i as i8;
                earliest = hint;
            }
        }
        if r >= 0 {
            idx[5] = r as u8;
            used[r as usize] = true;
        }
        // LAST2, LAST3
        for k in [1usize, 2] {
            if idx[k] == 0xff {
                let mut r: i8 = -1;
                for j in 0..NUM_REF_FRAMES {
                    let hint = shifted[j];
                    if !used[j] && hint < cur && (r < 0 || hint >= latest) {
                        r = j as i8;
                        latest = hint;
                    }
                }
                if r >= 0 {
                    idx[k] = r as u8;
                    used[r as usize] = true;
                }
            }
        }
        // Anything still unset: the earliest slot overall (may be reused).
        let mut r: i8 = -1;
        for i in 0..NUM_REF_FRAMES {
            let hint = shifted[i];
            if r < 0 || hint < earliest {
                r = i as i8;
                earliest = hint;
            }
        }
        for k in idx.iter_mut() {
            if *k == 0xff {
                *k = r as u8;
            }
        }
        Ok(idx)
    }

    pub fn parse_frame_header(
        &mut self,
        data: &[u8],
        temporal_id: u8,
        spatial_id: u8,
    ) -> Result<FrameHeader> {
        let seq = self.seq.clone().ok_or(Av1Error::NoSequenceHeader)?;
        let mut r = BitReader::new(data);
        let mut h = FrameHeader {
            primary_ref_frame: PRIMARY_REF_NONE,
            ..Default::default()
        };
        let id_len = seq.additional_frame_id_length_minus_1 + seq.delta_frame_id_length_minus_2 + 3;
        if seq.reduced_still_picture_header {
            h.frame_type = KEY_FRAME;
            h.frame_is_intra = true;
            h.show_frame = true;
        } else {
            h.show_existing_frame = r.flag()?;
            if h.show_existing_frame {
                h.frame_to_show_map_idx = r.f(3)? as u8;
                if seq.decoder_model_info_present && !seq.equal_picture_interval {
                    r.f(seq.frame_presentation_time_length_minus_1 + 1)?;
                }
                if seq.frame_id_numbers_present {
                    r.f(id_len)?;
                }
                let slot = self.refs[h.frame_to_show_map_idx as usize];
                h.frame_type = slot.frame_type;
                h.refresh_frame_flags = if h.frame_type == KEY_FRAME { 0xff } else { 0 };
                h.film_grain = slot.film_grain;
                h.show_frame = true;
                h.header_bytes = r.bit_pos().div_ceil(8);
                return Ok(h);
            }
            h.frame_type = r.f(2)? as u8;
            h.frame_is_intra = h.frame_type == INTRA_ONLY_FRAME || h.frame_type == KEY_FRAME;
            h.show_frame = r.flag()?;
            if h.show_frame && seq.decoder_model_info_present && !seq.equal_picture_interval {
                r.f(seq.frame_presentation_time_length_minus_1 + 1)?;
            }
            h.showable_frame = if h.show_frame {
                h.frame_type != KEY_FRAME
            } else {
                r.flag()?
            };
            h.error_resilient_mode =
                if h.frame_type == SWITCH_FRAME || (h.frame_type == KEY_FRAME && h.show_frame) {
                    true
                } else {
                    r.flag()?
                };
        }
        if h.frame_type == KEY_FRAME && h.show_frame {
            for s in self.refs.iter_mut() {
                s.valid = false;
                s.order_hint = 0;
            }
        }
        h.disable_cdf_update = r.flag()?;
        h.allow_screen_content_tools =
            if seq.seq_force_screen_content_tools == SELECT_SCREEN_CONTENT_TOOLS {
                r.flag()?
            } else {
                seq.seq_force_screen_content_tools != 0
            };
        if h.allow_screen_content_tools {
            h.force_integer_mv = if seq.seq_force_integer_mv == SELECT_INTEGER_MV {
                r.flag()?
            } else {
                seq.seq_force_integer_mv != 0
            };
        }
        if h.frame_is_intra {
            h.force_integer_mv = true;
        }
        if seq.frame_id_numbers_present {
            r.f(id_len)?;
        }
        h.frame_size_override_flag = if h.frame_type == SWITCH_FRAME {
            true
        } else if seq.reduced_still_picture_header {
            false
        } else {
            r.flag()?
        };
        h.order_hint = r.f(seq.order_hint_bits)?;
        if !(h.frame_is_intra || h.error_resilient_mode) {
            h.primary_ref_frame = r.f(3)? as u8;
        }
        if seq.decoder_model_info_present && r.flag()? {
            for op in &seq.operating_points {
                if op.decoder_model_present {
                    let in_t = (op.idc >> temporal_id) & 1;
                    let in_s = (op.idc >> (spatial_id + 8)) & 1;
                    if op.idc == 0 || (in_t != 0 && in_s != 0) {
                        r.f(seq.buffer_removal_time_length_minus_1 + 1)?;
                    }
                }
            }
        }
        h.refresh_frame_flags =
            if h.frame_type == SWITCH_FRAME || (h.frame_type == KEY_FRAME && h.show_frame) {
                0xff
            } else {
                r.f(8)? as u8
            };
        if (!h.frame_is_intra || h.refresh_frame_flags != 0xff)
            && h.error_resilient_mode
            && seq.enable_order_hint
        {
            for i in 0..NUM_REF_FRAMES {
                h.ref_order_hint[i] = r.f(seq.order_hint_bits)?;
                if h.ref_order_hint[i] != self.refs[i].order_hint || !self.refs[i].valid {
                    // Spec 7.20 note: a missing reference is "set up" with this order hint.
                    self.refs[i] = RefSlotState {
                        order_hint: h.ref_order_hint[i],
                        ..Default::default()
                    };
                }
            }
        }
        if h.frame_is_intra {
            self.frame_size(&seq, &mut r, &mut h)?;
            self.render_size(&mut r, &mut h)?;
            if h.allow_screen_content_tools && h.upscaled_width == h.frame_width {
                h.allow_intrabc = r.flag()?;
            }
        } else {
            let short = if seq.enable_order_hint {
                r.flag()?
            } else {
                false
            };
            if short {
                let last_frame_idx = r.f(3)? as u8;
                let gold_frame_idx = r.f(3)? as u8;
                h.ref_frame_idx = self.set_frame_refs(last_frame_idx, gold_frame_idx, h.order_hint)?;
            }
            for i in 0..REFS_PER_FRAME {
                if !short {
                    h.ref_frame_idx[i] = r.f(3)? as u8;
                }
                if seq.frame_id_numbers_present {
                    r.f(seq.delta_frame_id_length_minus_2 + 2)?;
                }
            }
            if h.frame_size_override_flag && !h.error_resilient_mode {
                // frame_size_with_refs()
                let mut found = false;
                for i in 0..REFS_PER_FRAME {
                    if r.flag()? {
                        let s = self.refs[h.ref_frame_idx[i] as usize];
                        h.upscaled_width = s.upscaled_width;
                        h.frame_width = h.upscaled_width;
                        h.frame_height = s.frame_height;
                        h.render_width = s.render_width;
                        h.render_height = s.render_height;
                        found = true;
                        break;
                    }
                }
                if !found {
                    self.frame_size(&seq, &mut r, &mut h)?;
                    self.render_size(&mut r, &mut h)?;
                } else {
                    superres_params(&seq, &mut r, &mut h)?;
                    compute_image_size(&mut h);
                }
            } else {
                self.frame_size(&seq, &mut r, &mut h)?;
                self.render_size(&mut r, &mut h)?;
            }
            h.allow_high_precision_mv = if h.force_integer_mv { false } else { r.flag()? };
            h.is_filter_switchable = r.flag()?;
            h.interpolation_filter = if h.is_filter_switchable {
                4
            } else {
                r.f(2)? as u8
            };
            h.is_motion_mode_switchable = r.flag()?;
            h.use_ref_frame_mvs = if h.error_resilient_mode || !seq.enable_ref_frame_mvs {
                false
            } else {
                r.flag()?
            };
            for i in 0..REFS_PER_FRAME {
                let hint = self.refs[h.ref_frame_idx[i] as usize].order_hint;
                h.order_hints[i] = hint;
                h.ref_frame_sign_bias[i] =
                    seq.enable_order_hint && get_relative_dist(&seq, hint, h.order_hint) > 0;
            }
        }
        h.disable_frame_end_update_cdf = if seq.reduced_still_picture_header || h.disable_cdf_update
        {
            true
        } else {
            r.flag()?
        };
        // setup_past_independence / load_previous
        let (prev_gm, prev_lf_ref, prev_lf_mode, prev_fe, prev_fd) =
            if h.primary_ref_frame == PRIMARY_REF_NONE {
                (
                    [identity_gm(); 8],
                    DEFAULT_LF_REF_DELTAS,
                    [0i8; 2],
                    [[false; 8]; 8],
                    [[0i16; 8]; 8],
                )
            } else {
                let s = &self.refs[h.ref_frame_idx[h.primary_ref_frame as usize] as usize];
                (
                    s.gm_params,
                    s.lf_ref_deltas,
                    s.lf_mode_deltas,
                    s.feature_enabled,
                    s.feature_data,
                )
            };
        h.loop_filter_ref_deltas = prev_lf_ref;
        h.loop_filter_mode_deltas = prev_lf_mode;
        h.feature_enabled = prev_fe;
        h.feature_data = prev_fd;
        if h.primary_ref_frame == PRIMARY_REF_NONE {
            h.loop_filter_delta_enabled = true;
        }
        self.tile_info(&seq, &mut r, &mut h)?;
        // quantization_params()
        h.base_q_idx = r.f(8)? as u8;
        let read_dq =
            |r: &mut BitReader| -> Result<i8> { Ok(if r.flag()? { r.su(7)? as i8 } else { 0 }) };
        h.delta_q_y_dc = read_dq(&mut r)?;
        if seq.num_planes() > 1 {
            let diff_uv = if seq.separate_uv_delta_q {
                r.flag()?
            } else {
                false
            };
            h.delta_q_u_dc = read_dq(&mut r)?;
            h.delta_q_u_ac = read_dq(&mut r)?;
            if diff_uv {
                h.delta_q_v_dc = read_dq(&mut r)?;
                h.delta_q_v_ac = read_dq(&mut r)?;
            } else {
                h.delta_q_v_dc = h.delta_q_u_dc;
                h.delta_q_v_ac = h.delta_q_u_ac;
            }
        }
        h.using_qmatrix = r.flag()?;
        if h.using_qmatrix {
            h.qm_y = r.f(4)? as u8;
            h.qm_u = r.f(4)? as u8;
            h.qm_v = if !seq.separate_uv_delta_q {
                h.qm_u
            } else {
                r.f(4)? as u8
            };
        }
        // segmentation_params()
        h.segmentation_enabled = r.flag()?;
        if h.segmentation_enabled {
            if h.primary_ref_frame == PRIMARY_REF_NONE {
                h.segmentation_update_map = true;
                h.segmentation_temporal_update = false;
                h.segmentation_update_data = true;
            } else {
                h.segmentation_update_map = r.flag()?;
                if h.segmentation_update_map {
                    h.segmentation_temporal_update = r.flag()?;
                }
                h.segmentation_update_data = r.flag()?;
            }
            if h.segmentation_update_data {
                for i in 0..8 {
                    for j in 0..8 {
                        let en = r.flag()?;
                        h.feature_enabled[i][j] = en;
                        let mut v = 0i32;
                        if en {
                            let bits = SEG_FEATURE_BITS[j];
                            let lim = SEG_FEATURE_MAX[j];
                            v = if SEG_FEATURE_SIGNED[j] {
                                r.su(1 + bits)?.clamp(-lim, lim)
                            } else {
                                (r.f(bits)? as i32).clamp(0, lim)
                            };
                        }
                        h.feature_data[i][j] = v as i16;
                    }
                }
            }
        } else {
            h.feature_enabled = [[false; 8]; 8];
            h.feature_data = [[0; 8]; 8];
        }
        for i in 0..8 {
            for j in 0..8 {
                if h.feature_enabled[i][j] {
                    h.last_active_seg_id = i as u8;
                    if j >= 5 {
                        h.seg_id_pre_skip = true;
                    }
                }
            }
        }
        // delta_q_params / delta_lf_params
        if h.base_q_idx > 0 {
            h.delta_q_present = r.flag()?;
        }
        if h.delta_q_present {
            h.delta_q_res = r.f(2)? as u8;
        }
        if h.delta_q_present {
            if !h.allow_intrabc {
                h.delta_lf_present = r.flag()?;
            }
            if h.delta_lf_present {
                h.delta_lf_res = r.f(2)? as u8;
                h.delta_lf_multi = r.flag()?;
            }
        }
        // lossless
        h.coded_lossless = true;
        for seg in 0..8 {
            let q = qindex(&h, seg);
            let lossless = q == 0
                && h.delta_q_y_dc == 0
                && h.delta_q_u_ac == 0
                && h.delta_q_u_dc == 0
                && h.delta_q_v_ac == 0
                && h.delta_q_v_dc == 0;
            if !lossless {
                h.coded_lossless = false;
            }
        }
        h.all_lossless = h.coded_lossless && h.frame_width == h.upscaled_width;
        // loop_filter_params()
        if h.coded_lossless || h.allow_intrabc {
            h.loop_filter_ref_deltas = DEFAULT_LF_REF_DELTAS;
            h.loop_filter_mode_deltas = [0; 2];
        } else {
            h.loop_filter_level[0] = r.f(6)? as u8;
            h.loop_filter_level[1] = r.f(6)? as u8;
            if seq.num_planes() > 1 && (h.loop_filter_level[0] != 0 || h.loop_filter_level[1] != 0)
            {
                h.loop_filter_level[2] = r.f(6)? as u8;
                h.loop_filter_level[3] = r.f(6)? as u8;
            }
            h.loop_filter_sharpness = r.f(3)? as u8;
            h.loop_filter_delta_enabled = r.flag()?;
            if h.loop_filter_delta_enabled && r.flag()? {
                for i in 0..8 {
                    if r.flag()? {
                        h.loop_filter_ref_deltas[i] = r.su(7)? as i8;
                    }
                }
                for i in 0..2 {
                    if r.flag()? {
                        h.loop_filter_mode_deltas[i] = r.su(7)? as i8;
                    }
                }
            }
        }
        // cdef_params()
        if h.coded_lossless || h.allow_intrabc || !seq.enable_cdef {
            h.cdef_damping_minus_3 = 0;
        } else {
            h.cdef_damping_minus_3 = r.f(2)? as u8;
            h.cdef_bits = r.f(2)? as u8;
            for i in 0..(1usize << h.cdef_bits) {
                h.cdef_y_pri[i] = r.f(4)? as u8;
                h.cdef_y_sec[i] = r.f(2)? as u8;
                if seq.num_planes() > 1 {
                    h.cdef_uv_pri[i] = r.f(4)? as u8;
                    h.cdef_uv_sec[i] = r.f(2)? as u8;
                }
            }
        }
        // lr_params()
        if !(h.all_lossless || h.allow_intrabc || !seq.enable_restoration) {
            const REMAP: [u8; 4] = [0, 3, 1, 2];
            let (mut uses_lr, mut uses_chroma_lr) = (false, false);
            for i in 0..seq.num_planes() {
                let t = REMAP[r.f(2)? as usize];
                h.frame_restoration_type[i] = t;
                if t != 0 {
                    uses_lr = true;
                    if i > 0 {
                        uses_chroma_lr = true;
                    }
                }
            }
            if uses_lr {
                if seq.use_128x128_superblock {
                    h.lr_unit_shift = r.f(1)? as u8 + 1;
                } else {
                    h.lr_unit_shift = r.f(1)? as u8;
                    if h.lr_unit_shift != 0 {
                        h.lr_unit_shift += r.f(1)? as u8;
                    }
                }
                if seq.subsampling_x && seq.subsampling_y && uses_chroma_lr {
                    h.lr_uv_shift = r.f(1)? as u8;
                }
            }
        }
        // read_tx_mode()
        h.tx_mode = if h.coded_lossless {
            0
        } else if r.flag()? {
            2
        } else {
            1
        };
        // frame_reference_mode()
        h.reference_select = if h.frame_is_intra { false } else { r.flag()? };
        // skip_mode_params()
        let skip_allowed = self.skip_mode_allowed(&seq, &mut h);
        h.skip_mode_present = if skip_allowed { r.flag()? } else { false };
        h.allow_warped_motion =
            if h.frame_is_intra || h.error_resilient_mode || !seq.enable_warped_motion {
                false
            } else {
                r.flag()?
            };
        h.reduced_tx_set = r.flag()?;
        // global_motion_params()
        h.gm_params = [identity_gm(); 8];
        if !h.frame_is_intra {
            for rf in 1..8 {
                let t = if r.flag()? {
                    if r.flag()? {
                        GM_ROTZOOM
                    } else if r.flag()? {
                        GM_TRANSLATION
                    } else {
                        GM_AFFINE
                    }
                } else {
                    GM_IDENTITY
                };
                h.gm_type[rf] = t;
                if t >= GM_ROTZOOM {
                    h.gm_params[rf][2] = read_global_param(&mut r, &h, &prev_gm, t, rf, 2)?;
                    h.gm_params[rf][3] = read_global_param(&mut r, &h, &prev_gm, t, rf, 3)?;
                    if t == GM_AFFINE {
                        h.gm_params[rf][4] = read_global_param(&mut r, &h, &prev_gm, t, rf, 4)?;
                        h.gm_params[rf][5] = read_global_param(&mut r, &h, &prev_gm, t, rf, 5)?;
                    } else {
                        h.gm_params[rf][4] = -h.gm_params[rf][3];
                        h.gm_params[rf][5] = h.gm_params[rf][2];
                    }
                }
                if t >= GM_TRANSLATION {
                    h.gm_params[rf][0] = read_global_param(&mut r, &h, &prev_gm, t, rf, 0)?;
                    h.gm_params[rf][1] = read_global_param(&mut r, &h, &prev_gm, t, rf, 1)?;
                }
            }
        }
        // film_grain_params()
        if seq.film_grain_params_present && (h.show_frame || h.showable_frame) {
            self.film_grain_params(&seq, &mut r, &mut h)?;
        }
        h.header_bytes = r.bit_pos().div_ceil(8);
        Ok(h)
    }

    fn frame_size(
        &self,
        seq: &SequenceHeader,
        r: &mut BitReader,
        h: &mut FrameHeader,
    ) -> Result<()> {
        if h.frame_size_override_flag {
            h.frame_width = r.f(seq.frame_width_bits_minus_1 + 1)? + 1;
            h.frame_height = r.f(seq.frame_height_bits_minus_1 + 1)? + 1;
        } else {
            h.frame_width = seq.max_frame_width_minus_1 + 1;
            h.frame_height = seq.max_frame_height_minus_1 + 1;
        }
        superres_params(seq, r, h)?;
        compute_image_size(h);
        Ok(())
    }

    fn render_size(&self, r: &mut BitReader, h: &mut FrameHeader) -> Result<()> {
        if r.flag()? {
            h.render_width = r.f(16)? + 1;
            h.render_height = r.f(16)? + 1;
        } else {
            h.render_width = h.upscaled_width;
            h.render_height = h.frame_height;
        }
        Ok(())
    }

    fn tile_info(
        &self,
        seq: &SequenceHeader,
        r: &mut BitReader,
        h: &mut FrameHeader,
    ) -> Result<()> {
        let (sb_cols, sb_rows, sb_shift) = if seq.use_128x128_superblock {
            ((h.mi_cols + 31) >> 5, (h.mi_rows + 31) >> 5, 5)
        } else {
            ((h.mi_cols + 15) >> 4, (h.mi_rows + 15) >> 4, 4)
        };
        let sb_size = sb_shift + 2;
        let max_tile_width_sb = 4096 >> sb_size;
        let mut max_tile_area_sb = (4096 * 2304) >> (2 * sb_size);
        let min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
        let max_log2_tile_cols = tile_log2(1, sb_cols.min(64));
        let max_log2_tile_rows = tile_log2(1, sb_rows.min(64));
        let min_log2_tiles = min_log2_tile_cols.max(tile_log2(max_tile_area_sb, sb_rows * sb_cols));
        let t = &mut h.tile;
        t.col_start_sb.clear();
        t.row_start_sb.clear();
        if r.flag()? {
            t.tile_cols_log2 = min_log2_tile_cols;
            while t.tile_cols_log2 < max_log2_tile_cols && r.flag()? {
                t.tile_cols_log2 += 1;
            }
            let w = (sb_cols + (1 << t.tile_cols_log2) - 1) >> t.tile_cols_log2;
            let mut s = 0;
            while s < sb_cols {
                t.col_start_sb.push(s);
                s += w;
            }
            t.col_start_sb.push(sb_cols);
            let min_log2_tile_rows = min_log2_tiles.saturating_sub(t.tile_cols_log2);
            t.tile_rows_log2 = min_log2_tile_rows;
            while t.tile_rows_log2 < max_log2_tile_rows && r.flag()? {
                t.tile_rows_log2 += 1;
            }
            let hh = (sb_rows + (1 << t.tile_rows_log2) - 1) >> t.tile_rows_log2;
            let mut s = 0;
            while s < sb_rows {
                t.row_start_sb.push(s);
                s += hh;
            }
            t.row_start_sb.push(sb_rows);
        } else {
            let mut widest = 0;
            let mut s = 0;
            while s < sb_cols {
                t.col_start_sb.push(s);
                let max_w = (sb_cols - s).min(max_tile_width_sb);
                let size = r.ns(max_w)? + 1;
                widest = widest.max(size);
                s += size;
            }
            t.col_start_sb.push(sb_cols);
            t.tile_cols_log2 = tile_log2(1, (t.col_start_sb.len() - 1) as u32);
            max_tile_area_sb = if min_log2_tiles > 0 {
                (sb_rows * sb_cols) >> (min_log2_tiles + 1)
            } else {
                sb_rows * sb_cols
            };
            let max_h = (max_tile_area_sb / widest).max(1);
            let mut s = 0;
            while s < sb_rows {
                t.row_start_sb.push(s);
                let mh = (sb_rows - s).min(max_h);
                s += r.ns(mh)? + 1;
            }
            t.row_start_sb.push(sb_rows);
            t.tile_rows_log2 = tile_log2(1, (t.row_start_sb.len() - 1) as u32);
        }
        t.tile_cols = (t.col_start_sb.len() - 1) as u32;
        t.tile_rows = (t.row_start_sb.len() - 1) as u32;
        if t.tile_cols_log2 > 0 || t.tile_rows_log2 > 0 {
            t.context_update_tile_id = r.f(t.tile_rows_log2 + t.tile_cols_log2)?;
            t.tile_size_bytes = r.f(2)? + 1;
        } else {
            t.context_update_tile_id = 0;
            t.tile_size_bytes = 4;
        }
        Ok(())
    }

    fn skip_mode_allowed(&self, seq: &SequenceHeader, h: &mut FrameHeader) -> bool {
        if h.frame_is_intra || !h.reference_select || !seq.enable_order_hint {
            return false;
        }
        let (mut fwd, mut bwd) = (-1i32, -1i32);
        let (mut fwd_hint, mut bwd_hint) = (0, 0);
        for i in 0..REFS_PER_FRAME {
            let rh = self.refs[h.ref_frame_idx[i] as usize].order_hint;
            if get_relative_dist(seq, rh, h.order_hint) < 0 {
                if fwd < 0 || get_relative_dist(seq, rh, fwd_hint) > 0 {
                    fwd = i as i32;
                    fwd_hint = rh;
                }
            } else if get_relative_dist(seq, rh, h.order_hint) > 0
                && (bwd < 0 || get_relative_dist(seq, rh, bwd_hint) < 0)
            {
                bwd = i as i32;
                bwd_hint = rh;
            }
        }
        if fwd < 0 {
            return false;
        }
        if bwd >= 0 {
            h.skip_mode_frame = [1 + fwd.min(bwd) as u8, 1 + fwd.max(bwd) as u8];
            return true;
        }
        let (mut sfwd, mut sfwd_hint) = (-1i32, 0);
        for i in 0..REFS_PER_FRAME {
            let rh = self.refs[h.ref_frame_idx[i] as usize].order_hint;
            if get_relative_dist(seq, rh, fwd_hint) < 0
                && (sfwd < 0 || get_relative_dist(seq, rh, sfwd_hint) > 0)
            {
                sfwd = i as i32;
                sfwd_hint = rh;
            }
        }
        if sfwd < 0 {
            return false;
        }
        h.skip_mode_frame = [1 + fwd.min(sfwd) as u8, 1 + fwd.max(sfwd) as u8];
        true
    }

    fn film_grain_params(
        &self,
        seq: &SequenceHeader,
        r: &mut BitReader,
        h: &mut FrameHeader,
    ) -> Result<()> {
        let mut g = FilmGrainParams {
            apply_grain: r.flag()?,
            ..Default::default()
        };
        if !g.apply_grain {
            h.film_grain = FilmGrainParams::default();
            return Ok(());
        }
        g.grain_seed = r.f(16)? as u16;
        g.update_grain = if h.frame_type == INTER_FRAME {
            r.flag()?
        } else {
            true
        };
        if !g.update_grain {
            let idx = r.f(3)? as usize;
            let seed = g.grain_seed;
            g = self.refs[idx].film_grain;
            g.grain_seed = seed;
            h.film_grain = g;
            return Ok(());
        }
        g.num_y_points = r.f(4)? as u8;
        if g.num_y_points > 14 {
            return Err(Av1Error::Invalid("num_y_points"));
        }
        for i in 0..g.num_y_points as usize {
            g.point_y_value[i] = r.f(8)? as u8;
            g.point_y_scaling[i] = r.f(8)? as u8;
        }
        g.chroma_scaling_from_luma = if seq.mono_chrome { false } else { r.flag()? };
        if !(seq.mono_chrome
            || g.chroma_scaling_from_luma
            || (seq.subsampling_x && seq.subsampling_y && g.num_y_points == 0))
        {
            g.num_cb_points = r.f(4)? as u8;
            if g.num_cb_points > 10 {
                return Err(Av1Error::Invalid("num_cb_points"));
            }
            for i in 0..g.num_cb_points as usize {
                g.point_cb_value[i] = r.f(8)? as u8;
                g.point_cb_scaling[i] = r.f(8)? as u8;
            }
            g.num_cr_points = r.f(4)? as u8;
            if g.num_cr_points > 10 {
                return Err(Av1Error::Invalid("num_cr_points"));
            }
            for i in 0..g.num_cr_points as usize {
                g.point_cr_value[i] = r.f(8)? as u8;
                g.point_cr_scaling[i] = r.f(8)? as u8;
            }
        }
        g.grain_scaling_minus_8 = r.f(2)? as u8;
        g.ar_coeff_lag = r.f(2)? as u8;
        let lag = g.ar_coeff_lag as usize;
        let num_pos_luma = 2 * lag * (lag + 1);
        let num_pos_chroma = if g.num_y_points > 0 {
            num_pos_luma + 1
        } else {
            num_pos_luma
        };
        if g.num_y_points > 0 {
            for i in 0..num_pos_luma {
                g.ar_coeffs_y_plus_128[i] = r.f(8)? as u8;
            }
        }
        if g.chroma_scaling_from_luma || g.num_cb_points > 0 {
            for i in 0..num_pos_chroma {
                g.ar_coeffs_cb_plus_128[i] = r.f(8)? as u8;
            }
        }
        if g.chroma_scaling_from_luma || g.num_cr_points > 0 {
            for i in 0..num_pos_chroma {
                g.ar_coeffs_cr_plus_128[i] = r.f(8)? as u8;
            }
        }
        g.ar_coeff_shift_minus_6 = r.f(2)? as u8;
        g.grain_scale_shift = r.f(2)? as u8;
        if g.num_cb_points > 0 {
            g.cb_mult = r.f(8)? as u8;
            g.cb_luma_mult = r.f(8)? as u8;
            g.cb_offset = r.f(9)? as u16;
        }
        if g.num_cr_points > 0 {
            g.cr_mult = r.f(8)? as u8;
            g.cr_luma_mult = r.f(8)? as u8;
            g.cr_offset = r.f(9)? as u16;
        }
        g.overlap_flag = r.flag()?;
        g.clip_to_restricted_range = r.flag()?;
        h.film_grain = g;
        Ok(())
    }

    /// Reference frame update process (spec 7.20) for a decoded frame.
    pub fn update_refs(&mut self, h: &FrameHeader) {
        if h.show_existing_frame {
            if h.frame_type == KEY_FRAME {
                // 7.21: load the shown key frame's state, then refresh every slot with it.
                let s = self.refs[h.frame_to_show_map_idx as usize];
                for slot in self.refs.iter_mut() {
                    *slot = s;
                }
            }
            return;
        }
        let mut saved_hints = [0u32; 8];
        saved_hints[1..8].copy_from_slice(&h.order_hints);
        let s = RefSlotState {
            valid: true,
            frame_type: h.frame_type,
            upscaled_width: h.upscaled_width,
            frame_width: h.frame_width,
            frame_height: h.frame_height,
            render_width: h.render_width,
            render_height: h.render_height,
            mi_cols: h.mi_cols,
            mi_rows: h.mi_rows,
            order_hint: h.order_hint,
            saved_order_hints: saved_hints,
            gm_params: h.gm_params,
            lf_ref_deltas: h.loop_filter_ref_deltas,
            lf_mode_deltas: h.loop_filter_mode_deltas,
            feature_enabled: h.feature_enabled,
            feature_data: h.feature_data,
            film_grain: h.film_grain,
            showable_frame: h.showable_frame,
        };
        for i in 0..NUM_REF_FRAMES {
            if h.refresh_frame_flags & (1 << i) != 0 {
                self.refs[i] = s;
            }
        }
    }
}

fn superres_params(seq: &SequenceHeader, r: &mut BitReader, h: &mut FrameHeader) -> Result<()> {
    h.use_superres = if seq.enable_superres {
        r.flag()?
    } else {
        false
    };
    h.superres_denom = if h.use_superres { r.f(3)? + 9 } else { 8 };
    h.upscaled_width = h.frame_width;
    h.frame_width = (h.upscaled_width * 8 + h.superres_denom / 2) / h.superres_denom;
    Ok(())
}

fn compute_image_size(h: &mut FrameHeader) {
    h.mi_cols = 2 * ((h.frame_width + 7) >> 3);
    h.mi_rows = 2 * ((h.frame_height + 7) >> 3);
}

/// get_qindex(ignoreDeltaQ = 1, segmentId) (spec 7.12.2).
pub fn qindex(h: &FrameHeader, seg: usize) -> i32 {
    if h.segmentation_enabled && h.feature_enabled[seg][0] {
        (h.base_q_idx as i32 + h.feature_data[seg][0] as i32).clamp(0, 255)
    } else {
        h.base_q_idx as i32
    }
}

fn read_global_param(
    r: &mut BitReader,
    h: &FrameHeader,
    prev: &[[i32; 6]; 8],
    t: u8,
    rf: usize,
    idx: usize,
) -> Result<i32> {
    let (mut abs_bits, mut prec_bits) = (12u32, 15u32);
    if idx < 2 {
        if t == GM_TRANSLATION {
            let hp = !h.allow_high_precision_mv as u32;
            abs_bits = 9 - hp;
            prec_bits = 3 - hp;
        } else {
            abs_bits = 12;
            prec_bits = 6;
        }
    }
    let prec_diff = WARPEDMODEL_PREC_BITS - prec_bits;
    let round = if idx % 3 == 2 {
        1i32 << WARPEDMODEL_PREC_BITS
    } else {
        0
    };
    let sub = if idx % 3 == 2 { 1i32 << prec_bits } else { 0 };
    let mx = 1i32 << abs_bits;
    let rr = (prev[rf][idx] >> prec_diff) - sub;
    Ok((decode_signed_subexp_with_ref(r, -mx, mx + 1, rr)? << prec_diff) + round)
}

/// One tile's byte range inside the concatenated tile-group payloads of a frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TileRange {
    pub start: u32,
    pub end: u32,
}

/// Parse one tile group OBU payload (spec 5.11.1); appends its tile data to `bitstream` and the tile ranges
/// (relative to `bitstream`, size fields skipped) to `tiles`. Returns true when the last tile was seen.
pub fn parse_tile_group(
    h: &FrameHeader,
    data: &[u8],
    bitstream: &mut Vec<u8>,
    tiles: &mut Vec<TileRange>,
) -> Result<bool> {
    let t = &h.tile;
    let num_tiles = t.tile_cols * t.tile_rows;
    let mut r = BitReader::new(data);
    let flag = if num_tiles > 1 { r.flag()? } else { false };
    let (start, end) = if num_tiles == 1 || !flag {
        (0, num_tiles - 1)
    } else {
        let bits = t.tile_cols_log2 + t.tile_rows_log2;
        (r.f(bits)?, r.f(bits)?)
    };
    r.byte_alignment();
    let hdr = r.bit_pos() / 8;
    let payload = data.get(hdr..).ok_or(Av1Error::Truncated("tile group"))?;
    let base = bitstream.len() as u32;
    bitstream.extend_from_slice(payload);
    let mut pos = 0usize;
    for n in start..=end {
        if n == end {
            tiles.push(TileRange {
                start: base + pos as u32,
                end: base + payload.len() as u32,
            });
        } else {
            let tsb = t.tile_size_bytes as usize;
            let b = payload
                .get(pos..pos + tsb)
                .ok_or(Av1Error::Truncated("tile_size_minus_1"))?;
            let mut sz = 0usize;
            for (i, v) in b.iter().enumerate() {
                sz |= (*v as usize) << (8 * i);
            }
            sz += 1;
            pos += tsb;
            if pos + sz > payload.len() {
                return Err(Av1Error::Truncated("tile data"));
            }
            tiles.push(TileRange {
                start: base + pos as u32,
                end: base + (pos + sz) as u32,
            });
            pos += sz;
        }
    }
    Ok(end == num_tiles - 1)
}
