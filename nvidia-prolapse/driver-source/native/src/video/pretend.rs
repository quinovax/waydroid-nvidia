//! Host-only "pretend class" builds (tests only): run a codec over a stream with the session's surface
//! accounting for any NVDEC class and host GPFIFO class, without a GPU, and return every picture's
//! descriptor slot and method list. Used to check the per-generation behaviour against stock's
//! per-class gates
//! and against stock traces captured on other generations (`dump_pretend`, compare script
//! `tools/video/compare_pretend.py`).
use super::{
    bits, session::DESCRIPTOR_BYTES, Allocator, BuildContext, CodecDecoder, Job, SurfaceRef,
    VResult, VideoError, CODEC_AV1, CODEC_H264, CODEC_HEVC, CODEC_VP9,
};

/// One built picture.
pub struct Built {
    pub desc: Vec<u8>,
    pub words: Vec<u32>,
    /// GPU addresses the build used (descriptor slot, bitstream slot).
    pub descriptor: u64,
    pub bitstream: u64,
    /// The job asked for a CPU readback (VP9 backward adaptation): fed zero counts here.
    pub readback: bool,
}

/// Bump allocator of 40-bit VAs (the INC relocation form needs `va >> 40 == 0`, as our session's).
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

/// The codec as `session.rs` creates it for `class`.
pub fn decoder(
    codec: u32,
    class: u32,
    w: u32,
    h: u32,
    bit_depth: u32,
    config: &[u8],
) -> VResult<Box<dyn CodecDecoder>> {
    Ok(match codec {
        CODEC_VP9 => Box::new(super::vp9::Vp9Decoder::new(class, w, h, bit_depth)),
        CODEC_AV1 => Box::new(super::av1::codec::Av1Decoder::new(w, h, bit_depth, config)?),
        CODEC_H264 => Box::new(super::h264::codec::H264Decoder::new(class, w, h, config)?),
        CODEC_HEVC => Box::new(super::hevc::codec::HevcDecoder::new(
            class, w, h, bit_depth, config,
        )?),
        c => return Err(VideoError::Unsupported(format!("codec {c}"))),
    })
}

/// Decode units as a caller feeds them: Annex-B access units, IVF frames.
pub fn units(codec: u32, data: &[u8]) -> Vec<Vec<u8>> {
    match codec {
        CODEC_H264 | CODEC_HEVC => bits::access_units(data, codec == CODEC_HEVC)
            .into_iter()
            .map(<[u8]>::to_vec)
            .collect(),
        _ => super::av1::obu::ivf_frames(data)
            .expect("IVF stream")
            .into_iter()
            .map(<[u8]>::to_vec)
            .collect(),
    }
}

/// Run `data` through the codec for `class` / `host_class`; the caller releases every picture at once.
pub fn run(
    codec: u32,
    class: u32,
    host_class: u32,
    data: &[u8],
    size: (u32, u32),
    bit_depth: u32,
) -> VResult<Vec<Built>> {
    Ok(run_units(codec, class, host_class, data, size, bit_depth)?
        .into_iter()
        .flatten()
        .collect())
}

