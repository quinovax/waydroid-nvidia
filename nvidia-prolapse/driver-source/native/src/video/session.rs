//! Codec-agnostic decode session: DPB surface pool, 4-slot descriptor /
//! bitstream ring, display queue, picture refcounts, and copy_out on the copy
//! engine with a sync_file release fence.
//!
//! Stock behaviour (libnvcuvid 615.71.09, ELF addresses cited):
//! - ring of 4 descriptor + bitstream slots, bitstream staging growth
//!   `(len + 0x8ff) & ~0xff`: 0x141d50, allocations at 0x1393f0.
//! - DPB surfaces in VRAM in the engine layout, chroma after an aligned luma
//!   plane: 0x13a310.
//! - stock un-tiles in MapVideoFrame with CUDA kernels; we copy with
//!   the copy engine instead: the VP9 engine output (tileformat 1 on Blackwell)
//!   is exactly the Blackwell 8-bit (luma) / 16-bit (chroma) GOB layout that the
//!   copy engine understands through SET_SRC_BLOCK_SIZE.GOB_KIND (measured
//!   2026-10-14, see video/README.md).
use super::{
    channel::{wait_sync_file, NvdecChannel},
    hdr::HdrStaticMetadata,
    open_render_node, open_rm, Allocator, BuildContext, CodecDecoder, DecodeJob, Job, NvdecGen,
    PictureInfo, SurfaceLayout, SurfaceRef, VResult, VideoDevice, VideoError, FORMAT_NV12,
    FORMAT_P010, FORMAT_YUV420P,
};
use crate::{
    events::RmEvents,
    gpu::{method, AddressSpace, GpuMapping, Push, Queue, SemaphoreOp, SubmitStatus},
    rm::{Device, Mapping, Memory},
    slots::{SlotPool, TimelineView, PAGE_BYTES},
};
use std::{
    collections::{HashMap, VecDeque},
    os::fd::{AsFd, BorrowedFd, OwnedFd},
    rc::Rc,
    sync::Arc,
    time::Duration,
};

/// Descriptor slot size (VP9: pic_s 0, tiles 0x200, probs 0x900, counts
/// 0x1800, status 0x4c00; stock 615.71.09 trace).
pub const DESCRIPTOR_BYTES: usize = 0x5000;
pub const RING_SLOTS: usize = 4;
const BITSTREAM_INITIAL: usize = 16 << 20;

pub struct SessionInfo {
    pub codec: u32,
    pub max_width: u32,
    pub max_height: u32,
    pub bit_depth: u32,
    pub extra_output_slots: u32,
}

struct VramBuffer {
    _memory: Rc<Memory>,
    cpu: Mapping,
    gpu: GpuMapping,
}

fn vram(device: &Rc<Device>, space: &AddressSpace, size: usize) -> VResult<VramBuffer> {
    let memory = device
        .allocate_vram(size)
        .map_err(|e| VideoError::NoMemory(format!("VRAM {size} bytes: {e}")))?;
    let gpu = space.map(&memory)?;
    if gpu.address() & 0xff != 0 || gpu.address() >> 40 != 0 {
        return Err(VideoError::DeviceLost(
            "NVDEC buffers need 256-byte aligned 40-bit VAs".into(),
        ));
    }
    let mut cpu = memory.map()?;
    let zero = vec![0u8; size.min(1 << 20)];
    let mut off = 0;
    while off < size {
        let n = zero.len().min(size - off);
        cpu.write_bytes(off, &zero[..n])?;
        off += n;
    }
    Ok(VramBuffer {
        _memory: memory,
        cpu,
        gpu,
    })
}

struct SideAllocator<'a> {
    device: &'a Rc<Device>,
    space: &'a AddressSpace,
    out: &'a mut Vec<VramBuffer>,
}
impl Allocator for SideAllocator<'_> {
    fn vram(&mut self, size: usize) -> VResult<u64> {
        let b = vram(self.device, self.space, size)?;
        let va = b.gpu.address();
        self.out.push(b);
        Ok(va)
    }
    fn write(&mut self, va: u64, offset: usize, data: &[u8]) -> VResult<()> {
        let b = self
            .out
            .iter_mut()
            .find(|b| b.gpu.address() == va)
            .ok_or_else(|| VideoError::Invalid("write to an unknown side buffer".into()))?;
        b.cpu.write_bytes(offset, data)?;
        Ok(())
    }
}

struct RingSlot {
    desc: VramBuffer,
    bits: Option<VramBuffer>,
    bits_size: usize,
    /// NVDEC timeline value of the last picture using this slot.
    busy_until: u64,
    /// The engine status of that picture has not been read yet (codecs with a
    /// `status_offset`).
    unchecked: bool,
    /// Picture index of that picture (error messages).
    picture: u32,
}

struct Picture {
    surface: usize,
    info: PictureInfo,
    timestamp: u64,
    /// NVDEC timeline value that completes this picture's decode.
    decoded_at: u64,
    /// HDR static metadata of the unit that decoded this surface (empty: none declared).
    hdr: HdrStaticMetadata,
}

/// An imported target dma-buf and its VA in the session's address space.
type Import = (Rc<Memory>, GpuMapping);

/// Copy-engine context: a backend Queue (copy engine only), its own slot
/// pool page, and per-copy command buffers.
struct CopyEngine {
    queue: Queue,
    copy_class: u32,
    _pool: Arc<SlotPool>,
    _page: (Rc<Memory>, Mapping, GpuMapping),
    _events: RmEvents,
    commands: Vec<(VramBuffer, u64)>,
    /// Imported targets kept alive until their copy retired.
    pending: VecDeque<(u64, Vec<Import>)>,
}

impl CopyEngine {
    fn new(device: &Rc<Device>, space: &AddressSpace) -> VResult<Self> {
        let caps = crate::capabilities::query(device)?;
        let view = if crate::gpu::modern_host(caps.channel)? {
            TimelineView::Native64
        } else {
            TimelineView::Split32
        };
        let pool = Arc::new(SlotPool::new(view));
        let page_mem = device.allocate_aligned(PAGE_BYTES, PAGE_BYTES, false, true)?;
        let page_cpu = page_mem.map()?;
        let page_gpu = space.map(&page_mem)?;
        pool.install_page(page_cpu.cpu_address(), page_gpu.address())?;
        let retire = pool
            .try_allocate()
            .ok_or_else(|| VideoError::DeviceLost("empty semaphore slot page".into()))?;
        let events = RmEvents::new(device)?;
        let queue = Queue::new_with_events(device, space, 1, &events, &pool, retire)?;
        Ok(Self {
            queue,
            copy_class: caps.copy,
            _pool: pool,
            _page: (page_mem, page_cpu, page_gpu),
            _events: events,
            commands: Vec::new(),
            pending: VecDeque::new(),
        })
    }
    fn retired(&self, token: u64) -> VResult<bool> {
        Ok(token == 0 || self.queue.shared().retired(token)?)
    }
    fn reap(&mut self) -> VResult<()> {
        while let Some((t, _)) = self.pending.front() {
            if !self.retired(*t)? {
                break;
            }
            self.pending.pop_front();
        }
        Ok(())
    }
}

