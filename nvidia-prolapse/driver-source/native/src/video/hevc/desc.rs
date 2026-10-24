//! `nvdec_hevc_pic_s` (NVIDIA MIT header `nvdec_drv.h`, 276 bytes) + the Blackwell tail, the
//! scaling-list block and the tile-size table, as one per-picture setup buffer.
//!
//! Field values follow the stock driver's (615.71.09) setup as observed in captures of 8- and
//! 10-bit streams; this file implements the values
//! independently. Buffer offsets inside our own filter buffer and surface geometry are inputs
//! ([`SessionInputs`]), never stock's numbers.
use super::ps::{diag_scan, Pps, ScalingList, Sps};
use crate::video::{bitstream, unsupported, VResult};

/// Size of the descriptor area (struct + tail, zero padded).
pub const DESC_SIZE: usize = 0x200;
/// HEVC_SET_SCALING_LIST (method 0x580) target: stock writes the block at setup + 0x200
/// (observed in hevc10.frames / hevc-cqm.frames: 8 DC bytes, 8 zero, 6×16 4x4, 6×64 8x8,
/// 6×64 16x16, 2×64 32x32; see [`write_scaling`] for the element order).
pub const SCALING_LIST_OFFSET: usize = 0x200;
pub const SCALING_LIST_SIZE: usize = 16 * 64; // GIP_ASIC_SCALING_LIST_SIZE
/// HEVC_SET_TILE_SIZES (method 0x584) target: setup + 0x600, right after the scaling block, as
/// stock (hevc-tiles-4k.frames). u16 (column width, row height) in CTBs, column-major; one entry
/// when tiles are off.
pub const TILE_SIZES_OFFSET: usize = SCALING_LIST_OFFSET + SCALING_LIST_SIZE;
pub const TILE_SIZES_SIZE: usize = (20 * 22 * 2 * 2 + 16 + 15) & !0xf; // GIP_ASIC_TILE_SIZE
/// Whole per-picture setup buffer.
pub const SETUP_SIZE: usize = TILE_SIZES_OFFSET + TILE_SIZES_SIZE;

/// Filter-buffer sub-offsets in 256-byte units, from OUR session's filter-buffer layout.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct FilterOffsets {
    /// desc+0x50 HevcSaoBufferOffset
    pub sao: u32,
    /// desc+0x54 HevcBsdCtrlOffset
    pub bsd_ctrl: u32,
    /// desc+0xf0 v1 HevcFltAboveOffset
    pub flt_above: u32,
    /// desc+0xf4 v1 HevcSaoAboveOffset
    pub sao_above: u32,
    /// desc+0x104 v3 HevcSliceEdgeOffset (stock: = sao_above)
    pub slice_edge: u32,
}

/// Values owned by the session (surfaces, buffers, engine limits), not by the bitstream.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SessionInputs {
    /// desc+0x44/0x48 framestride[2] (luma, chroma) of OUR decode surfaces.
    pub framestride: [u32; 2],
    pub filter: FilterOffsets,
    /// Clamp for gptimer_timeout_value (stock: a per-session limit; u32::MAX = none).
    pub gptimer_max: u32,
    /// desc+0x40: tileformat (bits 0-1) | gob_height (bits 2-4) of OUR surfaces (stock: 1 = block linear).
    pub general_bits: u8,
    /// desc+0x11a: stock takes this byte from an internal format query (observed 0).
    pub tail_11a: u8,
    /// The NVDEC class has the stock `blackwell_b` gate (caps & 0x1ec00000: D1B0/CFB0/D2B0): only then
    /// stock writes +0x71 plane kinds, +0xba format code, +0x11c width word and +0x120 = 0xffff
    /// (stock ELF 0x3e41d, 0x3e44f). Other classes leave them 0.
    pub blackwell_b: bool,
}