/// Like [`run`], but keeps the per-unit grouping: `out[i]` holds the pictures the i-th unit
/// produced, empty when the unit produced none (parameter sets only, a skipped RASL picture,
/// `pic_output_flag = 0`). This is the codec-side counterpart of `nv_video_decode`'s ABI v2
/// return value.
pub fn run_units(
    codec: u32,
    class: u32,
    host_class: u32,
    data: &[u8],
    (w, h): (u32, u32),
    bit_depth: u32,
) -> VResult<Vec<Vec<Built>>> {
    // AV1: the first temporal unit carries the sequence header (as the av1 tests pass it);
    // it is still decoded, like the session does when the caller sends frame 0 in-band.
    let units = units(codec, data);
    let config = if codec == CODEC_AV1 {
        units.first().cloned().unwrap_or_default()
    } else {
        Vec::new()
    };
    let mut dec = decoder(codec, class, w, h, bit_depth, &config)?;
    dec.allocate(&mut Mock(1 << 36), w, h)?;
    let layout = dec.surface_layout(w, h);
    let total = dec.dpb_size() + 2;
    let surfaces: Vec<SurfaceRef> = (0..total as u64)
        .map(|i| {
            let base = (2 << 36) + i * (layout.size as u64).next_multiple_of(1 << 16);
            SurfaceRef {
                luma: base,
                chroma: base + layout.chroma_offset,
            }
        })
        .collect();
    let mut out = Vec::with_capacity(units.len());
    let mut picture_index = 0u32;
    for unit in &units {
        let mut built = Vec::new();
        dec.push_unit(unit)?;
        while let Some(job) = dec.next_job()? {
            let Job::Decode(job) = job else { continue };
            let referenced = dec.referenced();
            let current = (0..total)
                .find(|i| !referenced.contains(i))
                .ok_or_else(|| VideoError::Invalid("no free surface".into()))?;
            let k = picture_index as u64 % 4;
            let ctx = BuildContext {
                decoder_class: class,
                host_class,
                descriptor: (3 << 36) + k * 0x1_0000,
                bitstream: (4 << 36) + k * (16 << 20),
                bitstream_len: job.bitstream.len(),
                picture_index,
                current,
                surfaces: &surfaces,
                layout,
            };
            let mut desc = vec![0u8; DESCRIPTOR_BYTES];
            let words = dec.build(&job, &ctx, &mut desc)?;
            let readback = job.readback.clone().map(|r| vec![0u8; r.len()]);
            dec.finish(&job, current, readback.as_deref())?;
            built.push(Built {
                desc,
                words,
                descriptor: ctx.descriptor,
                bitstream: ctx.bitstream,
                readback: readback.is_some(),
            });
            picture_index += 1;
        }
        out.push(built);
    }
    Ok(out)
}

/// Decoded class methods (subchannel 4) of a method list: (method, values) in order, any header form.
pub fn methods(words: &[u32]) -> Vec<(u32, &'static str, Vec<u32>)> {
    let mut r = Vec::new();
    let mut i = 0;
    while i < words.len() {
        let h = words[i];
        let (kind, n) = match h >> 29 {
            1 => ("INC", (h >> 16) & 0x1fff),
            3 => ("NONINC", (h >> 16) & 0x1fff),
            4 => ("IMM", 0),
            _ => ("?", (h >> 16) & 0x1fff),
        };
        let m = (h & 0x1fff) << 2;
        let vals = words[i + 1..i + 1 + n as usize].to_vec();
        r.push((m, kind, vals));
        i += 1 + n as usize;
    }
    r
}

/// Dump every picture of the streams in `$NV_PRETEND_STREAMS` built for `$NV_PRETEND_CLASS` (hex, default
/// c9b0) / `$NV_PRETEND_HOST` (hex, default c56f) into `$NV_PRETEND_OUT/<stream>/NNN.{desc,words}` for
/// `tools/video/compare_pretend.py` against stock traces of the same streams.
#[test]
#[ignore = "needs NV_PRETEND_STREAMS / NV_PRETEND_OUT"]
fn dump_pretend() {
    let (Ok(dir), Ok(out)) = (
        std::env::var("NV_PRETEND_STREAMS"),
        std::env::var("NV_PRETEND_OUT"),
    ) else {
        panic!("set NV_PRETEND_STREAMS and NV_PRETEND_OUT");
    };
    let hex = |k: &str, d: u32| {
        std::env::var(k).map_or(d, |v| {
            u32::from_str_radix(v.trim_start_matches("0x"), 16).unwrap()
        })
    };
    let (class, host) = (
        hex("NV_PRETEND_CLASS", 0xc9b0),
        hex("NV_PRETEND_HOST", 0xc56f),
    );
    let streams = [
        ("h264_high.h264", CODEC_H264, 8),
        ("h264_mbaff.h264", CODEC_H264, 8),
        ("hevc_main.hevc", CODEC_HEVC, 8),
        ("hevc_main10.hevc", CODEC_HEVC, 10),
        ("vp9_p0.ivf", CODEC_VP9, 8),
        ("vp9_p2.ivf", CODEC_VP9, 10),
        ("av1_8bit.ivf", CODEC_AV1, 8),
        ("av1_10bit.ivf", CODEC_AV1, 10),
    ];
    for (name, codec, bd) in streams {
        let data = std::fs::read(format!("{dir}/{name}")).unwrap();
        let size = if name.starts_with("h264_mbaff") {
            (640, 384)
        } else {
            (640, 360)
        };
        let stem = name.split('.').next().unwrap();
        let o = format!("{out}/{stem}");
        std::fs::create_dir_all(&o).unwrap();
        let pics = run(codec, class, host, &data, size, bd).unwrap();
        for (n, p) in pics.iter().enumerate() {
            std::fs::write(format!("{o}/{n:03}.desc"), &p.desc).unwrap();
            let w: Vec<u8> = p.words.iter().flat_map(|w| w.to_le_bytes()).collect();
            std::fs::write(format!("{o}/{n:03}.words"), w).unwrap();
            std::fs::write(
                format!("{o}/{n:03}.meta"),
                format!(
                    "descriptor {:#x}\nbitstream {:#x}\nreadback {}\n",
                    p.descriptor, p.bitstream, p.readback
                ),
            )
            .unwrap();
        }
        println!("{name}: {} pictures", pics.len());
    }
}