/// Admission of one decode unit (`NV_VIDEO_E_AGAIN` otherwise), decided before
/// the codec consumes any state. `need`: pictures the unit decodes; `free`:
/// surfaces neither referenced nor held now; `held`: distinct surfaces of
/// unreleased pictures; `total`: pool size. Either every picture can take a
/// surface that is free already, or (codecs bounding their references to
/// `max_referenced` distinct surfaces between pictures) the references, the
/// picture in flight and every held picture fit the pool at once, so each
/// picture of the unit finds a surface whatever the unit refreshes. A caller
/// that holds nothing is therefore never refused when
/// `max_referenced + 1 <= total`.
pub(crate) fn unit_admitted(
    need: usize,
    free: usize,
    max_referenced: Option<usize>,
    held: usize,
    total: usize,
) -> bool {
    need <= free || max_referenced.is_some_and(|n| n + 1 + held <= total)
}

/// Why a session's requested bit depth cannot be built, or `None` when the caps mask covers
/// it. H.264 has no 10/12-bit path in this library: the codec is constructed without a bit
/// depth (8-bit engine surfaces) and has no High-10 descriptor setup, so the refusal names
/// that instead of reporting a bare capability mask.
pub(crate) fn bit_depth_refusal(codec: u32, depth: u32, mask: u32) -> Option<String> {
    if depth < 32 && mask & (1 << depth) != 0 {
        return None;
    }
    let mut m = format!("bit depth {depth} for codec {codec} is not supported (mask {mask:#x})");
    if codec == super::CODEC_H264 && depth > 8 {
        m.push_str(
            ": 10/12-bit H.264 is not implemented - the codec is built with 8-bit surfaces \
             only and has no High-10 descriptor setup",
        );
    }
    Some(m)
}

/// The engine's `nvdec_status_s` head (nvdec_drv.h: mbs_correctly_decoded,
/// mbs_in_error, cycle_count, error_status): an error message when
/// error_status is non-zero (observed 0xd when the AV1 +0x244 word is missing).
pub(crate) fn engine_status_error(status: &[u8; 16], picture: u32) -> Option<String> {
    let word = |i: usize| {
        u32::from_le_bytes([
            status[4 * i],
            status[4 * i + 1],
            status[4 * i + 2],
            status[4 * i + 3],
        ])
    };
    (word(3) != 0).then(|| {
        format!(
            "NVDEC error_status {:#x} ({} blocks in error) on picture {picture}: the decoded picture \
             is wrong (malformed bitstream or a stream feature the descriptor does not describe); \
             flush and restart at a key frame",
            word(3),
            word(1)
        )
    })
}

pub struct Session {
    codec: Box<dyn CodecDecoder>,
    // Field order = drop order: channels before the memory they reference.
    nvdec: NvdecChannel,
    copy: CopyEngine,
    ring: Vec<RingSlot>,
    next_ring: usize,
    surfaces: Vec<VramBuffer>,
    surface_refs: Vec<SurfaceRef>,
    /// Last copy-engine token reading each surface.
    surface_copy: Vec<u64>,
    /// HDR static metadata declared by the unit that decoded each surface (a
    /// show_existing_frame outputs the metadata of the frame it shows).
    surface_hdr: Vec<HdrStaticMetadata>,
    /// Last HDR declaration of the stream (carried to pictures of later units).
    current_hdr: HdrStaticMetadata,
    /// Set when a unit returned `VideoError::Resize`: the session only drains from
    /// then on (the caller re-creates it at this size and resubmits the unit).
    resize_needed: Option<(u32, u32)>,
    /// Displayable pictures emitted since session start (nv_video_decode returns the
    /// increase per unit, the ABI v2 "this unit produced a picture" signal).
    shown: u64,
    layout: SurfaceLayout,
    _side: Vec<VramBuffer>,
    pictures: HashMap<u64, Picture>,
    display: VecDeque<u64>,
    reorder: Vec<(i64, u64)>,
    next_id: u64,
    picture_index: u32,
    info: SessionInfo,
    /// Sticky engine error: every later decode/copy_out fails until flush.
    failed: Option<String>,
    pub stats: Stats,
    drm: std::fs::File,
    space: AddressSpace,
    device: Rc<Device>,
}

/// Per-session timing (submit → completion is only measured where the CPU
/// waits anyway).
#[derive(Default, Debug, Clone)]
pub struct Stats {
    pub decoded: u64,
    pub cpu_waits: u64,
}

