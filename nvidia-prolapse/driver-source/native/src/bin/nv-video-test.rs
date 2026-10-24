//! GNU host test of nv_video.h through its C ABI: every frame goes through
//! nv_video_decode -> nv_video_next_picture -> nv_video_copy_out (LINEAR NV12
//! dma-buf target) -> wait release fence -> CPU readback -> compare with a
//! software (libvpx / dav1d) NV12 reference.
//! usage: nv-video-test <in.ivf> <reference.nv12|-> [max_w max_h]
//! NV_VIDEO_TARGET=yuv420p: copy out to a planar Y/Cb/Cr target (YV12 plane order and pitches)
//! and compare it repacked as NV12.
//! NV_VIDEO_BIT_DEPTH=10: create a 10-bit session and copy out P010 (references and dumps are then
//! p010le: 16-bit little-endian samples, 10 MSBs).
//! NV_VIDEO_DUMP=<out.nv12>: also write every output picture (display order,
//! display window, tightly packed NV12, concatenated) to that file.
use nv_bionic_native::video::ffi::NvVideoHdrMetadata;
use nv_bionic_native::video::{
    ffi::*, NvVideoDeviceInfo, TestDmaBuf, VideoDevice, CODEC_AV1, CODEC_VP9, FORMAT_NV12,
    FORMAT_P010, FORMAT_YUV420P,
};
use std::time::{Duration, Instant};

fn check(r: i32, what: &str) {
    if r < 0 {
        // SAFETY: nv_video_last_error returns a valid C string.
        let msg = unsafe { std::ffi::CStr::from_ptr(nv_video_last_error()) };
        eprintln!("{what}: error {r}: {}", msg.to_string_lossy());
        std::process::exit(2);
    }
}

