//! H.264 front end: Annex-B access unit → NEW_H264 picture job.
//!
//! Pipeline per access unit: parameter sets → slice headers → POC (8.2.1, types 0/1/2) →
//! frame_num gap handling (8.2.5.2) → descriptor DPB with stable positions + colocated slots →
//! initial reference lists (8.2.4.2) → setup bytes ([`desc`]) → reference marking (8.2.5:
//! sliding window, MMCO 1-6) after the picture.
//!
//! Engine recipe (stock libnvcuvid 615.71.09, observed in RM traces): progressive 8-bit 4:2:0 ≤ 4096×4096 uses APPLICATION_ID 8 with
//! a 0x200-byte setup + scaling at +0x200 + status at +0x700, a ring of 4 setup buffers, the
//! bitstream = the picture's slice NAL units each behind a 3-byte start code (exactly what CUVID
//! hands stock), PICTURE_INDEX = the current colocated slot, 17 interleaved luma/chroma surface
//! offsets indexed by slot (unused = current target).
//!
//! Stock behaviours reproduced from captures (not from the spec): descriptor DPB positions are
//! stable (a new reference takes the first free position), the colocated slot of the current
//! picture is sticky per target surface, empty DPB positions carry the previous
//! reference picture's slot, initial lists list every reference (no truncation to num_ref_idx_active).
//!
//! Interlaced SPSs (frame_mbs_only_flag = 0) use the legacy APPLICATION_ID 3 setup
//! ([`legacy`]) like stock; frame pictures (MBAFF or not) are supported.
//!
//! Not implemented (each returns `VideoError::Unsupported` naming it): field pictures (PAFF),
//! 4:0:0 / 4:2:2 / 4:4:4, bit depth > 8, FMO slice groups,
//! data partitioning, redundant pictures, MVC/SVC, pictures larger than 4096×4096.
pub mod codec;
pub mod desc;
pub mod legacy;
pub mod ps;
pub mod slice;
#[cfg(test)]
mod tests;

use super::{bits, bitstream, unsupported, VResult};
use desc::{DescInput, DescRef, EngineParams};
use ps::{Pps, Sps};
use slice::{Mmco, SliceHeader, SliceType};
use std::collections::BTreeMap;

/// Colocated slots (16 references + the current picture).
pub const MAX_SLOTS: usize = 17;
/// Setup buffers rotate through a ring of this many entries (stock).
pub const SETUP_RING: usize = 4;
/// Stock was fed by FFmpeg, which starts POC type 0 at PicOrderCntMsb = 65536 after an IDR
/// (h264dec.c idr(): `prev_poc_msb = 1 << 16`). A constant offset changes no POC difference;
/// we apply the same one so our setups equal stock's byte for byte.
pub const POC_TYPE0_BIAS: i32 = 1 << 16;

/// One picture ready for submission.
#[derive(Debug, Clone)]
pub struct PictureJob {
    pub app_id: u32,
    /// setup (0x200) + scaling matrices (0xe0); the engine status goes to setup + 0x700.
    pub setup: Vec<u8>,
    /// Slice NAL units of this picture, each behind `00 00 01` (IN_BUF_BASE).
    pub bitstream: Vec<u8>,
    /// Colocated slot of the current picture (setup+0x1cc bits 0-4).
    pub slot: u8,
    /// `SET_PICTURE_INDEX`: stock sends a running picture counter (observed 0, 1, 2, … in decode
    /// order).
    pub picture_index: u32,
    /// Caller's surface id for the current picture.
    pub surface: u32,
    /// Surface id per colocated slot for the 17 LUMA/CHROMA offset pairs; `None` = use the
    /// current target (stock fills unused slots with it).
    pub slot_surfaces: [Option<u32>; MAX_SLOTS],
    pub control_params: u32,
    /// Display-order key (min of the field POCs, unbiased spec value).
    pub poc: i32,
    pub idr: bool,
    pub is_reference: bool,
    /// Bumping hint: pictures the display process may hold back (VUI, else the DPB size).
    pub max_num_reorder: u32,
    pub width: u32,
    pub height: u32,
    pub crop: (u32, u32, u32, u32),
}