/// Bytes the stock driver writes whose meaning is not established (observed per bit depth;
/// see `OURS_VS_STOCK` reserved6 and the hevc10 capture). Copied, not interpreted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StockOpaque {
    pub reserved6: u8,
    pub reserved7: u16,
}
impl StockOpaque {
    /// Observed: 8-bit streams reserved6 = 9, reserved7 = 0; 10-bit reserved6 = 2, reserved7 = 3.
    /// These are the plane GOB kinds `luma | chroma << 2` and the stock surface-format code, the same
    /// pair VP9 (+0xeb, +0x43) and AV1 (stat word) carry (8/10-bit traces of all three codecs).
    pub fn observed(bit_depth_luma: u32) -> Self {
        let bps = crate::video::bytes_per_sample(bit_depth_luma);
        Self {
            reserved6: crate::video::plane_kind_bits(bps) as u8,
            reserved7: crate::video::surface_format_code(bit_depth_luma) as u16,
        }
    }
}

/// Per-picture values the decoder state supplies.
#[derive(Debug, Clone)]
pub struct PicState {
    pub stream_len: u32,
    pub idr: bool,
    pub irap: bool,
    pub curr_slot: u8,
    /// NumPocTotalCurr (no long-term, no SCC).
    pub num_poc_total_curr: u32,
    pub list0: [u8; 16],
    pub list1: [u8; 16],
    /// RefDiffPicOrderCnts indexed by slot (current slot 0).
    pub diff: [i16; 16],
    pub sw_hdr_skip_length: u16,
    pub num_bits_st_rps: u32,
    pub num_delta_pocs_ref: u32,
}

fn put32(d: &mut [u8], o: usize, v: u32) {
    d[o..o + 4].copy_from_slice(&v.to_le_bytes());
}
fn put16(d: &mut [u8], o: usize, v: u16) {
    d[o..o + 2].copy_from_slice(&v.to_le_bytes());
}

/// Bitfield packer with range checks (a value that does not fit is a bitstream we cannot describe).
struct Bits(u32);
impl Bits {
    fn set(&mut self, shift: u32, width: u32, v: u32, name: &str) -> VResult<()> {
        if v >= 1 << width {
            return bitstream(format!(
                "{name} = {v} does not fit its {width}-bit descriptor field"
            ));
        }
        self.0 |= v << shift;
        Ok(())
    }
}

