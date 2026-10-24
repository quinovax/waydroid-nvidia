//! HDR static metadata carried by the bitstream: SMPTE ST 2086 mastering display
//! colour volume and CEA-861.3 content light level (MaxCLL / MaxFALL).
//!
//! This is bitstream syntax from the public specifications, not engine state:
//! - H.264/HEVC: SEI messages `mastering_display_colour_volume` (payload type 137) and
//!   `content_light_level_info` (144) — ITU-T H.264 (2016) D.3.24/D.3.25 and ITU-T H.265
//!   D.3.22/D.3.23, found in prefix SEI (HEVC NAL 39; H.264 NAL 6) and suffix SEI (HEVC 40).
//! - AV1: metadata OBUs (`OBU_METADATA`, type 5), `metadata_type` 1 = HDR_CLL and 2 =
//!   HDR_MDCV — AV1 specification 5.8.2/6.7.3 (libaom `OBU_METADATA_TYPE_HDR_CLL` /
//!   `OBU_METADATA_TYPE_HDR_MDCV`).
//!
//! The session attaches what a unit carries to every displayable picture the unit
//! produces (`PictureInfo` does not carry it: codecs do not parse SEI). A temporal unit
//! with several AV1 frames therefore reports the unit's metadata for each of them; a
//! stream that changes the metadata inside one temporal unit is not represented (the C2
//! HAL attaches metadata per output buffer, and a unit maps to one time step).
use super::{bitstream, VResult, VideoError, CODEC_AV1, CODEC_H264, CODEC_HEVC};

/// `mastering_display_colour_volume()`: chromaticities in units of 0.00002, luminance in
/// units of 0.0001 cd/m² (ST 2086, as coded in the SEI / AV1 metadata).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MasteringDisplay {
    pub primaries_x: [u16; 3],
    pub primaries_y: [u16; 3],
    pub white_x: u16,
    pub white_y: u16,
    pub max_luminance: u32,
    pub min_luminance: u32,
}

/// What one unit of the bitstream declares. When several declarations are present the
/// last one wins per field group (streams repeat the same values per AU).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HdrStaticMetadata {
    pub mastering: Option<MasteringDisplay>,
    /// (max_content_light_level, max_pic_average_light_level) in cd/m².
    pub content_light: Option<(u16, u16)>,
}

impl HdrStaticMetadata {
    pub fn is_empty(&self) -> bool {
        self.mastering.is_none() && self.content_light.is_none()
    }
    /// Carry the last declaration forward: a stream declares the mastering display and the
    /// content light level once (an encoder may repeat them), and every later picture is
    /// displayed with them until a new declaration replaces that field group.
    pub fn merge(&mut self, other: Self) {
        if other.mastering.is_some() {
            self.mastering = other.mastering;
        }
        if other.content_light.is_some() {
            self.content_light = other.content_light;
        }
    }
}