#[derive(Debug, Clone, Copy)]
struct RefPic {
    surface: Option<u32>,
    slot: Option<u8>,
    frame_num: u32,
    long_term_idx: Option<u32>,
    poc: [i32; 2],
    non_existing: bool,
}
impl RefPic {
    fn frame_num_wrap(&self, cur_frame_num: u32, max: u32) -> i32 {
        if self.frame_num > cur_frame_num {
            self.frame_num as i32 - max as i32
        } else {
            self.frame_num as i32
        }
    }
    fn frame_poc(&self) -> i32 {
        self.poc[0].min(self.poc[1])
    }
}

#[derive(Default)]
struct PocState {
    prev_msb: i32,
    prev_lsb: i32,
    prev_frame_num_offset: i32,
    prev_frame_num: u32,
    prev_ref_frame_num: u32,
    prev_mmco5: bool,
}

pub struct Decoder {
    ep: EngineParams,
    sps: BTreeMap<u32, Sps>,
    pps: BTreeMap<u32, Pps>,
    dpb: [Option<RefPic>; 16],
    max_lt_idx: Option<u32>,
    poc: PocState,
    prev_slot: u8,
    err_slot: u8,
    seen_idr: bool,
    slot_surface: [Option<u32>; MAX_SLOTS],
    picture_count: u32,
    epoch: u32,
}

/// Picture-level metadata known after [`Decoder::begin`].
#[derive(Debug, Clone, Copy)]
pub struct PictureInfo {
    pub width: u32,
    pub height: u32,
    /// Crop in luma samples (left, right, top, bottom).
    pub crop: (u32, u32, u32, u32),
    /// colour_primaries, transfer_characteristics, matrix_coefficients, full range.
    pub colour: (u32, u32, u32, bool),
    pub keyframe: bool,
}

/// A parsed picture waiting for its target surface ([`Decoder::complete`]).
pub struct Begun {
    pic: Picture,
    poc: [i32; 2],
    msb: i32,
    /// Output order key: (IDR/MMCO5 epoch << 32) + PicOrderCnt.
    pub display_key: i64,
    pub info: PictureInfo,
}
impl Begun {
    pub fn bitstream(&self) -> &[u8] {
        &self.pic.bitstream
    }
}

struct Picture {
    hdr: SliceHeader,
    sps: Sps,
    pps: Pps,
    intra: bool,
    bitstream: Vec<u8>,
    slice_offsets: Vec<u32>,
}

impl Decoder {
    pub fn new(ep: EngineParams) -> Self {
        Self {
            ep,
            sps: BTreeMap::new(),
            pps: BTreeMap::new(),
            dpb: [None; 16],
            max_lt_idx: None,
            poc: PocState::default(),
            prev_slot: 0,
            err_slot: 0,
            seen_idr: false,
            slot_surface: [None; MAX_SLOTS],
            picture_count: 0,
            epoch: 0,
        }
    }

    /// Surfaces the reference state still needs; the caller must not reuse them as targets.
    pub fn referenced_surfaces(&self) -> Vec<u32> {
        self.dpb
            .iter()
            .flatten()
            .filter_map(|r| r.surface)
            .collect()
    }

    /// Drops all reference state (seek). The next picture must be an IDR.
    pub fn flush(&mut self) {
        self.dpb = [None; 16];
        self.max_lt_idx = None;
        self.seen_idr = false;
    }

    /// Feeds parameter sets out of band (Annex-B).
    pub fn configure(&mut self, annexb: &[u8]) -> VResult<()> {
        for n in bits::split_annexb(annexb) {
            self.parameter_set(n.nal)?;
        }
        Ok(())
    }