impl Session {
    pub fn new(dev: &VideoDevice, info: SessionInfo, codec_config: &[u8]) -> VResult<Self> {
        let class = dev.info.nvdec_class;
        if class == 0 {
            return Err(VideoError::Unsupported("GPU exposes no NVDEC class".into()));
        }
        let caps = super::caps(info.codec, class, &dev.engine0);
        if caps.supported == 0 {
            let known = super::NVDEC_CLASSES.iter().find(|c| c.0 == class);
            let why = if let Some(gap) = super::class_gap(info.codec, class) {
                gap.to_string()
            } else if !NvdecGen::of(class).turing_plus() {
                "stock NVDEC does not decode it on this class".to_string()
            } else if !dev.engine0.decodes(info.codec) {
                "NVDEC0's BSP caps (NV0080_CTRL_CMD_BSP_GET_CAPS_V2) disable this codec, as stock reads them"
                    .to_string()
            } else if info.codec == super::CODEC_AV1 && !NvdecGen::of(class).ga10x_plus() {
                "NVDEC AV1 needs GA10x or newer (stock: Turing, GA100 and Hopper have none)"
                    .to_string()
            } else if known.is_some_and(|c| !c.3) {
                "this NVDEC class never ran this library's decoder; set NV_VIDEO_UNTESTED_CLASSES=1 \
                 only to verify it"
                    .to_string()
            } else {
                "unknown NVDEC class".to_string()
            };
            return Err(VideoError::Unsupported(format!(
                "codec {} on NVDEC class {class:#x}: {why}",
                info.codec
            )));
        }
        if !super::output_layout_verified(class) {
            eprintln!(
                "nv_video: NVDEC class {class:#x}: engine output layout assumed GOB kind 0 (standard \
                 block linear) for copy_out; never measured on this class (verification run)"
            );
        }
        if info.max_width < caps.min_width
            || info.max_height < caps.min_height
            || info.max_width > caps.max_width
            || info.max_height > caps.max_height
        {
            return Err(VideoError::Unsupported(format!(
                "max size {}x{} outside {}x{}..{}x{}",
                info.max_width,
                info.max_height,
                caps.min_width,
                caps.min_height,
                caps.max_width,
                caps.max_height
            )));
        }
        if let Some(why) = bit_depth_refusal(info.codec, info.bit_depth, caps.bit_depth_mask) {
            let hint = if info.codec == super::CODEC_VP9 && !dev.engine0.vp9_high_bit_depth {
                ": NVDEC0's BSP caps report no high-bit-depth VP9 (capsTbl[4] bit 4), as stock reads them"
            } else {
                ""
            };
            return Err(VideoError::Unsupported(format!("{why}{hint}")));
        }
        let mut codec: Box<dyn CodecDecoder> = match info.codec {
            super::CODEC_VP9 => Box::new(super::vp9::Vp9Decoder::new(
                class,
                info.max_width,
                info.max_height,
                info.bit_depth,
            )),
            super::CODEC_AV1 => Box::new(super::av1::codec::Av1Decoder::new(
                info.max_width,
                info.max_height,
                info.bit_depth,
                codec_config,
            )?),
            super::CODEC_H264 => Box::new(super::h264::codec::H264Decoder::new(
                class,
                info.max_width,
                info.max_height,
                codec_config,
            )?),
            super::CODEC_HEVC => Box::new(super::hevc::codec::HevcDecoder::new(
                class,
                info.max_width,
                info.max_height,
                info.bit_depth,
                codec_config,
            )?),
            c => return Err(VideoError::Unsupported(format!("codec {c} not built in"))),
        };
        // The codec decides the engine surface layout from its own bit depth, and the DPB
        // surfaces below are sized from it: a codec built for another depth would allocate
        // the wrong sample container silently.
        if codec.bit_depth() != info.bit_depth {
            return Err(VideoError::Unsupported(format!(
                "codec {} was built for {}-bit surfaces but the session requested {} bits: the \
                 codec constructor does not take the session bit depth (H.264 is built 8-bit \
                 only; it has no High-10 descriptor setup)",
                info.codec,
                codec.bit_depth(),
                info.bit_depth
            )));
        }
        let (device, _) = open_rm(Some(dev.pci))?;
        let drm = open_render_node(dev.pci)?;
        let space = AddressSpace::new(&device)?;
        let nvdec = NvdecChannel::new(&device, &space, class)?;
        let copy = CopyEngine::new(&device, &space)?;
        let mut side = Vec::new();
        codec.allocate(
            &mut SideAllocator {
                device: &device,
                space: &space,
                out: &mut side,
            },
            info.max_width,
            info.max_height,
        )?;
        let layout = codec.surface_layout(info.max_width, info.max_height);
        let count = codec.dpb_size() + 2 + info.extra_output_slots as usize;
        let mut surfaces = Vec::with_capacity(count);
        let mut surface_refs = Vec::with_capacity(count);
        for _ in 0..count {
            let s = vram(&device, &space, layout.size)?;
            surface_refs.push(SurfaceRef {
                luma: s.gpu.address(),
                chroma: s.gpu.address() + layout.chroma_offset,
            });
            surfaces.push(s);
        }
        let mut ring = Vec::with_capacity(RING_SLOTS);
        for _ in 0..RING_SLOTS {
            ring.push(RingSlot {
                desc: vram(&device, &space, DESCRIPTOR_BYTES)?,
                bits: None,
                bits_size: 0,
                busy_until: 0,
                unchecked: false,
                picture: 0,
            });
        }
        Ok(Self {
            codec,
            nvdec,
            copy,
            ring,
            next_ring: 0,
            surface_copy: vec![0; surfaces.len()],
            surface_hdr: vec![HdrStaticMetadata::default(); surfaces.len()],
            current_hdr: HdrStaticMetadata::default(),
            resize_needed: None,
            shown: 0,
            surfaces,
            surface_refs,
            layout,
            _side: side,
            pictures: HashMap::new(),
            display: VecDeque::new(),
            reorder: Vec::new(),
            next_id: 1,
            picture_index: 0,
            info,
            failed: None,
            stats: Stats::default(),
            drm,
            space,
            device,
        })
    }

    pub fn decoder_class(&self) -> u32 {
        self.nvdec.decoder_class
    }

    fn free_surfaces(&self) -> Vec<usize> {
        let referenced = self.codec.referenced();
        (0..self.surfaces.len())
            .filter(|i| !referenced.contains(i) && self.pictures.values().all(|p| p.surface != *i))
            .collect()
    }

    /// Read the engine status of a completed picture once (codecs with a
    /// `status_offset`). A non-zero error_status means the picture (and every
    /// picture predicted from it) is wrong: fail loudly, sticky until flush.
    fn check_status(&mut self, k: usize) -> VResult<()> {
        let Some(off) = self.codec.status_offset() else {
            return Ok(());
        };
        if !self.ring[k].unchecked {
            return Ok(());
        }
        self.ring[k].unchecked = false;
        let mut b = [0u8; 16];
        self.ring[k].desc.cpu.read_bytes(off, &mut b)?;
        if let Some(m) = engine_status_error(&b, self.ring[k].picture) {
            self.failed = Some(m.clone());
            return Err(VideoError::Bitstream(m));
        }
        Ok(())
    }

    /// Check every completed, unchecked picture (no CPU wait).
    fn poll_status(&mut self) -> VResult<()> {
        if let Some(m) = &self.failed {
            return Err(VideoError::Bitstream(m.clone()));
        }
        if self.codec.status_offset().is_none() {
            return Ok(());
        }
        let done = self.nvdec.completed()?;
        for k in 0..RING_SLOTS {
            if self.ring[k].unchecked && self.ring[k].busy_until <= done {
                self.check_status(k)?;
            }
        }
        Ok(())
    }

    /// Submit one unit. Returns the number of displayable pictures this unit produced
    /// (ABI v2): 0 for a unit the decoder drops or decodes without output (parameter sets
    /// only, a skipped RASL picture, `pic_output_flag = 0`) and for a drain call; a caller
    /// may therefore retire the input buffer as soon as a unit reports 0.
    /// Submit one unit; returns the displayable pictures it produced. After a
    /// `VideoError::Resize` only an end-of-stream drain (no data) is accepted.
    pub fn decode(&mut self, data: &[u8], timestamp: u64, flags: u32) -> VResult<u32> {
        if let Some((w, h)) = self.resize_needed {
            if !data.is_empty() {
                return Err(VideoError::Invalid(format!(
                    "the session needs re-creation at {w}x{h} (NV_VIDEO_E_RESIZE): only an \
                     end-of-stream drain is accepted"
                )));
            }
        }
        let r = self.decode_unit(data, timestamp, flags);
        if let Err(VideoError::Resize { width, height, .. }) = &r {
            self.resize_needed = Some((*width, *height));
        }
        r
    }

    /// The coded size the last `VideoError::Resize` asked for.
    pub fn required_size(&self) -> Option<(u32, u32)> {
        self.resize_needed
    }

