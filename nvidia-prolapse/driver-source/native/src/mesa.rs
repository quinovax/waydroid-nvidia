//! Mesa nvkmd -> Rust RM bridge, executed entirely on the calling threads.
//! The C ABI is documented in mesa/nv_rm.h.
//!
//! There is no ordinary-path worker. Submission, status, reset, host signal,
//! CPU waits, export/import, retirement and loss checks run on the API caller
//! (stock 615 does every per-frame synchronization/submission operation on
//! the caller, as traced in the stock binary). The single
//! optional thread is the lazily started foreign-fence watcher (watcher.rs).
//!
//! Ownership model:
//! * `Core` (Arc, Send+Sync): loss flag, slot pool addresses, the waiter group,
//!   the watcher, the shared-payload broadcast port and the device RM event
//!   descriptor numbers. Plain data and atomics only.
//! * `Inner` (behind one Mutex): every Rc/RefCell RM owner (client, device,
//!   address space, channels, buffers, mappings, event sources, pool pages).
//!   See `Confined` for the Send argument. Lock order: Inner -> SyncObj state
//!   -> {Shared local, slot free list, watcher state, waiter condvar}. Nothing
//!   blocks on GPU progress or a foreign fence while holding Inner.
//! * Sync payloads (`SyncObj`, Arc): the C id is the raw Arc pointer; their
//!   mapped slots are read/written without Inner.
#![allow(clippy::missing_safety_doc)]
use crate::diag::diag;
use crate::{
    abi, capabilities,
    events::{HostListener, RmEvents, TriggerPort},
    gpu::{AddressSpace, GpuMapping, Push, Queue, QueueShared, SemaphoreOp, SubmitStatus},
    reclaim::Reclaim,
    rm::{Client, Device, Error, Mapping, Memory, Result},
    slots::{Slot, SlotPool, TimelineView, PAGE_BYTES},
    syncobj::{Foreign, Shared, SignalPlan, SyncObj, WaitPlan},
    waiters::Waiters,
    watcher::Watcher,
};
use std::{
    collections::{HashMap, VecDeque},
    ffi::c_void,
    os::fd::{AsRawFd, BorrowedFd, IntoRawFd, OwnedFd, RawFd},
    rc::Rc,
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
        Arc, Mutex, MutexGuard,
    },
    time::{Duration, Instant},
};
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Info {
    vendor_id: u32,
    device_id: u32,
    chipset: u32,
    sm: u32,
    pci_domain: u32,
    pci_bus: u32,
    pci_device: u32,
    pci_function: u32,
    gpc_count: u32,
    tpc_count: u32,
    mp_per_tpc: u32,
    max_warps_per_mp: u32,
    cls_copy: u32,
    cls_eng2d: u32,
    cls_eng3d: u32,
    cls_m2mf: u32,
    cls_compute: u32,
    generic_kind: u32,
    vram_size: u64,
    bar_size: u64,
    name: [u8; 64],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Allocation {
    id: u64,
    size: u64,
    cpu_address: usize,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SyncPoint {
    sync: u64,
    value: u64,
}

/// Thread-shareable device state (see module doc).
pub(crate) struct Core {
    pub(crate) pool: Arc<SlotPool>,
    pub(crate) waiters: Waiters,
    pub(crate) watcher: Watcher,
    pub(crate) trigger: TriggerPort,
    lost: AtomicBool,
    /// CPU waiters using VK_SYNC_WAIT_PENDING: only they observe submission
    /// (not completion) state, so only they need a wake from submit.
    pending_waiters: std::sync::atomic::AtomicU32,
    /// Nanoseconds since `epoch` of the last heavy channel-error scan.
    last_scan: AtomicU64,
    /// Deferred-reclamation groups waiting in Inner (lock-free hint so
    /// status/wait paths only try the lock when there is something to do).
    reclaim_pending: AtomicUsize,
    epoch: Instant,
    /// (HOST completion, channel RC) RM OS-event descriptors, owned by Inner.
    devices: (RawFd, RawFd),
}
impl Core {
    pub(crate) fn is_lost(&self) -> bool {
        self.lost.load(Ordering::Acquire)
    }
    /// Publish loss and wake everyone. CPU-only: atomics, a condvar notify and
    /// eventfd writes; safe from Mesa's loss callback under queue locks. The
    /// first publication logs where it came from (logcat on Android).
    #[track_caller]
    pub(crate) fn mark_lost(&self) {
        if !self.lost.swap(true, Ordering::SeqCst) {
            diag!(
                "nvk Rust RM: device lost, published at {}",
                std::panic::Location::caller()
            );
            self.waiters.broadcast();
            self.watcher.wake();
        }
    }
    fn live(&self) -> Result<()> {
        if self.is_lost() {
            Err(Error::DeviceLost("Mesa RM device lost".into()))
        } else {
            Ok(())
        }
    }
    /// Claim the 1 s heavy-scan slot (stock throttles its notifier scan the
    /// same way: one vDSO clock read + cached words per check).
    fn scan_due(&self, force: bool) -> bool {
        let now = self.epoch.elapsed().as_nanos() as u64;
        let last = self.last_scan.load(Ordering::Relaxed);
        (force || now.saturating_sub(last) >= 1_000_000_000)
            && self
                .last_scan
                .compare_exchange(last, now, Ordering::AcqRel, Ordering::Relaxed)
                .is_ok()
    }
}

struct Buffer {
    cpu: Rc<crate::cpu_map_cache::Slot<Mapping>>,
    memory: Rc<Memory>,
}
/// Deferred groups reclaimed per backend call at most (except at close).
const RECLAIM_BUDGET: usize = 64;
/// C callback returning a VA range to the caller's heap (nv_rm.h
/// NvRmVaRelease). Called at most once, after RM unmapped the range.
pub type VaRelease = Option<unsafe extern "C" fn(data: *mut c_void, address: u64, size: u64)>;
/// One deferred free (reclaim.rs). Items of a group run in order: the RM
/// unmaps first, then memory objects (a mapping holds its memory alive until
/// it is unmapped anyway), then the VA range is handed back to C.
enum ReclaimItem {
    Mapping(GpuMapping),
    Memory(Buffer),
    Va {
        address: u64,
        size: u64,
        release: unsafe extern "C" fn(*mut c_void, u64, u64),
        data: *mut c_void,
    },
}
/// RM owners of one slot-pool page. Field order: GPU VA, CPU VMA, memory.
struct PoolPage {
    _gpu: GpuMapping,
    _cpu: Mapping,
    _memory: Rc<Memory>,
}
/// Per-submission scratch kept in Inner (capacity retained across frames).
#[derive(Default)]
struct SubmitScratch {
    waits: Vec<SemaphoreOp>,
    signals: Vec<SemaphoreOp>,
    held: Vec<Slot>,
    /// Slots allocated for this batch (fresh payload slots, gates, points):
    /// released if the batch is not accepted.
    allocated: Vec<Slot>,
    actions: Vec<Action>,
    shadows: Vec<(Slot, u64)>,
    /// What each GPU acquire of this batch waits for (parallel to `waits`).
    acquires: Vec<Acquire>,
}
/// A GPU semaphore acquire of an accepted batch, as the CPU value that
/// satisfies it. Kept per context until the batch retires; the slot stays
/// alive meanwhile because the batch holds it (`held`, released only through
/// retired tokens).
#[derive(Clone, Copy)]
enum Acquire {
    /// Binary payload or gate slot: passes on 1.
    Binary(Slot),
    /// Timeline slot: passes at >= value (full-width value).
    Timeline(Slot, u64),
}
/// Post-acceptance state changes, applied only after the batch is accepted.
enum Action {
    /// (payload, consumed slot as planned, fresh replacement)
    Consume(*const SyncObj, Slot, Slot),
    /// (payload, value, slot the GPU release targets)
    Signal(*const SyncObj, u64, Slot),
    Rebind(*const SyncObj, Slot),
    ForeignGate(Arc<Foreign>, Slot),
    SharedGate(Arc<Shared>, u64, Slot),
    SharedPoint(Arc<Shared>, u64, Slot),
}

/// Rc-graph RM state. Field order is teardown order: channels first (their
/// held slot references last within each queue), then event listeners, pool
/// pages, the address space and finally the device/client.
struct Inner {
    queues: Vec<Option<Queue>>,
    /// Per context (same index as `queues`): acquires of accepted batches
    /// that may not have retired yet, oldest first (`unblock_locked`).
    acquires: Vec<VecDeque<(u64, Acquire)>>,
    export_queue: Option<u64>,
    maps: HashMap<u64, GpuMapping>,
    buffers: HashMap<u64, Buffer>,
    /// Frees waiting for the GPU work outstanding at free time (reclaim.rs).
    reclaim: Reclaim<ReclaimItem>,
    /// Reused output buffer of `Reclaim::pop_retired` (capacity retained).
    reclaimed: Vec<ReclaimItem>,
    /// Reused input buffer of nv_rm_va_free (capacity retained).
    freeing: Vec<ReclaimItem>,
    /// Mappings whose deferred RM unmap failed: their VA stays reserved (the
    /// C release is never called) and they are retried at teardown.
    unmap_failed: Vec<GpuMapping>,
    watcher_host: Option<HostListener>,
    events: RmEvents,
    pages: Vec<PoolPage>,
    space: AddressSpace,
    device: Rc<Device>,
    scratch: SubmitScratch,
    next: u64,
    loss_handled: bool,
    /// Perf capture (capture.rs); None unless configured for this process.
    capture: Option<crate::capture::Capture>,
    /// Late arming (capture.rs `ARM_POLL`): when the configuration was absent
    /// at open, the last time submit looked for it again.
    capture_probe: Option<Instant>,
    /// A late arm was refused (a context without timestamps): no more probes.
    capture_refused: bool,
    /// Capture device tag (PCI bus/slot), as at open.
    capture_tag: usize,
    /// Per context (same index as `queues`): the engine mask it was created
    /// with, for a capture armed after the context exists.
    context_engines: Vec<u32>,
    /// Per GPU mapping id in `maps`: (address, size, memory, offset) of the
    /// bind, so a capture armed late can index pushbuffer memory that was
    /// bound before it.
    bind_info: HashMap<u64, (u64, u64, u64, u64)>,
}
/// SAFETY (Send): the raw C `data` pointers of deferred VA releases are the
/// caller's device objects, which nv_rm.h requires to outlive nv_rm_close and
/// to be usable from any thread. Every Rc/Cell/RefCell reachable from Inner
/// was created inside it and never escapes: all entry points run with the Bridge mutex
/// held and return only plain data (ids, addresses, owned fds, Arcs of
/// Send+Sync types). Rc reference counts are therefore only modified by the
/// mutex owner, and the mutex provides the happens-before between successive
/// owners on different threads. RM objects are per-client descriptors without
/// thread affinity. Moving the whole graph between threads (never sharing it
/// unlocked) is exactly what `Send` permits.
struct Confined(Inner);
unsafe impl Send for Confined {}

pub struct Bridge {
    core: Arc<Core>,
    inner: Mutex<Confined>,
    /// Perf capture armed (frame markers take the lock only then); set at
    /// open or by a late arm from submit.
    capture: std::sync::atomic::AtomicBool,
}

fn sync_ref<'a>(id: u64) -> Result<&'a SyncObj> {
    let pointer = id as usize as *const SyncObj;
    if pointer.is_null() {
        return Err(Error::Contract("null synchronization id".into()));
    }
    // SAFETY: ids are Arc::into_raw pointers returned by nv_rm_sync_create;
    // the C caller keeps the object alive for the duration of the call
    // (Vulkan external synchronization of vk_sync lifetime).
    let sync = unsafe { &*pointer };
    if !sync.valid() {
        return Err(Error::Contract("invalid synchronization id".into()));
    }
    Ok(sync)
}

impl Bridge {
    fn lock(&self) -> MutexGuard<'_, Confined> {
        self.inner.lock().unwrap_or_else(|p| p.into_inner())
    }
    fn new(pci: [u32; 4]) -> Result<(Self, Info)> {
        crate::diag::install_panic_hook();
        let client = Client::open()?;
        let card = client
            .cards()?
            .into_iter()
            .find(|c| {
                [
                    c.pci.domain,
                    c.pci.bus as u32,
                    c.pci.slot as u32,
                    c.pci.function as u32,
                ] == pci
            })
            .ok_or_else(|| Error::Contract("requested PCI NVIDIA GPU absent".into()))?;
        let device = client.device(card)?;
        let info = query_info(&device)?;
        let space = AddressSpace::new(&device)?;
        let events = RmEvents::new(&device)?;
        let caps = capabilities::query(&device)?;
        let view = if crate::gpu::modern_host(caps.channel)? {
            TimelineView::Native64
        } else {
            TimelineView::Split32
        };
        let core = Arc::new(Core {
            pool: Arc::new(SlotPool::new(view)),
            waiters: Waiters::new()?,
            watcher: Watcher::new()?,
            trigger: events.trigger_port(),
            lost: AtomicBool::new(false),
            pending_waiters: std::sync::atomic::AtomicU32::new(0),
            last_scan: AtomicU64::new(0),
            reclaim_pending: AtomicUsize::new(0),
            epoch: Instant::now(),
            devices: (events.as_raw_fd(), events.error_fd()),
        });
        let capture = crate::capture::config().map(|c| {
            crate::capture::Capture::new(c, (pci[1] << 8 | pci[2]) as usize)
        });
        let bridge = Self {
            core,
            capture: std::sync::atomic::AtomicBool::new(capture.is_some()),
            inner: Mutex::new(Confined(Inner {
                queues: Vec::new(),
                acquires: Vec::new(),
                export_queue: None,
                maps: HashMap::new(),
                buffers: HashMap::new(),
                reclaim: Reclaim::default(),
                reclaimed: Vec::new(),
                freeing: Vec::new(),
                unmap_failed: Vec::new(),
                watcher_host: None,
                events,
                pages: Vec::new(),
                space,
                device,
                scratch: SubmitScratch::default(),
                next: 1,
                loss_handled: false,
                capture,
                capture_probe: None,
                capture_refused: false,
                capture_tag: (pci[1] << 8 | pci[2]) as usize,
                context_engines: Vec::new(),
                bind_info: HashMap::new(),
            })),
        };
        bridge.grow(&mut bridge.lock().0)?;
        Ok((bridge, info))
    }

    // ---- slot pool --------------------------------------------------------
    /// Map one more 64 KiB SYS page (CPU-cached, GPU-uncached; never unmapped
    /// before device teardown). Requires Inner.
    fn grow(&self, inner: &mut Inner) -> Result<()> {
        if self.core.pool.next_page().is_none() {
            return Err(Error::Contract(
                "semaphore slot pool exhausted (1M live slots)".into(),
            ));
        }
        let memory = inner
            .device
            .allocate_aligned(PAGE_BYTES, PAGE_BYTES, false, true)?;
        let cpu = memory.map()?;
        let gpu = inner.space.map(&memory)?;
        if gpu
            .address()
            .checked_add(memory.size as u64)
            .is_none_or(|end| end >= 1 << 34)
        {
            return Err(Error::Contract(
                "GPU semaphore pool collides with Mesa VA heap".into(),
            ));
        }
        self.core
            .pool
            .install_page(cpu.cpu_address(), gpu.address())?;
        inner.pages.push(PoolPage {
            _gpu: gpu,
            _cpu: cpu,
            _memory: memory,
        });
        Ok(())
    }
    fn slot_locked(&self, inner: &mut Inner) -> Result<Slot> {
        loop {
            if let Some(slot) = self.core.pool.try_allocate() {
                return Ok(slot);
            }
            self.grow(inner)?;
        }
    }
    /// Allocate without holding any lock; takes Inner only to grow the pool.
    fn slot(&self) -> Result<Slot> {
        loop {
            if let Some(slot) = self.core.pool.try_allocate() {
                return Ok(slot);
            }
            let mut guard = self.lock();
            if !self.core.pool.has_free() {
                self.grow(&mut guard.0)?;
            }
        }
    }

    // ---- loss ------------------------------------------------------------
    /// Heavy scan: read every channel's VRAM error notifier. Requires Inner.
    fn scan_locked(&self, inner: &mut Inner) -> Result<()> {
        if self.core.is_lost() {
            self.lose_locked(inner);
            return Err(Error::DeviceLost("Mesa RM device lost".into()));
        }
        for queue in inner.queues.iter_mut().flatten() {
            if let Err(error) = queue.check_error() {
                diag!("nvk Rust RM channel notification: {error}");
                self.lose_locked(inner);
                return Err(Error::DeviceLost(error.to_string()));
            }
        }
        if inner.device.cpu_mapping_failed() {
            self.lose_locked(inner);
            return Err(Error::DeviceLost(
                "CPU mapping cleanup previously failed".into(),
            ));
        }
        Ok(())
    }
    /// Cheap per-call loss check: flag load + throttle compare; at most one
    /// heavy scan per second (skipped if another thread holds Inner — that
    /// thread scans on its own path).
    fn check(&self) -> Result<()> {
        self.core.live()?;
        if self.core.scan_due(false) {
            if let Ok(mut guard) = self.inner.try_lock() {
                return self.scan_locked(&mut guard.0);
            }
        }
        Ok(())
    }
    #[track_caller]
    fn lose_locked(&self, inner: &mut Inner) {
        self.core.mark_lost();
        if inner.loss_handled {
            return;
        }
        inner.loss_handled = true;
        for queue in inner.queues.iter_mut().flatten() {
            queue.stop_submissions();
        }
        self.core.watcher.fail_all(&self.core);
    }

    // ---- CPU waits -------------------------------------------------------
    fn wait_until(
        &self,
        deadline: Option<Instant>,
        mut predicate: impl FnMut() -> Result<bool>,
    ) -> Result<bool> {
        let core = &self.core;
        let result = core.waiters.wait(
            deadline,
            Some(core.devices),
            || {
                core.live()?;
                predicate()
            },
            |_| {
                // RC readiness / EINTR / 1 s period: authoritative error state.
                core.scan_due(true);
                let mut guard = self.lock();
                self.scan_locked(&mut guard.0)
            },
        );
        if let Err(Error::DeviceLost(_)) = &result {
            core.mark_lost();
        }
        result
    }
    fn queue_shared(&self, context: u64) -> Result<Arc<QueueShared>> {
        let guard = self.lock();
        Ok(Arc::clone(queue_ref(&guard.0, context)?.shared()))
    }
    fn wait_token(&self, context: u64, token: u64, timeout_ns: u64) -> Result<bool> {
        let start = Instant::now();
        let shared = self.queue_shared(context)?;
        self.check()?;
        if shared.retired(token)? {
            return Ok(true);
        }
        if timeout_ns == 0 {
            return Ok(false);
        }
        self.wait_until(deadline(start, timeout_ns), || shared.retired(token))
    }
    fn drain_timeout(&self, timeout_ns: u64) -> Result<()> {
        let start = Instant::now();
        let pending: Vec<(u64, u64)> = {
            let guard = self.lock();
            self.core.live()?;
            guard
                .0
                .queues
                .iter()
                .enumerate()
                .filter_map(|(i, q)| q.as_ref().map(|q| (i as u64 + 1, q.last_token())))
                .collect()
        };
        for (context, token) in pending {
            let remaining = if timeout_ns == u64::MAX {
                u64::MAX
            } else {
                timeout_ns
                    .saturating_sub(start.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64)
            };
            if !self.wait_token(context, token, remaining)? {
                return Err(Error::Contract("RM teardown drain deadline reached".into()));
            }
        }
        Ok(())
    }

    // ---- loss teardown: never free a channel blocked on our semaphore ----
    /// Device loss or an expired bounded drain, before RM-freeing channels.
    /// Satisfy every GPU acquire of the context's unretired batches (gates of
    /// failed/pending foreign or shared producers, native binary/timeline
    /// payloads nobody will signal any more) so the channel runs to its
    /// completion release instead of being freed while blocked in a semaphore
    /// ACQUIRE: pre-Volta RM then preempts the channel and logs Xid 45. The
    /// device is already lost: results of the released work are undefined, and
    /// every CPU wait/status path already reports loss. Returns the context's
    /// last token and retirement view for `quiesce`.
    fn unblock_locked(&self, inner: &mut Inner, context: u64) -> Option<(u64, Arc<QueueShared>)> {
        let index = (context - 1) as usize;
        let queue = inner.queues.get(index)?.as_ref()?;
        let shared = Arc::clone(queue.shared());
        let token = queue.last_token();
        // Entries newer than `completed` are still held by their batch (held
        // slots are released only through retired tokens <= completed).
        let completed = shared.completed().unwrap_or(0);
        let pool = &self.core.pool;
        for &(batch, acquire) in inner.acquires[index].iter() {
            if batch <= completed {
                continue;
            }
            let result = match acquire {
                Acquire::Binary(slot) => pool.store32(slot, 1),
                Acquire::Timeline(slot, value) => pool.host_signal(slot, value).map(|_| ()),
            };
            if let Err(error) = result {
                diag!("nvk Rust RM teardown acquire release (context {context}): {error}");
            }
        }
        inner.acquires[index].clear();
        Some((token, shared))
    }
    /// Bounded wait, ignoring the device loss flag, until every listed channel
    /// retired its last token or reported a channel error (an RC'd channel
    /// never retires and is no longer running). Woken by the completion
    /// interrupt; channel error notifiers are rechecked on RC readiness.
    fn quiesce(&self, pending: &[(u64, Arc<QueueShared>)], timeout: Duration) -> bool {
        let start = Instant::now();
        let core = &self.core;
        let result = core.waiters.wait(
            Some(start + timeout),
            Some(core.devices),
            || {
                for (token, shared) in pending {
                    match shared.retired(*token) {
                        Ok(true) | Err(Error::DeviceLost(_)) => {}
                        Ok(false) => return Ok(false),
                        Err(error) => return Err(error),
                    }
                }
                Ok(true)
            },
            |_| {
                let mut guard = self.lock();
                for queue in guard.0.queues.iter_mut().flatten() {
                    let _ = queue.check_error();
                }
                Ok(())
            },
        );
        match result {
            Ok(true) => true,
            Ok(false) => {
                diag!(
                    "nvk Rust RM teardown: channels did not retire within {timeout:?} after releasing their acquires"
                );
                false
            }
            Err(error) => {
                diag!("nvk Rust RM teardown quiesce: {error}");
                false
            }
        }
    }
    /// Invariant at every channel free: no unretired work (a running or
    /// blocked channel). A channel that reported its own error is exempt.
    fn check_idle_before_free(context: u64, queue: &Queue) {
        let token = queue.last_token();
        let blocked = match queue.shared().retired(token) {
            Ok(retired) => !retired,
            Err(Error::DeviceLost(_)) => false,
            Err(_) => true,
        };
        if blocked {
            diag!(
                "nvk Rust RM: freeing channel of context {context} with unretired work (last token {token})"
            );
            if strict_teardown() {
                std::process::abort();
            }
        }
    }

    // ---- deferred reclamation (reclaim.rs) -------------------------------
    /// Open a reclaim group: snapshot every queue's unretired last token.
    /// Zero means no submitted work can still reach the object: reclaim now.
    /// A lost queue (retired() errors) counts as unretired: its objects wait
    /// for close, where channels are freed before anything is reclaimed.
    fn reclaim_begin(inner: &mut Inner) -> u32 {
        let Inner {
            queues, reclaim, ..
        } = inner;
        reclaim.begin(queues.iter().enumerate().filter_map(|(index, queue)| {
            let queue = queue.as_ref()?;
            let token = queue.last_token();
            (token != 0 && !matches!(queue.shared().retired(token), Ok(true)))
                .then_some((index as u32, token))
        }))
    }
    /// Defer `items` behind the current GPU work, or reclaim them now when
    /// nothing is outstanding. Returns Err only for an immediate unmap
    /// failure (the device is then lost and the VA stays reserved).
    fn reclaim_or_defer(
        &self,
        inner: &mut Inner,
        items: impl IntoIterator<Item = ReclaimItem>,
    ) -> Result<()> {
        // Older groups first (within the per-call budget). If the new
        // snapshot is empty, every older group has retired as well, so doing
        // this one now (possibly before an older retired one still in the
        // backlog) is safe: each group is self-contained (its mappings hold
        // their memory alive) and VA ranges of different groups are disjoint.
        self.reclaim_locked(inner, false);
        if Self::reclaim_begin(inner) != 0 {
            inner.reclaim.commit(items);
            self.core.reclaim_pending.fetch_add(1, Ordering::Release);
            return Ok(());
        }
        let mut failed = None;
        for item in items {
            if let Err(error) = self.reclaim_item(inner, item, failed.is_some()) {
                failed.get_or_insert(error);
            }
        }
        failed.map_or(Ok(()), Err)
    }
    /// Reclaim every group whose snapshot retired (all of them with `force`,
    /// used only at close after every channel was freed).
    fn reclaim_locked(&self, inner: &mut Inner, force: bool) {
        if inner.reclaim.is_empty() {
            return;
        }
        let mut out = std::mem::take(&mut inner.reclaimed);
        // Amortised: at most RECLAIM_BUDGET groups per call, so a backlog
        // that built up during a long GPU stall (tens of thousands of frees)
        // never holds Inner for one long stretch of RM unmap/free ioctls on a
        // caller (measured: 494 ms with an unbounded drain). Every call that
        // frees also reclaims up to the budget, so the backlog drains faster
        // than it grows once the work retired. Close is unbounded (force).
        let mut budget = RECLAIM_BUDGET;
        loop {
            if !force {
                if budget == 0 {
                    break;
                }
                budget -= 1;
            }
            let Inner {
                queues, reclaim, ..
            } = &mut *inner;
            let retired = |queue: u32, token: u64| match queues
                .get(queue as usize)
                .and_then(Option::as_ref)
            {
                // Destroyed context: its teardown waited (bounded) or lost,
                // then RM freed its channel (RM preempts/unbinds on free).
                None => true,
                Some(queue) => matches!(queue.shared().retired(token), Ok(true)),
            };
            if !reclaim.pop_retired(force, retired, &mut out) {
                break;
            }
            self.core.reclaim_pending.fetch_sub(1, Ordering::Release);
            let mut failed = false;
            for item in out.drain(..) {
                failed |= self.reclaim_item(inner, item, failed).is_err();
            }
        }
        inner.reclaimed = out;
    }
    /// Lock-free hint check, then reclaim if Inner is free right now (status
    /// and wait paths never block on another thread's allocation).
    fn reclaim_try(&self) {
        if self.core.reclaim_pending.load(Ordering::Acquire) != 0 {
            if let Ok(mut guard) = self.inner.try_lock() {
                self.reclaim_locked(&mut guard.0, false);
            }
        }
    }
    /// Perform one free. `group_failed`: an earlier unmap of the same group
    /// failed, so its VA range must stay reserved.
    fn reclaim_item(&self, inner: &mut Inner, item: ReclaimItem, group_failed: bool) -> Result<()> {
        match item {
            ReclaimItem::Mapping(mut mapping) => {
                if let Err(error) = mapping.try_unmap() {
                    diag!(
                        "nvk Rust RM deferred GPU unmap at {:#x} failed; VA reserved, device lost: {error}",
                        mapping.address()
                    );
                    inner.unmap_failed.push(mapping);
                    self.lose_locked(inner);
                    return Err(Error::DeviceLost(format!(
                        "GPU unmap failed; reservation retained: {error}"
                    )));
                }
            }
            // Memory::drop frees the RM object (its own diagnostic on error);
            // the CPU mapping was already evicted at free time.
            ReclaimItem::Memory(buffer) => drop(buffer),
            ReclaimItem::Va {
                address,
                size,
                release,
                data,
            } => {
                if group_failed {
                    diag!(
                        "nvk Rust RM VA {address:#x}+{size:#x} stays reserved after a failed unmap"
                    );
                } else {
                    // SAFETY: nv_rm_va_free's contract: `release(data, ..)` is
                    // valid until nv_rm_close returns and takes only the
                    // caller's VA-heap lock (never held across backend calls).
                    unsafe { release(data, address, size) };
                }
            }
        }
        Ok(())
    }

    fn broadcast(&self, changed: bool) {
        if changed {
            if let Err(error) = self.core.trigger.trigger() {
                diag!("RM shared payload broadcast: {error}");
            }
        }
    }

    /// Evaluate ANY/ALL over C points (no allocation).
    fn points_ready(&self, points: &[SyncPoint], flags: u32) -> Result<bool> {
        let any = flags & 2 != 0;
        let pending = flags & 1 != 0;
        let mut ready = !any;
        let mut broadcast = false;
        for point in points {
            let (one, changed) = sync_ref(point.sync)?.ready(point.value, pending)?;
            broadcast |= changed;
            if any {
                ready |= one;
                if one {
                    break;
                }
            } else {
                ready &= one;
                if !one {
                    break;
                }
            }
        }
        self.broadcast(broadcast);
        Ok(ready || points.is_empty())
    }
    fn wait_points(&self, points: &[SyncPoint], flags: u32, timeout_ns: u64) -> Result<bool> {
        let start = Instant::now();
        self.check()?;
        if self.points_ready(points, flags)? {
            return Ok(true);
        }
        if timeout_ns == 0 {
            return Ok(false);
        }
        // WAIT_PENDING observes submission state: register (Dekker with the
        // submitter's fence + load) before the predicate is re-evaluated.
        struct Pending<'a>(&'a std::sync::atomic::AtomicU32);
        impl Drop for Pending<'_> {
            fn drop(&mut self) {
                self.0.fetch_sub(1, Ordering::SeqCst);
            }
        }
        let _pending = (flags & 1 != 0).then(|| {
            self.core.pending_waiters.fetch_add(1, Ordering::SeqCst);
            Pending(&self.core.pending_waiters)
        });
        // Pending foreign sync_files wake us through the watcher; everything
        // else is a mapped word (HOST interrupt or CPU notify).
        for point in points {
            if let Some(foreign) = sync_ref(point.sync)?.foreign() {
                self.core.watcher.watch_cpu(&self.core, foreign)?;
            }
        }
        self.wait_until(deadline(start, timeout_ns), || {
            self.points_ready(points, flags)
        })
    }

    // ---- submission ------------------------------------------------------
    /// Waits + work + signals in one GPFIFO segment and one doorbell, on the
    /// calling thread. Phase 1 plans every payload without mutating it; Full
    /// (ring pressure) or an error releases everything allocated and leaves
    /// all payloads untouched; phase 3 applies payload changes only after the
    /// batch was accepted.
    fn submit(
        &self,
        context: u64,
        pushes: &[Push],
        waits: &[SyncPoint],
        foreign: &[Arc<Foreign>],
        signals: &[SyncPoint],
    ) -> Result<SubmitStatus> {
        self.core.live()?;
        let mut guard = self.lock();
        let inner = &mut guard.0;
        if self.core.scan_due(false) {
            self.scan_locked(inner)?;
        }
        // Amortised deferred reclamation (one emptiness check when idle).
        self.reclaim_locked(inner, false);
        if inner.capture.is_none() {
            self.capture_late_arm(inner);
        }
        let mut scratch = std::mem::take(&mut inner.scratch);
        scratch.waits.clear();
        scratch.signals.clear();
        scratch.held.clear();
        scratch.allocated.clear();
        scratch.actions.clear();
        scratch.shadows.clear();
        scratch.acquires.clear();
        let result = self.submit_locked(
            inner,
            context,
            pushes,
            waits,
            foreign,
            signals,
            &mut scratch,
        );
        match &result {
            Ok(SubmitStatus::Submitted(token)) => {
                let token = *token;
                if inner.capture.is_some() {
                    capture_submit(
                        inner,
                        context,
                        pushes,
                        [waits.len() as u32, foreign.len() as u32, signals.len() as u32],
                        token,
                    );
                }
                record_acquires(inner, context, token, scratch.acquires.drain(..));
                let mut notify = !scratch.signals.is_empty();
                for action in scratch.actions.drain(..) {
                    match action {
                        Action::Consume(sync, planned, fresh) => {
                            // SAFETY: C keeps payloads alive during the call.
                            let sync = unsafe { &*sync };
                            let mut state = sync.lock();
                            check_planned(&state, planned, "wait");
                            // The batch now holds the consumed slot (held
                            // above); drop the payload's own reference.
                            sync.rebind_native(&mut state, fresh);
                            notify = true;
                        }
                        Action::Rebind(sync, fresh) => {
                            let sync = unsafe { &*sync };
                            sync.rebind_native(&mut sync.lock(), fresh);
                        }
                        Action::Signal(sync, value, planned) => {
                            let sync = unsafe { &*sync };
                            let mut state = sync.lock();
                            check_planned(&state, planned, "signal");
                            sync.commit_signal(&mut state, context, token, value);
                        }
                        Action::ForeignGate(foreign, gate) => {
                            if let Err(error) = self.core.watcher.gate(&self.core, foreign, gate) {
                                // The batch is accepted and waits on a gate that
                                // can no longer be released: never as success.
                                diag!("nvk Rust RM foreign gate registration: {error}");
                                self.lose_locked(inner);
                            }
                        }
                        Action::SharedGate(shared, target, gate) => {
                            if let Err(error) = self.ensure_watcher_host(inner).and_then(|()| {
                                self.core
                                    .watcher
                                    .shared_gate(&self.core, shared, target, gate)
                            }) {
                                diag!("nvk Rust RM shared gate registration: {error}");
                                self.lose_locked(inner);
                            }
                        }
                        Action::SharedPoint(shared, target, point) => {
                            shared.add_point(target, point);
                            if let Err(error) = self
                                .ensure_watcher_host(inner)
                                .and_then(|()| self.core.watcher.publish(&self.core, shared))
                            {
                                diag!("nvk Rust RM shared publication: {error}");
                                self.lose_locked(inner);
                            }
                        }
                    }
                }
                // WAIT_PENDING waiters observe the pending state committed
                // above (under each payload lock); completion waiters are woken
                // by the GPU interrupt instead.
                std::sync::atomic::fence(Ordering::SeqCst);
                if notify && self.core.pending_waiters.load(Ordering::SeqCst) != 0 {
                    self.core.waiters.notify();
                }
            }
            _ => {
                // Not accepted: payloads untouched, batch-local slots freed.
                for slot in scratch.allocated.drain(..) {
                    self.core.pool.release(slot);
                }
                scratch.actions.clear();
            }
        }
        inner.scratch = scratch;
        result
    }
    #[allow(clippy::too_many_arguments)]
    fn submit_locked(
        &self,
        inner: &mut Inner,
        context: u64,
        pushes: &[Push],
        waits: &[SyncPoint],
        foreign: &[Arc<Foreign>],
        signals: &[SyncPoint],
        s: &mut SubmitScratch,
    ) -> Result<SubmitStatus> {
        let pool = &self.core.pool;
        queue_ref(inner, context)?;
        for f in foreign {
            if !f.ready(false)? {
                let gate = self.slot_locked(inner)?;
                s.allocated.push(gate);
                s.held.push(gate);
                s.waits.push(SemaphoreOp::AcquireBinary(pool.gpu(gate)));
                s.acquires.push(Acquire::Binary(gate));
                s.actions.push(Action::ForeignGate(Arc::clone(f), gate));
            }
        }
        for point in waits {
            let sync = sync_ref(point.sync)?;
            let (plan, consumes) = {
                let state = sync.lock();
                (
                    sync.plan_wait(&state, point.value)?,
                    sync.wait_consumes(&state),
                )
            };
            match plan {
                WaitPlan::Complete => {
                    if consumes {
                        // A satisfied binary/foreign wait still unsignals it.
                        let fresh = self.slot_locked(inner)?;
                        s.allocated.push(fresh);
                        s.actions.push(Action::Rebind(sync, fresh));
                    }
                }
                WaitPlan::Binary(slot) => {
                    let fresh = self.slot_locked(inner)?;
                    s.allocated.push(fresh);
                    s.held.push(slot);
                    s.waits.push(SemaphoreOp::AcquireBinary(pool.gpu(slot)));
                    s.acquires.push(Acquire::Binary(slot));
                    s.actions.push(Action::Consume(sync, slot, fresh));
                }
                WaitPlan::Timeline(slot, value) => {
                    s.held.push(slot);
                    s.acquires.push(Acquire::Timeline(slot, value));
                    let value = if pool.view == TimelineView::Native64 {
                        value
                    } else {
                        u64::from(value as u32)
                    };
                    s.waits
                        .push(SemaphoreOp::AcquireTimeline(pool.gpu(slot), value));
                }
                WaitPlan::Foreign(f) => {
                    let gate = self.slot_locked(inner)?;
                    let fresh = self.slot_locked(inner)?;
                    s.allocated.push(gate);
                    s.allocated.push(fresh);
                    s.held.push(gate);
                    s.waits.push(SemaphoreOp::AcquireBinary(pool.gpu(gate)));
                    s.acquires.push(Acquire::Binary(gate));
                    s.actions.push(Action::ForeignGate(f, gate));
                    s.actions.push(Action::Rebind(sync, fresh));
                }
                WaitPlan::Shared(shared, target) => {
                    let gate = self.slot_locked(inner)?;
                    s.allocated.push(gate);
                    s.held.push(gate);
                    s.waits.push(SemaphoreOp::AcquireBinary(pool.gpu(gate)));
                    s.acquires.push(Acquire::Binary(gate));
                    s.actions.push(Action::SharedGate(shared, target, gate));
                }
            }
        }
        for point in signals {
            let sync = sync_ref(point.sync)?;
            // A payload consumed by a wait in this same batch is signalled on
            // its replacement slot (Vulkan: wait then signal of one binary).
            let rebound = s.actions.iter().rev().find_map(|a| match a {
                Action::Consume(p, _, fresh) | Action::Rebind(p, fresh)
                    if std::ptr::eq(*p, sync) =>
                {
                    Some(*fresh)
                }
                _ => None,
            });
            let plan = match rebound {
                Some(fresh) if !sync.timeline => SignalPlan::Binary(fresh),
                _ => sync.plan_signal(&sync.lock(), point.value)?,
            };
            match plan {
                SignalPlan::Binary(slot) => {
                    s.held.push(slot);
                    s.signals.push(SemaphoreOp::ReleaseBinary(pool.gpu(slot)));
                    s.actions.push(Action::Signal(sync, point.value, slot));
                }
                SignalPlan::Timeline(slot, value) => {
                    pool.check_split_distance(slot, value)?;
                    s.held.push(slot);
                    if pool.view == TimelineView::Split32 {
                        s.shadows.push((slot, value));
                    }
                    let wire = if pool.view == TimelineView::Native64 {
                        value
                    } else {
                        u64::from(value as u32)
                    };
                    s.signals
                        .push(SemaphoreOp::ReleaseTimeline(pool.gpu(slot), wire));
                    s.actions.push(Action::Signal(sync, value, slot));
                }
                SignalPlan::ReplaceForeign => {
                    let fresh = self.slot_locked(inner)?;
                    s.allocated.push(fresh);
                    s.held.push(fresh);
                    s.signals.push(SemaphoreOp::ReleaseBinary(pool.gpu(fresh)));
                    s.actions.push(Action::Rebind(sync, fresh));
                    s.actions.push(Action::Signal(sync, point.value, fresh));
                }
                SignalPlan::Shared(shared, target) => {
                    let point_slot = self.slot_locked(inner)?;
                    s.allocated.push(point_slot);
                    s.held.push(point_slot);
                    s.signals
                        .push(SemaphoreOp::ReleaseBinary(pool.gpu(point_slot)));
                    s.actions
                        .push(Action::SharedPoint(shared, target, point_slot));
                }
            }
        }
        let space = &inner.space;
        let queue = inner.queues[(context - 1) as usize].as_mut().unwrap();
        let shadows = &s.shadows;
        let result = queue.submit(pushes, space, &s.waits, &s.signals, &s.held, |_| {
            for &(slot, value) in shadows {
                pool.raise_shadow(slot, value)?;
            }
            Ok(())
        });
        let status = match result {
            Ok(status) => status,
            Err(error) => {
                // The queue marked its channel lost (partially written ring or
                // failed overflow mapping): the device cannot continue.
                self.lose_locked(inner);
                return Err(Error::DeviceLost(error.to_string()));
            }
        };
        if let SubmitStatus::Submitted(_) = status {
            // Allocated references now belong to payloads/gates/points via
            // the actions; nothing to release.
            s.allocated.clear();
        }
        Ok(status)
    }

    fn ensure_watcher_host(&self, inner: &mut Inner) -> Result<()> {
        let fd = match &inner.watcher_host {
            Some(listener) => listener.as_raw_fd(),
            None => {
                let listener = inner.events.host_listener()?;
                let fd = listener.as_raw_fd();
                inner.watcher_host = Some(listener);
                fd
            }
        };
        if !self.core.watcher.has_host() {
            self.core.watcher.set_host(fd);
        }
        Ok(())
    }

    fn export_sync_file(&self, id: u64, drm_fd: i32) -> Result<Option<OwnedFd>> {
        let sync = sync_ref(id)?;
        if sync.timeline {
            return Err(Error::Contract("timeline SYNC_FD export is invalid".into()));
        }
        let mut guard = self.lock();
        let inner = &mut guard.0;
        self.core.live()?;
        enum Next {
            Done(Option<OwnedFd>),
            Fence(u64, u64),
            Gate(Arc<Shared>, u64),
            /// Reserved native slot; this export holds one reference.
            Slot(crate::slots::Slot),
        }
        let next = {
            let state = sync.lock();
            match &state.payload {
                crate::syncobj::Payload::Foreign(f) => Next::Done(Some(f.duplicate()?)),
                crate::syncobj::Payload::Native(slot) => {
                    if self.core.pool.read32(*slot)? == 1 {
                        Next::Done(None)
                    } else if state.pending == Some(crate::syncobj::RESERVED) {
                        // Signal reserved for a submit-thread submission that
                        // has not reached a ring yet: the export queue's GPU
                        // acquire waits on the reserved slot itself.
                        self.core.pool.retain(*slot);
                        Next::Slot(*slot)
                    } else if let Some((context, token)) = state.pending {
                        Next::Fence(context, token)
                    } else {
                        // No submitted signal. Vulkan forbids this export
                        // (VUID-VkSemaphoreGetFdInfoKHR-handleType-03254,
                        // VUID-VkFenceGetFdInfoKHR-handleType-01454). A GPU
                        // wait on this slot would be orphaned: Mesa resets the
                        // payload right after every SYNC_FD export
                        // (vk_semaphore.c:700, vk_fence.c:468), which rebinds a
                        // captured slot, so any later signal lands elsewhere
                        // and the exported fence would never signal. Fail
                        // loudly instead of handing out a dead fence.
                        diag!("nvk Rust RM: SYNC_FD export of an unsignaled binary payload with no submitted signal");
                        return Err(Error::Contract(
                            "SYNC_FD export without a submitted signal operation".into(),
                        ));
                    }
                }
                crate::syncobj::Payload::Shared(shared) => {
                    let shared = Arc::clone(shared);
                    let target = state.snapshot.unwrap_or_else(|| shared.target());
                    drop(state);
                    let changed = shared.refresh()?;
                    self.broadcast(changed);
                    if shared.completed() >= target {
                        Next::Done(None)
                    } else if shared.failed() {
                        return Err(Error::DeviceLost(
                            "shared synchronization producer failed".into(),
                        ));
                    } else {
                        Next::Gate(shared, target)
                    }
                }
            }
        };
        match next {
            Next::Done(fd) => Ok(fd),
            Next::Fence(context, token) => queue_ref(inner, context)?
                .context_fence(drm_fd, token)
                .map(Some),
            Next::Slot(slot) => {
                // The export batch acquires the reserved slot directly: the
                // submit thread's later GPU release completes it. No CPU wait
                // happens here; ring pressure is a named capacity error.
                let pool = &self.core.pool;
                let context = match inner.export_queue {
                    Some(context) => context,
                    None => match self.create_context(inner, 1) {
                        Ok(context) => {
                            inner.export_queue = Some(context);
                            context
                        }
                        Err(error) => {
                            pool.release(slot);
                            return Err(error);
                        }
                    },
                };
                let waits = [SemaphoreOp::AcquireBinary(pool.gpu(slot))];
                let space = &inner.space;
                let queue = inner.queues[(context - 1) as usize].as_mut().unwrap();
                let status = queue.submit(&[], space, &waits, &[], &[slot], |_| Ok(()));
                pool.release(slot); // the accepted batch holds its own reference
                let token = match status {
                    Ok(SubmitStatus::Submitted(token)) => token,
                    Ok(_) => {
                        return Err(Error::Io(
                            "external fence export capacity exhausted",
                            std::io::Error::from_raw_os_error(12),
                        ))
                    }
                    Err(error) => return Err(error),
                };
                record_acquires(inner, context, token, [Acquire::Binary(slot)]);
                queue_ref(inner, context)?
                    .context_fence(drm_fd, token)
                    .map(Some)
            }
            Next::Gate(shared, target) => {
                // Imported shared payloads have no process-local producer
                // queue: an internal export queue waits on a watcher-owned
                // gate (released when the shared generation completes).
                let context = match inner.export_queue {
                    Some(context) => context,
                    None => {
                        let context = self.create_context(inner, 1)?;
                        inner.export_queue = Some(context);
                        context
                    }
                };
                let pool = &self.core.pool;
                let gate = self.slot_locked(inner)?;
                let waits = [SemaphoreOp::AcquireBinary(pool.gpu(gate))];
                let space = &inner.space;
                let queue = inner.queues[(context - 1) as usize].as_mut().unwrap();
                let status = queue.submit(&[], space, &waits, &[], &[gate], |_| Ok(()));
                let token = match status {
                    Ok(SubmitStatus::Submitted(token)) => {
                        record_acquires(inner, context, token, [Acquire::Binary(gate)]);
                        token
                    }
                    other => {
                        pool.release(gate);
                        return match other {
                            Ok(_) => Err(Error::Io(
                                "external fence export capacity exhausted",
                                std::io::Error::from_raw_os_error(12),
                            )),
                            Err(error) => Err(error),
                        };
                    }
                };
                // The watcher takes the gate's CPU reference (the export
                // batch holds its own). If it cannot, the accepted export
                // batch can never complete: publish loss, never a dead fence.
                if let Err(error) = self.ensure_watcher_host(inner) {
                    pool.release(gate);
                    diag!("nvk Rust RM shared export gate registration: {error}");
                    self.lose_locked(inner);
                    return Err(error);
                }
                if let Err(error) = self
                    .core
                    .watcher
                    .shared_gate(&self.core, shared, target, gate)
                {
                    diag!("nvk Rust RM shared export gate registration: {error}");
                    self.lose_locked(inner);
                    return Err(error);
                }
                queue_ref(inner, context)?
                    .context_fence(drm_fd, token)
                    .map(Some)
            }
        }
    }

    /// Arm the perf capture in a process that is already running (capture.rs
    /// "Late arming"): at most every `ARM_POLL`, look for the configuration
    /// again; once present, every live context gets its timestamp ring (as
    /// create_context does for an armed process) and the capture's
    /// VA → allocation index is seeded from the live binds. A context whose
    /// timestamps cannot be enabled refuses the arm loudly (no partial
    /// capture) and the probe stops.
    fn capture_late_arm(&self, inner: &mut Inner) {
        if inner.capture_refused {
            return;
        }
        let now = Instant::now();
        if inner
            .capture_probe
            .is_some_and(|last| now.duration_since(last) < crate::capture::ARM_POLL)
        {
            return;
        }
        inner.capture_probe = Some(now);
        let Some(config) = crate::capture::config() else {
            return;
        };
        let mut cap = crate::capture::Capture::new(config, inner.capture_tag);
        for (i, queue) in inner.queues.iter_mut().enumerate() {
            let Some(queue) = queue.as_mut() else {
                continue;
            };
            if let Err(error) = queue.enable_timing(&inner.space) {
                crate::diag::diag!(
                    "nvk Rust RM capture: late arm refused, context {} has no timestamps: {error}",
                    i + 1
                );
                // Never retried: the probe interval would repeat the refusal.
                inner.capture_refused = true;
                return;
            }
            cap.add_context(
                i as u64 + 1,
                inner.context_engines.get(i).copied().unwrap_or(0),
                queue.channel_class(),
            );
        }
        for &(address, size, memory, offset) in inner.bind_info.values() {
            cap.binds.insert(address, (size, memory, offset));
        }
        crate::diag::diag!(
            "nvk Rust RM capture armed late: {} contexts, {} binds indexed",
            inner.queues.iter().flatten().count(),
            cap.binds.len()
        );
        inner.capture = Some(cap);
        self.capture
            .store(true, std::sync::atomic::Ordering::Relaxed);
    }
    fn create_context(&self, inner: &mut Inner, engines: u32) -> Result<u64> {
        let retire = self.slot_locked(inner)?;
        match Queue::new_with_events(
            &inner.device,
            &inner.space,
            engines,
            &inner.events,
            &self.core.pool,
            retire,
        ) {
            Ok(mut queue) => {
                if let Some(cap) = inner.capture.as_mut() {
                    // Loud: an armed capture without timestamps is refused.
                    queue.enable_timing(&inner.space)?;
                    cap.add_context(
                        inner.queues.len() as u64 + 1,
                        engines,
                        queue.channel_class(),
                    );
                }
                inner.queues.push(Some(queue));
                inner.acquires.push(VecDeque::new());
                inner.context_engines.push(engines);
                Ok(inner.queues.len() as u64)
            }
            Err(error) => {
                self.core.pool.release(retire);
                Err(error)
            }
        }
    }
}
/// Perf capture of one accepted batch (capture.rs): drains completed
/// timestamps of every queue and, while recording, writes the batch with the
/// pushbuffer words of each push that lies in a currently CPU-mapped
/// allocation (others are recorded without contents).
fn capture_submit(inner: &mut Inner, context: u64, pushes: &[Push], counts: [u32; 3], token: u64) {
    let Some(mut cap) = inner.capture.take() else {
        return;
    };
    let device = &inner.device;
    let active = cap.active(|| {
        let mut time = 0u64;
        let _ = device.client.control(device.subdevice, 0x20800403, &mut time);
        time
    });
    let mut times = Vec::new();
    for (i, q) in inner.queues.iter_mut().enumerate() {
        if let Some(q) = q.as_mut() {
            q.drain_timing(&mut times);
            if active {
                for (tok, start, end) in times.drain(..) {
                    cap.time(i as u64 + 1, tok, start, end);
                }
            }
            times.clear();
        }
    }
    if active {
        let data: Vec<_> = pushes
            .iter()
            .map(|p| {
                let words = if cap.config.push {
                    capture_push(inner, &cap.binds, p)
                } else {
                    None
                };
                (p.address, p.size, p.flags, words)
            })
            .collect();
        // Texture header / sampler pools this batch binds: their contents
        // as the CPU wrote them, read at submission (capture.rs REC_POOL).
        let pools = crate::capture::pool_binds(data.iter().filter_map(|d| d.3.as_deref()));
        cap.submit(context, token, counts, &data);
        for pool in pools {
            let bytes = capture_read(inner, &cap.binds, pool.address, pool.size);
            cap.pool(token, &pool, bytes.as_deref());
        }
    }
    inner.capture = Some(cap);
}
fn capture_push(
    inner: &Inner,
    binds: &std::collections::BTreeMap<u64, (u64, u64, u64)>,
    p: &Push,
) -> Option<Vec<u8>> {
    capture_read(inner, binds, p.address, p.size)
}
/// Bytes [address, address + size) of a CPU-mapped allocation (None when the
/// range is not inside one bind of a currently CPU-mapped allocation).
fn capture_read(
    inner: &Inner,
    binds: &std::collections::BTreeMap<u64, (u64, u64, u64)>,
    address: u64,
    size: u32,
) -> Option<Vec<u8>> {
    let (&start, &(len, memory, offset)) = binds.range(..=address).next_back()?;
    if address + u64::from(size) > start + len {
        return None;
    }
    let buffer = inner.buffers.get(&memory)?;
    if buffer.cpu.refs() == 0 {
        return None;
    }
    let at = (offset + address - start) as usize;
    let mut words = vec![0u8; size as usize];
    let read = buffer.cpu.acquire(
        || Err(Error::Contract("capture: mapping vanished".into())),
        |m: &Mapping| m.read_bytes(at, &mut words),
    );
    match read {
        Ok(r) => {
            let _ = buffer.cpu.release();
            r.ok().map(|()| words)
        }
        Err(_) => None,
    }
}
/// Plan/commit consistency: submission plans read each payload under its own
/// lock and commit after acceptance (under Inner). Only a host operation on
/// the same payload racing the submit call (a Vulkan external-synchronization
/// violation) can change the slot in between; the GPU then waits on or
/// releases a slot the payload no longer names. Make that visible.
fn check_planned(state: &crate::syncobj::SyncState, planned: Slot, what: &str) {
    if !matches!(state.payload, crate::syncobj::Payload::Native(slot) if slot == planned) {
        diag!(
            "nvk Rust RM: payload changed between {what} planning and commit              (concurrent host operation on a submitted semaphore/fence)"
        );
    }
}
/// Append an accepted batch's acquires; drop entries of batches the GPU has
/// already completed (one mapped-word read, only when something can go).
fn record_acquires(
    inner: &mut Inner,
    context: u64,
    token: u64,
    acquires: impl IntoIterator<Item = Acquire>,
) {
    let index = (context - 1) as usize;
    let list = &mut inner.acquires[index];
    if list.front().is_some() {
        if let Some(Ok(completed)) = inner.queues[index].as_ref().map(|q| q.shared().completed()) {
            while list.front().is_some_and(|(t, _)| *t <= completed) {
                list.pop_front();
            }
        }
    }
    list.extend(acquires.into_iter().map(|a| (token, a)));
}
/// Test hook (fixtures set it in the probe child): a channel freed while it
/// still has unretired work aborts the process instead of only logging, so
/// the violation is observable on bionic too (diagnostics go to logcat).
fn strict_teardown() -> bool {
    std::env::var_os("NV_RM_STRICT_TEARDOWN").is_some_and(|v| v != "0")
}
fn queue_ref(inner: &Inner, context: u64) -> Result<&Queue> {
    context
        .checked_sub(1)
        .and_then(|i| inner.queues.get(i as usize))
        .and_then(Option::as_ref)
        .ok_or_else(|| Error::Contract("unknown RM context".into()))
}
fn deadline(start: Instant, timeout_ns: u64) -> Option<Instant> {
    (timeout_ns != u64::MAX).then(|| start + Duration::from_nanos(timeout_ns))
}

