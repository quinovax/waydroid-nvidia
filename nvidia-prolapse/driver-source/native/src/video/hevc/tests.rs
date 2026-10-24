use super::desc::{FilterOffsets, SessionInputs};
use super::ps::{diag_scan, st_ref_pic_set, StRps, Tiles};
use super::*;
use crate::video::bits::split_annexb;
use crate::video::bits::BitReader;

/// Splits an Annex-B elementary stream into access units (7.4.2.4.4: an AU starts at an AUD,
/// VPS/SPS/PPS, prefix SEI or the first slice segment of a picture that follows a VCL NAL).
pub(super) fn access_units(data: &[u8]) -> Vec<&[u8]> {
    let nals = split_annexb(data);
    let mut out = Vec::new();
    let mut start = None::<usize>;
    let mut seen_vcl = false;
    for n in &nals {
        let t = (n.nal[0] >> 1) & 0x3f;
        let vcl = t < 32;
        let begins = if vcl {
            n.nal.len() > 2 && n.nal[2] & 0x80 != 0
        } else {
            matches!(t, 32..=35 | 39 | 41..=44 | 48..=55)
        };
        if begins && seen_vcl {
            out.push(&data[start.unwrap()..n.start]);
            start = Some(n.start);
            seen_vcl = false;
        }
        if start.is_none() {
            start = Some(n.start);
        }
        seen_vcl |= vcl;
    }
    if let Some(s) = start {
        out.push(&data[s..]);
    }
    out
}

fn rd32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

const FIELDS: &[(usize, usize, &str)] = &[
    (0x00, 0x30, "encryption keys/IV"),
    (0x30, 4, "stream_len"),
    (0x34, 8, "enable_encryption/encryption bits"),
    (0x3c, 4, "gptimer_timeout_value"),
    (0x40, 1, "tileformat/gob_height [session]"),
    (0x41, 1, "sw_start_code_e"),
    (0x42, 2, "disp_output_mode/reserved1"),
    (0x44, 8, "framestride [session]"),
    (0x4c, 4, "colMvBuffersize"),
    (0x50, 8, "HevcSao/BsdCtrl offsets [session]"),
    (0x58, 4, "pic width/height"),
    (0x5c, 4, "sps_bits0"),
    (0x60, 4, "sps_bits1"),
    (0x64, 4, "pps_bits0"),
    (0x68, 4, "cb/cr/beta/tc offsets"),
    (0x6c, 4, "pps_bits1"),
    (0x70, 1, "num_ref_frames"),
    (0x71, 1, "reserved6 [opaque]"),
    (0x72, 2, "longtermflag"),
    (0x74, 16, "initreflistidxl0"),
    (0x84, 16, "initreflistidxl1"),
    (0x94, 32, "RefDiffPicOrderCnts"),
    (0xb4, 2, "IDR/RAP flags"),
    (0xb6, 1, "curr_pic_idx"),
    (0xb7, 1, "pattern_id"),
    (0xb8, 2, "sw_hdr_skip_length"),
    (0xba, 2, "reserved7 [opaque]"),
    (0xbc, 24, "ecdma_cfg"),
    (0xd4, 4, "dxva bits"),
    (0xd8, 4, "num_bits_short_term_ref_pics_in_slice"),
    (0xdc, 4, "v1 sps ext bits"),
    (0xe0, 4, "v1 pps ext bits (log2MaxTransformSkipSize)"),
    (0xe4, 12, "v1 cb/cr qp adjustment"),
    (0xf0, 8, "v1 FltAbove/SaoAbove offsets [session]"),
    (0xf8, 4, "v1 bits"),
    (0xfc, 4, "v2"),
    (0x100, 4, "v3 bits"),
    (0x104, 4, "v3 HevcSliceEdgeOffset [session]"),
    (0x108, 12, "ssm"),
    (0x114, 6, "tail 0x114"),
    (0x11a, 1, "tail 0x11a [session]"),
    (0x11b, 1, "tail 0x11b"),
    (0x11c, 4, "tail 0x11c"),
    (0x120, 2, "tail 0x120"),
    (0x122, 0xde, "pad to 0x200"),
    (0x200, 8, "scaling DC (16x16 x6, 32x32 x2)"),
    (0x208, 8, "scaling pad"),
    (0x210, 96, "scaling 4x4 x6"),
    (0x270, 384, "scaling 8x8 x6"),
    (
        0x3f0,
        384,
        "scaling 16x16 x6 (captured from 0x400 only in 0x1000-byte captures)",
    ),
    (0x570, 128, "scaling 32x32 x2"),
    (0x5f0, 16, "scaling tail pad"),
    (
        0x600,
        super::desc::TILE_SIZES_SIZE,
        "tile sizes (0x1000-byte captures only)",
    ),
];

