//! C ABI of video/nv_video.h (v1 entry points, v2 additions: HDR static metadata and the
//! per-unit displayable-picture count).
use super::{
    hdr::HdrStaticMetadata,
    session::{Session, SessionInfo, Target},
    NvVideoCaps, NvVideoDeviceInfo, VideoDevice, VideoError,
};
use std::{cell::RefCell, ffi::CString, os::fd::IntoRawFd};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn fail(e: VideoError) -> i32 {
    let msg = e.message().replace('\0', " ");
    crate::diag::diag!("nv-video: {msg}");
    LAST_ERROR.with(|l| *l.borrow_mut() = CString::new(msg).unwrap_or_default());
    e.code()
}

fn guard(f: impl FnOnce() -> Result<i32, VideoError>) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => fail(e),
        Err(_) => fail(VideoError::DeviceLost("internal panic".into())),
    }
}

pub struct NvVideoDevice(VideoDevice);
pub struct NvVideoSession(Session);
// SAFETY: a session's RM objects are only reachable through the session; the
// header requires callers to serialise each session, so moving it between
// threads never shares its Rc graph concurrently.
unsafe impl Send for NvVideoSession {}

#[repr(C)]
pub struct NvVideoSessionInfo {
    pub codec: u32,
    pub max_width: u32,
    pub max_height: u32,
    pub bit_depth: u32,
    pub extra_output_slots: u32,
    pub codec_config: *const u8,
    pub codec_config_len: usize,
    pub reserved: [u32; 8],
}

#[repr(C)]
pub struct NvVideoPicture {
    pub id: u64,
    pub timestamp: u64,
    pub width: u32,
    pub height: u32,
    pub crop_left: u32,
    pub crop_top: u32,
    pub crop_width: u32,
    pub crop_height: u32,
    pub bit_depth: u32,
    pub color_primaries: u32,
    pub transfer: u32,
    pub matrix: u32,
    pub full_range: u32,
    pub keyframe: u32,
    pub reserved: [u32; 5],
}

/// `mastering_display_colour_volume` as coded in the bitstream (ST 2086 units).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NvVideoMasteringDisplay {
    pub display_primaries_x: [u16; 3],
    pub display_primaries_y: [u16; 3],
    pub white_point_x: u16,
    pub white_point_y: u16,
    pub max_luminance: u32,
    pub min_luminance: u32,
}

/// HDR static metadata of a picture (ABI v2, `nv_video_picture_hdr`). A field group with
/// its `has_*` flag clear carries no values.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NvVideoHdrMetadata {
    pub has_mastering_display: u32,
    pub mastering_display: NvVideoMasteringDisplay,
    pub has_content_light_level: u32,
    pub max_content_light_level: u32,
    pub max_pic_average_light_level: u32,
    pub reserved: [u32; 6],
}

impl From<HdrStaticMetadata> for NvVideoHdrMetadata {
    fn from(m: HdrStaticMetadata) -> Self {
        let mut out = Self::default();
        if let Some(d) = m.mastering {
            out.has_mastering_display = 1;
            out.mastering_display = NvVideoMasteringDisplay {
                display_primaries_x: d.primaries_x,
                display_primaries_y: d.primaries_y,
                white_point_x: d.white_x,
                white_point_y: d.white_y,
                max_luminance: d.max_luminance,
                min_luminance: d.min_luminance,
            };
        }
        if let Some((cll, fall)) = m.content_light {
            out.has_content_light_level = 1;
            out.max_content_light_level = u32::from(cll);
            out.max_pic_average_light_level = u32::from(fall);
        }
        out
    }
}

#[repr(C)]
pub struct NvVideoTarget {
    pub dmabuf_fd: [i32; 2],
    pub offset: [u64; 2],
    pub pitch: [u32; 2],
    pub modifier: u64,
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub acquire_fence_fd: i32,
    pub cr_dmabuf_fd: i32,
    pub cr_pitch: u32,
    pub cr_offset: u64,
    pub reserved: [u32; 2],
}

fn null<T>(p: *const T, what: &str) -> Result<(), VideoError> {
    if p.is_null() {
        Err(VideoError::Invalid(format!("{what} is NULL")))
    } else {
        Ok(())
    }
}

/// # Safety
/// `out` must be writable; `info` NULL or writable.
#[no_mangle]
pub unsafe extern "C" fn nv_video_device_open(
    out: *mut *mut NvVideoDevice,
    info: *mut NvVideoDeviceInfo,
) -> i32 {
    guard(|| {
        null(out, "out")?;
        let dev = VideoDevice::open()?;
        if !info.is_null() {
            // SAFETY: caller-provided writable struct.
            unsafe { info.write(dev.info) };
        }
        // SAFETY: checked non-NULL.
        unsafe { out.write(Box::into_raw(Box::new(NvVideoDevice(dev)))) };
        Ok(0)
    })
}

