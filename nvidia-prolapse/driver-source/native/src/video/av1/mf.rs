//! Motion-field reference selection for NVDEC (which up to three references the engine projects, and their
//! order-hint offsets). Selection order follows AV1 spec 7.9.1 / libaom av1_setup_motion_field: LAST unless
//! it is an overlay of GOLDEN, then BWDREF, ALTREF2, ALTREF when after the current frame (stack budget), then
//! LAST2. Matches stock's selection on all captured inter frames.

use super::desc::MotionField;
use super::header::{get_relative_dist, FrameHeader, SequenceHeader};
use super::PicRecord;

const LAST: usize = 0;
const LAST2: usize = 1;
const GOLDEN: usize = 3;
const BWDREF: usize = 4;
const ALTREF2: usize = 5;
const ALTREF: usize = 6;
const MAX_OFFSET: i32 = 31;

/// Returns the motion-field parameters and the DPB slot of each candidate.
pub fn select(
    seq: &SequenceHeader,
    h: &FrameHeader,
    recs: &[Option<PicRecord>; 7],
    slots: &[Option<usize>; 7],
) -> (MotionField, [Option<usize>; 3]) {
    let mut mf = MotionField::default();
    let mut out = [None; 3];
    if h.frame_is_intra || !seq.enable_order_hint {
        return (mf, out);
    }
    // Stock libnvcuvid 615.78.08 compares each reference's stored width (the upscaled width, as
    // CUVID reports it: "coded width, if superres enabled then it is upscaled width") with the
    // current picture's coded width taken from pic_s, both rounded to 8 (ELF 0x454a0). A
    // superres frame therefore has no motion-field candidates: its coded grid differs from its
    // own references' upscaled geometry. Our session always sizes surfaces by the session max,
    // so the pitch never changes and only these dimensions can disagree.
    let usable = |k: usize| -> bool {
        match recs[k] {
            Some(r) => {
                !r.intra
                    && (r.upscaled_width + 7) >> 3 == (h.frame_width + 7) >> 3
                    && (r.frame_height + 7) >> 3 == (h.frame_height + 7) >> 3
            }
            None => false,
        }
    };
    let mut cands: Vec<usize> = Vec::new();
    let mut stamp: i32 = 2;
    if let Some(last) = recs[LAST] {
        if last.ref_hints[ALTREF] != h.order_hints[GOLDEN] && usable(LAST) {
            cands.push(LAST);
        }
        stamp -= 1;
    }
    for k in [BWDREF, ALTREF2, ALTREF] {
        if k == ALTREF && stamp < 0 {
            break;
        }
        if get_relative_dist(seq, h.order_hints[k], h.order_hint) > 0 && usable(k) {
            cands.push(k);
            stamp -= 1;
        }
    }
    if stamp >= 0 && usable(LAST2) {
        cands.push(LAST2);
    }
    for (j, &k) in cands.iter().take(3).enumerate() {
        mf.mf_type[j] = (k + 1) as u8;
        let r = recs[k].unwrap();
        let dist = get_relative_dist(seq, h.order_hint, r.order_hint);
        if h.use_ref_frame_mvs && dist.abs() <= MAX_OFFSET {
            mf.use_temporal[j] = true;
            out[j] = slots[k];
            for rr in 0..7 {
                mf.mfmv_offset[j][rr] =
                    get_relative_dist(seq, r.order_hint, r.ref_hints[rr]) as i16;
                mf.mfmv_side[j][rr] =
                    (get_relative_dist(seq, r.ref_hints[rr], r.order_hint) > 0) as i8;
            }
        }
    }
    (mf, out)
}