impl Drop for Bridge {
    fn drop(&mut self) {
        // Bounded drain (5 s) on the closing thread, then stop the only other
        // thread, then RM teardown. Nothing continues after this returns:
        // callers may dlclose the DSO immediately.
        let drained = self.drain_timeout(5_000_000_000);
        if let Err(e) = &drained {
            diag!("RM close drain failed; tearing down lost channels: {e}");
            let mut guard = self.lock();
            self.lose_locked(&mut guard.0);
        }
        self.core.watcher.stop(&self.core);
        if drained.is_err() {
            // Release what the lost channels wait on, then let them run out
            // (bounded): never free a channel blocked in an acquire.
            let pending: Vec<_> = {
                let mut guard = self.lock();
                let inner = &mut guard.0;
                (1..=inner.queues.len() as u64)
                    .filter_map(|context| self.unblock_locked(inner, context))
                    .collect()
            };
            self.quiesce(&pending, Duration::from_secs(5));
        }
        let mut guard = self.lock();
        let inner = &mut guard.0;
        for (index, queue) in inner.queues.iter().enumerate() {
            if let Some(queue) = queue {
                Self::check_idle_before_free(index as u64 + 1, queue);
            }
        }
        // Leaked payloads (API misuse) must not touch unmapped pages.
        self.core.pool.close();
        // Free every channel directly, with no schedule-off first (stock
        // order: drain, then RM free; libnvidia-glcore 580 ELF b78280).
        // Channels go first (Inner's first
        // field, so the same order as the implicit drop), then nothing can
        // reach deferred objects any more:
        // unmap/free them and return their VA ranges to C before this
        // returns (nv_rm.h: releases never run after nv_rm_close).
        inner.queues.clear();
        inner.export_queue = None;
        self.reclaim_locked(inner, true);
        // The rest of Inner (mappings, listeners, pool pages, space, device)
        // drops with the Bridge after this function, in field order.
    }
}