    fn decode_unit(&mut self, data: &[u8], timestamp: u64, flags: u32) -> VResult<u32> {
        self.poll_status()?;
        let shown_before = self.shown;
        if !data.is_empty() {
            // HDR static metadata is stream syntax (SEI / metadata OBU), not engine
            // state: parse it before any decoder state changes and attach it to the
            // pictures this unit produces. Declarations persist: a stream usually
            // declares the mastering display / content light level once, and every later
            // picture is displayed with them until a new declaration replaces the group.
            let declared = super::hdr::parse_unit(self.info.codec, data)?;
            self.current_hdr.merge(declared);
            let hdr = self.current_hdr;
            let need = self.codec.surfaces_needed(data)?;
            let free = self.free_surfaces().len();
            let held: std::collections::HashSet<usize> =
                self.pictures.values().map(|p| p.surface).collect();
            if !unit_admitted(
                need,
                free,
                self.codec.max_referenced(),
                held.len(),
                self.surfaces.len(),
            ) {
                return Err(VideoError::Again(format!(
                    "unit needs {need} free surfaces, {free} free: release pictures first"
                )));
            }
            self.codec.push_unit(data)?;
            while let Some(job) = self.codec.next_job()? {
                match job {
                    Job::ShowExisting { surface, info } => {
                        let decoded_at = self
                            .pictures
                            .values()
                            .filter(|p| p.surface == surface)
                            .map(|p| p.decoded_at)
                            .max()
                            .unwrap_or(self.nvdec.submitted);
                        let hdr = self.surface_hdr[surface];
                        self.emit(surface, info, timestamp, decoded_at, None, hdr);
                    }
                    Job::Decode(job) => self.decode_job(job, timestamp, hdr)?,
                }
            }
        }
        if flags & 1 != 0 {
            self.reorder.sort_unstable();
            for (_, id) in std::mem::take(&mut self.reorder) {
                self.display.push_back(id);
            }
        }
        u32::try_from(self.shown - shown_before)
            .map_err(|_| VideoError::Invalid("unit produced more than 2^32 pictures".into()))
    }

    fn emit(
        &mut self,
        surface: usize,
        info: PictureInfo,
        timestamp: u64,
        decoded_at: u64,
        key: Option<i64>,
        hdr: HdrStaticMetadata,
    ) {
        let id = self.next_id;
        self.next_id += 1;
        self.shown += 1;
        self.pictures.insert(
            id,
            Picture {
                surface,
                info,
                timestamp,
                decoded_at,
                hdr,
            },
        );
        match key {
            None => self.display.push_back(id),
            Some(k) => {
                self.reorder.push((k, id));
                self.reorder.sort_unstable();
                while !self.reorder.is_empty() && self.codec.bump_ready(self.reorder.len()) {
                    let (_, id) = self.reorder.remove(0);
                    self.display.push_back(id);
                }
            }
        }
    }

    fn decode_job(
        &mut self,
        job: DecodeJob,
        timestamp: u64,
        hdr: HdrStaticMetadata,
    ) -> VResult<()> {
        let current = *self
            .free_surfaces()
            .first()
            // The unit was admitted by `decode`; running out now is an
            // accounting bug (the codec already consumed state), not AGAIN.
            .ok_or_else(|| {
                VideoError::Invalid("no free DPB surface inside an admitted unit".into())
            })?;
        // A copy_out may still read this surface (picture released early).
        if self.surface_copy[current] != 0 && !self.copy.retired(self.surface_copy[current])? {
            let fd = self.copy.queue.context_fence(
                std::os::fd::AsRawFd::as_raw_fd(&self.drm),
                self.surface_copy[current],
            )?;
            wait_sync_file(std::os::fd::AsRawFd::as_raw_fd(&fd), Duration::from_secs(2))
                .map_err(VideoError::DeviceLost)?;
        }
        let k = self.next_ring;
        self.next_ring = (k + 1) % RING_SLOTS;
        // Ring slot reuse: the picture that last used it must be done.
        if self.ring[k].busy_until > self.nvdec.completed()? {
            self.stats.cpu_waits += 1;
            self.nvdec.wait(&self.drm, self.ring[k].busy_until)?;
        }
        self.check_status(k)?;
        let len = job.bitstream.len();
        if self.ring[k].bits.is_none() || len + 256 > self.ring[k].bits_size {
            // Stock growth rule (common.rs stage_bitstream_141d50).
            let size = BITSTREAM_INITIAL.max((len + 0x8ff) & !0xff);
            self.ring[k].bits = None;
            self.ring[k].bits = Some(vram(&self.device, &self.space, size)?);
            self.ring[k].bits_size = size;
        }
        let ctx = BuildContext {
            decoder_class: self.nvdec.decoder_class,
            host_class: self.nvdec.host_class,
            descriptor: self.ring[k].desc.gpu.address(),
            bitstream: self.ring[k].bits.as_ref().map_or(0, |b| b.gpu.address()),
            bitstream_len: len,
            picture_index: self.picture_index,
            current,
            surfaces: &self.surface_refs,
            layout: self.layout,
        };
        let mut desc = vec![0u8; DESCRIPTOR_BYTES];
        let words = self.codec.build(&job, &ctx, &mut desc)?;
        {
            let slot = &mut self.ring[k];
            slot.desc.cpu.write_bytes(0, &desc)?;
            let bits = slot.bits.as_mut().expect("allocated above");
            bits.cpu.write_bytes(0, &job.bitstream)?;
            bits.cpu.write_bytes(len, &[0u8; 256])?;
        }
        let value = self.nvdec.submit(words)?;
        self.ring[k].busy_until = value;
        self.ring[k].unchecked = self.codec.status_offset().is_some();
        self.ring[k].picture = self.picture_index;
        self.picture_index = self.picture_index.wrapping_add(1);
        self.stats.decoded += 1;
        let readback = match &job.readback {
            Some(r) => {
                self.stats.cpu_waits += 1;
                self.nvdec.wait(&self.drm, value)?;
                let mut out = vec![0u8; r.len()];
                self.ring[k].desc.cpu.read_bytes(r.start, &mut out)?;
                Some(out)
            }
            None => None,
        };
        self.codec.finish(&job, current, readback.as_deref())?;
        // Keep the unit's metadata on the surface even when the picture is hidden: a
        // later show_existing_frame outputs the metadata of the frame it shows.
        self.surface_hdr[current] = hdr;
        if job.show {
            self.emit(current, job.info, timestamp, value, job.display_key, hdr);
        }
        Ok(())
    }

    /// Next picture in display order (its decode may still run).
    pub fn next_picture(&mut self) -> Option<(u64, u64, PictureInfo)> {
        let id = self.display.pop_front()?;
        let p = &self.pictures[&id];
        Some((id, p.timestamp, p.info))
    }

    /// HDR static metadata of picture `id` (ABI v2). A picture whose unit declared none
    /// returns the empty value; an unknown id is a caller error.
    pub fn picture_hdr(&self, id: u64) -> VResult<HdrStaticMetadata> {
        self.pictures
            .get(&id)
            .map(|p| p.hdr)
            .ok_or_else(|| VideoError::Invalid(format!("unknown picture id {id}")))
    }