    fn parameter_set(&mut self, nal: &[u8]) -> VResult<bool> {
        match nal[0] & 0x1f {
            7 => {
                let s = ps::parse_sps(&bits::unescape(nal))?;
                self.sps.insert(s.id, s);
                Ok(true)
            }
            8 => {
                let p = ps::parse_pps(&bits::unescape(nal), |id| self.sps.get(&id).cloned())?;
                self.pps.insert(p.id, p);
                Ok(true)
            }
            15 => unsupported("H.264 subset SPS (SVC/MVC)"),
            _ => Ok(false),
        }
    }

    fn collect(&mut self, au: &[u8]) -> VResult<Option<Picture>> {
        let mut pic: Option<Picture> = None;
        for n in bits::split_annexb(au) {
            let t = n.nal[0] & 0x1f;
            if self.parameter_set(n.nal)? {
                continue;
            }
            match t {
                1 | 5 => {}
                2..=4 => return unsupported("H.264 data partitioning (NAL types 2-4)"),
                14 | 20 => return unsupported("H.264 MVC/SVC NAL units"),
                _ => continue,
            }
            let hdr = slice::parse_slice_header(&bits::unescape(n.nal), |id| {
                let p = self.pps.get(&id)?;
                Some((self.sps.get(&p.sps_id)?.clone(), p.clone()))
            })?;
            if hdr.redundant_pic_cnt > 0 {
                return unsupported("H.264 redundant pictures (redundant_pic_cnt > 0)");
            }
            let intra = matches!(hdr.slice_type, SliceType::I | SliceType::Si);
            match &mut pic {
                None => {
                    let pps = self.pps[&hdr.pps_id].clone();
                    let sps = self.sps[&pps.sps_id].clone();
                    let mut bs = Vec::with_capacity(au.len());
                    bs.extend_from_slice(&[0, 0, 1]);
                    bs.extend_from_slice(n.nal);
                    pic = Some(Picture {
                        hdr,
                        sps,
                        pps,
                        intra,
                        bitstream: bs,
                        slice_offsets: vec![0],
                    });
                }
                Some(p) => {
                    if hdr.frame_num != p.hdr.frame_num
                        || hdr.pps_id != p.hdr.pps_id
                        || hdr.field_pic != p.hdr.field_pic
                        || hdr.bottom_field != p.hdr.bottom_field
                        || hdr.nal_unit_type != p.hdr.nal_unit_type
                        || hdr.pic_order_cnt_lsb != p.hdr.pic_order_cnt_lsb
                    {
                        return unsupported("more than one H.264 picture in one access unit");
                    }
                    p.intra &= intra;
                    p.slice_offsets.push(p.bitstream.len() as u32);
                    p.bitstream.extend_from_slice(&[0, 0, 1]);
                    p.bitstream.extend_from_slice(n.nal);
                }
            }
        }
        Ok(pic)
    }

    /// 8.2.1: TopFieldOrderCnt / BottomFieldOrderCnt of a frame (unbiased) and PicOrderCntMsb.
    fn compute_poc(&self, h: &SliceHeader, sps: &Sps) -> ([i32; 2], i32) {
        let max_frame_num = sps.max_frame_num() as i32;
        let st = &self.poc;
        match sps.pic_order_cnt_type {
            0 => {
                let (prev_msb, prev_lsb) = if h.idr() {
                    (0, 0)
                } else {
                    (st.prev_msb, st.prev_lsb)
                };
                let max_lsb = 1i32 << (sps.log2_max_poc_lsb_minus4 + 4);
                let lsb = h.pic_order_cnt_lsb as i32;
                let msb = if lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2 {
                    prev_msb + max_lsb
                } else if lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2 {
                    prev_msb - max_lsb
                } else {
                    prev_msb
                };
                let top = msb + lsb;
                ([top, top + h.delta_pic_order_cnt_bottom], msb)
            }
            1 => {
                let fno = self.frame_num_offset(h, max_frame_num);
                let cycle = sps.offset_for_ref_frame.len() as i32;
                let mut abs = if cycle != 0 {
                    fno + h.frame_num as i32
                } else {
                    0
                };
                if h.nal_ref_idc == 0 && abs > 0 {
                    abs -= 1;
                }
                let mut expected = 0;
                if abs > 0 {
                    let delta: i32 = sps.offset_for_ref_frame.iter().sum();
                    let cnt = (abs - 1) / cycle;
                    let in_cycle = (abs - 1) % cycle;
                    expected = cnt * delta
                        + sps.offset_for_ref_frame[..=in_cycle as usize]
                            .iter()
                            .sum::<i32>();
                }
                if h.nal_ref_idc == 0 {
                    expected += sps.offset_for_non_ref_pic;
                }
                let top = expected + h.delta_pic_order_cnt[0];
                (
                    [
                        top,
                        top + sps.offset_for_top_to_bottom_field + h.delta_pic_order_cnt[1],
                    ],
                    0,
                )
            }
            _ => {
                let fno = self.frame_num_offset(h, max_frame_num);
                let t = if h.idr() {
                    0
                } else if h.nal_ref_idc == 0 {
                    2 * (fno + h.frame_num as i32) - 1
                } else {
                    2 * (fno + h.frame_num as i32)
                };
                ([t, t], 0)
            }
        }
    }