/// Expected per-class behaviour, written down independently from stock's per-class consequences: (class, host GPFIFO class of that family, H.264 progressive application id,
/// relocation NONINC pair, method 0x4c8, method 0x4dc / blackwell_b fields, AV1 available).
const PER_CLASS: [(u32, u32, u32, bool, bool, bool, bool); 4] = [
    (0xc4b0, 0xc46f, 3, false, false, false, false), // Turing
    (0xc7b0, 0xc56f, 3, false, true, false, true),   // Ampere GA10x
    (0xc9b0, 0xc56f, 3, false, true, false, true),   // Ada
    (0xcfb0, 0xc96f, 8, true, true, true, true),     // Blackwell GB20x
];

const H264: &[u8] = include_bytes!("testdata/h264_160x144.h264");
const HEVC: &[u8] = include_bytes!("testdata/hevc_160x144.hevc");
const VP9: &[u8] = include_bytes!("testdata/vp9_160x144.ivf");
const AV1: &[u8] = include_bytes!("av1/testdata/aom_hidden_640.ivf");

fn rd32(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(d[o..o + 4].try_into().unwrap())
}

/// Every buffer method has the stock relocation form of the class's host GPFIFO class.
fn check_relocs(class: u32, pics: &[Built], noninc: bool) {
    for (n, p) in pics.iter().enumerate() {
        for (m, kind, vals) in methods(&p.words) {
            if [0x200, 0x400, 0x40c, 0x300].contains(&m) {
                assert_eq!((kind, vals.len()), ("INC", 1), "{class:#x} pic {n} {m:#x}");
            } else if noninc {
                assert_eq!(
                    (kind, vals.len()),
                    ("NONINC", 2),
                    "{class:#x} pic {n} {m:#x}"
                );
            } else {
                assert_eq!((kind, vals.len()), ("INC", 1), "{class:#x} pic {n} {m:#x}");
            }
        }
    }
}

fn has(p: &Built, m: u32) -> bool {
    methods(&p.words).iter().any(|x| x.0 == m)
}

