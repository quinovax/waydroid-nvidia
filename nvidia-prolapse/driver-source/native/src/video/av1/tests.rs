use super::*;

/// SHA-1 prefixes (12 hex) of the stock default CDF buffers per q-context, from captures of stock
/// libnvcuvid 615.71.09 (`cdf_read_sha1` of default-CDF frames).
const STOCK_DEFAULT_CDF_SHA1: [&str; 4] = [
    "1b1d8ee1bfe7",
    "7ad7bea45698",
    "69f5c4f05098",
    "3db1ac6bc790",
];

fn sha1_hex(data: &[u8]) -> String {
    // Minimal SHA-1 (tests only).
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut m = data.to_vec();
    let bl = (data.len() as u64) * 8;
    m.push(0x80);
    while m.len() % 64 != 56 {
        m.push(0);
    }
    m.extend_from_slice(&bl.to_be_bytes());
    for c in m.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(c[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut cc, mut d, mut e] = h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & cc) | (!b & d), 0x5A827999),
                20..=39 => (b ^ cc ^ d, 0x6ED9EBA1),
                40..=59 => ((b & cc) | (b & d) | (cc & d), 0x8F1BBCDC),
                _ => (b ^ cc ^ d, 0xCA62C1D6),
            };
            let t = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(*wi);
            e = d;
            d = cc;
            cc = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in h.iter_mut().zip([a, b, cc, d, e]) {
            *x = x.wrapping_add(y);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}

#[test]
fn default_cdfs_match_stock_hashes() {
    assert_eq!(&sha1_hex(b"abc")[..12], "a9993e364706");
    for q in 0..4u8 {
        let mut b = vec![0u8; CDF_BUFFER_SIZE];
        write_default_cdfs(q, &mut b);
        assert_eq!(
            &sha1_hex(&b)[..12],
            STOCK_DEFAULT_CDF_SHA1[q as usize],
            "qctx {q}"
        );
    }
}

#[test]
fn qctx_thresholds() {
    assert_eq!(
        [0, 20, 21, 60, 61, 120, 121, 255].map(default_cdf_qctx),
        [0, 0, 1, 1, 2, 2, 3, 3]
    );
}

/// Dump every picture job of the IVF streams in $AV1_IVF_DIR to $AV1_OUT_DIR (descriptor block + job
/// summary) for comparison with stock captures (compare script
/// native/tools/av1/compare_stock.py).
#[test]
#[ignore]
fn dump_jobs() {
    let (Ok(dir), Ok(out)) = (std::env::var("AV1_IVF_DIR"), std::env::var("AV1_OUT_DIR")) else {
        panic!("set AV1_IVF_DIR and AV1_OUT_DIR");
    };
    for name in ["a_640", "fg_640", "nocdf_640", "t_1080"] {
        let data = std::fs::read(format!("{dir}/{name}.ivf")).unwrap();
        let mut codec = Av1Codec::new();
        let mut n = 0;
        let mut summary = String::new();
        for tu in obu::ivf_frames(&data).unwrap() {
            for job in codec.decode_temporal_unit(tu).unwrap() {
                let mut block = vec![0u8; desc::DESC_BLOCK_SIZE];
                job.build_descriptor(job.hdr.upscaled_width, true, &mut block);
                std::fs::write(format!("{out}/{name}_{n:03}.bin"), &block).unwrap();
                std::fs::write(format!("{out}/{name}_{n:03}.bits"), &job.bitstream).unwrap();
                summary += &format!(
                    "{n} slot={} cdf_r={} cdf_w={} default={:?} refs={:?} colmv={:?} seg={:?} grain={}\n",
                    job.slot, job.cdf_read, job.cdf_write, job.cdf_default_qctx, job.ref_slots, job.colmv_reads,
                    job.seg_read, job.apply_grain
                );
                n += 1;
            }
        }
        std::fs::write(format!("{out}/{name}.txt"), summary).unwrap();
    }
}

