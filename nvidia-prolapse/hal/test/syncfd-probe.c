/* syncfd-probe: the two NVIDIA driver defects with temporarily imported SYNC_FD payloads that make the HAL's
 * vkAcquireImageANDROID wait for the native fence (src/device.c, acquire_wait_flags). Runs on the RAW ICD, nothing
 * of ours in between, so a driver bump can be checked in a minute:
 *
 *   syncfd-probe --icd /usr/lib/libGLX_nvidia.so.0      (or `make syncfd-check`)
 *
 * Each case runs in its own child process (a crash is a result, not the end of the run):
 *   fence-pending-wait   vkWaitForFences on a fence whose TEMPORARY SYNC_FD payload has not signalled yet.
 *                        DEFECT = the child dies (550.163.01: SIGSEGV in libnvidia-eglcore.so.550.163.01 at
 *                        ELF 0x9f6276, the pollfd setup before poll()); 555.58.02 and 560.35.03 too.
 *   sem-signalled-leak   4 x 2 rounds: export a SYNC_FD from a fence of an empty submission (already signalled when
 *                        imported), import it TEMPORARY into a binary semaphore, submit a wait on it. DEFECT = the
 *                        process holds more fds after round 2 than after round 1 (580.178.04: 3 per 4 waits, kept
 *                        through vkDestroySemaphore and vkDestroyDevice).
 *   sem-pending          the same with a sync_file that is still pending at the import (control: no fd kept).
 *   sem-minus-one        the same with fd -1, "already signalled" (control: what the HAL imports instead).
 *
 * A pending payload is made with a timeline semaphore that the submission waits on and the host signals later.
 * Exit status: 0 = no defect, 1 = at least one defect case failed, 2 = setup failed (no NVIDIA device, missing
 * extension or feature). Prints one line per case plus the driver's branch and the HAL's matching decision.
 */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FN(n) static PFN_vk##n p##n
FN(CreateInstance); FN(EnumeratePhysicalDevices); FN(GetPhysicalDeviceProperties); FN(GetPhysicalDeviceProperties2);
FN(CreateDevice); FN(GetDeviceProcAddr); FN(GetDeviceQueue); FN(CreateFence); FN(DestroyFence); FN(CreateSemaphore);
FN(DestroySemaphore); FN(QueueSubmit); FN(QueueWaitIdle); FN(WaitForFences); FN(GetFenceFdKHR); FN(ImportFenceFdKHR);
FN(ImportSemaphoreFdKHR); FN(SignalSemaphore); FN(DestroyDevice);
static VkDevice dev;
static VkQueue queue;

static void die(const char *what, VkResult r) {
    printf("FATAL: %s -> %d\n", what, r);
    exit(2);
}
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die(#x, r_); } while (0)

static int count_fds(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) n += e->d_name[0] != '.';
    closedir(d);
    return n - 1; /* the directory's own fd */
}

static VkSemaphore timeline(void) {
    VkSemaphoreTypeCreateInfo t = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                   .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &t};
    VkSemaphore s;
    CK(pCreateSemaphore(dev, &ci, NULL, &s));
    return s;
}
static void host_signal(VkSemaphore tl) {
    VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = tl, .value = 1};
    CK(pSignalSemaphore(dev, &si));
}
/* a SYNC_FD of a submission on `queue`; with tl, the submission waits for tl >= 1 (pending until host_signal) */
static int export_fd(VkFence xf, VkSemaphore tl) {
    uint64_t one = 1;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &one};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (tl) {
        si.pNext = &ts;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &tl;
        si.pWaitDstStageMask = &st;
    }
    CK(pQueueSubmit(queue, 1, &si, xf));
    VkFenceGetFdInfoKHR g = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = xf,
                             .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -1;
    CK(pGetFenceFdKHR(dev, &g, &fd));
    return fd;
}
static void import_sem(VkSemaphore s, int fd) {
    VkImportSemaphoreFdInfoKHR i = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = s,
                                    .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd};
    CK(pImportSemaphoreFdKHR(dev, &i));
}
static void wait_sem(VkSemaphore s) {
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &s,
                      .pWaitDstStageMask = &st};
    CK(pQueueSubmit(queue, 1, &w, VK_NULL_HANDLE));
}