    pub fn release(&mut self, id: u64) -> VResult<()> {
        self.display.retain(|d| *d != id);
        self.pictures
            .remove(&id)
            .map(|_| ())
            .ok_or_else(|| VideoError::Invalid(format!("unknown picture id {id}")))
    }

    pub fn flush(&mut self) -> VResult<()> {
        self.codec.flush();
        self.failed = None;
        self.resize_needed = None;
        // A seek may land in a different grade: the next declaration replaces it.
        self.current_hdr = HdrStaticMetadata::default();
        for id in std::mem::take(&mut self.display) {
            self.pictures.remove(&id);
        }
        for (_, id) in std::mem::take(&mut self.reorder) {
            self.pictures.remove(&id);
        }
        Ok(())
    }

    /// Wait for all NVDEC work (session teardown, tests).
    pub fn idle(&mut self) -> VResult<()> {
        self.nvdec.wait(&self.drm, self.nvdec.submitted)
    }

    pub fn copy_out(&mut self, id: u64, t: &Target) -> VResult<Option<OwnedFd>> {
        self.copy.reap()?;
        let pic = self
            .pictures
            .get(&id)
            .ok_or_else(|| VideoError::Invalid(format!("unknown picture id {id}")))?;
        let info = pic.info;
        let surface = pic.surface;
        let decoded_at = pic.decoded_at;
        // Never hand out a picture whose engine status is unknown: wait for its
        // decode (CPU) if that is still unchecked, then check every completed
        // picture (codecs with a status_offset).
        if (0..RING_SLOTS).any(|k| self.ring[k].unchecked && self.ring[k].busy_until == decoded_at)
        {
            self.stats.cpu_waits += 1;
            self.nvdec.wait(&self.drm, decoded_at)?;
        }
        self.poll_status()?;
        // The engine writes 10-bit pictures in 16-bit containers with the sample in the 10 MSBs
        // (P010 bit layout, measured: the copy of the engine surface is bit-exact against FFmpeg's
        // p010le), so P010 is a plain copy like NV12; no depth conversion in either direction.
        let bps =
            match (t.format, info.bit_depth) {
                (FORMAT_NV12 | FORMAT_YUV420P, 8) => 1u32,
                (FORMAT_P010, 10) => 2u32,
                (FORMAT_YUV420P, d) => {
                    return Err(VideoError::Unsupported(format!(
                        "planar YUV 4:2:0 target for a {d}-bit picture (8-bit only)"
                    )))
                }
                (FORMAT_NV12 | FORMAT_P010, d) => {
                    return Err(VideoError::Unsupported(format!(
                    "{} target for a {d}-bit picture (NV12 carries 8-bit, P010 10-bit pictures; \
                     the copy engine does not convert depth)",
                    if t.format == FORMAT_NV12 { "NV12" } else { "P010" }
                )))
                }
                (f, _) => return Err(VideoError::Invalid(format!("unknown target format {f}"))),
            };
        if bps != self.layout.bytes_per_sample {
            return Err(VideoError::Invalid(format!(
                "{}-bit picture on {}-byte engine surfaces",
                info.bit_depth, self.layout.bytes_per_sample
            )));
        }
        let [cl, ct, cw, ch] = info.crop;
        if t.width < cw || t.height < ch {
            return Err(VideoError::Invalid(format!(
                "target {}x{} smaller than the display window {cw}x{ch}",
                t.width, t.height
            )));
        }
        let dst = TargetLayout::parse(t, self.copy.copy_class, bps)?;
        // Planes written: Y + CbCr (NV12/P010), or Y + Cb + Cr (planar).
        let planes = dst.planes();
        // Import (fds stay the caller's; RM keeps its own reference). Planes that share a
        // dma-buf share its import.
        let mut imports: Vec<(Rc<Memory>, GpuMapping)> = Vec::new();
        let mut import_of = [0usize; 3];
        let mut plane_va = [0u64; 3];
        #[allow(clippy::needless_range_loop)] // p indexes fd, offset and plane_va together
        for p in 0..planes {
            let fd = t.dmabuf_fd[p];
            if fd < 0 {
                return Err(VideoError::Invalid(format!("plane {p} dma-buf fd {fd}")));
            }
            match (0..p).find(|&q| t.dmabuf_fd[q] == fd) {
                Some(q) => import_of[p] = import_of[q],
                None => {
                    // SAFETY: the caller lends a live fd for the duration of the call.
                    let borrowed = unsafe { BorrowedFd::borrow_raw(fd) };
                    let mem = crate::external::import(&self.device, self.drm.as_fd(), borrowed)
                        .map_err(|e| {
                            VideoError::Invalid(format!("dma-buf import plane {p}: {e}"))
                        })?;
                    let va = self.space.map(&mem)?;
                    imports.push((mem, va));
                    import_of[p] = imports.len() - 1;
                }
            }
            let (mem, va) = &imports[import_of[p]];
            let rows = if p == 0 { ch } else { ch.div_ceil(2) };
            let need = t.offset[p] + dst.plane_bytes(p, rows, cw, bps);
            if need > mem.size as u64 {
                return Err(VideoError::Invalid(format!(
                    "plane {p} needs {need} bytes, dma-buf has {}",
                    mem.size
                )));
            }
            plane_va[p] = va.address() + t.offset[p];
        }
        // The target must be free (acquire fence, an arbitrary dma-fence sync_file from
        // the caller). Waited on the CPU: one poll(2) on the fd, which returns immediately
        // when the producer already signalled it (measured: see video/README.md
        // "copy_out"). A GPU-side wait needs the sync_file imported as an RM semaphore
        // (dma-fence -> semaphore), which this backend does not implement, and stock has
        // no equivalent to follow (libnvcuvid has no fence-fd API; its consumers
        // synchronise through CUDA stream semantics), so no GPU method is invented here.
        if t.acquire_fence_fd >= 0 {
            wait_sync_file(t.acquire_fence_fd, Duration::from_secs(5))
                .map_err(|e| VideoError::Invalid(format!("acquire fence: {e}")))?;
        }
        let src_kinds = self.codec.gob_kinds(self.nvdec.decoder_class);
        let blackwell_ce = self.copy.copy_class >= 0xc9b5;
        let mut w = Vec::new();
        method(&mut w, 4, 0, &[self.copy.copy_class]); // SET_OBJECT
        #[allow(clippy::needless_range_loop)] // plane index selects several per-plane values
        for p in 0..planes {
            let src = self.surface_refs[surface];
            // Planar chroma: the engine's CbCr plane is split by component remapping
            // (SET_REMAP_COMPONENTS, LAUNCH_DMA REMAP_ENABLE; cl90b5.h). With remapping on,
            // X dimensions (source width, origin, line length) count elements of
            // NUM_SRC_COMPONENTS * COMPONENT_SIZE bytes, pitches stay in bytes, as NVK's
            // nouveau_copy_rect programs them (Mesa 26.2.5 nvk_cmd_copy.c:195-215, 257, 286, 296,
            // 368). Element = one CbCr pair (2 x 1 byte); Cb is component X, Cr component Y.
            let remap = (p > 0 && planes == 3).then_some(if p == 1 { 0u32 } else { 1u32 });
            let (src_va, rows_alloc, rows, line_bytes, ox, oy) = if p == 0 {
                (src.luma, self.layout.luma_rows, ch, cw * bps, cl * bps, ct)
            } else if remap.is_some() {
                (
                    src.chroma,
                    self.layout.chroma_rows,
                    ch.div_ceil(2),
                    cw.div_ceil(2),
                    cl / 2,
                    ct / 2,
                )
            } else {
                (
                    src.chroma,
                    self.layout.chroma_rows,
                    ch.div_ceil(2),
                    cw.div_ceil(2) * 2 * bps,
                    cl / 2 * 2 * bps,
                    ct / 2,
                )
            };
            let src_width = if remap.is_some() {
                self.layout.pitch / 2
            } else {
                self.layout.pitch
            };
            // Source: engine block linear, 1-GOB wide, 2-GOB high blocks.
            let gob_kind = if blackwell_ce {
                src_kinds[p.min(1)] << 16
            } else {
                0
            };
            method(&mut w, 4, 0x728, &[1 << 4 | 1 << 12 | gob_kind]); // SET_SRC_BLOCK_SIZE
            method(&mut w, 4, 0x72c, &[src_width, rows_alloc, 1, 0]); // SRC W/H/D/LAYER
            method(&mut w, 4, 0x744, &[ox, oy]); // SRC_ORIGIN_X/Y
            let mut launch = 2 | 1 << 2 | 1 << 9; // non-pipelined, flush, multi-line
            if let Some(component) = remap {
                // DST_X = SRC_X (Cb) or SRC_Y (Cr), DST_Y/Z/W = NO_WRITE (6), COMPONENT_SIZE ONE,
                // NUM_SRC_COMPONENTS TWO, NUM_DST_COMPONENTS ONE.
                method(
                    &mut w,
                    4,
                    0x708,
                    &[component | 6 << 4 | 6 << 8 | 6 << 12 | 1 << 20],
                );
                launch |= 1 << 10; // REMAP_ENABLE
            }
            match dst.block {
                None => launch |= 1 << 8, // DST pitch
                Some((h, kinds)) => {
                    let dk = if blackwell_ce {
                        kinds[p.min(1)] << 16
                    } else {
                        0
                    };
                    method(&mut w, 4, 0x70c, &[h << 4 | 1 << 12 | dk]); // SET_DST_BLOCK_SIZE
                    let dst_rows = rows.next_multiple_of(8 << h);
                    method(
                        &mut w,
                        4,
                        0x710,
                        &[line_bytes.next_multiple_of(64), dst_rows, 1, 0],
                    );
                    method(&mut w, 4, 0x74c, &[0, 0]); // DST_ORIGIN_X/Y
                }
            }
            let [ih, il] = crate::gpu::hi_lo(src_va);
            let [oh, ol] = crate::gpu::hi_lo(plane_va[p]);
            method(&mut w, 4, 0x400, &[ih, il, oh, ol]);
            method(
                &mut w,
                4,
                0x410,
                &[self.layout.pitch, t.pitch[p], line_bytes, rows],
            );
            method(&mut w, 4, 0x300, &[launch]);
        }
        // Command buffer: reuse one whose copy retired.
        let ci = match self.copy.commands.iter().position(|(_, tok)| {
            *tok == 0 || self.copy.queue.shared().retired(*tok).unwrap_or(false)
        }) {
            Some(i) => i,
            None => {
                self.copy
                    .commands
                    .push((vram(&self.device, &self.space, 4096)?, 0));
                self.copy.commands.len() - 1
            }
        };
        let cmd = &mut self.copy.commands[ci].0;
        for (i, word) in w.iter().enumerate() {
            cmd.cpu.write_u32(i * 4, *word)?;
        }
        crate::platform::device_full_barrier();
        let push = Push {
            address: cmd.gpu.address(),
            size: (w.len() * 4) as u32,
            flags: 0,
        };
        // GPU-side ordering after the decode: acquire the NVDEC timeline.
        let mut waits = Vec::new();
        if self.nvdec.completed()? < decoded_at {
            if self.nvdec.timeline.native64() {
                waits.push(SemaphoreOp::AcquireTimeline(
                    self.nvdec.timeline.address(),
                    decoded_at,
                ));
            } else {
                self.nvdec.wait(&self.drm, decoded_at)?;
            }
        }
        let token = match self
            .copy
            .queue
            .submit(&[push], &self.space, &waits, &[], &[], |_| Ok(()))?
        {
            SubmitStatus::Submitted(t) => t,
            SubmitStatus::Full(_) => {
                return Err(VideoError::Again("copy queue full".into()));
            }
        };
        self.copy.commands[ci].1 = token;
        self.surface_copy[surface] = token;
        self.copy.pending.push_back((token, imports));
        if self.copy.retired(token)? {
            return Ok(None);
        }
        let fd = self
            .copy
            .queue
            .context_fence(std::os::fd::AsRawFd::as_raw_fd(&self.drm), token)?;
        Ok(Some(fd))
    }