/// Parse the HDR declarations of one `nv_video_decode()` unit (Annex-B access unit for
/// H.264/HEVC, low-overhead OBUs for AV1). A recognized declaration with the wrong size is
/// a malformed bitstream; unknown payload types and other OBUs are skipped.
pub fn parse_unit(codec: u32, unit: &[u8]) -> VResult<HdrStaticMetadata> {
    let mut m = HdrStaticMetadata::default();
    match codec {
        CODEC_H264 | CODEC_HEVC => {
            for nal in annexb_nals(unit) {
                let (is_sei, escaped) = if codec == CODEC_HEVC {
                    let t = (nal[0] >> 1) & 0x3f;
                    (nal.len() > 2 && (t == 39 || t == 40), &nal[2..])
                } else {
                    (nal.len() > 1 && nal[0] & 0x1f == 6, &nal[1..])
                };
                if !is_sei {
                    continue;
                }
                let rbsp = unescape(escaped);
                for (kind, payload) in sei_payloads(&rbsp)? {
                    match kind {
                        137 => m.merge(HdrStaticMetadata {
                            mastering: Some(mastering_display(payload)?),
                            content_light: None,
                        }),
                        144 => m.merge(HdrStaticMetadata {
                            mastering: None,
                            content_light: Some(content_light(payload)?),
                        }),
                        _ => {}
                    }
                }
            }
        }
        CODEC_AV1 => {
            for (kind, payload) in obus(unit) {
                if kind != 5 {
                    continue;
                }
                let Some((metadata_type, used)) = leb128(payload, 0) else {
                    return bitstream("AV1 metadata OBU: truncated metadata_type");
                };
                let Some(body) = payload.get(used..) else {
                    return bitstream("AV1 metadata OBU: metadata_type past the payload");
                };
                let metadata_type = u32::try_from(metadata_type).map_err(|_| {
                    VideoError::Bitstream("AV1 metadata OBU: metadata_type overflow".into())
                })?;
                match metadata_type {
                    1 => m.merge(HdrStaticMetadata {
                        mastering: None,
                        content_light: Some(content_light(body)?),
                    }),
                    2 => m.merge(HdrStaticMetadata {
                        mastering: Some(av1_mastering_display(body)?),
                        content_light: None,
                    }),
                    _ => {}
                }
            }
        }
        _ => {}
    }
    Ok(m)
}

fn u16_at(b: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([b[o], b[o + 1]])
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// `mastering_display_colour_volume(payloadSize)`, 24 bytes: per component the x/y
/// chromaticity pair, then the white point, then max/min luminance.
fn mastering_display(p: &[u8]) -> VResult<MasteringDisplay> {
    // The fields are a fixed prefix; padding bits/bytes after them are allowed and ignored.
    if p.len() < 24 {
        return bitstream(format!(
            "SEI mastering display colour volume: {} payload bytes, at least 24 expected",
            p.len()
        ));
    }
    Ok(MasteringDisplay {
        primaries_x: [u16_at(p, 0), u16_at(p, 4), u16_at(p, 8)],
        primaries_y: [u16_at(p, 2), u16_at(p, 6), u16_at(p, 10)],
        white_x: u16_at(p, 12),
        white_y: u16_at(p, 14),
        max_luminance: u32_at(p, 16),
        min_luminance: u32_at(p, 20),
    })
}

fn content_light(p: &[u8]) -> VResult<(u16, u16)> {
    if p.len() < 4 {
        return bitstream(format!(
            "content light level info: {} payload bytes, at least 4 expected",
            p.len()
        ));
    }
    Ok((u16_at(p, 0), u16_at(p, 2)))
}

/// AV1 `metadata_type == OBU_METADATA_TYPE_HDR_MDCV` (2): 24 bytes, the same field order as
/// the SEI payload (per primary the x/y pair, then the white point, then the luminances;
/// AV1 5.8.2, FFmpeg cbs_av1_syntax_template.c `metadata_hdr_mdcv`).
fn av1_mastering_display(p: &[u8]) -> VResult<MasteringDisplay> {
    if p.len() < 24 {
        return bitstream(format!(
            "AV1 HDR_MDCV metadata: {} payload bytes, at least 24 expected",
            p.len()
        ));
    }
    Ok(MasteringDisplay {
        primaries_x: [u16_at(p, 0), u16_at(p, 4), u16_at(p, 8)],
        primaries_y: [u16_at(p, 2), u16_at(p, 6), u16_at(p, 10)],
        white_x: u16_at(p, 12),
        white_y: u16_at(p, 14),
        max_luminance: u32_at(p, 16),
        min_luminance: u32_at(p, 20),
    })
}

/// NAL units of an Annex-B byte stream (3- or 4-byte start codes), without the start code
/// and without trailing zero bytes (the byte stream's trailing_zero_8bits).
fn annexb_nals(unit: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut i = 0;
    let mut start = None;
    while i + 2 < unit.len() {
        if unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1 {
            if let Some(s) = start {
                out.push(trim(&unit[s..i]));
            }
            i += 3;
            start = Some(i);
        } else if unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 0 && i + 3 < unit.len() {
            // A 4-byte start code ends here when the fourth byte is 1.
            if unit[i + 3] == 1 {
                if let Some(s) = start {
                    out.push(trim(&unit[s..i]));
                }
                i += 4;
                start = Some(i);
            } else {
                i += 1;
            }
        } else {
            i += 1;
        }
    }
    if let Some(s) = start {
        out.push(trim(&unit[s..]));
    }
    out.into_iter().filter(|n| !n.is_empty()).collect()
}

fn trim(mut n: &[u8]) -> &[u8] {
    while n.last() == Some(&0) {
        n = &n[..n.len() - 1];
    }
    n
}

/// Emulation-prevention removal (H.264 7.4.1 / H.265 7.4.2): drop the 0x03 of an
/// emulation_prevention_three_byte.
fn unescape(b: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(b.len());
    let mut zeros = 0usize;
    for &x in b {
        if zeros >= 2 && x == 0x03 {
            zeros = 0;
            continue;
        }
        zeros = if x == 0 { zeros + 1 } else { 0 };
        out.push(x);
    }
    out
}

/// `sei_message()` list: (payloadType, payload).
fn sei_payloads(b: &[u8]) -> VResult<Vec<(u32, &[u8])>> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        // rbsp_trailing_bits (stop bit + zeros) ends the SEI message list.
        if (b[i] == 0x80 || b[i] == 0) && b[i..].iter().all(|&x| x == 0x80 || x == 0) {
            break;
        }
        let mut kind = 0u32;
        while i < b.len() && b[i] == 0xff {
            kind += 255;
            i += 1;
        }
        if i >= b.len() {
            return bitstream("SEI: truncated payloadType");
        }
        kind += u32::from(b[i]);
        i += 1;
        let mut size = 0usize;
        while i < b.len() && b[i] == 0xff {
            size += 255;
            i += 1;
        }
        if i >= b.len() {
            return bitstream("SEI: truncated payloadSize");
        }
        size += usize::from(b[i]);
        i += 1;
        let end = i
            .checked_add(size)
            .filter(|e| *e <= b.len())
            .ok_or_else(|| {
                VideoError::Bitstream(format!(
                    "SEI payload type {kind}: size {size} runs past the NAL ({} bytes)",
                    b.len()
                ))
            })?;
        out.push((kind, &b[i..end]));
        i = end;
    }
    Ok(out)
}

