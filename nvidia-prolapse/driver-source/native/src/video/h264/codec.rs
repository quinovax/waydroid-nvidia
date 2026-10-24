//! H.264 on the nv_video session layer (`CodecDecoder`).
//!
//! Methods and buffers follow stock libnvcuvid 615.71.09 as captured in RM traces of test
//! streams:
//! - descriptor slot: setup at 0, scaling matrices at +0x200 (NEW_H264), slice offsets at
//!   +0x300 (legacy), engine status at +0x700;
//! - 17 interleaved LUMA/CHROMA offsets indexed by colocated slot, unused = current target;
//! - one colocated-MV buffer (slot i at i × per-slot size), a filter buffer with INTRA_TOP at
//!   `width·16 + 256` and the 0x4dc buffer at `align(width·4, 256)` after it (measured at widths
//!   176, 352, 1920), legacy MBHIST/HISTORY instead of filter/intra/0x4dc.
use super::{desc, legacy, Begun, Decoder, MAX_SLOTS};
use crate::gpu::method;
use crate::video::{
    bits, Allocator, BuildContext, CodecDecoder, DecodeJob, Job, NvdecGen,
    PictureInfo as SessionInfo, SurfaceLayout, VResult, VideoError, CODEC_H264,
};
use std::collections::VecDeque;

const SCALING: u64 = 0x200;
const SLICE_OFFSETS: u64 = 0x300;
const STATUS: u64 = 0x700;
/// Blackwell 0x4dc buffer (tinygrad `_ensure_has_vid_hw`: 2 MiB; stock places it after
/// INTRA_TOP).
const SIZE_4DC: u64 = 2 << 20;

pub struct H264Decoder {
    dec: Decoder,
    units: VecDeque<Vec<u8>>,
    pending: Option<Begun>,
    max: (u32, u32),
    colmv: u64,
    colmv_slot: u64,
    filter: u64,
    intra_top: u64,
    buf_4dc: u64,
    mbhist: u64,
    history: u64,
    reorder: u32,
    gen: NvdecGen,
}

impl H264Decoder {
    pub fn new(
        decoder_class: u32,
        max_width: u32,
        max_height: u32,
        codec_config: &[u8],
    ) -> VResult<Self> {
        if max_width > 4096 || max_height > 4096 {
            return Err(VideoError::Unsupported(format!(
                "H.264 session {max_width}x{max_height} above 4096x4096"
            )));
        }
        let layout = SurfaceLayout::new(max_width, max_height, 1);
        let mut dec = Decoder::new(desc::EngineParams::for_class(
            decoder_class,
            Some(layout.pitch),
        ));
        if !codec_config.is_empty() {
            if codec_config[0] == 1 {
                return Err(VideoError::Unsupported(
                    "H.264 avcC codec_config (pass Annex-B parameter sets)".into(),
                ));
            }
            dec.configure(codec_config)?;
        }
        Ok(Self {
            dec,
            units: VecDeque::new(),
            pending: None,
            max: (max_width, max_height),
            colmv: 0,
            colmv_slot: 0,
            filter: 0,
            intra_top: 0,
            buf_4dc: 0,
            mbhist: 0,
            history: 0,
            reorder: 16,
            gen: NvdecGen::of(decoder_class),
        })
    }
}