/// Builds the setup buffer ([`SETUP_SIZE`] bytes).
pub fn build(
    sps: &Sps,
    pps: &Pps,
    st: &PicState,
    si: &SessionInputs,
    op: StockOpaque,
) -> VResult<Vec<u8>> {
    if sps.pcm {
        return unsupported(
            "HEVC PCM (pcm_* descriptor field encoding not verified against stock)",
        );
    }
    let mut d = vec![0u8; SETUP_SIZE];
    // 0x30 stream_len: stock max(nBitstreamDataLen, 16) (stock ELF 0x3e364).
    put32(&mut d, 0x30, st.stream_len.max(16));
    // 0x3c gptimer: max(W/16 * H/16, 8100) * 10000, clamped (stock ELF 0x3e817;
    // the MB counts are the truncating ones CUVID consumers pass).
    let mbs = ((sps.width / 16) * (sps.height / 16)).max(8100);
    put32(&mut d, 0x3c, mbs.wrapping_mul(10000).min(si.gptimer_max));
    d[0x40] = si.general_bits;
    d[0x41] = 1; // sw_start_code_e: slices carry 00 00 01
    put32(&mut d, 0x44, si.framestride[0]);
    put32(&mut d, 0x48, si.framestride[1]);
    // colMvBuffersize: 64-aligned picture, 16 bytes per 16x16 block, in 256-byte units.
    let colmv = (sps.width.div_ceil(64) * 64) * (sps.height.div_ceil(64) * 64) / 16 / 256;
    put32(&mut d, 0x4c, colmv);
    put32(&mut d, 0x50, si.filter.sao);
    put32(&mut d, 0x54, si.filter.bsd_ctrl);
    if sps.width > 0xffff || sps.height > 0xffff {
        return bitstream("picture size exceeds 16 bits");
    }
    put16(&mut d, 0x58, sps.width as u16);
    put16(&mut d, 0x5a, sps.height as u16);
    let mut b = Bits(0);
    b.set(0, 4, sps.chroma_format_idc, "chroma_format_idc")?;
    b.set(4, 4, sps.bit_depth_luma, "bit_depth_luma")?;
    b.set(8, 4, sps.bit_depth_chroma, "bit_depth_chroma")?;
    b.set(12, 4, sps.log2_min_cb, "log2_min_luma_coding_block_size")?;
    b.set(16, 4, sps.log2_ctb, "log2_max_luma_coding_block_size")?;
    b.set(20, 4, sps.log2_min_tb, "log2_min_transform_block_size")?;
    b.set(24, 4, sps.log2_max_tb, "log2_max_transform_block_size")?;
    put32(&mut d, 0x5c, b.0);
    let scaling = pps.scaling_list.as_ref().or(sps.scaling_list.as_ref());
    let mut b = Bits(0);
    b.set(
        0,
        3,
        sps.max_th_depth_inter,
        "max_transform_hierarchy_depth_inter",
    )?;
    b.set(
        3,
        3,
        sps.max_th_depth_intra,
        "max_transform_hierarchy_depth_intra",
    )?;
    b.set(
        6,
        1,
        sps.scaling_list_enabled as u32,
        "scaling_list_enabled_flag",
    )?;
    b.set(7, 1, sps.amp as u32, "amp_enabled_flag")?;
    b.set(8, 1, sps.sao as u32, "sample_adaptive_offset_enabled_flag")?;
    b.set(
        27,
        1,
        sps.temporal_mvp as u32,
        "sps_temporal_mvp_enabled_flag",
    )?;
    b.set(
        28,
        1,
        sps.strong_intra_smoothing as u32,
        "strong_intra_smoothing_enabled_flag",
    )?;
    put32(&mut d, 0x60, b.0);
    let mut b = Bits(0);
    b.set(
        0,
        1,
        pps.dependent_slice_segments_enabled as u32,
        "dependent_slice_segments_enabled_flag",
    )?;
    b.set(
        1,
        1,
        pps.output_flag_present as u32,
        "output_flag_present_flag",
    )?;
    b.set(
        2,
        3,
        pps.num_extra_slice_header_bits,
        "num_extra_slice_header_bits",
    )?;
    b.set(
        5,
        1,
        pps.sign_data_hiding as u32,
        "sign_data_hiding_enabled_flag",
    )?;
    b.set(
        6,
        1,
        pps.cabac_init_present as u32,
        "cabac_init_present_flag",
    )?;
    b.set(
        7,
        4,
        pps.num_ref_idx_l0_default,
        "num_ref_idx_l0_default_active",
    )?;
    b.set(
        11,
        4,
        pps.num_ref_idx_l1_default,
        "num_ref_idx_l1_default_active",
    )?;
    // init_qp = 26 + init_qp_minus26 + QpBdOffsetY (hevc10 capture: 38 for init_qp_minus26 0, 10-bit).
    let init_qp = 26 + pps.init_qp_minus26 + 6 * (sps.bit_depth_luma as i32 - 8);
    b.set(15, 7, init_qp as u32, "init_qp")?;
    b.set(
        22,
        1,
        pps.constrained_intra_pred as u32,
        "constrained_intra_pred_flag",
    )?;
    b.set(
        23,
        1,
        pps.transform_skip as u32,
        "transform_skip_enabled_flag",
    )?;
    b.set(24, 1, pps.cu_qp_delta as u32, "cu_qp_delta_enabled_flag")?;
    b.set(25, 2, pps.diff_cu_qp_delta_depth, "diff_cu_qp_delta_depth")?;
    put32(&mut d, 0x64, b.0);
    d[0x68] = pps.cb_qp_offset as i8 as u8;
    d[0x69] = pps.cr_qp_offset as i8 as u8;
    d[0x6a] = pps.beta_offset_div2 as i8 as u8;
    d[0x6b] = pps.tc_offset_div2 as i8 as u8;
    let mut b = Bits(0);
    b.set(
        0,
        1,
        pps.slice_chroma_qp_offsets_present as u32,
        "pps_slice_chroma_qp_offsets_present_flag",
    )?;
    b.set(1, 1, pps.weighted_pred as u32, "weighted_pred_flag")?;
    b.set(2, 1, pps.weighted_bipred as u32, "weighted_bipred_flag")?;
    b.set(
        3,
        1,
        pps.transquant_bypass as u32,
        "transquant_bypass_enabled_flag",
    )?;
    b.set(4, 1, pps.tiles.is_some() as u32, "tiles_enabled_flag")?;
    b.set(
        5,
        1,
        pps.entropy_coding_sync as u32,
        "entropy_coding_sync_enabled_flag",
    )?;
    if let Some(t) = &pps.tiles {
        b.set(6, 5, t.cols, "num_tile_columns")?;
        b.set(11, 5, t.rows, "num_tile_rows")?;
    }
    // loop_filter_across_tiles_enabled_flag is inferred 1 when absent (7.4.3.3.1), as stock writes it.
    let lfat = pps
        .tiles
        .as_ref()
        .is_none_or(|t| t.loop_filter_across_tiles);
    b.set(16, 1, lfat as u32, "loop_filter_across_tiles_enabled_flag")?;
    b.set(
        17,
        1,
        pps.loop_filter_across_slices as u32,
        "pps_loop_filter_across_slices_enabled_flag",
    )?;
    b.set(
        18,
        1,
        pps.deblocking_control_present as u32,
        "deblocking_filter_control_present_flag",
    )?;
    b.set(
        19,
        1,
        pps.deblocking_override_enabled as u32,
        "deblocking_filter_override_enabled_flag",
    )?;
    b.set(
        20,
        1,
        pps.deblocking_disabled as u32,
        "pps_deblocking_filter_disabled_flag",
    )?;
    b.set(
        21,
        1,
        pps.lists_modification_present as u32,
        "lists_modification_present_flag",
    )?;
    b.set(
        22,
        3,
        pps.log2_parallel_merge_level,
        "log2_parallel_merge_level",
    )?;
    b.set(
        25,
        1,
        pps.slice_header_extension_present as u32,
        "slice_segment_header_extension_present_flag",
    )?;
    put32(&mut d, 0x6c, b.0);
    // 0x70 num_ref_frames = min(NumPocTotalCurr, 8) (stock ELF 0x3e795).
    let nrf = st.num_poc_total_curr.min(8) as u8;
    d[0x70] = nrf;
    if si.blackwell_b {
        d[0x71] = op.reserved6;
    }
    put16(&mut d, 0x72, 0); // longtermflag: long-term references are rejected in the slice parser
    d[0x74..0x84].copy_from_slice(&st.list0);
    d[0x84..0x94].copy_from_slice(&st.list1);
    for (i, v) in st.diff.iter().enumerate() {
        put16(&mut d, 0x94 + 2 * i, *v as u16);
    }
    d[0xb4] = st.idr as u8;
    d[0xb5] = st.irap as u8;
    d[0xb6] = st.curr_slot;
    d[0xb7] = 0; // pattern_id: stock never writes it
    put16(&mut d, 0xb8, st.sw_hdr_skip_length);
    if si.blackwell_b {
        put16(&mut d, 0xba, op.reserved7);
    }
    let mut b = Bits(0);
    b.set(
        0,
        1,
        sps.separate_colour_plane as u32,
        "separate_colour_plane_flag",
    )?;
    b.set(
        1,
        4,
        sps.log2_max_poc_lsb - 4,
        "log2_max_pic_order_cnt_lsb_minus4",
    )?;
    b.set(5, 7, sps.st_rps.len() as u32, "num_short_term_ref_pic_sets")?;
    b.set(
        12,
        6,
        sps.num_long_term_ref_pics_sps,
        "num_long_term_ref_pics_sps",
    )?;
    b.set(19, 8, st.num_delta_pocs_ref, "num_delta_pocs_of_rps_idx")?;
    b.set(
        27,
        1,
        sps.long_term_ref_pics_present as u32,
        "long_term_ref_pics_present_flag",
    )?;
    put32(&mut d, 0xd4, b.0);
    put32(&mut d, 0xd8, st.num_bits_st_rps);
    // v1 main10_444 pps word: log2MaxTransformSkipSize = log2_max_transform_skip_block_size_minus2 + 2
    // (= 2 without the range extension; stock writes (old & 0xfffe07f0) | 2).
    put32(&mut d, 0xe0, 2);
    put32(&mut d, 0xf0, si.filter.flt_above);
    put32(&mut d, 0xf4, si.filter.sao_above);
    // v3: slice_ec_mv_type = 1 (co-located), slice_ec_slice_type bit 5 for pictures without refs.
    put32(&mut d, 0x100, 4 | (((nrf == 0) as u32) << 5));
    put32(&mut d, 0x104, si.filter.slice_edge);
    // Tail beyond the public struct (stock ELF 0x3e44f): 0x11a byte, 0x11c, 0x120.
    d[0x11a] = si.tail_11a;
    if si.blackwell_b {
        put32(
            &mut d,
            0x11c,
            ((((sps.width as i32) * 2 + 0x7e) & 0x3ff80) + 0x13f) as u32 >> 8,
        );
        put16(&mut d, 0x120, 0xffff);
    }
    if let Some(sl) = scaling.filter(|_| sps.scaling_list_enabled) {
        write_scaling(
            &mut d[SCALING_LIST_OFFSET..SCALING_LIST_OFFSET + SCALING_LIST_SIZE],
            sl,
        );
    }
    // Tile-size table: one 4-byte entry per tile in raster-scan order (rows outer, columns
    // inner), entry = {u16 column width in CTBs, u16 row height in CTBs}; the rest of the
    // 0x700-byte area stays zero. Stock libnvcuvid 615.78.08 builds exactly this at ELF
    // 0x3ec2b..0x3ecfc (uniform spacing: width = ((i+1)*W)/n - (i*W)/n per column, height by
    // the same 6-3 split over the rows) and 0x3eef2..0x3f01c (explicit column_width_minus1 /
    // row_height_minus1 + 1, last column/row = the remainder), then copies the table to
    // setup+0x600 (0x3eb43..0x3ec03); with tiles off it writes the single {W, H} entry
    // (0x3eba7..0x3ebb7).
    let (cols, rows) = match &pps.tiles {
        Some(t) => t.sizes(sps.width_ctbs(), sps.height_ctbs()),
        None => (vec![sps.width_ctbs()], vec![sps.height_ctbs()]),
    };
    let mut o = TILE_SIZES_OFFSET;
    for &r in &rows {
        for &c in &cols {
            put16(&mut d, o, c as u16);
            put16(&mut d, o + 2, r as u16);
            o += 4;
        }
    }
    Ok(d)
}