    fn frame_num_offset(&self, h: &SliceHeader, max_frame_num: i32) -> i32 {
        let st = &self.poc;
        let prev = if st.prev_mmco5 {
            0
        } else {
            st.prev_frame_num_offset
        };
        if h.idr() {
            0
        } else if st.prev_frame_num > h.frame_num {
            prev + max_frame_num
        } else {
            prev
        }
    }

    fn insert_ref(&mut self, r: RefPic) -> VResult<()> {
        match self.dpb.iter_mut().find(|e| e.is_none()) {
            Some(e) => {
                *e = Some(r);
                Ok(())
            }
            None => bitstream("H.264 DPB overflow (more than 16 reference frames)"),
        }
    }

    /// 8.2.5.3 sliding window (frames).
    fn sliding_window(&mut self, sps: &Sps, cur_frame_num: u32) -> VResult<()> {
        let n = self.dpb.iter().flatten().count() as u32;
        if n >= sps.max_num_ref_frames.max(1) {
            let max = sps.max_frame_num();
            let victim = self
                .dpb
                .iter()
                .enumerate()
                .filter_map(|(i, e)| {
                    e.filter(|r| r.long_term_idx.is_none())
                        .map(|r| (i, r.frame_num_wrap(cur_frame_num, max)))
                })
                .min_by_key(|&(_, w)| w);
            match victim {
                Some((i, _)) => self.dpb[i] = None,
                None => return bitstream("sliding window with no short-term reference to remove"),
            }
        }
        Ok(())
    }

    /// 8.2.5.2: frames inferred for gaps in frame_num.
    fn fill_frame_num_gap(&mut self, h: &SliceHeader, sps: &Sps) -> VResult<()> {
        let max = sps.max_frame_num();
        let prev = self.poc.prev_ref_frame_num;
        if h.frame_num == prev || h.frame_num == (prev + 1) % max {
            return Ok(());
        }
        if !sps.gaps_in_frame_num_allowed {
            return bitstream(format!(
                "frame_num gap {prev} -> {} without gaps_in_frame_num_value_allowed_flag (lost pictures)",
                h.frame_num
            ));
        }
        let mut f = (prev + 1) % max;
        while f != h.frame_num {
            self.sliding_window(sps, f)?;
            self.insert_ref(RefPic {
                surface: None,
                slot: None,
                frame_num: f,
                long_term_idx: None,
                poc: [0; 2],
                non_existing: true,
            })?;
            self.poc.prev_ref_frame_num = f;
            // 8.2.1: the inferred frames update prevFrameNum / prevFrameNumOffset as well.
            let fno = if self.poc.prev_frame_num > f {
                self.poc.prev_frame_num_offset + max as i32
            } else {
                self.poc.prev_frame_num_offset
            };
            self.poc.prev_frame_num_offset = fno;
            self.poc.prev_frame_num = f;
            self.poc.prev_mmco5 = false;
            f = (f + 1) % max;
        }
        Ok(())
    }