fn wait_fd(fd: i32) {
    if fd < 0 {
        return;
    }
    #[repr(C)]
    struct P {
        fd: i32,
        e: i16,
        r: i16,
    }
    extern "C" {
        fn poll(f: *mut P, n: std::ffi::c_ulong, t: i32) -> i32;
        fn close(fd: i32) -> i32;
    }
    let mut p = P { fd, e: 1, r: 0 };
    // SAFETY: one pollfd; fd owned by us.
    unsafe {
        if poll(&mut p, 1, 2000) != 1 {
            eprintln!("release fence did not signal");
            std::process::exit(3);
        }
        close(fd);
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let data = std::fs::read(&args[0]).expect("read ivf");
    let packets: Vec<Vec<u8>> = if data.get(8..12) == Some(b"AV01") {
        nv_bionic_native::video::av1::obu::ivf_frames(&data)
            .expect("ivf")
            .into_iter()
            .map(<[u8]>::to_vec)
            .collect()
    } else {
        nv_bionic_native::vp9::ivf_frames(&data)
            .expect("ivf")
            .into_iter()
            .map(|p| p.to_vec())
            .collect()
    };
    let reference = (args.get(1).map(String::as_str) != Some("-"))
        .then(|| std::fs::read(&args[1]).expect("read reference"));
    let w0 = u16::from_le_bytes([data[12], data[13]]) as u32;
    let h0 = u16::from_le_bytes([data[14], data[15]]) as u32;
    let max_w: u32 = args.get(2).map_or(w0, |v| v.parse().unwrap());
    let max_h: u32 = args.get(3).map_or(h0, |v| v.parse().unwrap());

    let mut dev = std::ptr::null_mut();
    let mut info = std::mem::MaybeUninit::<NvVideoDeviceInfo>::zeroed();
    check(
        unsafe { nv_video_device_open(&mut dev, info.as_mut_ptr()) },
        "device_open",
    );
    let info = unsafe { info.assume_init() };
    let name = String::from_utf8_lossy(&info.name);
    println!(
        "device {:04x} {} class {:#x} engines {}",
        info.pci_device,
        name.trim_end_matches('\0'),
        info.nvdec_class,
        info.nvdec_engines
    );
    // AV1: hand the first sequence header OBU over as codec_config (what Android passes as csd-0).
    let mut config = Vec::new();
    if &data[8..12] == b"AV01" {
        use nv_bionic_native::video::av1::obu;
        if let Some(o) = obu::split_obus(&packets[0])
            .expect("obus")
            .into_iter()
            .find(|o| o.obu_type == obu::OBU_SEQUENCE_HEADER)
        {
            config.push(0x0a); // obu_type 1, has_size_field
            let mut n = o.payload.len();
            loop {
                let b = (n & 0x7f) as u8;
                n >>= 7;
                config.push(if n > 0 { b | 0x80 } else { b });
                if n == 0 {
                    break;
                }
            }
            config.extend_from_slice(o.payload);
        }
    }
    // NV_VIDEO_REPORT=1: per-unit picture counts (nv_video_decode's ABI 2 return) and the
    // HDR static metadata of every picture that carries any.
    let report = std::env::var_os("NV_VIDEO_REPORT").is_some();
    let bit_depth: u32 =
        std::env::var("NV_VIDEO_BIT_DEPTH").map_or(8, |v| v.parse().expect("NV_VIDEO_BIT_DEPTH"));
    let bps = if bit_depth > 8 { 2usize } else { 1 };
    let planar = match std::env::var("NV_VIDEO_TARGET").as_deref() {
        Ok("yuv420p") => true,
        Ok("nv12") | Err(_) => false,
        Ok(other) => panic!("NV_VIDEO_TARGET={other}: nv12 or yuv420p"),
    };
    let si = NvVideoSessionInfo {
        // IVF fourcc selects the codec (VP90 / AV01).
        codec: if &data[8..12] == b"AV01" {
            CODEC_AV1
        } else {
            CODEC_VP9
        },
        max_width: max_w,
        max_height: max_h,
        bit_depth,
        extra_output_slots: 2,
        codec_config: if config.is_empty() {
            std::ptr::null()
        } else {
            config.as_ptr()
        },
        codec_config_len: config.len(),
        reserved: [0; 8],
    };
    let mut s = std::ptr::null_mut();
    check(
        unsafe { nv_video_session_create(dev, &si, &mut s) },
        "session_create",
    );
    let vdev = VideoDevice::open().expect("device");
    let pitch = (max_w * bps as u32).next_multiple_of(256);
    let luma = pitch as usize * max_h as usize;
    let size = luma + pitch as usize * max_h.div_ceil(2) as usize;
    let target = TestDmaBuf::new(&vdev, size).expect("dma-buf");
    use std::os::fd::AsRawFd;
    let tfd = target.fd.as_raw_fd();

    let mut off = 0usize;
    let (mut frames, mut bad, mut diff_total) = (0usize, 0usize, 0usize);
    let mut dec_times = Vec::new();
    let mut copy_times = Vec::new();
    let mut out = vec![0u8; size];
    let mut dump = std::env::var_os("NV_VIDEO_DUMP")
        .map(|p| std::io::BufWriter::new(std::fs::File::create(p).expect("create NV_VIDEO_DUMP")));
    let mut handle = |s: *mut NvVideoSession,
                      frames: &mut usize,
                      bad: &mut usize,
                      diff_total: &mut usize,
                      off: &mut usize,
                      copy_times: &mut Vec<Duration>| {
        loop {
            let mut pic = std::mem::MaybeUninit::<NvVideoPicture>::zeroed();
            let r = unsafe { nv_video_next_picture(s, pic.as_mut_ptr()) };
            check(r, "next_picture");
            if r == 0 {
                break;
            }
            let pic = unsafe { pic.assume_init() };
            if report {
                let mut hdr = std::mem::MaybeUninit::<NvVideoHdrMetadata>::zeroed();
                check(
                    unsafe { nv_video_picture_hdr(s, pic.id, hdr.as_mut_ptr()) },
                    "picture_hdr",
                );
                let hdr = unsafe { hdr.assume_init() };
                if hdr.has_mastering_display != 0 || hdr.has_content_light_level != 0 {
                    println!(
                        "picture {} HDR: mdcv {} R({},{}) G({},{}) B({},{}) WP({},{}) L({},{}) \
                         CLL {} ({},{})",
                        pic.id,
                        hdr.has_mastering_display,
                        hdr.mastering_display.display_primaries_x[0],
                        hdr.mastering_display.display_primaries_y[0],
                        hdr.mastering_display.display_primaries_x[1],
                        hdr.mastering_display.display_primaries_y[1],
                        hdr.mastering_display.display_primaries_x[2],
                        hdr.mastering_display.display_primaries_y[2],
                        hdr.mastering_display.white_point_x,
                        hdr.mastering_display.white_point_y,
                        hdr.mastering_display.max_luminance,
                        hdr.mastering_display.min_luminance,
                        hdr.has_content_light_level,
                        hdr.max_content_light_level,
                        hdr.max_pic_average_light_level
                    );
                }
            }
            let (w, h) = (pic.crop_width as usize, pic.crop_height as usize);
            target.fill(0xa5).unwrap();
            // NV_VIDEO_TARGET=yuv420p: planar target laid out like an Android YV12 buffer (Y, then
            // Cr, then Cb; chroma pitch = luma pitch / 2), repacked to NV12 for the comparison.
            let cpitch = pitch / 2;
            let cr = luma as u64;
            let cb = cr + u64::from(cpitch) * u64::from(max_h.div_ceil(2));
            let t = NvVideoTarget {
                dmabuf_fd: [tfd, tfd],
                offset: [0, if planar { cb } else { luma as u64 }],
                pitch: [pitch, if planar { cpitch } else { pitch }],
                modifier: 0,
                format: if planar {
                    FORMAT_YUV420P
                } else if bps == 2 {
                    FORMAT_P010
                } else {
                    FORMAT_NV12
                },
                width: max_w,
                height: max_h,
                acquire_fence_fd: -1,
                cr_dmabuf_fd: if planar { tfd } else { -1 },
                cr_pitch: if planar { cpitch } else { 0 },
                cr_offset: if planar { cr } else { 0 },
                reserved: [0; 2],
            };
            let mut fence = -1;
            let t0 = Instant::now();
            check(
                unsafe { nv_video_copy_out(s, pic.id, &t, &mut fence) },
                "copy_out",
            );
            wait_fd(fence);
            copy_times.push(t0.elapsed());
            target.read(0, &mut out).unwrap();
            let mut nv12 = Vec::with_capacity(w * h * 3 / 2 * bps);
            for y in 0..h {
                nv12.extend_from_slice(&out[y * pitch as usize..][..w * bps]);
            }
            for y in 0..h.div_ceil(2) {
                if planar {
                    let row =
                        |base: u64| &out[base as usize + y * cpitch as usize..][..w.div_ceil(2)];
                    for (u, v) in row(cb).iter().zip(row(cr)) {
                        nv12.extend_from_slice(&[*u, *v]);
                    }
                } else {
                    nv12.extend_from_slice(
                        &out[luma + y * pitch as usize..][..w.div_ceil(2) * 2 * bps],
                    );
                }
            }
            if let Some(d) = dump.as_mut() {
                std::io::Write::write_all(d, &nv12).expect("write NV_VIDEO_DUMP");
            }
            if let Some(r) = &reference {
                let refp = &r[*off..*off + nv12.len()];
                let d = nv12.iter().zip(refp).filter(|(a, b)| a != b).count();
                if d != 0 {
                    *bad += 1;
                    *diff_total += d;
                    if *bad <= 5 {
                        println!(
                            "frame {}: {d} of {} bytes differ ({w}x{h})",
                            *frames,
                            nv12.len()
                        );
                    }
                }
            }
            *off += nv12.len();
            *frames += 1;
            check(unsafe { nv_video_picture_release(s, pic.id) }, "release");
        }
    };
    let start = Instant::now();
    let mut empty_units = 0usize;
    for (i, p) in packets.iter().enumerate() {
        let t0 = Instant::now();
        let shown = unsafe { nv_video_decode(s, p.as_ptr(), p.len(), i as u64, 0) };
        check(shown, "decode");
        if shown == 0 {
            empty_units += 1;
        }
        if report {
            println!("unit {i}: {shown} displayable pictures, {} bytes", p.len());
        }
        dec_times.push(t0.elapsed());
        handle(
            s,
            &mut frames,
            &mut bad,
            &mut diff_total,
            &mut off,
            &mut copy_times,
        );
    }
    check(
        unsafe { nv_video_decode(s, std::ptr::null(), 0, 0, 1) },
        "drain",
    );
    handle(
        s,
        &mut frames,
        &mut bad,
        &mut diff_total,
        &mut off,
        &mut copy_times,
    );
    let wall = start.elapsed();
    if let Some(d) = dump.as_mut() {
        std::io::Write::flush(d).expect("flush NV_VIDEO_DUMP");
    }
    let stat = |v: &mut Vec<Duration>| {
        v.sort();
        let total: Duration = v.iter().sum();
        (
            total / v.len().max(1) as u32,
            v.get(v.len() / 2).copied().unwrap_or_default(),
            v.last().copied().unwrap_or_default(),
        )
    };
    let (dm, dmed, dmax) = stat(&mut dec_times);
    let (cm, cmed, cmax) = stat(&mut copy_times);
    println!(
        "{} packets ({empty_units} without a displayable picture), {frames} pictures, max {max_w}x{max_h}; nv_video_decode mean {dm:?} median {dmed:?} max {dmax:?}; copy_out+fence mean {cm:?} median {cmed:?} max {cmax:?}; wall {wall:?}",
        packets.len()
    );
    unsafe {
        nv_video_session_destroy(s);
        nv_video_device_close(dev);
    }
    if let Some(r) = &reference {
        if off != r.len() {
            println!("MISMATCH: produced {off} bytes, reference {}", r.len());
            std::process::exit(1);
        } else if bad == 0 {
            println!("PASS: all {frames} pictures bit-exact against the software reference");
        } else {
            println!("MISMATCH: {bad} pictures differ ({diff_total} bytes)");
            std::process::exit(1);
        }
    }
}
