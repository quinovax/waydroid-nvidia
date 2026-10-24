//! H.264 front-end tests. The stock comparison reads captures that are not in this repository
//! (stock libnvcuvid 615.71.09 setups paired with the CUVIDPICPARAMS it received) from the
//! directory in `NVDEC_STOCK_EVIDENCE`; run with `cargo test -- --ignored`.
use super::*;
use crate::video::bits::split_annexb;

pub(crate) struct StockFrame {
    pub app: u32,
    pub ctrl: u32,
    pub slot: u32,
    pub pp: Vec<u8>,
    pub desc: Vec<u8>,
}

/// Parses a `.frames` capture into (PP list in decode order, method blocks).
pub(crate) fn load_frames(text: &str) -> (Vec<Vec<u8>>, Vec<StockFrame>) {
    let hex = |s: &str| {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect::<Vec<u8>>()
    };
    let mut pps = Vec::new();
    let mut blocks: Vec<StockFrame> = Vec::new();
    for l in text.lines() {
        if let Some(r) = l.strip_prefix("FRAME ") {
            let t: Vec<&str> = r.split_whitespace().collect();
            blocks.push(StockFrame {
                app: u32::from_str_radix(t[2], 16).unwrap(),
                ctrl: u32::from_str_radix(t[4], 16).unwrap(),
                slot: t[6].parse().unwrap(),
                pp: vec![],
                desc: vec![],
            });
        } else if let Some(r) = l.strip_prefix("PP ") {
            pps.push(hex(r));
            blocks.last_mut().unwrap().pp = hex(r);
        } else if let Some(r) = l.strip_prefix("DESC ") {
            blocks.last_mut().unwrap().desc = hex(r);
        }
    }
    (pps, blocks)
}

pub(crate) fn evidence(name: &str) -> Vec<u8> {
    let dir = std::env::var("NVDEC_STOCK_EVIDENCE").expect(
        "set NVDEC_STOCK_EVIDENCE=<stock captures dir> for the stock comparison",
    );
    std::fs::read(format!("{dir}/{name}")).unwrap_or_else(|e| panic!("{dir}/{name}: {e}"))
}

/// Groups Annex-B NAL units into access units (7.4.1.2.3: AUD/SPS/PPS/SEI or a slice with
/// first_mb_in_slice = 0 after a slice starts a new one).
pub(crate) fn access_units(data: &[u8]) -> Vec<&[u8]> {
    let nals = split_annexb(data);
    let mut out = Vec::new();
    let (mut start, mut has_slice) = (0usize, false);
    for n in &nals {
        let t = n.nal[0] & 0x1f;
        let first_slice = (t == 1 || t == 5) && n.nal.len() > 1 && n.nal[1] & 0x80 != 0;
        if has_slice && (matches!(t, 6..=9) || first_slice) {
            out.push(&data[start..n.start]);
            start = n.start;
            has_slice = false;
        }
        has_slice |= t == 1 || t == 5;
    }
    out.push(&data[start..]);
    out
}

fn rd32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