#[test]
fn per_generation_method_lists_and_fields() {
    for (class, host, h264_app, noninc, m4c8, bw_b, av1) in PER_CLASS {
        // H.264 progressive High: NEW_H264 (8) on Blackwell, legacy (3, 0x410/0x500/0x418) elsewhere.
        let pics = run(CODEC_H264, class, host, H264, (160, 144), 8).unwrap();
        assert_eq!(pics.len(), 4);
        check_relocs(class, &pics, noninc);
        for p in &pics {
            let app = methods(&p.words)[0].2[0];
            assert_eq!(app, h264_app, "{class:#x}");
            let legacy = app == 3;
            assert_eq!(
                has(p, 0x410) && has(p, 0x500) && has(p, 0x418),
                legacy,
                "{class:#x}"
            );
            assert_eq!(has(p, 0x4c8), !legacy && m4c8, "{class:#x}");
            assert_eq!(has(p, 0x4dc), !legacy && bw_b, "{class:#x}");
            if !legacy {
                // NEW_H264 ext fields (0x1fc width word) only on blackwell_b classes.
                assert_eq!(rd32(&p.desc, 0x1fc) != 0, bw_b, "{class:#x}");
            }
        }
        // HEVC: 0x4c8 GA10x+, 0x4dc and +0x71/+0xba/+0x11c/+0x120 blackwell_b only.
        let pics = run(CODEC_HEVC, class, host, HEVC, (160, 144), 8).unwrap();
        assert_eq!(pics.len(), 4);
        check_relocs(class, &pics, noninc);
        for p in &pics {
            assert_eq!(methods(&p.words)[0].2[0], 7);
            assert_eq!(has(p, 0x4c8), m4c8, "{class:#x}");
            assert_eq!(has(p, 0x4dc), bw_b, "{class:#x}");
            assert_eq!(p.desc[0x71] == 9, bw_b, "{class:#x}");
            assert_eq!(rd32(&p.desc, 0x11c) != 0, bw_b, "{class:#x}");
            assert_eq!(
                u16::from_le_bytes([p.desc[0x120], p.desc[0x121]]) == 0xffff,
                bw_b
            );
            if !bw_b {
                assert_eq!((p.desc[0x71], p.desc[0xba], p.desc[0xbb]), (0, 0, 0));
            }
        }
        // VP9: 0x5dc at filter + 0x10000 with 0x4c8 = filter on GA10x+, at filter + 0 on Turing;
        // 0x4dc, +0xeb, +0x43, +0x108 blackwell_b only.
        let pics = run(CODEC_VP9, class, host, VP9, (160, 144), 8).unwrap();
        assert_eq!(pics.len(), 4);
        check_relocs(class, &pics, noninc);
        for p in &pics {
            let ms = methods(&p.words);
            let va = |m: u32| ms.iter().find(|x| x.0 == m).map(|x| x.2[0]);
            assert_eq!(ms[0].2[0], 9);
            assert_eq!(va(0x4c8).is_some(), m4c8, "{class:#x}");
            if let Some(f) = va(0x4c8) {
                assert_eq!(va(0x5dc), Some(f + 0x100), "{class:#x}");
            }
            assert_eq!(has(p, 0x4dc), bw_b, "{class:#x}");
            assert_eq!(p.desc[0xeb] == 9, bw_b, "{class:#x}");
            assert_eq!(rd32(&p.desc, 0x108) != 0, bw_b, "{class:#x}");
            if !bw_b {
                assert_eq!(p.desc[0x43], 0);
            }
        }
        // AV1: GA10x+ only (caps); 0x4dc, stat word, +0x244, +0x54 blackwell_b only.
        assert_eq!(
            super::caps(CODEC_AV1, class, &super::NvdecEngineCaps::ALL).max_width,
            8192
        );
        assert_eq!(
            super::NvdecGen::of(class).ga10x_plus(),
            av1,
            "{class:#x} AV1 availability"
        );
        if av1 {
            let pics = run(CODEC_AV1, class, host, AV1, (640, 360), 8).unwrap();
            check_relocs(class, &pics, noninc);
            for p in &pics {
                assert_eq!(has(p, 0x4c8), m4c8);
                assert_eq!(has(p, 0x4dc), bw_b, "{class:#x}");
                assert_eq!(rd32(&p.desc, 0x238) != 0, bw_b, "{class:#x}");
                assert_eq!(rd32(&p.desc, 0x244) != 0, bw_b, "{class:#x}");
                assert_eq!(rd32(&p.desc, 0x54) == 0xffff, bw_b, "{class:#x}");
            }
        }
    }
}

/// A buffer above 40 bits needs the NONINC pair on every class (stock reloc_4a8a0).
#[test]
fn relocation_form() {
    let mut w = Vec::new();
    super::reloc(&mut w, 0xc56f, 0x404, 0x12_3456_7800);
    assert_eq!(w, [0x2001_8101, 0x1234_5678]);
    w.clear();
    super::reloc(&mut w, 0xc56f, 0x404, 0x100_0000_0000);
    assert_eq!(w, [0x6002_8101, 0, 1]);
    w.clear();
    super::reloc(&mut w, 0xc96f, 0x404, 0x12_3456_7800);
    assert_eq!(w, [0x6002_8101, 0x1234_5678, 0]);
}