fn query_info(d: &Device) -> Result<Info> {
    crate::external::validate(d)?;
    let mut arch = [0u32; 4];
    d.client.control(d.subdevice, 0x20801701, &mut arch)?;
    let mut name = [0u32; 17];
    d.client.control(d.subdevice, 0x20800110, &mut name)?;
    let mut gr = [[0xcu32, 0], [0xd, 0], [0x20, 0]];
    let mut query = abi::GrInfo {
        count: 3,
        entries: gr.as_mut_ptr() as u64,
        ..Default::default()
    };
    d.client.control(d.subdevice, 0x20801201, &mut query)?;
    let caps = capabilities::select(&d.classes()?, gr[0][1])?;
    // Route info is {u32 flags,pad,u64 data}; final mask follows at offset16.
    let mut gpc = [0u64; 3];
    d.client.control(d.subdevice, 0x2080122a, &mut gpc)?;
    let mut tpc_count = 0;
    for i in 0..32 {
        if gpc[2] as u32 & (1 << i) != 0 {
            let mut tpc = [0u64, 0, i as u64];
            d.client.control(d.subdevice, 0x2080122b, &mut tpc)?;
            tpc_count += ((tpc[2] >> 32) as u32).count_ones();
        }
    }
    let mut fb = [0x7u32, 0];
    let mut fbq = [1u64, fb.as_mut_ptr() as u64];
    d.client.control(d.subdevice, 0x20801301, &mut fbq)?;
    let mut text = [0; 64];
    for (i, w) in name[1..].iter().enumerate() {
        text[i * 4..i * 4 + 4].copy_from_slice(&w.to_ne_bytes());
    }
    let c = d.card;
    Ok(Info {
        vendor_id: c.pci.vendor as u32,
        device_id: c.pci.device as u32,
        chipset: arch[0]
            .checked_add(arch[1])
            .ok_or_else(|| Error::Contract("chipset identity overflow".into()))?,
        sm: caps.sm,
        pci_domain: c.pci.domain,
        pci_bus: c.pci.bus as u32,
        pci_device: c.pci.slot as u32,
        pci_function: c.pci.function as u32,
        gpc_count: (gpc[2] as u32).count_ones(),
        tpc_count,
        mp_per_tpc: gr[2][1],
        max_warps_per_mp: gr[1][1],
        cls_copy: caps.copy,
        cls_eng2d: caps.eng2d,
        cls_eng3d: caps.eng3d,
        cls_m2mf: caps.m2mf,
        cls_compute: caps.compute,
        generic_kind: caps.generic_kind,
        vram_size: fb[1] as u64 * 1024,
        bar_size: c.framebuffer_size,
        name: text,
    })
}
/// Every C entry point runs inside this guard: a Rust panic on a caller
/// thread must not unwind into C (process abort) and must not leave callers
/// blocked. It publishes device loss (waking every waiter, as the former
/// worker's liveness guard did) and returns VK_ERROR_DEVICE_LOST. Inner may be
/// poisoned afterwards; later calls only see the loss and tear down.
fn ffi(bridge: *const Bridge, f: impl FnOnce() -> i32) -> i32 {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(status) => status,
        Err(payload) => {
            let message = payload
                .downcast_ref::<&str>()
                .map(|s| s.to_string())
                .or_else(|| payload.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "non-string payload".into());
            // The panic hook (diag::install_panic_hook) logged the location.
            diag!("nvk Rust RM: internal panic on caller thread ({message}); device lost");
            // SAFETY: the caller keeps the bridge alive for this call.
            if let Some(bridge) = unsafe { bridge.as_ref() } {
                bridge.core.mark_lost();
            }
            -4
        }
    }
}
fn report(result: Result<()>) -> i32 {
    match result {
        Ok(()) => 0,
        Err(e) => {
            diag!("nvk Rust RM: {e}");
            match e {
                Error::DeviceLost(_) => -4,
                Error::Io(_, ref error) if error.raw_os_error() == Some(12) => -1,
                _ => -3,
            }
        }
    }
}
unsafe fn bridge<'a>(p: *mut Bridge) -> Result<&'a Bridge> {
    unsafe { p.as_ref() }.ok_or_else(|| Error::Contract("null RM bridge".into()))
}
unsafe fn output<T>(p: *mut T, value: T) -> Result<()> {
    if p.is_null() {
        return Err(Error::Contract("null RM output".into()));
    }
    unsafe { p.write(value) };
    Ok(())
}
unsafe fn input<'a, T>(ptr: *const T, count: u32, maximum: u32) -> Result<&'a [T]> {
    if count > maximum || (count != 0 && ptr.is_null()) {
        return Err(Error::Contract("invalid RM input array".into()));
    }
    Ok(if count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(ptr, count as usize) }
    })
}
fn status_of(result: Result<SubmitStatus>, out: *mut u64) -> i32 {
    let mut status = 0;
    let code = report((|| {
        let token = match result? {
            SubmitStatus::Submitted(token) => token,
            SubmitStatus::Full(oldest) => {
                status = 1;
                oldest
            }
        };
        unsafe { output(out, token) }
    })());
    if code == 0 {
        status
    } else {
        code
    }
}