/// Print the parsed headers of $AV1_IVF (debug aid).
#[test]
#[ignore]
fn print_headers() {
    let data = std::fs::read(std::env::var("AV1_IVF").expect("set AV1_IVF")).unwrap();
    let mut codec = Av1Codec::new();
    let mut n = 0;
    for tu in obu::ivf_frames(&data).unwrap() {
        for job in codec.decode_temporal_unit(tu).unwrap() {
            if n == 0 {
                println!("{:?}", codec.parser.seq);
            }
            if n < 2 {
                println!("{n}: {:?}", job.hdr);
            }
            n += 1;
        }
    }
}

/// What one simulated session run saw.
#[derive(Default, Debug)]
struct SessionRun {
    decoded: usize,
    shown: usize,
    hidden: usize,
    hidden_grain: usize,
    show_existing: usize,
    show_existing_grain: usize,
    /// Units the pre-fix admission (`need <= free` only) would have refused.
    refused_before_fix: usize,
    max_unit_frames: usize,
}

/// Drive `Av1Decoder` through the session's surface accounting (same `free`, `referenced`, admission and
/// surface choice as `session.rs`) with a caller that copies out and releases every picture right after
/// each unit. Pool = dpb_size + 2 + `extra` surfaces. Checks that no unit is refused, no admitted unit runs
/// out of surfaces, every film-grain decode writes its display picture (0x428) to the session surface it
/// was given, and every show_existing_frame outputs a surface last written by a decode with the same
/// film-grain state.
fn simulate_session(ivf: &[u8], extra: usize) -> SessionRun {
    use crate::video::{
        session::unit_admitted, Allocator, BuildContext, CodecDecoder, Job, SurfaceRef, VResult,
    };
    struct Mock(u64);
    impl Allocator for Mock {
        fn vram(&mut self, size: usize) -> VResult<u64> {
            let va = self.0;
            self.0 += (size as u64).next_multiple_of(1 << 16);
            Ok(va)
        }
        fn write(&mut self, _va: u64, _offset: usize, _data: &[u8]) -> VResult<()> {
            Ok(())
        }
    }
    let tus = obu::ivf_frames(ivf).unwrap();
    let (w, h) = (
        u16::from_le_bytes([ivf[12], ivf[13]]) as u32,
        u16::from_le_bytes([ivf[14], ivf[15]]) as u32,
    );
    let mut dec = codec::Av1Decoder::new(w, h, 8, tus[0]).unwrap();
    dec.allocate(&mut Mock(1 << 32), w, h).unwrap();
    let layout = dec.surface_layout(w, h);
    let total = dec.dpb_size() + 2 + extra;
    let surfaces: Vec<SurfaceRef> = (0..total as u64)
        .map(|i| SurfaceRef {
            luma: (2 << 32) + i * layout.size as u64,
            chroma: (2 << 32) + i * layout.size as u64 + layout.chroma_offset,
        })
        .collect();
    // Last decode that wrote each session surface: Some(grain applied).
    let mut written: Vec<Option<bool>> = vec![None; total];
    let mut run = SessionRun::default();
    for (n, tu) in tus.iter().enumerate() {
        let mut held: Vec<usize> = Vec::new();
        let free = |dec: &codec::Av1Decoder, held: &[usize]| -> Vec<usize> {
            let r = dec.referenced();
            (0..total)
                .filter(|i| !r.contains(i) && !held.contains(i))
                .collect()
        };
        let need = dec.surfaces_needed(tu).unwrap();
        let free_now = free(&dec, &held).len();
        run.max_unit_frames = run.max_unit_frames.max(need);
        if need > free_now {
            run.refused_before_fix += 1;
        }
        assert!(
            unit_admitted(need, free_now, dec.max_referenced(), 0, total),
            "unit {n} refused: needs {need}, {free_now} free"
        );
        dec.push_unit(tu).unwrap();
        while let Some(job) = dec.next_job().unwrap() {
            match job {
                Job::ShowExisting { surface, .. } => {
                    let g = written[surface].expect("show_existing of an unwritten surface");
                    run.show_existing += 1;
                    run.show_existing_grain += g as usize;
                    held.push(surface);
                }
                Job::Decode(job) => {
                    let cur = *free(&dec, &held)
                        .first()
                        .unwrap_or_else(|| panic!("unit {n}: admitted but no free surface"));
                    let ctx = BuildContext {
                        decoder_class: 0xcfb0,
                        host_class: 0xc96f,
                        descriptor: 3 << 32,
                        bitstream: 4 << 32,
                        bitstream_len: job.bitstream.len(),
                        picture_index: run.decoded as u32,
                        current: cur,
                        surfaces: &surfaces,
                        layout,
                    };
                    let mut d = vec![0u8; desc::DESC_BLOCK_SIZE];
                    let words = dec.build(&job, &ctx, &mut d).unwrap();
                    // Address methods are 3-word NONINC (header, va >> 8, va >> 40).
                    let display = words
                        .windows(2)
                        .find(|x| x[0] == (3 << 29 | 2 << 16 | 4 << 13 | 0x428 >> 2))
                        .map(|x| x[1]);
                    let grain = display.is_some();
                    if let Some(va) = display {
                        assert_eq!(va as u64, surfaces[cur].luma >> 8, "unit {n}: 0x428 target");
                    }
                    dec.finish(&job, cur, None).unwrap();
                    written[cur] = Some(grain);
                    run.decoded += 1;
                    if job.show {
                        run.shown += 1;
                        held.push(cur);
                    } else {
                        run.hidden += 1;
                        run.hidden_grain += grain as usize;
                    }
                }
            }
        }
        // The caller copies out and releases every picture of the unit.
    }
    run
}