/// Session-level capabilities per class as stock's caps query (gens.rs `decoder_caps_2c520`), with the
/// verification opt-in off.
#[test]
fn caps_per_generation() {
    use super::{class_gap, NvdecEngineCaps};
    let caps = |codec, class| super::caps(codec, class, &NvdecEngineCaps::ALL);
    let ok = |codec, class| caps(codec, class).max_width != 0 && class_gap(codec, class).is_none();
    // AV1 needs GA10x+: not Turing, GA100, Hopper.
    for c in [0xc4b0, 0xc6b0, 0xb8b0] {
        assert!(!super::NvdecGen::of(c).ga10x_plus(), "{c:#x}");
    }
    // Pre-Turing classes decode nothing.
    assert!(!super::NvdecGen::of(0xc3b0).turing_plus());
    assert_eq!(caps(CODEC_H264, 0xc3b0).supported, 0);
    // H.264: 4096² everywhere; min height 64 on Blackwell, 16 before.
    assert_eq!(caps(CODEC_H264, 0xc9b0).min_height, 16);
    assert_eq!(caps(CODEC_H264, 0xcfb0).min_height, 64);
    // VP9 profile 2 follows the engine's BSP caps (stock 0x2c470), on every Turing+ class.
    let hbd = |t: [u8; 8], class| {
        super::caps(
            CODEC_VP9,
            class,
            &NvdecEngineCaps::from_caps_table(t, class),
        )
        .bit_depth_mask
    };
    for class in [0xc4b0, 0xc6b0, 0xc7b0, 0xc9b0, 0xcfb0] {
        assert_eq!(
            hbd([1, 0, 0, 0, 0x10, 0, 0, 0], class),
            1 << 8 | 1 << 10,
            "{class:#x}"
        );
        assert_eq!(hbd([1, 0, 0, 0, 0, 0, 0, 0], class), 1 << 8, "{class:#x}");
    }
    // capsTbl[3] bit 1 vetoes VP9 from Ada on only; bit 0 vetoes AV1 from Ada on only.
    let e = |t, class| NvdecEngineCaps::from_caps_table(t, class);
    assert!(e([1, 0, 0, 2, 0x10, 0, 0, 0], 0xc7b0).decodes(CODEC_VP9));
    assert!(!e([1, 0, 0, 2, 0x10, 0, 0, 0], 0xc9b0).decodes(CODEC_VP9));
    assert!(!e([1, 0, 0, 2, 0x10, 0, 0, 0], 0xc9b0).vp9_high_bit_depth);
    assert!(e([1, 0, 0, 1, 0, 0, 0, 0], 0xc7b0).decodes(CODEC_AV1));
    assert!(!e([1, 0, 0, 1, 0, 0, 0, 0], 0xcfb0).decodes(CODEC_AV1));
    assert!(!e([0, 0, 0, 0, 0x10, 0, 0, 0], 0xc7b0).decodes(CODEC_VP9));
    assert!(!e([1, 1, 1, 0, 0, 0, 0, 0], 0xc7b0).decodes(CODEC_HEVC));
    // D1B0: extra setup word not reproduced → refused with a reason.
    assert!(class_gap(CODEC_HEVC, 0xd1b0).is_some() && class_gap(CODEC_AV1, 0xd1b0).is_none());
    assert!(ok(CODEC_HEVC, 0xc4b0));
    // Classes verified on hardware (CFB0/C9B0/C7B0/C4B0) are enabled without the opt-in; GA100 is not.
    if !super::untested_classes_allowed() {
        for class in [0xcfb0, 0xc9b0, 0xc7b0, 0xc4b0] {
            assert_eq!(caps(CODEC_HEVC, class).supported, 1, "{class:#x}");
        }
        assert_eq!(caps(CODEC_HEVC, 0xc6b0).supported, 0);
    }
}