/// Low-overhead OBUs: (obu_type, payload). OBUs without a size field are skipped (the
/// spec allows that only for the fixed-size OBUs, which carry no metadata).
fn obus(unit: &[u8]) -> Vec<(u32, &[u8])> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < unit.len() {
        let header = unit[i];
        if header & 0x80 != 0 {
            break; // forbidden bit: not an OBU stream
        }
        let kind = u32::from((header >> 3) & 0xf);
        let mut p = i + 1;
        if header & 0x04 != 0 {
            p += 1; // extension header
        }
        if header & 0x02 == 0 {
            i = p;
            continue; // no size field
        }
        let Some((size, used)) = leb128(unit, p) else {
            break;
        };
        let start = p + used;
        let Ok(size) = usize::try_from(size) else {
            break;
        };
        let Some(end) = start.checked_add(size).filter(|e| *e <= unit.len()) else {
            break;
        };
        out.push((kind, &unit[start..end]));
        i = end;
    }
    out
}

/// LEB128 (AV1's `leb128()`), returning the value and the bytes consumed.
fn leb128(b: &[u8], mut i: usize) -> Option<(u64, usize)> {
    let start = i;
    let mut value = 0u64;
    for shift in (0..64).step_by(7) {
        let byte = *b.get(i)?;
        i += 1;
        value |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            return Some((value, i - start));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Emulation prevention as an encoder applies it (H.264 7.4.1): the payload size
    /// counts RBSP bytes, the byte stream carries the escaped form.
    fn escape(b: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(b.len());
        let mut zeros = 0usize;
        for &x in b {
            if zeros >= 2 && x <= 3 {
                out.push(3);
                zeros = 0;
            }
            out.push(x);
            zeros = if x == 0 { zeros + 1 } else { 0 };
        }
        out
    }

    /// A prefix SEI NAL (HEVC type 39) with the given payload types, Annex-B framed.
    fn hevc_sei(payloads: &[(u32, Vec<u8>)]) -> Vec<u8> {
        let mut nal = vec![39 << 1, 1]; // nal_unit_type 39, nuh_layer_id 0, temporal_id_plus1 1
        for (kind, body) in payloads {
            nal.push(*kind as u8);
            nal.push(body.len() as u8);
            nal.extend_from_slice(&escape(body));
        }
        nal.push(0x80); // rbsp_trailing_bits
        let mut out = vec![0, 0, 0, 1];
        out.extend_from_slice(&nal);
        out
    }

    fn mdcv() -> Vec<u8> {
        let mut v = Vec::new();
        for (x, y) in [(34000u16, 16000u16), (13250, 34500), (7500, 3000)] {
            v.extend_from_slice(&x.to_be_bytes());
            v.extend_from_slice(&y.to_be_bytes());
        }
        v.extend_from_slice(&15635u16.to_be_bytes());
        v.extend_from_slice(&16450u16.to_be_bytes());
        v.extend_from_slice(&10_000_000u32.to_be_bytes());
        v.extend_from_slice(&1u32.to_be_bytes());
        v
    }

    /// HEVC: the encoder's `master-display`/`max-cll` SEI pair, in one access unit that also
    /// carries a slice NAL (start codes of both lengths, an emulation-prevention byte).
    #[test]
    fn hevc_sei_mastering_and_cll() {
        let mut unit = hevc_sei(&[(137, mdcv()), (144, vec![0x03, 0xe8, 0x01, 0x90])]);
        unit.extend_from_slice(&[0, 0, 1, 0x26, 0x01]); // slice NAL, ignored by the parser
        let m = parse_unit(CODEC_HEVC, &unit).unwrap();
        let d = m.mastering.expect("mastering display");
        assert_eq!(d.primaries_x, [34000, 13250, 7500]);
        assert_eq!(d.primaries_y, [16000, 34500, 3000]);
        assert_eq!((d.white_x, d.white_y), (15635, 16450));
        assert_eq!((d.max_luminance, d.min_luminance), (10_000_000, 1));
        assert_eq!(m.content_light, Some((1000, 400)));
    }

    /// Only a 0x03 after two zeros is emulation prevention; a byte stream escape inside the
    /// RBSP is restored before the payload is read.
    #[test]
    fn unescape_only_removes_emulation_prevention() {
        assert_eq!(unescape(&[0, 0, 3, 1]), vec![0, 0, 1]);
        assert_eq!(unescape(&[0, 3, 1]), vec![0, 3, 1]);
        // RBSP 00 00 03 is coded 00 00 03 03: the second 03 is data.
        assert_eq!(unescape(&[0, 0, 3, 3]), vec![0, 0, 3]);
    }

    #[test]
    fn h264_sei_types() {
        let mut nal = vec![6]; // nal_unit_type 6 (SEI), nal_ref_idc 0
        nal.push(144);
        nal.push(4);
        nal.extend_from_slice(&[0x03, 0xe8, 0x01, 0x90]);
        nal.push(0x80);
        let mut unit = vec![0, 0, 0, 1];
        unit.extend_from_slice(&nal);
        let m = parse_unit(CODEC_H264, &unit).unwrap();
        assert_eq!(m.content_light, Some((1000, 400)));
        assert!(m.mastering.is_none());
    }

    #[test]
    fn sei_unknown_payload_skipped_and_sizes_checked() {
        let unit = hevc_sei(&[(5, vec![1, 2, 3]), (144, vec![0, 1, 0, 2])]);
        assert_eq!(
            parse_unit(CODEC_HEVC, &unit).unwrap().content_light,
            Some((1, 2))
        );
        let bad = hevc_sei(&[(137, vec![0; 20])]);
        let e = parse_unit(CODEC_HEVC, &bad).unwrap_err();
        assert!(
            e.message().contains("at least 24 expected"),
            "{}",
            e.message()
        );
        let trunc = hevc_sei(&[(144, vec![0, 1])]);
        assert!(parse_unit(CODEC_HEVC, &trunc).is_err());
    }

    fn obu(kind: u32, payload: &[u8]) -> Vec<u8> {
        let mut out = vec![(kind as u8) << 3 | 0x02]; // has_size_field
        let mut n = payload.len();
        loop {
            let b = (n & 0x7f) as u8;
            n >>= 7;
            out.push(if n > 0 { b | 0x80 } else { b });
            if n == 0 {
                break;
            }
        }
        out.extend_from_slice(payload);
        out
    }

    /// AV1: metadata OBUs HDR_CLL (1) and HDR_MDCV (2) followed by a frame OBU.
    #[test]
    fn av1_metadata_obus() {
        let mut cll = vec![1u8]; // metadata_type 1
        cll.extend_from_slice(&[0x03, 0xe8, 0x01, 0x90]);
        let mut mdcv_body = vec![2u8]; // metadata_type 2
        mdcv_body.extend_from_slice(&mdcv()); // same field order as the SEI payload
        let mut unit = obu(5, &cll);
        unit.extend_from_slice(&obu(5, &mdcv_body));
        unit.extend_from_slice(&obu(6, &[0xaa; 3]));
        let m = parse_unit(CODEC_AV1, &unit).unwrap();
        assert_eq!(m.content_light, Some((1000, 400)));
        let d = m.mastering.expect("mastering display");
        assert_eq!(d.primaries_x, [34000, 13250, 7500]);
        assert_eq!(d.primaries_y, [16000, 34500, 3000]);
        assert_eq!((d.white_x, d.white_y), (15635, 16450));
        assert_eq!((d.max_luminance, d.min_luminance), (10_000_000, 1));
    }

    #[test]
    fn av1_wrong_hdr_size_is_an_error() {
        let mut cll = vec![1u8];
        cll.extend_from_slice(&[0, 1]);
        let e = parse_unit(CODEC_AV1, &obu(5, &cll)).unwrap_err();
        assert!(
            e.message().contains("at least 4 expected"),
            "{}",
            e.message()
        );
    }

    #[test]
    fn merge_carries_each_field_group_forward() {
        let mut cur = HdrStaticMetadata::default();
        cur.merge(HdrStaticMetadata {
            mastering: Some(MasteringDisplay {
                primaries_x: [1, 2, 3],
                ..Default::default()
            }),
            content_light: None,
        });
        cur.merge(HdrStaticMetadata {
            mastering: None,
            content_light: Some((1000, 400)),
        });
        assert_eq!(cur.mastering.unwrap().primaries_x, [1, 2, 3]);
        assert_eq!(cur.content_light, Some((1000, 400)));
        // A later declaration replaces only its own group.
        cur.merge(HdrStaticMetadata {
            mastering: Some(MasteringDisplay {
                primaries_x: [4, 5, 6],
                ..Default::default()
            }),
            content_light: None,
        });
        assert_eq!(cur.mastering.unwrap().primaries_x, [4, 5, 6]);
        assert_eq!(cur.content_light, Some((1000, 400)));
    }

    #[test]
    fn empty_unit_has_no_metadata() {
        assert!(parse_unit(CODEC_HEVC, &[]).unwrap().is_empty());
        assert!(parse_unit(CODEC_AV1, &[]).unwrap().is_empty());
    }
}