const AOM_HIDDEN: &[u8] = include_bytes!("testdata/aom_hidden_640.ivf");
const SVT_GRAIN: &[u8] = include_bytes!("testdata/svt_grain_640.ivf");

/// Gallery bug (av1_aom): a unit with several hidden alt-ref frames was refused ("needs 6 free surfaces,
/// 4 free") although the caller held nothing; references refreshed inside the unit free the surfaces.
#[test]
fn hidden_frame_units_never_refused_when_caller_holds_nothing() {
    for extra in [0, 2] {
        let r = simulate_session(AOM_HIDDEN, extra);
        assert_eq!(r.shown + r.show_existing, 40, "{r:?}");
        assert!(
            r.hidden > 0 && r.show_existing > 0 && r.max_unit_frames >= 3,
            "{r:?}"
        );
        if extra == 0 {
            assert!(
                r.refused_before_fix > 0,
                "stream no longer exercises the bug: {r:?}"
            );
        }
    }
}

/// Gallery bug (av1_grain): film grain on hidden frames was refused. Hidden grain frames now decode with
/// the grain-applied picture in a session surface (stock: 0x428/0x42c on every grain frame) and
/// show_existing_frame outputs that surface.
#[test]
fn film_grain_hidden_frames_and_show_existing() {
    let r = simulate_session(SVT_GRAIN, 0);
    assert_eq!(r.shown + r.show_existing, 40, "{r:?}");
    assert!(r.hidden_grain > 0 && r.show_existing_grain > 0, "{r:?}");
}