    pub fn layout(&self) -> SurfaceLayout {
        self.layout
    }
    pub fn info(&self) -> &SessionInfo {
        &self.info
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // Let in-flight work finish before memory goes away (bounded).
        let _ = self.nvdec.wait(&self.drm, self.nvdec.submitted);
        let last = self.copy.queue.last_token();
        if last != 0 {
            if let Ok(fd) = self
                .copy
                .queue
                .context_fence(std::os::fd::AsRawFd::as_raw_fd(&self.drm), last)
            {
                let _ =
                    wait_sync_file(std::os::fd::AsRawFd::as_raw_fd(&fd), Duration::from_secs(2));
            }
        }
    }
}

/// Target description (NvVideoTarget, fds borrowed).
pub struct Target {
    /// Y, CbCr (NV12/P010) or Y, Cb, Cr (FORMAT_YUV420P); [2] unused for the bi-planar formats.
    pub dmabuf_fd: [i32; 3],
    pub offset: [u64; 3],
    pub pitch: [u32; 3],
    pub modifier: u64,
    pub format: u32,
    pub width: u32,
    pub height: u32,
    pub acquire_fence_fd: i32,
}

/// Decoded destination layout: pitch, or block linear with log2 block height
/// and per-plane copy-engine GOB kinds.
#[derive(Debug)]
struct TargetLayout {
    block: Option<(u32, [u32; 2])>,
    pitch: [u32; 3],
    /// Y + Cb + Cr planes (planar 4:2:0) instead of Y + CbCr.
    planar: bool,
}

const DRM_FORMAT_MOD_LINEAR: u64 = 0;
const VENDOR_NVIDIA: u64 = 0x03;