static VkFence export_fence(void) {
    VkExportFenceCreateInfo e = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                 .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &e};
    VkFence f;
    CK(pCreateFence(dev, &ci, NULL, &f));
    return f;
}

/* child: exit 0 = correct, 1 = defect seen (a crash shows as a signal) */
static int case_fence_pending_wait(void) {
    VkSemaphore tl = timeline();
    VkFence xf = export_fence(), af;
    VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CK(pCreateFence(dev, &ci, NULL, &af));
    int fd = export_fd(xf, tl);
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 0) != 0) { printf("    sync_file %d not pending at the import\n", fd); return 1; }
    VkImportFenceFdInfoKHR i = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, .fence = af,
                                .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
                                .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd};
    CK(pImportFenceFdKHR(dev, &i));
    fflush(stdout);
    VkResult w1 = pWaitForFences(dev, 1, &af, VK_TRUE, 100000000ull);
    host_signal(tl);
    VkResult w2 = pWaitForFences(dev, 1, &af, VK_TRUE, 5000000000ull);
    CK(pQueueWaitIdle(queue));
    printf("    vkWaitForFences while pending -> %d (want VK_TIMEOUT), after the signal -> %d\n", w1, w2);
    return !(w1 == VK_TIMEOUT && w2 == VK_SUCCESS);
}

enum sem_kind { SEM_SIGNALLED, SEM_PENDING, SEM_MINUS_ONE };
static int case_sem(enum sem_kind kind) {
    VkFence xf = export_fence();
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    CK(pCreateSemaphore(dev, &ci, NULL, &s));
    int fds[3];
    fds[0] = count_fds();
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 4; i++) {
            VkSemaphore tl = kind == SEM_PENDING ? timeline() : VK_NULL_HANDLE;
            int fd = kind == SEM_MINUS_ONE ? -1 : export_fd(xf, tl);
            import_sem(s, fd);
            wait_sem(s);
            if (tl) {
                host_signal(tl);
                CK(pQueueWaitIdle(queue));
                pDestroySemaphore(dev, tl, NULL);
            }
        }
        CK(pQueueWaitIdle(queue));
        fds[round + 1] = count_fds();
    }
    pDestroySemaphore(dev, s, NULL);
    pDestroyFence(dev, xf, NULL);
    printf("    open fds %d -> %d (round 1) -> %d (round 2)\n", fds[0], fds[1], fds[2]);
    return fds[2] != fds[1];
}

static void setup(int quiet);
static int run_case(const char *name, int (*fn)(int), int arg) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setup(1);
        int r = fn(arg);
        pDestroyDevice(dev, NULL);
        fflush(stdout);
        _exit(r);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    const char *verdict = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok" : "DEFECT";
    if (WIFSIGNALED(st)) printf("  %-20s DEFECT (child killed by signal %d)\n", name, WTERMSIG(st));
    else printf("  %-20s %s\n", name, verdict);
    return strcmp(verdict, "ok") != 0;
}
static int fence_case(int unused) { (void)unused; return case_fence_pending_wait(); }
static int sem_case(int kind) { return case_sem((enum sem_kind)kind); }

static const char *icd;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};

