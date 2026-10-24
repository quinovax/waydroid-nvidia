//! HEVC on the nv_video session layer (`CodecDecoder`).
//!
//! Method list as stock 615.71.09 emits it (RM traces of test streams): APPLICATION_ID 7, CONTROL_PARAMS, DRV_PIC_SETUP,
//! IN_BUF_BASE, 16 LUMA/CHROMA pairs by DPB slot (unused = current target), COLOC_DATA,
//! NVDEC_STATUS (setup + 0xd00), PICTURE_INDEX, HEVC_SET_SCALING_LIST (setup + 0x200),
//! HEVC_SET_TILE_SIZES (setup + 0x600), FILTER_BUFFER, INTRA_TOP, 0x4dc, slot 16 = current target,
//! EXECUTE.
//!
//! Side buffers use the layout proven bit-exact by the V0 path (`crate::nvdec::decode_hevc`,
//! tinygrad ops_nv.py): filter sub-buffers of 608 / 4864 / 152 / 2000 bytes per luma row,
//! INTRA_TOP after them, the Blackwell 0x4dc buffer 64 KiB further; colocated MVs
//! `h64² + w64·h64/16`. All sized from the session maximum.
use super::desc::{FilterOffsets, SessionInputs, SETUP_SIZE};
use super::Decoder;
use crate::gpu::method;
use crate::video::{
    bits, Allocator, BuildContext, CodecDecoder, DecodeJob, Job, NvdecGen, PictureInfo,
    SurfaceLayout, VResult, VideoError, CODEC_HEVC,
};
use std::collections::{HashMap, VecDeque};

const STATUS: u64 = 0xd00;
const SLOTS: usize = 17;

pub struct HevcDecoder {
    dec: Decoder,
    units: VecDeque<Vec<u8>>,
    pending: Option<super::PictureJob>,
    max: (u32, u32),
    colmv: u64,
    filter: u64,
    intra_top: u64,
    buf_4dc: u64,
    surface_of: HashMap<u64, usize>,
    live: Vec<u64>,
    reorder: u32,
    epoch: i64,
    epoch_max_poc: Option<i32>,
    /// Session bit depth (8 = Main, 10 = Main 10); surfaces are sized for it.
    bit_depth: u32,
    gen: NvdecGen,
}

fn filter_layout(max_height: u32) -> (FilterOffsets, u64, u64, u64) {
    let h64 = u64::from(max_height.next_multiple_of(64));
    let step = |prev: u32, bytes_per_row: u64| ((u64::from(prev) << 8) + bytes_per_row * h64) >> 8;
    let sao = step(0, 608) as u32;
    let bsd = step(sao, 4864) as u32;
    let flt = step(bsd, 152) as u32;
    let sao_above = step(flt, 2000) as u32;
    let intra_top = h64 * (608 + 4864 + 152 + 2000);
    let off_4dc = intra_top.next_multiple_of(0x10000) + (64 << 10);
    let size = (off_4dc + (2 << 20)).next_multiple_of(2 << 20);
    (
        FilterOffsets {
            sao,
            bsd_ctrl: bsd,
            flt_above: flt,
            sao_above,
            slice_edge: sao_above,
        },
        intra_top,
        off_4dc,
        size,
    )
}

impl HevcDecoder {
    pub fn new(
        decoder_class: u32,
        max_width: u32,
        max_height: u32,
        bit_depth: u32,
        codec_config: &[u8],
    ) -> VResult<Self> {
        let layout = SurfaceLayout::new(
            max_width,
            max_height,
            super::super::bytes_per_sample(bit_depth),
        );
        let inputs = SessionInputs {
            // framestride in samples: stock hevc10 capture (352x288) writes 352 for its 704-byte
            // 16-bit pitch, hevc8 384 for a 384-byte pitch.
            framestride: [layout.pitch / layout.bytes_per_sample; 2],
            filter: filter_layout(max_height).0,
            gptimer_max: u32::MAX,
            general_bits: 1, // tileformat 1, GOB_2 (stock and V0)
            tail_11a: 0,
            blackwell_b: NvdecGen::of(decoder_class).blackwell_b(),
        };
        let mut dec = Decoder::new(inputs);
        if !codec_config.is_empty() {
            if codec_config[0] == 1 {
                return Err(VideoError::Unsupported(
                    "HEVC hvcC codec_config (pass Annex-B parameter sets)".into(),
                ));
            }
            if dec.decode_au(codec_config)?.is_some() {
                return Err(VideoError::Invalid(
                    "HEVC codec_config contains a picture".into(),
                ));
            }
        }
        Ok(Self {
            dec,
            units: VecDeque::new(),
            pending: None,
            max: (max_width, max_height),
            colmv: 0,
            filter: 0,
            intra_top: 0,
            buf_4dc: 0,
            surface_of: HashMap::new(),
            live: Vec::new(),
            reorder: 16,
            epoch: 0,
            epoch_max_poc: None,
            bit_depth,
            gen: NvdecGen::of(decoder_class),
        })
    }
}