/// Spec 7.8 set_frame_refs: reference slots derived from order hints. The two
/// cases walk every branch of the selection order (ALTREF/BWDREF/ALTREF2 from
/// the future, then LAST2/LAST3, then the earliest-slot fallback), with the
/// expected values worked out from the order hints by hand.
#[test]
fn frame_refs_short_signaling_derivation() {
    let seq = SequenceHeader {
        enable_order_hint: true,
        order_hint_bits: 8,
        ..Default::default()
    };
    let mut p = ParserState {
        seq: Some(seq),
        refs: [header::RefSlotState::default(); header::NUM_REF_FRAMES],
    };
    // Current frame order hint 50; all reference hints are before it.
    for (i, hint) in [40u32, 45, 48, 30, 20, 10, 0, 35].into_iter().enumerate() {
        p.refs[i].order_hint = hint;
    }
    // LAST = slot 0 (hint 40), GOLDEN = slot 3 (hint 30). LAST2 = slot 2 (126,
    // the largest below the frame and >= LAST's 118); LAST3 starts over at the
    // first unused slot below the frame (slot 1, hint 123); the three future
    // slots fall back to the earliest slot overall (slot 6, hint 0).
    assert_eq!(p.set_frame_refs(0, 3, 50).unwrap(), [0, 2, 1, 3, 6, 6, 6]);

    // Future references: 60 (slot 4), 55 (slot 5), 70 (slot 6).
    for (i, hint) in [40u32, 45, 48, 30, 60, 55, 70, 20].into_iter().enumerate() {
        p.refs[i].order_hint = hint;
    }
    // ALTREF = 70, BWDREF = 55 (earliest in the future), ALTREF2 = 60;
    // LAST2 = slot 2 (126) and LAST3 = slot 1 (123), the same two-step as above.
    assert_eq!(p.set_frame_refs(0, 3, 50).unwrap(), [0, 2, 1, 3, 5, 4, 6]);

    // get_relative_dist: signed wrap around the order-hint modulus.
    let s = p.seq.clone().unwrap();
    assert_eq!(header::get_relative_dist(&s, 40, 50), -10);
    assert_eq!(header::get_relative_dist(&s, 60, 50), 10);
    assert_eq!(header::get_relative_dist(&s, 250, 5), -11);
}

/// Superres inter frames: stock writes the coded (pre-superres) width into the
/// current frame info and its motion-field size gate then finds no candidate
/// (reference widths are upscaled), so use_temporal stays 0 and the motion-field
/// words are zero. Both are pinned here on the values measured from a stock
/// libnvcuvid 615.78.08 capture (320-wide upscaled / 213-wide coded picture,
/// denom 12).
#[test]
fn superres_geometry_and_motion_field_gate() {
    let seq = SequenceHeader {
        enable_order_hint: true,
        order_hint_bits: 8,
        ..Default::default()
    };
    let cur = header::FrameHeader {
        frame_type: header::INTER_FRAME,
        use_superres: true,
        superres_denom: 12,
        upscaled_width: 320,
        frame_width: 213,
        frame_height: 240,
        mi_cols: 40,
        mi_rows: 30,
        order_hint: 50,
        use_ref_frame_mvs: true,
        ..Default::default()
    };
    let rec = PicRecord {
        // A reference of the same stream: same coded size, upscaled to 320.
        upscaled_width: 320,
        frame_width: 213,
        frame_height: 240,
        mi_cols: 40,
        mi_rows: 30,
        ..Default::default()
    };
    let (mf, _) = mf::select(&seq, &cur, &[Some(rec); 7], &[None; 7]);
    assert!(
        mf.use_temporal.iter().all(|&t| !t),
        "stock projects no motion field on a superres frame: {mf:?}"
    );
    // The same reference geometry without superres is usable.
    let cur2 = header::FrameHeader {
        frame_width: 320,
        upscaled_width: 320,
        use_superres: false,
        order_hints: [0; 7],
        ..cur
    };
    let rec2 = PicRecord {
        upscaled_width: 320,
        ..rec
    };
    let (mf2, _) = mf::select(&seq, &cur2, &[Some(rec2); 7], &[None; 7]);
    assert!(mf2.mf_type[0] != 0, "a same-size reference must be a candidate");
}

/// Large-scale tile (OBU_TILE_LIST, type 8) is still refused loudly: NVDEC
/// decodes ordinary frame/tile-group OBUs only, and the tile-list mode (a
/// still-picture map of independently decodable tiles) has no engine path.
#[test]
fn large_scale_tile_list_stays_unsupported() {
    let mut c = Av1Codec::new();
    // OBU header: forbidden 0, type 8, extension 0, has_size_field 1, reserved 0.
    c.push_unit(&[0x42, 0x01, 0x00]);
    match c.next_out() {
        Err(obu::Av1Error::Unsupported(msg)) => assert!(msg.contains("OBU_TILE_LIST"), "{msg}"),
        Err(e) => panic!("expected Unsupported, got {e}"),
        Ok(_) => panic!("expected Unsupported, got a picture"),
    }
}