/* the ICD, an instance and a device: in each child, never across fork() */
static void setup(int quiet) {
    void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FATAL: dlopen(%s): %s\n", icd, dlerror()); exit(2); }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { printf("FATAL: %s has no vk_icdGetInstanceProcAddr\n", icd); exit(2); }
    pCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst;
    CK(pCreateInstance(&ici, NULL, &inst));
#define IFN(n) p##n = (PFN_vk##n)gipa(inst, "vk" #n)
    IFN(EnumeratePhysicalDevices); IFN(GetPhysicalDeviceProperties); IFN(GetPhysicalDeviceProperties2);
    IFN(CreateDevice); IFN(GetDeviceProcAddr);
    VkPhysicalDevice pds[16], pd = VK_NULL_HANDLE;
    uint32_t n = 16;
    CK(pEnumeratePhysicalDevices(inst, &n, pds));
    for (uint32_t i = 0; i < n && !pd; i++) {
        pGetPhysicalDeviceProperties(pds[i], &props);
        if (props.vendorID == 0x10de) pd = pds[i]; /* by vendor ID, never by node path */
    }
    if (!pd || props.apiVersion < VK_API_VERSION_1_2) { printf("FATAL: no NVIDIA Vulkan 1.2 device\n"); exit(2); }
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    pGetPhysicalDeviceProperties2(pd, &p2);
    if (!quiet) printf("device: %s, driver %s %s (branch %u)\n", props.deviceName, drv.driverName, drv.driverInfo,
           props.driverVersion >> 22);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {"VK_KHR_external_fence_fd", "VK_KHR_external_semaphore_fd"};
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                     .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &tsf, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    CK(pCreateDevice(pd, &dci, NULL, &dev));
#define DFN(n) do { p##n = (PFN_vk##n)pGetDeviceProcAddr(dev, "vk" #n); if (!p##n) die("vkGetDeviceProcAddr vk" #n, 0); } while (0)
    DFN(GetDeviceQueue); DFN(CreateFence); DFN(DestroyFence); DFN(CreateSemaphore); DFN(DestroySemaphore);
    DFN(QueueSubmit); DFN(QueueWaitIdle); DFN(WaitForFences); DFN(GetFenceFdKHR); DFN(ImportFenceFdKHR);
    DFN(ImportSemaphoreFdKHR); DFN(SignalSemaphore); DFN(DestroyDevice);
    pGetDeviceQueue(dev, 0, 0, &queue);

}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
    if (!icd) { printf("usage: %s --icd libGLX_nvidia.so.0\n", argv[0]); return 2; }
    /* the device identity, in a child too: no driver state in the parent that forks the cases */
    int pipefd[2];
    if (pipe(pipefd)) return 2;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setup(0);
        uint32_t id[2] = {props.driverVersion, drv.driverID};
        fflush(stdout);
        _exit(write(pipefd[1], id, sizeof id) == sizeof id ? 0 : 2);
    }
    uint32_t id[2] = {0, 0};
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || read(pipefd[0], id, sizeof id) != sizeof id) return 2;

    int fence_bad = run_case("fence-pending-wait", fence_case, 0);
    int sem_bad = run_case("sem-signalled-leak", sem_case, SEM_SIGNALLED);
    int controls = run_case("sem-pending", sem_case, SEM_PENDING) + run_case("sem-minus-one", sem_case, SEM_MINUS_ONE);
    uint32_t branch = id[0] >> 22;
    int hal_fence = id[1] == VK_DRIVER_ID_NVIDIA_PROPRIETARY && branch < 565;
    int hal_sem = id[1] == VK_DRIVER_ID_NVIDIA_PROPRIETARY && branch == 580;
    printf("HAL decision for branch %u: wait before a fence import %s, before a semaphore import %s\n", branch,
           hal_fence ? "yes" : "no", hal_sem ? "yes" : "no");
    if ((fence_bad && !hal_fence) || (sem_bad && !hal_sem))
        printf("MISMATCH: a defect is not covered by the HAL's acquire_wait_flags (src/device.c)\n");
    printf("%s: fence-pending-wait %s, sem-signalled-leak %s, controls %s\n",
           fence_bad || sem_bad || controls ? "DEFECT" : "PASS", fence_bad ? "DEFECT" : "ok", sem_bad ? "DEFECT" : "ok",
           controls ? "DEFECT" : "ok");
    return fence_bad || sem_bad || controls ? 1 : 0;
}