#[no_mangle]
pub unsafe extern "C" fn nv_rm_open(
    domain: u32,
    bus: u32,
    device: u32,
    function: u32,
    out: *mut *mut Bridge,
    info: *mut Info,
) -> i32 {
    ffi(std::ptr::null(), move || {
        report((|| {
            if out.is_null() || info.is_null() {
                return Err(Error::Contract("null RM open output".into()));
            }
            let (b, i) = Bridge::new([domain, bus, device, function])?;
            unsafe {
                info.write(i);
                out.write(Box::into_raw(Box::new(b)));
            }
            Ok(())
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_close(p: *mut Bridge) {
    if !p.is_null() {
        // Teardown panics must not unwind into C either; the bridge memory is
        // then leaked rather than freed in an unknown state.
        let owned = unsafe { Box::from_raw(p) };
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || drop(owned))).is_err() {
            diag!("nvk Rust RM: panic during device close");
        }
    }
}
/// nv_rm.h NV_RM_PLACEMENT_VRAM_COMPRESSIBLE.
const PLACEMENT_VRAM_COMPRESSIBLE: u32 = 2;
#[no_mangle]
pub unsafe extern "C" fn nv_rm_alloc(
    p: *mut Bridge,
    size: u64,
    alignment: u64,
    placement: u32,
    kind: u32,
    tile: u32,
    out: *mut Allocation,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null allocation output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            // `tile` (nouveau's per-BO tile mode for legacy GL importers) has
            // no RM memory attribute: the block height travels in the DRM
            // modifier. A kind is only meaningful for an uncompressed
            // block-linear VRAM/GART allocation (Device::allocate_block_linear).
            if placement > PLACEMENT_VRAM_COMPRESSIBLE
                || tile != 0
                || (kind != 0 && placement == PLACEMENT_VRAM_COMPRESSIBLE)
            {
                return Err(Error::Contract(
                    "unsupported memory placement/tiled allocation (tile mode, or a kind on compressible memory)".into(),
                ));
            }
            let mut guard = b.lock();
            let s = &mut guard.0;
            // Retired frees first: their memory may be what this needs.
            b.reclaim_locked(s, false);
            let size = usize::try_from(size)
                .map_err(|_| Error::Contract("memory size overflow".into()))?;
            let alignment = usize::try_from(alignment.max(4096))
                .map_err(|_| Error::Contract("memory alignment overflow".into()))?;
            let memory = if placement == PLACEMENT_VRAM_COMPRESSIBLE {
                s.device.allocate_compressible(size, alignment)?
            } else if kind != 0 {
                s.device
                    .allocate_block_linear(size, alignment, placement == 1, kind)?
            } else {
                s.device
                    .allocate_aligned(size, alignment, false, placement == 1)?
            };
            let id = next_id(s)?;
            let a = Allocation {
                id,
                size: memory.size as u64,
                cpu_address: 0,
            };
            s.buffers.insert(
                id,
                Buffer {
                    cpu: s.device.cpu_mapping_slot(),
                    memory,
                },
            );
            unsafe { output(out, a) }
        })())
    })
}
fn next_id(s: &mut Inner) -> Result<u64> {
    let id = s.next;
    s.next = id
        .checked_add(1)
        .ok_or_else(|| Error::Contract("RM object ID exhausted".into()))?;
    Ok(id)
}
/// Logical maps are independent from the lazy retained allocation mapping.
/// Idle mappings remain reclaimable; allocation free performs real teardown.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_map(p: *mut Bridge, memory: u64, out: *mut usize) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null CPU map output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let mut guard = b.lock();
            let s = &mut guard.0;
            b.reclaim_locked(s, false);
            let buffer = s
                .buffers
                .get(&memory)
                .ok_or_else(|| Error::Contract("map unknown RM memory".into()))?;
            let result = buffer
                .cpu
                .acquire(|| buffer.memory.map(), Mapping::cpu_address);
            if s.device.cpu_mapping_failed() {
                b.lose_locked(s);
            }
            let address = result?;
            unsafe { output(out, address) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_unmap(p: *mut Bridge, memory: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            // Cleanup remains possible after loss. A failed final teardown keeps
            // both its logical reference and the partially released lease owned.
            let mut guard = b.lock();
            let s = &mut guard.0;
            let result = s
                .buffers
                .get(&memory)
                .ok_or_else(|| Error::Contract("unmap unknown RM memory".into()))?
                .cpu
                .release();
            if result.is_err() {
                b.lose_locked(s);
            }
            result
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_free(p: *mut Bridge, memory: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            // Never waits for GPU work (it may be gated on a fence this very
            // thread must produce): the RM object is freed once the work
            // outstanding now has retired (reclaim.rs). Cleanup stays possible
            // after loss; lost queues keep the object until close.
            let mut guard = b.lock();
            let s = &mut guard.0;
            let buffer = s
                .buffers
                .get(&memory)
                .ok_or_else(|| Error::Contract("free unknown RM memory".into()))?;
            if buffer.cpu.refs() != 0 {
                return Err(Error::Contract(
                    "free CPU-mapped RM memory; unmap it first".into(),
                ));
            }
            // The CPU VMA is not GPU-visible: evict it now.
            if let Err(error) = buffer.cpu.evict() {
                // Do not remove the buffer or free its RM handle while cleanup
                // is incomplete; explicit retry/teardown retains ownership.
                b.lose_locked(s);
                return Err(error);
            }
            let buffer = s.buffers.remove(&memory).unwrap();
            b.reclaim_or_defer(s, [ReclaimItem::Memory(buffer)])
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_bind(
    p: *mut Bridge,
    memory: u64,
    offset: u64,
    size: u64,
    address: u64,
    kind: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null bind output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if address < 1 << 34 || address.checked_add(size).is_none_or(|n| n > 1 << 40) {
                return Err(Error::Contract("Mesa VA outside reserved range".into()));
            }
            let mut guard = b.lock();
            let s = &mut guard.0;
            b.reclaim_locked(s, false);
            let buffer = s
                .buffers
                .get(&memory)
                .ok_or_else(|| Error::Contract("bind unknown RM memory".into()))?;
            let map = s
                .space
                .map_range(&buffer.memory, offset, size, Some(address), kind)?;
            if map.address() != address {
                return Err(Error::Contract(
                    "RM did not honor fixed mapping address".into(),
                ));
            }
            let id = next_id(s)?;
            s.maps.insert(id, map);
            s.bind_info.insert(id, (address, size, memory, offset));
            if let Some(cap) = s.capture.as_mut() {
                // VA → allocation index for pushbuffer capture; a reused VA
                // replaces every stale overlapping entry.
                let stale: Vec<u64> = cap
                    .binds
                    .range(..address + size)
                    .filter(|(&start, &(len, _, _))| start + len > address)
                    .map(|(&start, _)| start)
                    .collect();
                for start in stale {
                    cap.binds.remove(&start);
                }
                cap.binds.insert(address, (size, memory, offset));
            }
            unsafe { output(out, id) }
        })())
    })
}
/// Immediate RM unmap, no GPU wait. Only for application-ordered Vulkan
/// binds: Vulkan forbids rebinding/unbinding memory that pending work uses
/// (see nv_rm.h). Driver teardown of a VA uses nv_rm_va_free instead.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_unbind(p: *mut Bridge, mapping: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            let mut guard = b.lock();
            let s = &mut guard.0;
            b.reclaim_locked(s, false);
            let result = s
                .maps
                .get_mut(&mapping)
                .ok_or_else(|| Error::Contract("unbind unknown GPU mapping".into()))?
                .try_unmap();
            if let Err(error) = result {
                // Keep the mapping and typed-VA reservation owned. C must not
                // recycle this address after a failed kernel unmap.
                b.lose_locked(s);
                return Err(Error::DeviceLost(format!(
                    "GPU unmap failed; reservation retained: {error}"
                )));
            }
            s.maps.remove(&mapping);
            s.bind_info.remove(&mapping);
            Ok(())
        })())
    })
}
/// Release a VA reservation and its mappings without waiting for the GPU:
/// the RM unmaps run and `release(data, address, size)` hands the range back
/// to the caller's heap only once every queue's work submitted before this
/// call has retired (immediately when nothing is outstanding). See nv_rm.h.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_va_free(
    p: *mut Bridge,
    mappings: *const u64,
    count: u32,
    address: u64,
    size: u64,
    release: VaRelease,
    data: *mut c_void,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            let ids = unsafe { input(mappings, count, 65536) }?;
            let release =
                release.ok_or_else(|| Error::Contract("null VA release callback".into()))?;
            let end = address
                .checked_add(size)
                .filter(|&end| size != 0 && address >= 1 << 34 && end <= 1 << 40)
                .ok_or_else(|| Error::Contract("VA free outside reserved range".into()))?;
            let mut guard = b.lock();
            let s = &mut guard.0;
            // Transactional: validate every id (known, inside this range, no
            // duplicates) before taking any; on error C keeps the range.
            for (i, id) in ids.iter().enumerate() {
                let map = s
                    .maps
                    .get(id)
                    .ok_or_else(|| Error::Contract("VA free of unknown GPU mapping".into()))?;
                if map.address() < address
                    || map.address().saturating_add(map.length()) > end
                    || ids[..i].contains(id)
                {
                    return Err(Error::Contract(
                        "VA free mapping outside its range or repeated".into(),
                    ));
                }
            }
            if ids.is_empty() {
                // Nothing is mapped in the range: no GPU access can reach it.
                b.reclaim_locked(s, false);
                unsafe { release(data, address, size) };
                return Ok(());
            }
            // Group: unmaps, then the range release (in order). The scratch
            // vector keeps its capacity: no allocation per free.
            let mut freeing = std::mem::take(&mut s.freeing);
            for id in ids {
                s.bind_info.remove(id);
            }
            freeing.extend(
                ids.iter()
                    .map(|id| ReclaimItem::Mapping(s.maps.remove(id).unwrap())),
            );
            freeing.push(ReclaimItem::Va {
                address,
                size,
                release,
                data,
            });
            let result = b.reclaim_or_defer(s, freeing.drain(..));
            s.freeing = freeing;
            result
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_context_create(p: *mut Bridge, engines: u32, out: *mut u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null context output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let mut guard = b.lock();
            let id = b.create_context(&mut guard.0, engines)?;
            unsafe { output(out, id) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_context_destroy(p: *mut Bridge, id: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            let token = queue_ref(&b.lock().0, id)?.last_token();
            // Bounded quiesce: an unsignaled (e.g. foreign-gated) batch is lost
            // after 5 s; its acquires are then released so the channel runs
            // out, and only the idle channel is RM-freed (no schedule-off
            // first, as stock) before memory is released.
            if !b.wait_token(id, token, 5_000_000_000).unwrap_or(false) {
                let pending = {
                    let mut guard = b.lock();
                    b.lose_locked(&mut guard.0);
                    b.unblock_locked(&mut guard.0, id)
                };
                // Released acquires let the channel run out before its free.
                b.quiesce(pending.as_slice(), Duration::from_secs(5));
            }
            let mut guard = b.lock();
            let s = &mut guard.0;
            let queue = s
                .queues
                .get_mut((id - 1) as usize)
                .and_then(Option::take)
                .ok_or_else(|| Error::Contract("destroy unknown context".into()))?;
            Bridge::check_idle_before_free(id, &queue);
            s.acquires[(id - 1) as usize].clear();
            drop(queue);
            // Groups that waited only on this queue can go now.
            b.reclaim_locked(s, false);
            Ok(())
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_submit_waits(
    p: *mut Bridge,
    id: u64,
    pushes: *const Push,
    count: u32,
    drm_fd: i32,
    wait_fds: *const i32,
    wait_count: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || {
        let result = (|| {
            if out.is_null() {
                return Err(Error::Contract("null submit token output".into()));
            }
            let pushes = unsafe { input(pushes, count, 1000) }?;
            let fds = unsafe { input(wait_fds, wait_count, 1024) }?;
            if !fds.is_empty() && drm_fd < 0 {
                return Err(Error::Contract("missing dependency DRM fd".into()));
            }
            // Retain borrowed FDs before returning. We own the CPU->GPU gate;
            // no uncancellable NVIDIA DRM WAIT callbacks are installed.
            let mut foreign = Vec::with_capacity(fds.len());
            for &fd in fds {
                if fd < 0 {
                    return Err(Error::Contract("negative dependency fd".into()));
                }
                let status = crate::fences::sync_status(fd)?;
                let owned = unsafe { BorrowedFd::borrow_raw(fd) }
                    .try_clone_to_owned()
                    .map_err(|e| Error::Io("retain submit dependency", e))?;
                foreign.push(Arc::new(Foreign::new(owned, status)));
            }
            unsafe { bridge(p) }?.submit(id, pushes, &[], &foreign, &[])
        })();
        status_of(result, out)
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_submit(
    p: *mut Bridge,
    id: u64,
    pushes: *const Push,
    count: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || unsafe {
        nv_rm_submit_waits(p, id, pushes, count, -1, std::ptr::null(), 0, out)
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_poll(
    p: *mut Bridge,
    id: u64,
    token: u64,
    complete: *mut u32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if complete.is_null() {
                return Err(Error::Contract("null poll output".into()));
            }
            let b = unsafe { bridge(p) }?;
            let done = b.wait_token(id, token, 0)?;
            b.reclaim_try();
            unsafe { output(complete, u32::from(done)) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_wait(p: *mut Bridge, id: u64, token: u64, timeout_ns: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        let mut status = 0;
        let result = report((|| {
            let b = unsafe { bridge(p) }?;
            if !b.wait_token(id, token, timeout_ns)? {
                status = 2;
            }
            b.reclaim_try();
            Ok(())
        })());
        if result == 0 {
            status
        } else {
            result
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_context_fence(
    p: *mut Bridge,
    id: u64,
    token: u64,
    drm_fd: i32,
    out: *mut i32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() || drm_fd < 0 {
                return Err(Error::Contract("invalid context fence output/fd".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if b.capture.load(std::sync::atomic::Ordering::Relaxed) {
                if let Some(cap) = b.lock().0.capture.as_mut() {
                    cap.mark(id, token, 1);
                }
            }
            let fd = queue_ref(&b.lock().0, id)?.context_fence(drm_fd, token)?;
            unsafe { output(out, fd.into_raw_fd()) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_exec(
    p: *mut Bridge,
    id: u64,
    pushes: *const Push,
    count: u32,
) -> i32 {
    ffi(p as *const Bridge, move || loop {
        let mut token = 0;
        let result = unsafe { nv_rm_submit(p, id, pushes, count, &mut token) };
        if result != 0 && result != 1 {
            return result;
        }
        let wait = unsafe { nv_rm_wait(p, id, token, u64::MAX) };
        if wait != 0 {
            return wait;
        }
        if result == 0 {
            return 0;
        }
    })
}
/// Mesa's device-status hook (after Vulkan waits): loss flag plus the
/// throttled authoritative channel scan; foreign/shared producer errors were
/// already published as loss by their observer.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_check_status(p: *mut Bridge) -> i32 {
    ffi(p as *const Bridge, move || {
        let result = report((|| {
            let b = unsafe { bridge(p) }?;
            b.check()?;
            b.reclaim_try();
            Ok(())
        })());
        if result == 0 {
            0
        } else {
            -4 // VK_ERROR_DEVICE_LOST
        }
    })
}
/// Mesa's software loss paths must wake caller waits too. This callback can
/// run under a Mesa queue mutex: atomics, condvar notify and eventfd writes
/// only — never Inner, RM ioctls or GPU waits. Submission stop and shared
/// failure publication follow on the next locked call / watcher / close.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_mark_lost(p: *const Bridge) {
    if let Some(bridge) = unsafe { p.as_ref() } {
        bridge.core.mark_lost();
    }
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_is_lost(p: *const Bridge) -> i32 {
    ffi(p, move || {
        if p.is_null() {
            return 1;
        }
        i32::from(unsafe { &*p }.core.is_lost())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_timestamp(p: *mut Bridge, out: *mut u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            let guard = b.lock();
            let device = &guard.0.device;
            let mut time = 0u64;
            device
                .client
                .control(device.subdevice, 0x20800403, &mut time)?;
            unsafe { output(out, time) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_export(
    p: *mut Bridge,
    memory: u64,
    drm_fd: i32,
    out: *mut i32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if drm_fd < 0 || out.is_null() {
                return Err(Error::Contract("invalid external-memory output/fd".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let guard = b.lock();
            let buffer = guard
                .0
                .buffers
                .get(&memory)
                .ok_or_else(|| Error::Contract("export unknown allocation".into()))?;
            // Caller retains fd for the duration of this synchronous call.
            let fd =
                crate::external::export(&buffer.memory, unsafe { BorrowedFd::borrow_raw(drm_fd) })?;
            unsafe { output(out, fd.into_raw_fd()) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_import(
    p: *mut Bridge,
    drm_fd: i32,
    dma_fd: i32,
    out: *mut Allocation,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if drm_fd < 0 || dma_fd < 0 || out.is_null() {
                return Err(Error::Contract("invalid dma-buf import arguments".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let mut guard = b.lock();
            let s = &mut guard.0;
            b.reclaim_locked(s, false);
            let memory = crate::external::import(
                &s.device,
                unsafe { BorrowedFd::borrow_raw(drm_fd) },
                unsafe { BorrowedFd::borrow_raw(dma_fd) },
            )?;
            let id = next_id(s)?;
            let result = Allocation {
                id,
                size: memory.size as u64,
                cpu_address: 0,
            };
            s.buffers.insert(
                id,
                Buffer {
                    cpu: s.device.cpu_mapping_slot(),
                    memory,
                },
            );
            unsafe { output(out, result) }
        })())
    })
}

// ---- synchronization ABI (all caller-side) ------------------------------
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_create(
    p: *mut Bridge,
    flags: u32,
    initial: u64,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() || flags & !7 != 0 {
                return Err(Error::Contract("invalid sync create arguments".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let slot = b.slot()?;
            let sync = match SyncObj::new(&b.core.pool, flags & 1 != 0, initial, slot) {
                Ok(sync) => sync,
                Err(error) => {
                    b.core.pool.release(slot);
                    return Err(error);
                }
            };
            let id = Arc::into_raw(Arc::new(sync)) as usize as u64;
            unsafe { output(out, id) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_destroy(_p: *mut Bridge, id: u64) -> i32 {
    ffi(_p as *const Bridge, move || {
        report((|| {
            sync_ref(id)?;
            // SAFETY: id is the Arc pointer created by nv_rm_sync_create; Mesa
            // destroys each vk_sync exactly once. Accepted submissions own their
            // slot references independently of this payload.
            drop(unsafe { Arc::from_raw(id as usize as *const SyncObj) });
            Ok(())
        })())
    })
}
/// Kept for ABI stability: destruction is now immediate on the caller (there
/// is no worker queue to defer to); Mesa's void finish callback still uses it.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_destroy_deferred(p: *mut Bridge, id: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        if p.is_null() {
            return report(Err(Error::Contract("null RM bridge".into())));
        }
        unsafe { nv_rm_sync_destroy(p, id) }
    })
}
fn needs_slot(error: &Error) -> bool {
    matches!(error, Error::Contract(m) if m.ends_with("needs a replacement slot"))
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_reset(p: *mut Bridge, id: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let sync = sync_ref(id)?;
            let mut fresh = if sync.needs_fresh_slot() {
                Some(b.slot()?)
            } else {
                None
            };
            loop {
                match sync.reset(fresh.take()) {
                    Ok((unused, changed)) => {
                        if let Some(slot) = unused {
                            b.core.pool.release(slot);
                        }
                        b.broadcast(changed);
                        return Ok(());
                    }
                    Err(error) if needs_slot(&error) => fresh = Some(b.slot()?),
                    Err(error) => return Err(error),
                }
            }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_move(
    p: *mut Bridge,
    dst: u64,
    src: u64,
    _shared_source: u32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if src == dst {
                return Err(Error::Contract("invalid binary sync move".into()));
            }
            let (source, target) = (sync_ref(src)?, sync_ref(dst)?);
            if source.timeline || target.timeline {
                return Err(Error::Contract("invalid binary sync move".into()));
            }
            let fresh = b.slot()?;
            let (moved, changed) = match source.take_for_move(fresh) {
                Ok(moved) => moved,
                Err(error) => {
                    b.core.pool.release(fresh);
                    return Err(error);
                }
            };
            target.install_moved(moved);
            b.broadcast(changed);
            Ok(())
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_signal(p: *mut Bridge, id: u64, value: u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            let sync = sync_ref(id)?;
            let mut fresh = if !sync.timeline && sync.needs_fresh_slot() {
                Some(b.slot()?)
            } else {
                None
            };
            loop {
                match sync.signal(value, fresh.take()) {
                    Ok((unused, changed)) => {
                        if let Some(slot) = unused {
                            b.core.pool.release(slot);
                        }
                        b.broadcast(changed);
                        // Dekker: store above, then wake local waiters if any.
                        b.core.waiters.notify();
                        return Ok(());
                    }
                    Err(error) if needs_slot(&error) => fresh = Some(b.slot()?),
                    Err(error) => return Err(error),
                }
            }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_get_value(p: *mut Bridge, id: u64, out: *mut u64) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null counter output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.check()?;
            let (value, changed) = sync_ref(id)?.value()?;
            b.broadcast(changed);
            unsafe { output(out, value) }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_status_many(
    p: *mut Bridge,
    points: *const SyncPoint,
    count: u32,
    flags: u32,
    out: *mut u32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() || flags & !3 != 0 {
                return Err(Error::Contract("invalid sync status arguments".into()));
            }
            let points = unsafe { input(points, count, 65536) }?;
            let b = unsafe { bridge(p) }?;
            b.check()?;
            let ready = b.points_ready(points, flags)?;
            b.reclaim_try();
            unsafe { output(out, u32::from(ready)) }
        })())
    })
}
/// Returns 0 on completion, 2 on caller timeout, negative on failure. Waits
/// on the calling thread (leader/follower on the device HOST descriptor).
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_wait_many(
    p: *mut Bridge,
    points: *const SyncPoint,
    count: u32,
    flags: u32,
    timeout_ns: u64,
) -> i32 {
    ffi(p as *const Bridge, move || {
        let result = (|| {
            if flags & !3 != 0 {
                return Err(Error::Contract("invalid sync wait flags".into()));
            }
            let points = unsafe { input(points, count, 65536) }?;
            let b = unsafe { bridge(p) }?;
            let ready = b.wait_points(points, flags, timeout_ns)?;
            b.reclaim_try();
            Ok(ready)
        })();
        match result {
            Ok(true) => 0,
            Ok(false) => 2,
            Err(e) => report(Err(e)),
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_import_fd(
    p: *mut Bridge,
    id: u64,
    fd: i32,
    opaque: u32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if fd < 0 {
                return Err(Error::Contract("invalid synchronization import fd".into()));
            }
            let sync = sync_ref(id)?;
            let fd = unsafe { BorrowedFd::borrow_raw(fd) };
            if opaque != 0 {
                sync.import_opaque(fd)
            } else {
                sync.import_sync_file(fd)
            }
        })())
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_export_fd(
    p: *mut Bridge,
    id: u64,
    opaque: u32,
    drm_fd: i32,
    out: *mut i32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if out.is_null() {
                return Err(Error::Contract("null synchronization fd output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if b.capture.load(std::sync::atomic::Ordering::Relaxed) {
                if let Some(cap) = b.lock().0.capture.as_mut() {
                    cap.mark(0, id, 2 + opaque);
                }
            }
            let fd = if opaque != 0 {
                let shared = sync_ref(id)?.share()?;
                if shared.has_local_work() {
                    let mut guard = b.lock();
                    b.ensure_watcher_host(&mut guard.0)?;
                    drop(guard);
                    b.core.watcher.publish(&b.core, Arc::clone(&shared))?;
                }
                Some(shared.export()?)
            } else {
                b.export_sync_file(id, drm_fd)?
            };
            unsafe { output(out, fd.map_or(-1, IntoRawFd::into_raw_fd)) }
        })())
    })
}
/// Reserve a binary signal for a deferred (submit-thread) submission: `dst`
/// becomes the handle the deferred batch signals, bound to `src`'s current
/// slot; `src` reports the signal as pending. `*reserved` = 0 when `src`
/// cannot be reserved (not an unsignaled native binary without a pending
/// signal), in which case nothing changed.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_sync_reserve(
    p: *mut Bridge,
    dst: u64,
    src: u64,
    reserved: *mut u32,
) -> i32 {
    ffi(p as *const Bridge, move || {
        report((|| {
            if reserved.is_null() {
                return Err(Error::Contract("null reservation output".into()));
            }
            let b = unsafe { bridge(p) }?;
            b.core.live()?;
            if src == dst {
                return Err(Error::Contract("invalid binary signal reservation".into()));
            }
            let (done, changed) = sync_ref(src)?.reserve_into(sync_ref(dst)?)?;
            b.broadcast(changed);
            // WAIT_PENDING waiters observe the reservation as a submission.
            std::sync::atomic::fence(Ordering::SeqCst);
            if done && b.core.pending_waiters.load(Ordering::SeqCst) != 0 {
                b.core.waiters.notify();
            }
            unsafe { output(reserved, u32::from(done)) }
        })())
    })
}
/// Submit commands, dependencies and final signal payloads in one transaction.
#[no_mangle]
pub unsafe extern "C" fn nv_rm_submit_batch(
    p: *mut Bridge,
    id: u64,
    pushes: *const Push,
    count: u32,
    waits: *const SyncPoint,
    wait_count: u32,
    signals: *const SyncPoint,
    signal_count: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || {
        let result = (|| {
            if out.is_null() {
                return Err(Error::Contract("null submit token output".into()));
            }
            let pushes = unsafe { input(pushes, count, 1000) }?;
            let waits = unsafe { input(waits, wait_count, 1024) }?;
            let signals = unsafe { input(signals, signal_count, 65536) }?;
            unsafe { bridge(p) }?.submit(id, pushes, waits, &[], signals)
        })();
        status_of(result, out)
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_submit_sync(
    p: *mut Bridge,
    id: u64,
    pushes: *const Push,
    count: u32,
    waits: *const SyncPoint,
    wait_count: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || unsafe {
        nv_rm_submit_batch(
            p,
            id,
            pushes,
            count,
            waits,
            wait_count,
            std::ptr::null(),
            0,
            out,
        )
    })
}
#[no_mangle]
pub unsafe extern "C" fn nv_rm_context_signal(
    p: *mut Bridge,
    id: u64,
    signals: *const SyncPoint,
    count: u32,
    out: *mut u64,
) -> i32 {
    ffi(p as *const Bridge, move || unsafe {
        nv_rm_submit_batch(
            p,
            id,
            std::ptr::null(),
            0,
            std::ptr::null(),
            0,
            signals,
            count,
            out,
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{offset_of, size_of};
    #[test]
    fn bridge_info_matches_independent_c_oracle() {
        // mesa/abi-check.c: host and Android compile from the public bridge header.
        assert_eq!(size_of::<Info>(), 152);
        assert_eq!(offset_of!(Info, generic_kind), 68);
        assert_eq!(offset_of!(Info, vram_size), 72);
        assert_eq!(offset_of!(Info, bar_size), 80);
        assert_eq!(offset_of!(Info, name), 88);
        assert_eq!(offset_of!(Allocation, cpu_address), 16);
        assert_eq!(size_of::<SyncPoint>(), 16);
    }
    #[test]
    fn caller_panic_becomes_device_lost_instead_of_unwinding_into_c() {
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let status = ffi(std::ptr::null(), || panic!("injected backend panic"));
        std::panic::set_hook(hook);
        assert_eq!(status, -4);
        assert_eq!(ffi(std::ptr::null(), || 7), 7);
    }
    #[test]
    fn null_and_invalid_handles_fail_without_touching_state() {
        assert_eq!(unsafe { nv_rm_is_lost(std::ptr::null()) }, 1);
        unsafe { nv_rm_mark_lost(std::ptr::null()) };
        assert_ne!(
            unsafe { nv_rm_sync_destroy_deferred(std::ptr::null_mut(), 7) },
            0
        );
        assert_ne!(unsafe { nv_rm_sync_reset(std::ptr::null_mut(), 7) }, 0);
        assert!(sync_ref(0).is_err());
    }
}

#[cfg(test)]
#[path = "mesa_hardware_tests.rs"]
mod hardware_tests;

#[cfg(test)]
#[path = "mesa_reclaim_tests.rs"]
mod reclaim_tests;