/// # Safety
/// `dev` from nv_video_device_open (or NULL), not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn nv_video_device_close(dev: *mut NvVideoDevice) {
    if !dev.is_null() {
        // SAFETY: ownership returns from the caller.
        drop(unsafe { Box::from_raw(dev) });
    }
}

/// # Safety
/// Valid device and writable caps.
#[no_mangle]
pub unsafe extern "C" fn nv_video_query_caps(
    dev: *mut NvVideoDevice,
    codec: u32,
    caps: *mut NvVideoCaps,
) -> i32 {
    guard(|| {
        null(dev, "dev")?;
        null(caps, "caps")?;
        // SAFETY: checked non-NULL, caller keeps them valid.
        let c = unsafe { &*dev }.0.caps(codec)?;
        unsafe { caps.write(c) };
        Ok(0)
    })
}

/// # Safety
/// Valid device, info and out pointers; codec_config readable for its length.
#[no_mangle]
pub unsafe extern "C" fn nv_video_session_create(
    dev: *mut NvVideoDevice,
    info: *const NvVideoSessionInfo,
    out: *mut *mut NvVideoSession,
) -> i32 {
    guard(|| {
        null(dev, "dev")?;
        null(info, "info")?;
        null(out, "out")?;
        // SAFETY: checked non-NULL.
        let i = unsafe { &*info };
        let cfg = if i.codec_config.is_null() || i.codec_config_len == 0 {
            &[][..]
        } else {
            // SAFETY: caller guarantees the buffer.
            unsafe { std::slice::from_raw_parts(i.codec_config, i.codec_config_len) }
        };
        let s = Session::new(
            &unsafe { &*dev }.0,
            SessionInfo {
                codec: i.codec,
                max_width: i.max_width,
                max_height: i.max_height,
                bit_depth: i.bit_depth,
                extra_output_slots: i.extra_output_slots,
            },
            cfg,
        )?;
        unsafe { out.write(Box::into_raw(Box::new(NvVideoSession(s)))) };
        Ok(0)
    })
}

/// # Safety
/// `s` from nv_video_session_create (or NULL), not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn nv_video_session_destroy(s: *mut NvVideoSession) {
    if !s.is_null() {
        // SAFETY: ownership returns from the caller.
        drop(unsafe { Box::from_raw(s) });
    }
}

/// # Safety
/// Valid session; `data` readable for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn nv_video_decode(
    s: *mut NvVideoSession,
    data: *const u8,
    len: usize,
    timestamp: u64,
    flags: u32,
) -> i32 {
    guard(|| {
        null(s, "session")?;
        if data.is_null() && len != 0 {
            return Err(VideoError::Invalid("data NULL with nonzero length".into()));
        }
        let d = if len == 0 {
            &[][..]
        } else {
            // SAFETY: caller guarantees the buffer.
            unsafe { std::slice::from_raw_parts(data, len) }
        };
        let shown = unsafe { &mut *s }.0.decode(d, timestamp, flags)?;
        // ABI v2: the displayable pictures this unit produced (callers test rc < 0).
        i32::try_from(shown)
            .map_err(|_| VideoError::Invalid("unit produced more than 2^31 pictures".into()))
    })
}

/// # Safety
/// Valid session and writable picture.
#[no_mangle]
pub unsafe extern "C" fn nv_video_next_picture(
    s: *mut NvVideoSession,
    pic: *mut NvVideoPicture,
) -> i32 {
    guard(|| {
        null(s, "session")?;
        null(pic, "pic")?;
        let Some((id, timestamp, i)) = unsafe { &mut *s }.0.next_picture() else {
            return Ok(0);
        };
        let p = NvVideoPicture {
            id,
            timestamp,
            width: i.width,
            height: i.height,
            crop_left: i.crop[0],
            crop_top: i.crop[1],
            crop_width: i.crop[2],
            crop_height: i.crop[3],
            bit_depth: i.bit_depth,
            color_primaries: i.color_primaries,
            transfer: i.transfer,
            matrix: i.matrix,
            full_range: u32::from(i.full_range),
            keyframe: u32::from(i.keyframe),
            reserved: [0; 5],
        };
        unsafe { pic.write(p) };
        Ok(1)
    })
}