impl TargetLayout {
    /// DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(c, s, g, k, h) (drm_fourcc.h):
    /// 0x10 | h[3:0] | k[19:12] | g[21:20] | s[22] | c[25:23].
    fn parse(t: &Target, copy_class: u32, bytes_per_sample: u32) -> VResult<Self> {
        let planar = t.format == FORMAT_YUV420P;
        if planar && t.modifier != DRM_FORMAT_MOD_LINEAR {
            return Err(VideoError::Unsupported(format!(
                "planar YUV 4:2:0 target with modifier {:#x} (LINEAR only)",
                t.modifier
            )));
        }
        if t.modifier == DRM_FORMAT_MOD_LINEAR {
            for p in 0..if planar { 3 } else { 2 } {
                if t.pitch[p] == 0 || !t.pitch[p].is_multiple_of(4) {
                    return Err(VideoError::Invalid(format!(
                        "LINEAR plane {p} pitch {}",
                        t.pitch[p]
                    )));
                }
            }
            return Ok(Self {
                block: None,
                pitch: t.pitch,
                planar,
            });
        }
        // The 2-byte-sample (P010) block-linear destination write is derived, not measured:
        // the destination GOB layout the copy engine produces for 16-bit luma / 32-bit CbCr
        // has never been read back and compared on hardware. Refuse it rather than write a
        // possibly wrong layout; LINEAR P010 is bit-exact (host test) and NV12 block-linear
        // (1-byte samples) is measured.
        if bytes_per_sample != 1 {
            return Err(VideoError::Unsupported(format!(
                "P010 block-linear target (modifier {:#x}): the copy engine's 16-bit \
                 block-linear write is not verified end to end; use a LINEAR P010 target",
                t.modifier
            )));
        }
        let v = t.modifier & ((1 << 56) - 1);
        if t.modifier >> 56 != VENDOR_NVIDIA || v & 0x10 == 0 || v >> 26 != 0 {
            return Err(VideoError::Unsupported(format!(
                "modifier {:#x}: only LINEAR and NVIDIA 2D block-linear",
                t.modifier
            )));
        }
        let h = (v & 0xf) as u32;
        let g = ((v >> 20) & 3) as u32;
        let c = ((v >> 23) & 7) as u32;
        if c != 0 || h > 5 {
            return Err(VideoError::Unsupported(format!(
                "modifier {:#x}: compressed or block height > 32 GOBs",
                t.modifier
            )));
        }
        // g = 3: Blackwell GOBs whose layout depends on bytes per element:
        // NV12 luma 1 B -> GOB_KIND_8, CbCr 2 B -> GOB_KIND_16; P010 luma 2 B -> GOB_KIND_16,
        // CbCr 4 B -> GOB_KIND_32 (0), the same per-plane kinds the engine writes.
        let kinds = match g {
            3 if copy_class >= 0xc9b5 => super::blackwell_gob_kinds(bytes_per_sample),
            2 => [0, 0],
            _ => {
                return Err(VideoError::Unsupported(format!(
                    "modifier {:#x}: GOB generation {g} not writable by copy class {copy_class:#x}",
                    t.modifier
                )))
            }
        };
        Ok(Self {
            block: Some((h, kinds)),
            pitch: [0, 0, 0],
            planar: false,
        })
    }
    fn planes(&self) -> usize {
        if self.planar {
            3
        } else {
            2
        }
    }
    fn plane_bytes(&self, p: usize, rows: u32, width: u32, bps: u32) -> u64 {
        let line = if p == 0 {
            width * bps
        } else if self.planar {
            width.div_ceil(2) * bps
        } else {
            width.div_ceil(2) * 2 * bps
        };
        match self.block {
            None => u64::from(self.pitch[p]) * u64::from(rows.saturating_sub(1)) + u64::from(line),
            Some((h, _)) => {
                u64::from(line.next_multiple_of(64)) * u64::from(rows.next_multiple_of(8 << h))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn target(modifier: u64) -> Target {
        Target {
            dmabuf_fd: [3, 3, -1],
            offset: [0, 0, 0],
            pitch: [256, 256, 0],
            modifier,
            format: FORMAT_NV12,
            width: 64,
            height: 64,
            acquire_fence_fd: -1,
        }
    }
    #[test]
    fn modifiers() {
        let bl = |c: u64, s: u64, g: u64, k: u64, h: u64| {
            0x03 << 56 | 0x10 | h | k << 12 | g << 20 | s << 22 | c << 23
        };
        assert!(TargetLayout::parse(&target(0), 0xcab5, 1)
            .unwrap()
            .block
            .is_none());
        let t = TargetLayout::parse(&target(bl(0, 1, 3, 0xfe, 4)), 0xcab5, 1).unwrap();
        assert_eq!(t.block, Some((4, [1, 2])));
        // P010 (2-byte samples) block-linear: verified end to end nowhere, refused with the
        // reason on both GOB generations instead of writing a guessed layout.
        for modif in [bl(0, 1, 3, 0xfe, 4), bl(0, 1, 2, 0xfe, 1)] {
            let e = TargetLayout::parse(&target(modif), 0xcab5, 2).unwrap_err();
            assert!(
                e.message().contains("P010 block-linear") && e.message().contains("not verified"),
                "{}",
                e.message()
            );
            assert_eq!(e.code(), -2); // NV_VIDEO_E_UNSUPPORTED
        }
        let t = TargetLayout::parse(&target(bl(0, 1, 2, 0xfe, 1)), 0xc7b5, 1).unwrap();
        assert_eq!(t.block, Some((1, [0, 0])));
        assert!(TargetLayout::parse(&target(bl(1, 1, 2, 0xfe, 1)), 0xcab5, 1).is_err()); // compressed
        assert!(TargetLayout::parse(&target(bl(0, 1, 3, 0xfe, 1)), 0xc7b5, 1).is_err()); // g=3 pre-Blackwell
        assert!(TargetLayout::parse(&target(0x01 << 56 | 1), 0xcab5, 1).is_err()); // other vendor
        assert_eq!(
            TargetLayout::parse(&target(0), 0xcab5, 1)
                .unwrap()
                .plane_bytes(1, 32, 63, 1),
            256 * 31 + 64
        );
        // Planar 4:2:0: LINEAR only, three pitched planes, chroma rows of width/2 samples.
        let planar = |modifier, pitch: [u32; 3]| Target {
            dmabuf_fd: [3, 3, 3],
            offset: [0, 0, 0],
            pitch,
            modifier,
            format: FORMAT_YUV420P,
            width: 64,
            height: 64,
            acquire_fence_fd: -1,
        };
        let t = TargetLayout::parse(&planar(0, [128, 64, 64]), 0xcab5, 1).unwrap();
        assert_eq!((t.planes(), t.plane_bytes(2, 32, 63, 1)), (3, 64 * 31 + 32));
        assert!(TargetLayout::parse(&planar(0, [128, 64, 0]), 0xcab5, 1).is_err()); // no Cr pitch
        assert!(TargetLayout::parse(&planar(bl(0, 1, 3, 0xfe, 4), [0; 3]), 0xcab5, 1).is_err());
    }
    #[test]
    fn admission_never_refuses_an_idle_caller() {
        // AV1: 8 references + the picture in flight fit the 10-surface pool
        // (dpb_size 8 + 2, no extra slots) even when 6 hidden frames arrive
        // with only 2 free surfaces (gallery av1_aom: "needs 6, 4 free").
        assert!(unit_admitted(6, 2, Some(8), 0, 10));
        assert!(unit_admitted(6, 4, Some(8), 0, 12));
        assert!(unit_admitted(6, 4, Some(8), 3, 12));
        assert!(!unit_admitted(6, 4, Some(8), 4, 12));
        // Codecs without the bound keep the plain check.
        assert!(!unit_admitted(2, 1, None, 0, 12));
        assert!(unit_admitted(2, 2, None, 5, 12));
    }

    #[test]
    fn engine_status_error_word3() {
        let mut s = [0u8; 16];
        s[0] = 0x40; // mbs_correctly_decoded
        assert!(engine_status_error(&s, 3).is_none());
        s[4] = 7; // mbs_in_error
        s[12] = 0xd; // error_status
        let m = engine_status_error(&s, 3).expect("error");
        assert!(
            m.contains("error_status 0xd") && m.contains("7 blocks") && m.contains("picture 3")
        );
    }

    /// The ABI v2 "this unit produced a picture" signal is 0 exactly when the codec hands
    /// the session no displayable job. A unit that carries only parameter sets is the
    /// simplest such unit (a skipped RASL picture and `pic_output_flag = 0` are the
    /// stream cases, see `skipped_rasl_units_produce_no_picture`).
    #[test]
    fn parameter_set_unit_produces_no_picture() {
        use super::super::{bits, pretend, CODEC_HEVC};
        let stream: &[u8] = include_bytes!("testdata/hevc_160x144.hevc");
        let aus = bits::access_units(stream, true);
        let mut ps = Vec::new();
        for n in bits::split_annexb(aus[0]) {
            if (32..=34).contains(&((n.nal[0] >> 1) & 0x3f)) {
                ps.extend_from_slice(&[0, 0, 1]);
                ps.extend_from_slice(n.nal);
            }
        }
        assert!(!ps.is_empty(), "no parameter sets in the bundled stream");
        let out = pretend::run_units(CODEC_HEVC, 0xcfb0, 0xc96f, &ps, (160, 144), 8).unwrap();
        assert_eq!(out.len(), 1);
        assert!(
            out[0].is_empty(),
            "parameter-set unit built {} pictures",
            out[0].len()
        );
        let first = pretend::run(CODEC_HEVC, 0xcfb0, 0xc96f, aus[0], (160, 144), 8).unwrap();
        assert_eq!(first.len(), 1, "the first access unit decodes one picture");
    }

    /// Random access at a CRA: the RASL pictures that follow it are skipped (HEVC 8.1.3,
    /// NoRaslOutputFlag), and exactly those units produce no picture — the C2 core relies on
    /// this signal to retire their input instead of holding them until EOS/flush.
    #[test]
    #[ignore = "needs NVDEC_STOCK_EVIDENCE (streams/x265-opengop.hevc)"]
    fn skipped_rasl_units_produce_no_picture() {
        use super::super::{bits, pretend, CODEC_HEVC};
        let dir = std::env::var("NVDEC_STOCK_EVIDENCE")
            .expect("NVDEC_STOCK_EVIDENCE=<stock captures dir>");
        let stream = std::fs::read(format!("{dir}/streams/x265-opengop.hevc")).unwrap();
        let aus = bits::access_units(&stream, true);
        let types = |a: &[u8]| -> Vec<u32> {
            bits::split_annexb(a)
                .into_iter()
                .map(|n| u32::from((n.nal[0] >> 1) & 0x3f))
                .collect()
        };
        let cra: Vec<usize> = aus
            .iter()
            .enumerate()
            .filter(|(_, a)| types(a).contains(&21))
            .map(|(i, _)| i)
            .collect();
        let start = cra[1];
        let mut data = Vec::new();
        for n in bits::split_annexb(aus[0]) {
            if (32..=34).contains(&((n.nal[0] >> 1) & 0x3f)) {
                data.extend_from_slice(&[0, 0, 1]);
                data.extend_from_slice(n.nal);
            }
        }
        data.extend(aus[start..].iter().flat_map(|a| a.iter().copied()));
        let out = pretend::run_units(CODEC_HEVC, 0xcfb0, 0xc96f, &data, (176, 144), 8).unwrap();
        assert_eq!(out.len(), aus.len() - start);
        // Only the leading pictures of the random-access CRA are skipped (8.1.3); RASL
        // pictures after the next CRA decode normally (NoRaslOutputFlag = 0).
        let is_rasl = |a: &[u8]| types(a).iter().any(|&x| x == 8 || x == 9);
        let lead = cra[2] - start;
        let rasl_lead = aus[start..cra[2]].iter().filter(|a| is_rasl(a)).count();
        assert!(rasl_lead > 0, "stream has no RASL pictures after the CRA");
        let empty = out[..lead].iter().filter(|p| p.is_empty()).count();
        assert_eq!(
            empty, rasl_lead,
            "skipped units must be exactly the leading RASLs"
        );
        for (i, pics) in out.iter().enumerate() {
            if i < lead && is_rasl(aus[start + i]) {
                continue;
            }
            assert_eq!(
                pics.len(),
                1,
                "unit {i} (types {:?}) must decode one picture",
                types(aus[start + i])
            );
        }
    }

    /// A 10-bit H.264 session is refused with the reason (no 16-bit surfaces are ever
    /// allocated for it); the 8-bit path and the codecs that do take a depth are unaffected.
    #[test]
    fn h264_high_bit_depth_is_refused_by_name() {
        let h264_mask = 1 << 8;
        let m = bit_depth_refusal(super::super::CODEC_H264, 10, h264_mask).expect("refused");
        assert!(m.contains("High-10") && m.contains("no High-10"), "{m}");
        assert!(bit_depth_refusal(super::super::CODEC_H264, 8, h264_mask).is_none());
        let hevc_mask = 1 << 8 | 1 << 10;
        assert!(bit_depth_refusal(super::super::CODEC_HEVC, 10, hevc_mask).is_none());
        let m = bit_depth_refusal(super::super::CODEC_HEVC, 12, hevc_mask).expect("refused");
        assert!(!m.contains("High-10"), "{m}");
        assert!(bit_depth_refusal(super::super::CODEC_VP9, 32, 1 << 8).is_some());
    }

    #[test]
    fn surface_layout_constant_pitch() {
        let l = SurfaceLayout::new(1928, 1208, 1);
        assert_eq!(l.pitch, 1984);
        assert_eq!(l.luma_rows, 1216);
        assert_eq!(l.chroma_offset, 1984 * 1216);
        assert!(l.size >= l.chroma_offset as usize + 1984 * 640);
    }
}