impl CodecDecoder for H264Decoder {
    fn codec(&self) -> u32 {
        CODEC_H264
    }
    fn bit_depth(&self) -> u32 {
        8
    }
    fn dpb_size(&self) -> usize {
        16
    }
    fn allocate(
        &mut self,
        alloc: &mut dyn Allocator,
        max_width: u32,
        max_height: u32,
    ) -> VResult<()> {
        let w_mbs = u64::from(max_width.div_ceil(16));
        let h_mbs = u64::from(max_height.div_ceil(16));
        // Per slot: colMV row (worst case 16 vectors per MB, 8 B each) × MB rows.
        self.colmv_slot = (w_mbs * 16 * 8).next_multiple_of(256) * h_mbs;
        self.colmv = alloc.vram((self.colmv_slot * MAX_SLOTS as u64) as usize)?;
        let filter = u64::from(max_width.next_multiple_of(16)) * 16 + 256;
        let intra = (u64::from(max_width.next_multiple_of(16)) * 4).next_multiple_of(256);
        self.filter = alloc.vram((filter + intra + SIZE_4DC) as usize)?;
        self.intra_top = self.filter + filter;
        self.buf_4dc = self.intra_top + intra;
        // Legacy (interlaced) side buffers: MBHIST 128 B per MB row, HISTORY 768 B per MB column.
        self.mbhist = alloc.vram((h_mbs * 128).next_multiple_of(256) as usize)?;
        self.history = alloc.vram((w_mbs * 768) as usize)?;
        Ok(())
    }
    fn surfaces_needed(&self, data: &[u8]) -> VResult<usize> {
        Ok(bits::split_annexb(data)
            .iter()
            .any(|n| matches!(n.nal[0] & 0x1f, 1 | 5)) as usize)
    }
    fn push_unit(&mut self, data: &[u8]) -> VResult<()> {
        self.units.push_back(data.to_vec());
        Ok(())
    }
    fn next_job(&mut self) -> VResult<Option<Job>> {
        while let Some(au) = self.units.pop_front() {
            let Some(b) = self.dec.begin(&au)? else {
                continue;
            };
            // Coded size may exceed the (display) session max by the MB padding; surfaces are
            // allocated from the max rounded to 16 or more (SurfaceLayout).
            if b.info.width > self.max.0.next_multiple_of(16)
                || b.info.height > self.max.1.next_multiple_of(16)
            {
                return Err(crate::video::too_large(
                    "H.264",
                    b.info.width,
                    b.info.height,
                    self.max,
                    b.info.keyframe,
                ));
            }
            let i = b.info;
            let (l, r, t, bo) = i.crop;
            let info = SessionInfo {
                width: i.width,
                height: i.height,
                crop: [l, t, i.width - l - r, i.height - t - bo],
                bit_depth: 8,
                color_primaries: i.colour.0,
                transfer: i.colour.1,
                matrix: i.colour.2,
                full_range: i.colour.3,
                keyframe: i.keyframe,
            };
            let job = DecodeJob {
                info,
                show: true,
                display_key: Some(b.display_key),
                bitstream: b.bitstream().to_vec(),
                readback: None,
            };
            self.pending = Some(b);
            return Ok(Some(Job::Decode(job)));
        }
        Ok(None)
    }
    fn build(&mut self, _job: &DecodeJob, ctx: &BuildContext, d: &mut [u8]) -> VResult<Vec<u32>> {
        let b = self
            .pending
            .take()
            .ok_or_else(|| VideoError::Invalid("H.264 build without a parsed picture".into()))?;
        let mut job = self.dec.complete(b, ctx.current as u32)?;
        self.reorder = job.max_num_reorder;
        if job.app_id == desc::APP_ID_NEW_H264 {
            job.setup.truncate(0x2e0);
        }
        d[..job.setup.len()].copy_from_slice(&job.setup);
        let da = ctx.descriptor;
        let cur = ctx.surfaces[ctx.current];
        let mut w = Vec::new();
        // Relocation encoding per host class (stock reloc_4a8a0).
        let reloc = |w: &mut Vec<u32>, m: u32, va: u64| ctx.reloc(w, m, va);
        method(&mut w, 4, 0x200, &[job.app_id]);
        method(&mut w, 4, 0x400, &[job.control_params]);
        reloc(&mut w, 0x404, da);
        reloc(&mut w, 0x408, ctx.bitstream);
        let legacy = job.app_id == legacy::APP_ID_H264;
        if legacy {
            reloc(&mut w, 0x410, da + SLICE_OFFSETS);
        }
        for (i, s) in job.slot_surfaces.iter().enumerate() {
            let s = s.map_or(cur, |s| ctx.surfaces[s as usize]);
            reloc(&mut w, 0x430 + 4 * i as u32, s.luma);
            reloc(&mut w, 0x474 + 4 * i as u32, s.chroma);
        }
        reloc(&mut w, 0x414, self.colmv);
        reloc(&mut w, 0x424, da + STATUS);
        method(&mut w, 4, 0x40c, &[ctx.picture_index]);
        if legacy {
            reloc(&mut w, 0x500, self.mbhist);
            reloc(&mut w, 0x418, self.history);
        } else {
            reloc(&mut w, 0x680, da + SCALING);
            reloc(&mut w, 0x4d0, self.filter);
            // NEW_H264 runs on Blackwell+ only (all GA10x+); 0x4dc on blackwell_b classes only
            // (gens.rs DECODE_GATES 0x443bc / 0x43ec5).
            if self.gen.ga10x_plus() {
                reloc(&mut w, 0x4c8, self.intra_top);
            }
            if self.gen.blackwell_b() {
                reloc(&mut w, 0x4dc, self.buf_4dc);
            }
        }
        method(&mut w, 4, 0x300, &[0]);
        Ok(w)
    }
    fn finish(
        &mut self,
        _job: &DecodeJob,
        _current: usize,
        _readback: Option<&[u8]>,
    ) -> VResult<()> {
        Ok(())
    }
    fn referenced(&self) -> Vec<usize> {
        self.dec
            .referenced_surfaces()
            .into_iter()
            .map(|s| s as usize)
            .collect()
    }
    fn bump_ready(&self, pending: usize) -> bool {
        pending > self.reorder as usize
    }
    fn flush(&mut self) {
        self.dec.flush();
        self.units.clear();
        self.pending = None;
    }
}