    /// Initial reference lists (8.2.4.2.1 P frames, 8.2.4.2.3 B frames) as DPB positions.
    fn initial_lists(&self, cur_frame_num: u32, cur_poc: i32, max_frame_num: u32) -> [Vec<u8>; 3] {
        let refs: Vec<(u8, RefPic)> = self
            .dpb
            .iter()
            .enumerate()
            .filter_map(|(i, e)| e.map(|r| (i as u8, r)))
            .collect();
        let mut st: Vec<(u8, RefPic)> = refs
            .iter()
            .copied()
            .filter(|(_, r)| r.long_term_idx.is_none())
            .collect();
        let mut lt: Vec<(u8, RefPic)> = refs
            .iter()
            .copied()
            .filter(|(_, r)| r.long_term_idx.is_some())
            .collect();
        lt.sort_by_key(|(_, r)| r.long_term_idx);
        st.sort_by_key(|(_, r)| -r.frame_num_wrap(cur_frame_num, max_frame_num));
        let p: Vec<u8> = st.iter().chain(lt.iter()).map(|(i, _)| *i).collect();
        // Non-existing frames have no POC; they are left out of the B lists.
        let mut before: Vec<(u8, RefPic)> = st
            .iter()
            .copied()
            .filter(|(_, r)| !r.non_existing && r.frame_poc() < cur_poc)
            .collect();
        let mut after: Vec<(u8, RefPic)> = st
            .iter()
            .copied()
            .filter(|(_, r)| !r.non_existing && r.frame_poc() > cur_poc)
            .collect();
        before.sort_by_key(|(_, r)| -r.frame_poc());
        after.sort_by_key(|(_, r)| r.frame_poc());
        let ltp: Vec<u8> = lt.iter().map(|(i, _)| *i).collect();
        let b0: Vec<u8> = before
            .iter()
            .chain(after.iter())
            .map(|(i, _)| *i)
            .chain(ltp.iter().copied())
            .collect();
        let mut b1: Vec<u8> = after
            .iter()
            .chain(before.iter())
            .map(|(i, _)| *i)
            .chain(ltp.iter().copied())
            .collect();
        if b1.len() > 1 && b1 == b0 {
            b1.swap(0, 1);
        }
        [p, b0, b1]
    }

    /// Builds the picture job for one access unit. `surface` is the caller's target surface id
    /// for this picture; it must not be in [`Self::referenced_surfaces`]. Returns `None` for an
    /// access unit without slices (parameter sets / SEI only).
    pub fn decode_au(&mut self, au: &[u8], surface: u32) -> VResult<Option<PictureJob>> {
        match self.begin(au)? {
            Some(b) => self.complete(b, surface).map(Some),
            None => Ok(None),
        }
    }

    /// First phase: parse the access unit, check support, infer frame_num-gap frames, compute
    /// the POC. Nothing here depends on the target surface.
    pub fn begin(&mut self, au: &[u8]) -> VResult<Option<Begun>> {
        let Some(pic) = self.collect(au)? else {
            return Ok(None);
        };
        let h = &pic.hdr;
        let sps = &pic.sps;
        if h.field_pic {
            return unsupported(
                "H.264 field pictures (PAFF): field-pair DPB marking and the legacy field setup are not implemented",
            );
        }
        if sps.chroma_format_idc != 1 {
            return unsupported(format!(
                "H.264 chroma_format_idc {} (only 4:2:0: the session's surfaces and copy-out are \
                 NV12/P010 4:2:0, so 4:2:2/4:4:4 needs a new surface layout and copy path)",
                sps.chroma_format_idc
            ));
        }
        if sps.bit_depth_luma_minus8 != 0 || sps.bit_depth_chroma_minus8 != 0 {
            return unsupported(
                "H.264 bit depth above 8 (the H.264 codec creates 8-bit surfaces -- the session \
                 passes no bit depth to it -- and there is no 10-bit setup path)",
            );
        }
        if sps.width() > 4096 || sps.height() > 4096 {
            return unsupported(format!(
                "H.264 picture {}x{} above 4096x4096",
                sps.width(),
                sps.height()
            ));
        }
        if !h.idr() && !self.seen_idr {
            return bitstream("H.264 stream does not start with an IDR picture");
        }
        let (h, sps) = (h.clone(), sps.clone());
        if h.idr() {
            self.dpb = [None; 16];
            self.max_lt_idx = None;
            self.seen_idr = true;
            self.epoch += 1;
        } else {
            self.fill_frame_num_gap(&h, &sps)?;
        }
        let (poc, msb) = self.compute_poc(&h, &sps);
        let mut key = poc[0].min(poc[1]);
        if h.mmco
            .as_ref()
            .is_some_and(|ops| ops.contains(&Mmco::Reset))
        {
            // 8.2.1: the MMCO5 picture's POC is re-based to 0 and starts a new output epoch.
            self.epoch += 1;
            key = 0;
        }
        let vui = &sps.vui;
        let info = PictureInfo {
            width: sps.width(),
            height: sps.height(),
            crop: sps.crop,
            colour: (
                vui.colour_primaries,
                vui.transfer_characteristics,
                vui.matrix_coefficients,
                vui.full_range,
            ),
            keyframe: h.idr(),
        };
        Ok(Some(Begun {
            pic,
            poc,
            msb,
            display_key: ((self.epoch as i64) << 32) + key as i64,
            info,
        }))
    }