/// Engine scaling block (observed layout, see [`SCALING_LIST_OFFSET`]): DC[16x16 ×6, 32x32 ×2],
/// 8 zero bytes, then 4x4 ×6, 8x8 ×6, 16x16 ×6, 32x32 (matrixId 0, 3), each stored as the spec's
/// `ScalingFactor[x][y]` array (x = horizontal position, so byte `x * n + y`): verified with the
/// asymmetric scaling lists of a captured stream.
fn write_scaling(out: &mut [u8], sl: &ScalingList) {
    out[..6].copy_from_slice(&sl.dc[0]);
    out[6] = sl.dc[1][0];
    out[7] = sl.dc[1][3];
    let d4 = diag_scan(4);
    let d8 = diag_scan(8);
    let mut o = 16;
    for m in 0..6 {
        for (i, &(x, y)) in d4.iter().enumerate() {
            out[o + x * 4 + y] = sl.lists[0][m][i];
        }
        o += 16;
    }
    for size in 1..4 {
        let ms: &[usize] = if size == 3 {
            &[0, 3]
        } else {
            &[0, 1, 2, 3, 4, 5]
        };
        for &m in ms {
            for (i, &(x, y)) in d8.iter().enumerate() {
                out[o + x * 8 + y] = sl.lists[size][m][i];
            }
            o += 64;
        }
    }
}