/// # Safety
/// Valid session, writable metadata.
#[no_mangle]
pub unsafe extern "C" fn nv_video_picture_hdr(
    s: *mut NvVideoSession,
    id: u64,
    metadata: *mut NvVideoHdrMetadata,
) -> i32 {
    guard(|| {
        null(s, "session")?;
        null(metadata, "metadata")?;
        let m = unsafe { &*s }.0.picture_hdr(id)?;
        unsafe { metadata.write(NvVideoHdrMetadata::from(m)) };
        Ok(0)
    })
}

/// # Safety
/// Valid session, writable width/height.
#[no_mangle]
pub unsafe extern "C" fn nv_video_required_size(
    s: *mut NvVideoSession,
    width: *mut u32,
    height: *mut u32,
) -> i32 {
    guard(|| {
        null(s, "session")?;
        null(width, "width")?;
        null(height, "height")?;
        let (w, h) = unsafe { &*s }.0.required_size().ok_or_else(|| {
            VideoError::Invalid("no unit of this session returned NV_VIDEO_E_RESIZE".into())
        })?;
        unsafe {
            width.write(w);
            height.write(h);
        }
        Ok(0)
    })
}

/// # Safety
/// Valid session.
#[no_mangle]
pub unsafe extern "C" fn nv_video_flush(s: *mut NvVideoSession) -> i32 {
    guard(|| {
        null(s, "session")?;
        unsafe { &mut *s }.0.flush()?;
        Ok(0)
    })
}

/// # Safety
/// Valid session, target and fence pointer; target fds live for the call.
#[no_mangle]
pub unsafe extern "C" fn nv_video_copy_out(
    s: *mut NvVideoSession,
    id: u64,
    target: *const NvVideoTarget,
    release_fence_fd: *mut i32,
) -> i32 {
    guard(|| {
        null(s, "session")?;
        null(target, "target")?;
        null(release_fence_fd, "release_fence_fd")?;
        let t = unsafe { &*target };
        let fd = unsafe { &mut *s }.0.copy_out(
            id,
            &Target {
                dmabuf_fd: [t.dmabuf_fd[0], t.dmabuf_fd[1], t.cr_dmabuf_fd],
                offset: [t.offset[0], t.offset[1], t.cr_offset],
                pitch: [t.pitch[0], t.pitch[1], t.cr_pitch],
                modifier: t.modifier,
                format: t.format,
                width: t.width,
                height: t.height,
                acquire_fence_fd: t.acquire_fence_fd,
            },
        )?;
        unsafe { release_fence_fd.write(fd.map_or(-1, |f| f.into_raw_fd())) };
        Ok(0)
    })
}

/// # Safety
/// Valid session.
#[no_mangle]
pub unsafe extern "C" fn nv_video_picture_release(s: *mut NvVideoSession, id: u64) -> i32 {
    guard(|| {
        null(s, "session")?;
        unsafe { &mut *s }.0.release(id)?;
        Ok(0)
    })
}

#[no_mangle]
pub extern "C" fn nv_video_last_error() -> *const std::ffi::c_char {
    LAST_ERROR.with(|l| l.borrow().as_ptr())
}

// The C struct keeps its v1 size (the Cr-plane fields took over reserved words).
const _: () = assert!(std::mem::size_of::<NvVideoTarget>() == 80);
// ABI v2 structs: fixed C layout, no implicit padding.
const _: () = assert!(std::mem::size_of::<NvVideoMasteringDisplay>() == 24);
const _: () = assert!(std::mem::size_of::<NvVideoHdrMetadata>() == 64);

#[cfg(test)]
mod tests {
    use super::super::hdr::{HdrStaticMetadata, MasteringDisplay};
    use super::NvVideoHdrMetadata;

    #[test]
    fn hdr_metadata_conversion() {
        let m = HdrStaticMetadata {
            mastering: Some(MasteringDisplay {
                primaries_x: [34000, 13250, 7500],
                primaries_y: [16000, 34500, 3000],
                white_x: 15635,
                white_y: 16450,
                max_luminance: 10_000_000,
                min_luminance: 1,
            }),
            content_light: Some((1000, 400)),
        };
        let c = NvVideoHdrMetadata::from(m);
        assert_eq!((c.has_mastering_display, c.has_content_light_level), (1, 1));
        assert_eq!(
            c.mastering_display.display_primaries_x,
            [34000, 13250, 7500]
        );
        assert_eq!(c.mastering_display.max_luminance, 10_000_000);
        assert_eq!(
            (c.max_content_light_level, c.max_pic_average_light_level),
            (1000, 400)
        );
        let e = NvVideoHdrMetadata::from(HdrStaticMetadata::default());
        assert_eq!((e.has_mastering_display, e.has_content_light_level), (0, 0));
        assert_eq!(e, NvVideoHdrMetadata::default());
    }
}