    /// Second phase: place the picture into `surface`, build the setup and mark references.
    pub fn complete(&mut self, b: Begun, surface: u32) -> VResult<PictureJob> {
        let Begun { pic, poc, msb, .. } = b;
        let Picture {
            hdr: h,
            sps,
            pps,
            intra,
            bitstream: bs,
            slice_offsets,
        } = pic;
        if self.referenced_surfaces().contains(&surface) {
            return bitstream(format!("target surface {surface} is still a reference"));
        }
        let bias = if sps.pic_order_cnt_type == 0 {
            POC_TYPE0_BIAS
        } else {
            0
        };
        let cur_poc = poc[0].min(poc[1]);
        let slot = self.colocated_slot(surface);
        let lists = self.initial_lists(h.frame_num, cur_poc, sps.max_frame_num());
        let dpb: [Option<DescRef>; 16] = std::array::from_fn(|i| {
            self.dpb[i].map(|r| DescRef {
                slot: r.slot,
                state: 3,
                long_term: r.long_term_idx.is_some(),
                non_existing: r.non_existing,
                poc: if r.non_existing {
                    [0; 2]
                } else {
                    [r.poc[0] + bias, r.poc[1] + bias]
                },
                frame_idx: r.long_term_idx.unwrap_or(r.frame_num),
            })
        });
        let inp = DescInput {
            sps: &sps,
            pps: &pps,
            dpb: &dpb,
            empty_slot: self.prev_slot,
            lists: [&lists[0], &lists[1], &lists[2]],
            cur_slot: slot,
            surface_id: surface,
            bitstream_len: bs.len() as u32,
            frame_num: h.frame_num,
            field_pic: false,
            bottom_field: false,
            second_field: false,
            ref_pic: h.nal_ref_idc != 0,
            intra_pic: intra,
            poc: [poc[0] + bias, poc[1] + bias],
        };
        // Path selection as stock (ELF 0x450c0): on Blackwell+
        // (`ep.new_h264`) NEW_H264 for progressive SPSs, the legacy setup for interlaced ones (MBAFF or
        // not); on older classes the legacy setup for every picture.
        let new_h264 = self.ep.new_h264 && sps.frame_mbs_only;
        let (app_id, setup) = if new_h264 {
            (desc::APP_ID_NEW_H264, desc::build(&inp, &self.ep))
        } else {
            let lay = legacy::LegacyLayout::stock_fit(
                self.ep.pitch(&sps),
                sps.pic_width_in_mbs,
                sps.frame_height_in_mbs(),
            );
            let mbaff = sps.mb_adaptive_frame_field && !h.field_pic;
            (
                legacy::APP_ID_H264,
                legacy::build(&inp, &self.ep, &lay, mbaff, &slice_offsets),
            )
        };
        // Error-concealment reference (CONTROL_PARAMS bits 7-11). NEW_H264: the first existing
        // DPB position whose slot is not the current one, kept from the previous picture when
        // there is none (stock ELF 0x3ceea). Legacy: the last
        // reference picture's slot (observed in the MBAFF capture and, for progressive pictures on
        // the legacy path, in the Ada High-profile capture).
        if new_h264 {
            if let Some(s) = self
                .dpb
                .iter()
                .flatten()
                .filter(|r| !r.non_existing)
                .filter_map(|r| r.slot)
                .find(|&s| s != slot)
            {
                self.err_slot = s;
            }
        } else {
            self.err_slot = self.prev_slot;
        }
        let mut slot_surfaces = [None; MAX_SLOTS];
        for r in self.dpb.iter().flatten() {
            if let (Some(s), Some(surf)) = (r.slot, r.surface) {
                slot_surfaces[s as usize] = Some(surf);
            }
        }
        slot_surfaces[slot as usize] = Some(surface);
        let job = PictureJob {
            app_id,
            setup,
            bitstream: bs,
            slot,
            picture_index: self.picture_count,
            surface,
            slot_surfaces,
            control_params: desc::control_params(self.err_slot),
            poc: cur_poc,
            idr: h.idr(),
            is_reference: h.nal_ref_idc != 0,
            max_num_reorder: sps.vui.max_num_reorder_frames.unwrap_or(sps.dpb_frames()),
            width: sps.width(),
            height: sps.height(),
            crop: sps.crop,
        };
        self.mark(&h, &sps, poc, msb, surface, slot)?;
        self.picture_count += 1;
        if h.nal_ref_idc != 0 {
            // Stock: empty DPB positions carry the slot of the last reference picture.
            self.prev_slot = slot;
        }
        Ok(job)
    }