/// Runs our decoder over `stream` with the surfaces stock's caller used, compares every byte of
/// setup + scaling and the method values with stock's. Returns the number of frames compared.
fn compare_with_stock(stream: &str, frames: &str) -> usize {
    let data = evidence(&format!("streams/{stream}"));
    let (pps, blocks) = load_frames(std::str::from_utf8(&evidence(frames)).unwrap());
    let mut dec = Decoder::new(EngineParams::blackwell(None));
    let mut k = 0;
    let mut compared = 0;
    for au in access_units(&data) {
        let Some(pp) = pps.get(k) else { break };
        let surface = rd32(pp, 8);
        let Some(job) = dec.decode_au(au, surface).unwrap() else {
            continue;
        };
        k += 1;
        assert_eq!(
            job.bitstream.len() as u32,
            rd32(pp, 0x18),
            "{stream} frame {}: nBitstreamDataLen",
            k - 1
        );
        // The tracer's DESC order is not decode order: pair by stream_len + POC (+0x1c4, +0x1ec).
        let (len_off, poc_off) = if job.app_id == desc::APP_ID_NEW_H264 {
            (0x1c4, 0x1ec)
        } else {
            (0x48, 0xb8)
        };
        let hits: Vec<&StockFrame> = blocks
            .iter()
            .filter(|b| {
                rd32(&b.desc, len_off) == job.bitstream.len() as u32
                    && rd32(&b.desc, poc_off) == rd32(pp, 0x118)
            })
            .collect();
        assert_eq!(
            hits.len(),
            1,
            "{stream} frame {}: stock setups with this stream_len + POC",
            k - 1
        );
        let s = hits[0];
        assert_eq!(
            job.app_id,
            s.app,
            "{stream} frame {}: APPLICATION_ID",
            k - 1
        );
        for o in 0..job.setup.len() {
            assert_eq!(
                job.setup[o],
                s.desc[o],
                "{stream} frame {} byte {o:#x}: ours {:02x} stock {:02x}",
                k - 1,
                job.setup[o],
                s.desc[o]
            );
        }
        assert_eq!(
            job.picture_index,
            s.slot,
            "{stream} frame {}: PICTURE_INDEX",
            k - 1
        );
        assert_eq!(
            job.control_params,
            s.ctrl,
            "{stream} frame {}: CONTROL_PARAMS ours {:#x} stock {:#x}",
            k - 1,
            job.control_params,
            s.ctrl
        );
        compared += 1;
    }
    assert_eq!(k, pps.len(), "{stream}: all captured pictures decoded");
    compared
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_cabac() {
    assert_eq!(
        compare_with_stock("h264-cabac.h264", "h264-cabac-recapture.frames"),
        12
    );
}
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_cavlc_weightp() {
    assert_eq!(
        compare_with_stock("h264-cavlc.h264", "h264-cavlc.frames"),
        12
    );
}
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_cqm_two_slices() {
    assert_eq!(compare_with_stock("h264-cqm.h264", "h264-cqm.frames"), 12);
}
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_legacy_h264_setup_mbaff() {
    assert_eq!(
        compare_with_stock("h264-mbaff.h264", "h264-mbaff.frames"),
        12
    );
}

/// Minimal MSB-first bit writer for the hand-built streams of the refusal tests below.
struct Bw {
    bytes: Vec<u8>,
    nbits: usize,
}
impl Bw {
    fn new() -> Self {
        Bw {
            bytes: Vec::new(),
            nbits: 0,
        }
    }
    fn bit(&mut self, b: bool) {
        if self.nbits % 8 == 0 {
            self.bytes.push(0);
        }
        if b {
            self.bytes[self.nbits / 8] |= 0x80 >> (self.nbits % 8);
        }
        self.nbits += 1;
    }
    fn bits(&mut self, n: u32, v: u32) {
        for i in (0..n).rev() {
            self.bit((v >> i) & 1 != 0);
        }
    }
    fn ue(&mut self, v: u32) {
        let x = v + 1;
        let n = 32 - x.leading_zeros();
        self.bits(n - 1, 0);
        self.bits(n, x);
    }
    fn se(&mut self, v: i32) {
        self.ue(if v <= 0 {
            (-2 * v) as u32
        } else {
            (2 * v - 1) as u32
        });
    }
    /// rbsp_trailing_bits: a stop bit, then zero to the byte boundary.
    fn finish(mut self) -> Vec<u8> {
        self.bit(true);
        self.bytes
    }
}

/// A syntax-valid SPS: baseline (`profile_idc` 66) or High (100, with `ext` = chroma format,
/// bit depths). `poc_order_cnt_type` is 2 so the slice header below carries no POC fields.
fn sps_nal(profile: u8, ext: Option<(u32, u32, u32)>, frame_mbs_only: bool) -> Vec<u8> {
    let mut w = Bw::new();
    w.bits(8, 0x67); // nal_ref_idc 3, nal_unit_type 7
    w.bits(8, u32::from(profile));
    w.bits(8, 0); // constraint flags
    w.bits(8, 40); // level
    w.ue(0); // seq_parameter_set_id
    if let Some((chroma, luma, chroma_depth)) = ext {
        w.ue(chroma);
        if chroma == 3 {
            w.bit(false); // separate_colour_plane_flag
        }
        w.ue(luma); // bit_depth_luma_minus8
        w.ue(chroma_depth); // bit_depth_chroma_minus8
        w.bit(false); // qpprime_y_zero_transform_bypass_flag
        w.bit(false); // seq_scaling_matrix_present_flag
    }
    w.ue(0); // log2_max_frame_num_minus4
    w.ue(2); // pic_order_cnt_type
    w.ue(1); // max_num_ref_frames
    w.bit(false); // gaps_in_frame_num_value_allowed_flag
    w.ue(1); // pic_width_in_mbs_minus1 (2 MBs)
    w.ue(1); // pic_height_in_map_units_minus1
    w.bit(frame_mbs_only);
    if !frame_mbs_only {
        w.bit(false); // mb_adaptive_frame_field_flag
    }
    w.bit(true); // direct_8x8_inference_flag
    w.bit(false); // frame_cropping_flag
    w.bit(false); // vui_parameters_present_flag
    w.finish()
}

fn pps_nal() -> Vec<u8> {
    let mut w = Bw::new();
    w.bits(8, 0x68); // nal_ref_idc 3, nal_unit_type 8
    w.ue(0); // pic_parameter_set_id
    w.ue(0); // seq_parameter_set_id
    w.bit(false); // entropy_coding_mode_flag
    w.bit(false); // bottom_field_pic_order_in_frame_present_flag
    w.ue(0); // num_slice_groups_minus1
    w.ue(0); // num_ref_idx_l0_default_active_minus1
    w.ue(0); // num_ref_idx_l1_default_active_minus1
    w.bit(false); // weighted_pred_flag
    w.bits(2, 0); // weighted_bipred_idc
    w.se(0); // pic_init_qp_minus26
    w.se(0); // pic_init_qs_minus26
    w.se(0); // chroma_qp_index_offset
    w.bit(false); // deblocking_filter_control_present_flag
    w.bit(false); // constrained_intra_pred_flag
    w.bit(false); // redundant_pic_cnt_present_flag
    w.finish()
}

/// An IDR I slice with only the fields the parser needs for the picture-level checks.
fn idr_i_slice_nal(field: bool, bottom: bool) -> Vec<u8> {
    let mut w = Bw::new();
    w.bits(8, 0x65); // nal_ref_idc 3, nal_unit_type 5
    w.ue(0); // first_mb_in_slice
    w.ue(2); // slice_type 2 = I
    w.ue(0); // pic_parameter_set_id
    w.bits(4, 0); // frame_num (log2_max_frame_num_minus4 = 0)
    if field {
        w.bit(true); // field_pic_flag
        w.bit(bottom); // bottom_field_flag
    }
    w.ue(0); // idr_pic_id
    // pic_order_cnt_type 2: no POC fields.
    w.bit(false); // no_output_of_prior_pics_flag
    w.bit(false); // long_term_reference_flag
    w.finish()
}

fn au(sps: &[u8], pps: &[u8], slice: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    for n in [sps, pps, slice] {
        v.extend_from_slice(&[0, 0, 1]);
        v.extend_from_slice(n);
    }
    v
}

/// Runs the front end over `data` and requires the loud refusal to name `feature`.
fn expect_unsupported(data: &[u8], feature: &str) {
    let mut dec = Decoder::new(EngineParams::blackwell(None));
    match dec.begin(data) {
        Err(crate::video::VideoError::Unsupported(reason)) => assert!(
            reason.contains(feature),
            "refusal reason {reason:?} does not name {feature:?}"
        ),
        Err(e) => panic!("expected Unsupported({feature:?}), got {e:?}"),
        Ok(_) => panic!("expected Unsupported({feature:?}), the picture was accepted"),
    }
}

#[test]
fn field_pictures_refuse_naming_paff() {
    // Interlaced SPS, field_pic_flag = 1: PAFF field pictures stay a named refusal: the
    // legacy setup's field-picture fields have no verified values (no PAFF capture).
    let data = au(
        &sps_nal(66, None, false),
        &pps_nal(),
        &idr_i_slice_nal(true, false),
    );
    expect_unsupported(&data, "field pictures (PAFF)");
}

#[test]
fn chroma_422_and_444_refuse_naming_the_format() {
    for chroma in [2, 3] {
        let data = au(
            &sps_nal(100, Some((chroma, 0, 0)), true),
            &pps_nal(),
            &idr_i_slice_nal(false, false),
        );
        expect_unsupported(&data, &format!("chroma_format_idc {chroma}"));
    }
}

#[test]
fn high_bit_depth_refuses_naming_the_gate() {
    let data = au(
        &sps_nal(100, Some((1, 2, 2)), true),
        &pps_nal(),
        &idr_i_slice_nal(false, false),
    );
    expect_unsupported(&data, "bit depth above 8");
}

#[test]
fn scaling_list_fallback_rules() {
    // SPS with seq_scaling_matrix_present and every list absent → fall-back rule A = defaults.
    // Built by hand: High profile, 4:2:0, 8-bit.
    let mut bits = String::new();
    let ue = |v: u32| {
        let x = v + 1;
        let n = 32 - x.leading_zeros();
        format!("{}{:0w$b}", "0".repeat(n as usize - 1), x, w = n as usize)
    };
    bits += "01100111"; // NAL header SPS
    bits += &format!("{:08b}{:08b}{:08b}", 100, 0, 40);
    bits += &ue(0); // sps id
    bits += &ue(1); // chroma 4:2:0
    bits += &ue(0);
    bits += &ue(0);
    bits += "0"; // bypass
    bits += "1"; // seq_scaling_matrix_present
    bits += "00000000"; // 8 lists absent
    bits += &ue(0); // log2_max_frame_num-4
    bits += &ue(2); // poc type 2
    bits += &ue(1); // max refs
    bits += "0";
    bits += &ue(1); // width 2 MBs
    bits += &ue(1);
    bits += "1"; // frame_mbs_only
    bits += "1"; // direct8x8
    bits += "0"; // crop
    bits += "0"; // vui
    bits += "1";
    while !bits.len().is_multiple_of(8) {
        bits += "0";
    }
    let bytes: Vec<u8> = (0..bits.len())
        .step_by(8)
        .map(|i| u8::from_str_radix(&bits[i..i + 8], 2).unwrap())
        .collect();
    let sps = ps::parse_sps(&bytes).unwrap();
    assert_eq!(sps.scaling.m4[0][0], 6); // Default_4x4_Intra[0]
    assert_eq!(sps.scaling.m4[0][15], 42);
    assert_eq!(sps.scaling.m4[2], sps.scaling.m4[0]);
    assert_eq!(sps.scaling.m4[3][0], 10); // Default_4x4_Inter
    assert_eq!(sps.scaling.m4[0][1], 13); // raster (0,1) = zig-zag index 1
    assert_eq!(sps.scaling.m4[0][4], 13); // raster (1,0) = zig-zag index 2
    assert_eq!(sps.scaling.m8[1][63], 35);
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_poc_type2_1080p() {
    assert_eq!(compare_with_stock("h264-poc2.h264", "h264-poc2.frames"), 10);
}
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_b_pyramid_mmco() {
    assert_eq!(
        compare_with_stock("h264-pyramid.h264", "h264-pyramid.frames"),
        20
    );
}
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn stock_new_h264_setup_frame_num_and_poc_wrap() {
    assert_eq!(
        compare_with_stock("h264-wrap.h264", "h264-wrap.frames"),
        300
    );
}