impl CodecDecoder for HevcDecoder {
    fn codec(&self) -> u32 {
        CODEC_HEVC
    }
    fn bit_depth(&self) -> u32 {
        self.bit_depth
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
        let (w64, h64) = (
            u64::from(max_width.next_multiple_of(64)),
            u64::from(max_height.next_multiple_of(64)),
        );
        self.colmv = alloc.vram((h64 * h64 + w64 * h64 / 16).next_multiple_of(2 << 20) as usize)?;
        let (_, intra, off_4dc, size) = filter_layout(max_height);
        self.filter = alloc.vram(size as usize)?;
        self.intra_top = self.filter + intra;
        self.buf_4dc = self.filter + off_4dc;
        Ok(())
    }
    fn surfaces_needed(&self, data: &[u8]) -> VResult<usize> {
        Ok(bits::split_annexb(data)
            .iter()
            .any(|n| (n.nal[0] >> 1) & 0x3f < 32) as usize)
    }
    fn push_unit(&mut self, data: &[u8]) -> VResult<()> {
        self.units.push_back(data.to_vec());
        Ok(())
    }
    fn next_job(&mut self) -> VResult<Option<Job>> {
        while let Some(au) = self.units.pop_front() {
            let Some(j) = self.dec.decode_au(&au)? else {
                continue;
            };
            if j.bit_depth != self.bit_depth {
                return Err(VideoError::Unsupported(format!(
                    "HEVC {}-bit picture in a {}-bit session (create the session with the stream's \
                     bit depth)",
                    j.bit_depth, self.bit_depth
                )));
            }
            if j.width > self.max.0.next_multiple_of(8) || j.height > self.max.1.next_multiple_of(8)
            {
                return Err(crate::video::too_large(
                    "HEVC", j.width, j.height, self.max, j.keyframe,
                ));
            }
            // Output epochs: an IRAP whose POC does not exceed every POC of the current epoch
            // resets POC (IDR/BLA/CRA with NoRaslOutputFlag); pictures before it output first.
            if j.keyframe && self.epoch_max_poc.is_some_and(|m| j.poc <= m) {
                self.epoch += 1;
                self.epoch_max_poc = None;
            }
            self.epoch_max_poc = Some(self.epoch_max_poc.map_or(j.poc, |m| m.max(j.poc)));
            self.reorder = j.max_num_reorder;
            self.live = j.refs.iter().map(|r| r.1).collect();
            self.surface_of.retain(|id, _| self.live.contains(id));
            let [l, r, t, b] = j.crop;
            let info = PictureInfo {
                width: j.width,
                height: j.height,
                crop: [l, t, j.width - l - r, j.height - t - b],
                bit_depth: j.bit_depth,
                color_primaries: 2,
                transfer: 2,
                matrix: 2,
                full_range: false,
                keyframe: j.keyframe,
            };
            let job = DecodeJob {
                info,
                show: j.output,
                display_key: Some((self.epoch << 32) + i64::from(j.poc)),
                bitstream: j.bitstream.clone(),
                readback: None,
            };
            self.pending = Some(j);
            return Ok(Some(Job::Decode(job)));
        }
        Ok(None)
    }
    fn build(&mut self, _job: &DecodeJob, ctx: &BuildContext, d: &mut [u8]) -> VResult<Vec<u32>> {
        let j = self
            .pending
            .take()
            .ok_or_else(|| VideoError::Invalid("HEVC build without a parsed picture".into()))?;
        d[..SETUP_SIZE].copy_from_slice(&j.setup);
        let cur = ctx.surfaces[ctx.current];
        let mut slot_surface = [cur; SLOTS];
        for &(slot, id) in &j.refs {
            let s = self.surface_of.get(&id).ok_or_else(|| {
                VideoError::Invalid(format!("HEVC reference picture {id} has no surface"))
            })?;
            slot_surface[slot as usize] = ctx.surfaces[*s];
        }
        slot_surface[j.slot as usize] = cur;
        self.surface_of.insert(j.pic_id, ctx.current);
        self.live.push(j.pic_id);
        let da = ctx.descriptor;
        let m = &j.methods;
        let mut w = Vec::new();
        // Relocation encoding per host class (stock reloc_4a8a0).
        let reloc = |w: &mut Vec<u32>, m: u32, va: u64| ctx.reloc(w, m, va);
        method(&mut w, 4, 0x200, &[m.app_id]);
        method(&mut w, 4, 0x400, &[m.control_params]);
        reloc(&mut w, 0x404, da);
        reloc(&mut w, 0x408, ctx.bitstream);
        for (i, s) in slot_surface.iter().enumerate().take(16) {
            reloc(&mut w, 0x430 + 4 * i as u32, s.luma);
            reloc(&mut w, 0x474 + 4 * i as u32, s.chroma);
        }
        reloc(&mut w, 0x414, self.colmv);
        reloc(&mut w, 0x424, da + STATUS);
        method(&mut w, 4, 0x40c, &[ctx.picture_index]);
        reloc(
            &mut w,
            0x580,
            da + u64::from(m.scaling_list_offset.unwrap_or(0x200)),
        );
        reloc(&mut w, 0x584, da + u64::from(m.tile_sizes_offset));
        reloc(&mut w, 0x588, self.filter);
        // Stock method list (gens.rs DECODE_GATES 0x443bc / 0x443c7): 0x4c8 on GA10x+, 0x4dc on
        // blackwell_b classes only (Ada trace: 0x588, 0x4c8, no 0x4dc).
        if self.gen.ga10x_plus() {
            reloc(&mut w, 0x4c8, self.intra_top);
        }
        if self.gen.blackwell_b() {
            reloc(&mut w, 0x4dc, self.buf_4dc);
        }
        reloc(&mut w, 0x470, slot_surface[16].luma);
        reloc(&mut w, 0x4b4, slot_surface[16].chroma);
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
        self.live
            .iter()
            .filter_map(|id| self.surface_of.get(id).copied())
            .collect()
    }
    fn bump_ready(&self, pending: usize) -> bool {
        pending > self.reorder as usize
    }
    fn flush(&mut self) {
        self.dec.flush();
        self.units.clear();
        self.pending = None;
        self.live.clear();
        self.surface_of.clear();
        self.epoch += 1;
        self.epoch_max_poc = None;
    }
}