    /// 8.2.5 decoded reference picture marking + POC/frame_num state for the next picture.
    fn mark(
        &mut self,
        h: &SliceHeader,
        sps: &Sps,
        mut poc: [i32; 2],
        msb: i32,
        surface: u32,
        slot: u8,
    ) -> VResult<()> {
        let mut mmco5 = false;
        let mut cur_lt: Option<u32> = None;
        let max = sps.max_frame_num();
        if h.nal_ref_idc != 0 {
            if h.idr() {
                if h.long_term_reference_flag {
                    cur_lt = Some(0);
                    self.max_lt_idx = Some(0);
                } else {
                    self.max_lt_idx = None;
                }
            } else if let Some(ops) = &h.mmco {
                let cur_pic_num = h.frame_num as i32;
                for op in ops {
                    match *op {
                        Mmco::ShortUnused(d) => {
                            let x = cur_pic_num - (d as i32 + 1);
                            match self.find_short(x, h.frame_num, max) {
                                Some(i) => self.dpb[i] = None,
                                None => return bitstream(format!("MMCO1 on missing PicNum {x}")),
                            }
                        }
                        Mmco::LongUnused(n) => match self
                            .dpb
                            .iter()
                            .position(|e| e.is_some_and(|r| r.long_term_idx == Some(n)))
                        {
                            Some(i) => self.dpb[i] = None,
                            None => {
                                return bitstream(format!("MMCO2 on missing LongTermPicNum {n}"))
                            }
                        },
                        Mmco::ShortToLong(d, idx) => {
                            let x = cur_pic_num - (d as i32 + 1);
                            if self.max_lt_idx.is_none_or(|m| idx > m) {
                                return bitstream(
                                    "MMCO3 LongTermFrameIdx above MaxLongTermFrameIdx",
                                );
                            }
                            let target = self.find_short(x, h.frame_num, max);
                            if let Some(j) = self
                                .dpb
                                .iter()
                                .position(|e| e.is_some_and(|r| r.long_term_idx == Some(idx)))
                            {
                                if Some(j) != target {
                                    self.dpb[j] = None;
                                }
                            }
                            match target {
                                Some(i) => self.dpb[i].as_mut().unwrap().long_term_idx = Some(idx),
                                None => return bitstream(format!("MMCO3 on missing PicNum {x}")),
                            }
                        }
                        Mmco::MaxLongTermIdx(p1) => {
                            self.max_lt_idx = p1.checked_sub(1);
                            let m = self.max_lt_idx;
                            for e in self.dpb.iter_mut() {
                                if e.is_some_and(|r| {
                                    r.long_term_idx.is_some_and(|i| m.is_none_or(|m| i > m))
                                }) {
                                    *e = None;
                                }
                            }
                        }
                        Mmco::Reset => {
                            self.dpb = [None; 16];
                            self.max_lt_idx = None;
                            mmco5 = true;
                        }
                        Mmco::CurrentToLong(idx) => {
                            if self.max_lt_idx.is_none_or(|m| idx > m) {
                                return bitstream(
                                    "MMCO6 LongTermFrameIdx above MaxLongTermFrameIdx",
                                );
                            }
                            for e in self.dpb.iter_mut() {
                                if e.is_some_and(|r| r.long_term_idx == Some(idx)) {
                                    *e = None;
                                }
                            }
                            cur_lt = Some(idx);
                        }
                    }
                }
            } else {
                self.sliding_window(sps, h.frame_num)?;
            }
            if mmco5 {
                let t = poc[0].min(poc[1]);
                poc = [poc[0] - t, poc[1] - t];
            }
            let n = self.dpb.iter().flatten().count() as u32;
            if n >= sps.max_num_ref_frames.max(1) {
                return bitstream(format!(
                    "reference marking leaves {} frames with max_num_ref_frames {}",
                    n + 1,
                    sps.max_num_ref_frames
                ));
            }
            self.insert_ref(RefPic {
                surface: Some(surface),
                slot: Some(slot),
                frame_num: if mmco5 { 0 } else { h.frame_num },
                long_term_idx: cur_lt,
                poc,
                non_existing: false,
            })?;
        }
        // POC / frame_num state for the next picture (8.2.1).
        let fno = if sps.pic_order_cnt_type == 0 {
            0
        } else {
            self.frame_num_offset(h, max as i32)
        };
        let st = &mut self.poc;
        if h.nal_ref_idc != 0 {
            if mmco5 {
                st.prev_msb = 0;
                st.prev_lsb = poc[0];
            } else {
                st.prev_msb = msb;
                st.prev_lsb = h.pic_order_cnt_lsb as i32;
            }
            st.prev_ref_frame_num = if mmco5 { 0 } else { h.frame_num };
        }
        st.prev_mmco5 = mmco5;
        st.prev_frame_num_offset = if mmco5 { 0 } else { fno };
        st.prev_frame_num = if mmco5 { 0 } else { h.frame_num };
        Ok(())
    }