/// Runs the decoder over `streams/<name>.hevc` and compares every setup byte with the stock
/// capture `<name>.frames` (CUVIDPICPARAMS + DRV_PIC_SETUP pairs, matched by stream_len).
/// Session-owned values are taken from the capture's first descriptor and listed in FIELDS.
fn compare_with_stock(name: &str, by_index: bool) -> Vec<String> {
    compare_with_stock_stream(name, name, by_index)
}

fn compare_with_stock_stream(name: &str, stream_name: &str, by_index: bool) -> Vec<String> {
    let dir = std::env::var("NVDEC_STOCK_EVIDENCE")
        .expect("NVDEC_STOCK_EVIDENCE=<stock captures dir>");
    let stream = std::fs::read(format!("{dir}/streams/{stream_name}.hevc")).unwrap();
    let frames = std::fs::read_to_string(format!("{dir}/{name}.frames")).unwrap();
    let hex = |s: &str| -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    };
    let descs: Vec<Vec<u8>> = frames
        .lines()
        .filter_map(|l| l.strip_prefix("DESC "))
        .map(hex)
        .collect();
    // FRAME <n> app <id> ctrl <hex> picidx <n>: tracer order, same order as DESC.
    let hdrs: Vec<(u32, u32, u32)> = frames
        .lines()
        .filter_map(|l| l.strip_prefix("FRAME "))
        .map(|l| {
            let t: Vec<&str> = l.split_whitespace().collect();
            (
                u32::from_str_radix(t[2], 16).unwrap(),
                u32::from_str_radix(t[4], 16).unwrap(),
                t[6].parse().unwrap(),
            )
        })
        .collect();
    assert_eq!(hdrs.len(), descs.len());
    let pps: Vec<Vec<u8>> = frames
        .lines()
        .filter_map(|l| l.strip_prefix("PP "))
        .map(hex)
        .collect();
    let d0 = &descs[0];
    let inputs = SessionInputs {
        framestride: [rd32(d0, 0x44), rd32(d0, 0x48)],
        filter: FilterOffsets {
            sao: rd32(d0, 0x50),
            bsd_ctrl: rd32(d0, 0x54),
            flt_above: rd32(d0, 0xf0),
            sao_above: rd32(d0, 0xf4),
            slice_edge: rd32(d0, 0x104),
        },
        gptimer_max: u32::MAX,
        general_bits: d0[0x40],
        tail_11a: d0[0x11a],
        blackwell_b: true,
    };
    let mut dec = Decoder::new(inputs);
    let mut report = Vec::new();
    let mut matched = 0;
    let mut framing = 0;
    let mut mism: Vec<(usize, usize)> = Vec::new(); // (frame, offset)
    for (k, au) in access_units(&stream).iter().enumerate() {
        let Some(job) = dec.decode_au(au).unwrap() else {
            continue;
        };
        // FFmpeg (the stock capture's CUVID client) hands each slice NAL over up to the next
        // `00 00 01`, so a slice followed by a 4-byte start code keeps its zero_byte; we cut it.
        let nals = split_annexb(au);
        let mut extra = 0u32;
        for (i, n) in nals.iter().enumerate() {
            let vcl = (n.nal[0] >> 1) & 0x3f < 32;
            let next_vcl = nals[i + 1..].iter().any(|m| (m.nal[0] >> 1) & 0x3f < 32);
            if vcl && next_vcl {
                let next = &nals[i + 1];
                let payload = next.nal.as_ptr() as usize - au.as_ptr() as usize;
                extra += (payload - next.start == 4) as u32;
            }
        }
        if extra > 0 {
            framing += 1;
        }
        let len = job.bitstream.len() as u32 + extra;
        let mut job = job;
        job.setup[0x30..0x34].copy_from_slice(&len.max(16).to_le_bytes());
        let di = if by_index {
            if matched >= descs.len() {
                break;
            }
            matched
        } else {
            let Some(di) = descs.iter().position(|d| rd32(d, 0x30) == len.max(16)) else {
                continue;
            };
            di
        };
        let d = &descs[di];
        let (app, ctrl, picidx) = hdrs[di];
        assert_eq!(app, job.methods.app_id, "{name} AU {k} APPLICATION_ID");
        assert_eq!(
            ctrl, job.methods.control_params,
            "{name} AU {k} CONTROL_PARAMS"
        );
        assert_eq!(
            picidx, job.methods.picture_index,
            "{name} AU {k} SET_PICTURE_INDEX"
        );
        let p = if by_index {
            &pps[di]
        } else {
            pps.iter()
                .find(|p| rd32(p, 0x18) == len)
                .expect("CUVIDPICPARAMS with our bitstream length")
        };
        assert_eq!(rd32(p, 0x18), len, "{name} AU {k} nBitstreamDataLen");
        // CUVID cross-checks: NumPocTotalCurr (+416), NumBitsForShortTermRPSInSlice (+408), POC (+432).
        assert_eq!(
            rd32(p, 416),
            job.setup[0x70] as u32,
            "{name} AU {k} NumPocTotalCurr"
        );
        assert_eq!(
            rd32(p, 408),
            rd32(&job.setup, 0xd8),
            "{name} AU {k} NumBitsForShortTermRPSInSlice"
        );
        assert_eq!(
            rd32(p, 412),
            (rd32(&job.setup, 0xd4) >> 19) & 0xff,
            "{name} AU {k} NumDeltaPocsOfRefRpsIdx"
        );
        assert_eq!(
            rd32(p, 432) as i32,
            job.poc,
            "{name} AU {k} CurrPicOrderCntVal"
        );
        matched += 1;
        for (o, (ours, stock)) in job
            .setup
            .iter()
            .zip(d)
            .take(super::desc::SETUP_SIZE)
            .enumerate()
        {
            if ours != stock {
                mism.push((k, o));
            }
        }
    }
    report.push(format!(
        "{name}: {matched} frames paired ({framing} with FFmpeg's extra zero_byte in stream_len)"
    ));
    for &(o, n, f) in FIELDS {
        let bad: Vec<_> = mism.iter().filter(|(_, x)| *x >= o && *x < o + n).collect();
        let status = if bad.is_empty() {
            "match".to_string()
        } else {
            format!(
                "DIFFERS in {} bytes (first: frame {} off {:#x})",
                bad.len(),
                bad[0].0,
                bad[0].1
            )
        };
        report.push(format!("  {o:#05x} {f}: {status}"));
    }
    assert!(
        matched >= descs.len().min(8) && (!by_index || matched == descs.len()),
        "{name}: only {matched} of {} frames paired",
        descs.len()
    );
    report
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_hevc8() {
    let r = compare_with_stock("hevc8", false);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_hevc10() {
    let r = compare_with_stock("hevc10", false);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_cqm() {
    let r = compare_with_stock("hevc-cqm", true);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_tiles_4k() {
    let r = compare_with_stock_stream("hevc-tiles-4k", "hevc-tiles", true);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
fn diagonal_scan_4x4() {
    // 6.5.3: (x, y) of the first positions of a 4x4 up-right diagonal scan.
    assert_eq!(
        &diag_scan(4)[..6],
        &[(0, 0), (0, 1), (1, 0), (0, 2), (1, 1), (2, 0)]
    );
}

/// MSB-first bit writer for hand-built syntax.
struct W(Vec<bool>);
impl W {
    fn u(&mut self, n: u32, v: u32) -> &mut Self {
        for i in (0..n).rev() {
            self.0.push(v >> i & 1 != 0);
        }
        self
    }
    fn ue(&mut self, v: u32) -> &mut Self {
        let x = v + 1;
        let n = 32 - x.leading_zeros();
        self.u(n - 1, 0).u(n, x)
    }
    fn bytes(&self) -> Vec<u8> {
        let mut b = vec![0u8; self.0.len().div_ceil(8) + 1];
        for (i, &x) in self.0.iter().enumerate() {
            b[i / 8] |= (x as u8) << (7 - i % 8);
        }
        b
    }
}

#[test]
fn inter_rps_prediction_follows_7_4_8() {
    // Reference set: S0 = {-1, -3}, S1 = {+2} (all used). Predict with deltaRps = -1 and every
    // use_delta_flag = 1 / used_by_curr_pic_flag = 1:
    // candidates dPoc = {-2, -4, +1} and deltaRps itself (-1) -> S0 = {-1, -2, -4}, S1 = {+1}.
    let rf = StRps {
        s0: vec![(-1, true), (-3, true)],
        s1: vec![(2, true)],
    };
    let mut w = W(Vec::new());
    w.u(1, 1); // inter_ref_pic_set_prediction_flag
    w.ue(0); // delta_idx_minus1 (slice header)
    w.u(1, 1).ue(0); // delta_rps_sign = 1, abs_delta_rps_minus1 = 0 -> deltaRps = -1
    for _ in 0..4 {
        w.u(1, 1); // used_by_curr_pic_flag
    }
    let b = w.bytes();
    let mut r = BitReader::new(&b);
    let (s, ndp) = st_ref_pic_set(&mut r, 1, std::slice::from_ref(&rf), true).unwrap();
    assert_eq!(ndp, Some(3)); // NumDeltaPocs[RefRpsIdx]
    assert_eq!(s.s0, vec![(-1, true), (-2, true), (-4, true)]);
    assert_eq!(s.s1, vec![(1, true)]);
    assert_eq!(r.pos(), w.0.len()); // st_rps bit count (NumBitsForShortTermRPSInSlice)
}

#[test]
fn uniform_tile_spacing_follows_6_3() {
    let t = Tiles {
        uniform: true,
        col_minus1: vec![],
        row_minus1: vec![],
        cols: 3,
        rows: 2,
        loop_filter_across_tiles: true,
    };
    // 6-3: colWidth[i] = ((i+1)*W)/n - (i*W)/n with W = 10 CTBs, n = 3 -> 3, 3, 4; rows 7 / 2 -> 3, 4.
    assert_eq!(t.sizes(10, 7), (vec![3, 3, 4], vec![3, 4]));
    let e = Tiles {
        uniform: false,
        col_minus1: vec![1, 4],
        row_minus1: vec![],
        cols: 3,
        rows: 1,
        loop_filter_across_tiles: false,
    };
    assert_eq!(e.sizes(10, 7), (vec![2, 5, 3], vec![7]));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_pocwrap() {
    let r = compare_with_stock("hevc-pocwrap", true);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_multislice() {
    let r = compare_with_stock("hevc-multislice", true);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (stock captures, not in the repository)"]
fn setup_matches_stock_tiles() {
    let r = compare_with_stock("hevc-tiles", true);
    println!("{}", r.join("\n"));
    assert!(r.iter().all(|l| !l.contains("DIFFERS")), "{}", r.join("\n"));
}

fn nal_type(au: &[u8]) -> Vec<u8> {
    split_annexb(au)
        .iter()
        .map(|n| (n.nal[0] >> 1) & 0x3f)
        .collect()
}

/// libx265 streams generated locally (`ffmpeg -f lavfi -i testsrc=size=176x144:rate=25 -pix_fmt yuv420p
/// -c:v libx265 -x265-params keyint=24:min-keyint=24:open-gop=1:bframes=3:scenecut=0` 80 frames;
/// `keyint=40:bframes=4:b-pyramid=1:ref=4:slices=3` 40 frames): open GOP with CRA + RASL, random access
/// at a CRA (RASL pictures skipped per 8.1.3), B-pyramid with 3 slices per picture.
#[test]
#[ignore = "needs NVDEC_STOCK_EVIDENCE (streams/x265-*.hevc)"]
fn libx265_open_gop_random_access_and_pyramid() {
    let dir = std::env::var("NVDEC_STOCK_EVIDENCE")
        .expect("NVDEC_STOCK_EVIDENCE=<stock captures dir>");
    let si = SessionInputs {
        framestride: [0; 2],
        filter: FilterOffsets::default(),
        gptimer_max: u32::MAX,
        general_bits: 1,
        tail_11a: 0,
        blackwell_b: true,
    };
    let og = std::fs::read(format!("{dir}/streams/x265-opengop.hevc")).unwrap();
    let aus = access_units(&og);
    let mut dec = Decoder::new(si);
    let pocs: Vec<i32> = aus
        .iter()
        .filter_map(|a| dec.decode_au(a).unwrap())
        .map(|j| j.poc)
        .collect();
    assert_eq!(pocs.len(), 80);
    let mut sorted = pocs.clone();
    sorted.sort();
    assert_eq!(
        sorted,
        (0..80).collect::<Vec<_>>(),
        "open-GOP POCs (single CVS from the IDR)"
    );
    // Random access at the second CRA: its RASL pictures are skipped, everything else decodes.
    let cra: Vec<usize> = aus
        .iter()
        .enumerate()
        .filter(|(_, a)| nal_type(a).contains(&21))
        .map(|(i, _)| i)
        .collect();
    let start = cra[1];
    let ps: Vec<u8> = aus[..start]
        .iter()
        .flat_map(|a| {
            let mut v = Vec::new();
            for n in split_annexb(a) {
                if matches!((n.nal[0] >> 1) & 0x3f, 32..=34) {
                    v.extend_from_slice(&[0, 0, 1]);
                    v.extend_from_slice(n.nal);
                }
            }
            v
        })
        .collect();
    let mut dec = Decoder::new(si);
    assert!(dec.decode_au(&ps).unwrap().is_none());
    let mut skipped = 0;
    let mut decoded = 0;
    for a in &aus[start..] {
        match dec.decode_au(a).unwrap() {
            Some(_) => decoded += 1,
            None => skipped += 1,
        }
    }
    let rasl_after = aus[start..cra[2]]
        .iter()
        .filter(|a| nal_type(a).iter().any(|&t| t == 8 || t == 9))
        .count();
    assert!(rasl_after > 0);
    assert_eq!(skipped, rasl_after);
    assert_eq!(decoded + skipped, aus.len() - start);
    // B-pyramid, 3 slices per picture.
    let py = std::fs::read(format!("{dir}/streams/x265-pyramid-3slice.hevc")).unwrap();
    let mut dec = Decoder::new(si);
    let jobs: Vec<PictureJob> = access_units(&py)
        .iter()
        .filter_map(|a| dec.decode_au(a).unwrap())
        .collect();
    assert_eq!(jobs.len(), 40);
    let mut p: Vec<i32> = jobs.iter().map(|j| j.poc).collect();
    p.sort();
    assert_eq!(p, (0..40).collect::<Vec<_>>());
    assert!(jobs
        .iter()
        .all(|j| j.bitstream.windows(3).filter(|w| *w == [0, 0, 1]).count() >= 3));
}

// ---------------------------------------------------------------------------------------------------------------
// Refusal tests: features that stay a loud E_UNSUPPORTED until their stock contract is established.
// ---------------------------------------------------------------------------------------------------------------

/// SPS/PPS of the checked-in 160x144 stream, for the descriptor-builder refusal tests.
fn testdata_parameter_sets() -> (Sps, Pps) {
    const HEVC: &[u8] = include_bytes!("../testdata/hevc_160x144.hevc");
    let mut sps = None;
    for n in split_annexb(HEVC) {
        match (n.nal[0] >> 1) & 0x3f {
            33 => sps = Some(ps::parse_sps(&crate::video::bits::unescape(n.nal)).unwrap()),
            34 => {
                let p = ps::parse_pps(&crate::video::bits::unescape(n.nal)).unwrap();
                return (sps.unwrap(), p);
            }
            _ => {}
        }
    }
    panic!("testdata has no SPS/PPS pair");
}

fn idle_pic_state() -> PicState {
    PicState {
        stream_len: 16,
        idr: true,
        irap: true,
        curr_slot: 0,
        num_poc_total_curr: 0,
        list0: [0; 16],
        list1: [0; 16],
        diff: [0; 16],
        sw_hdr_skip_length: 0,
        num_bits_st_rps: 0,
        num_delta_pocs_ref: 0,
    }
}

fn session_inputs() -> SessionInputs {
    SessionInputs {
        framestride: [160, 320],
        filter: FilterOffsets::default(),
        gptimer_max: u32::MAX,
        general_bits: 1,
        tail_11a: 0,
        blackwell_b: true,
    }
}

fn expect_build_unsupported(sps: &Sps, pps: &Pps, feature: &str) {
    match desc::build(
        sps,
        pps,
        &idle_pic_state(),
        &session_inputs(),
        StockOpaque::observed(sps.bit_depth_luma),
    ) {
        Err(VideoError::Unsupported(m)) => assert!(
            m.contains(feature),
            "refusal reason {m:?} does not name {feature:?}"
        ),
        Err(e) => panic!("expected Unsupported({feature:?}), got {e:?}"),
        Ok(_) => panic!("expected Unsupported({feature:?}), the descriptor was built"),
    }
}

#[test]
fn two_dimensional_tile_grids_are_raster_ordered() {
    // Stock builds one {u16 width, u16 height} entry per tile in raster-scan order (rows outer,
    // columns inner) at setup+0x600: uniform path ELF 0x3ec2b..0x3ecfc, explicit path
    // 0x3eef2..0x3f01c of libnvcuvid 615.78.08 (measured with read watchpoints on
    // column_width_minus1 / row_height_minus1).
    let (sps, mut pps) = testdata_parameter_sets();
    let (w, h) = (sps.width_ctbs(), sps.height_ctbs());
    assert_eq!((w, h), (3, 3), "testdata grid");
    // Explicit 2x2: columns [2, 1], rows [1, 2].
    pps.tiles = Some(Tiles {
        uniform: false,
        col_minus1: vec![1],
        row_minus1: vec![0],
        cols: 2,
        rows: 2,
        loop_filter_across_tiles: true,
    });
    let d = desc::build(
        &sps,
        &pps,
        &idle_pic_state(),
        &session_inputs(),
        StockOpaque::observed(sps.bit_depth_luma),
    )
    .unwrap();
    let entry = |i: usize| {
        (
            u16::from_le_bytes([d[desc::TILE_SIZES_OFFSET + 4 * i], d[desc::TILE_SIZES_OFFSET + 4 * i + 1]]),
            u16::from_le_bytes([d[desc::TILE_SIZES_OFFSET + 4 * i + 2], d[desc::TILE_SIZES_OFFSET + 4 * i + 3]]),
        )
    };
    assert_eq!(
        [entry(0), entry(1), entry(2), entry(3)],
        [(2, 1), (1, 1), (2, 2), (1, 2)],
        "explicit 2x2 entries are raster ordered"
    );
    // Uniform 2x2 over 3x3 CTBs: columns [1, 2], rows [1, 2].
    pps.tiles = Some(Tiles {
        uniform: true,
        col_minus1: vec![],
        row_minus1: vec![],
        cols: 2,
        rows: 2,
        loop_filter_across_tiles: true,
    });
    let d = desc::build(
        &sps,
        &pps,
        &idle_pic_state(),
        &session_inputs(),
        StockOpaque::observed(sps.bit_depth_luma),
    )
    .unwrap();
    let entry = |i: usize| {
        (
            u16::from_le_bytes([d[desc::TILE_SIZES_OFFSET + 4 * i], d[desc::TILE_SIZES_OFFSET + 4 * i + 1]]),
            u16::from_le_bytes([d[desc::TILE_SIZES_OFFSET + 4 * i + 2], d[desc::TILE_SIZES_OFFSET + 4 * i + 3]]),
        )
    };
    assert_eq!(
        [entry(0), entry(1), entry(2), entry(3)],
        [(1, 1), (2, 1), (1, 2), (2, 2)],
        "uniform 2x2 entries are raster ordered"
    );
}

#[test]
fn pcm_is_refused() {
    // pcm_enabled_flag alone is not enough: the PCM sample depths and block sizes are not
    // verified in the stock descriptor.
    let (mut sps, pps) = testdata_parameter_sets();
    sps.pcm = true;
    expect_build_unsupported(&sps, &pps, "HEVC PCM");
}

#[test]
fn slice_long_term_references_are_refused() {
    use super::slice::parse_slice_header;
    let (mut sps, pps) = testdata_parameter_sets();
    sps.long_term_ref_pics_present = true;
    sps.num_long_term_ref_pics_sps = 1;
    // P slice header: first slice segment, our PPS, one inline (empty) short-term RPS, then a
    // single num_long_term_sps entry (the long-term syntax itself is what is unimplemented).
    let mut w = W(Vec::new());
    w.u(1, 1); // first_slice_segment_in_pic_flag
    w.ue(pps.id);
    w.u(pps.num_extra_slice_header_bits, 0);
    w.ue(1); // slice_type = P (non-IDR, so no no_output_of_prior_pics_flag)
    if pps.output_flag_present {
        w.u(1, 1);
    }
    if sps.separate_colour_plane {
        w.u(2, 0);
    }
    w.u(sps.log2_max_poc_lsb, 0); // slice_pic_order_cnt_lsb
    w.u(1, 0); // short_term_ref_pic_set_sps_flag = 0 (inline set)
    w.ue(0); // num_negative_pics
    w.ue(0); // num_positive_pics
    w.ue(1); // num_long_term_sps
    w.ue(0); // num_long_term_pics
    let mut rbsp = vec![0u8, 0u8]; // the parser skips a 2-byte NAL header
    rbsp.extend_from_slice(&w.bytes());
    let r = parse_slice_header(
        &rbsp,
        1,
        |i| (i == pps.id).then_some(&pps),
        |i| (i == sps.id).then_some(&sps),
    );
    match r {
        Err(VideoError::Unsupported(m)) => assert!(
            m.contains("long-term reference"),
            "refusal reason {m:?} does not name the feature"
        ),
        Err(e) => panic!("expected Unsupported(long-term), got {e:?}"),
        Ok(_) => panic!("long-term slice references were accepted"),
    }
}

#[test]
fn range_extension_profiles_are_refused() {
    // profile_idc 4 = Format Range Extensions (SCC is 9); parse_sps refuses before the coding
    // tools, so only the profile_tier_level is needed.
    let mut w = W(Vec::new());
    w.u(4, 0); // sps_video_parameter_set_id
    w.u(3, 0); // sps_max_sub_layers_minus1
    w.u(1, 1); // sps_temporal_id_nesting_flag
    w.u(2 + 1, 0); // general_profile_space, general_tier_flag
    w.u(5, 4); // general_profile_idc = 4
    w.u(32, 0); // general_profile_compatibility_flags
    w.u(32, 0).u(16, 0); // source flags, 43 constraint bits, inbld
    w.u(8, 0); // general_level_idc
    let mut nal = vec![0u8; 2]; // NAL header (skipped by the parser)
    nal.extend_from_slice(&w.bytes());
    match ps::parse_sps(&nal) {
        Err(VideoError::Unsupported(m)) => {
            assert!(m.contains("profile_idc 4"), "reason {m:?} does not name the profile")
        }
        Err(e) => panic!("expected Unsupported(profile_idc 4), got {e:?}"),
        Ok(_) => panic!("range-extension profile accepted"),
    }
}