    /// Colocated slot for the target surface. Slots are sticky per surface (stock keeps a record
    /// {surface → slot} per slot, ELF 0x35a70): a surface reuses its slot;
    /// a new surface takes the lowest unassigned slot, else the lowest slot whose surface is no
    /// longer a reference (this rule never moves a live reference, which is the engine
    /// contract).
    fn colocated_slot(&mut self, surface: u32) -> u8 {
        if let Some(s) = self.slot_surface.iter().position(|&e| e == Some(surface)) {
            return s as u8;
        }
        let refs: Vec<u8> = self.dpb.iter().flatten().filter_map(|r| r.slot).collect();
        let s = match self.slot_surface.iter().position(|e| e.is_none()) {
            Some(s) => s,
            None => (0..MAX_SLOTS)
                .find(|s| !refs.contains(&(*s as u8)))
                .expect("17 slots for at most 16 references"),
        };
        self.slot_surface[s] = Some(surface);
        s as u8
    }

    fn find_short(&self, pic_num: i32, cur_frame_num: u32, max: u32) -> Option<usize> {
        self.dpb.iter().position(|e| {
            e.is_some_and(|r| {
                r.long_term_idx.is_none() && r.frame_num_wrap(cur_frame_num, max) == pic_num
            })
        })
    }
}
