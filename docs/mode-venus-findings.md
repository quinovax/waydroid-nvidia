# venus 模式的 3DMark 实测（2026-10-25 21:39-21:50）

## 结论（一句话）

**venus 模式下 3DMark 根本进不到 workload**：它在自己的启动器 UI 上 ANR（ConstraintLayout 求解在 ART 解释器里跑），所以「venus 的 3DMark 分数」目前不是 GPU 结论，而是 UI 侧卡死。

## 事实

- 切模式成功：`NVIDIA driver mode: venus (socket /run/waydroid-venus/venus.sock); GL: ANGLE on the Vulkan HAL (ro.hardware.egl=angle, nvidia_native_gl = off)`，配置备份 /var/lib/waydroid/before-source-native-20261025-214506。
- 容器 ~16 s 到 boot；3DMark（主进程 3060）21:48:38 启动。
- bench3d 的播放键判别在 21:49 给出 `978,537 DISC n=515 span=82x68 flat=0.80` —— **假阳性**（正常是 1844,376 附近的橙色圆盘，n≈750 span≈60x62）。两次 tap 都没起 workload（app 那时还在画第一帧）。
- `am_anr 21:49:26 [0,3060,...] Reason: executing service com.futuremark.dmandroid.application/com.google.firebase.sessions.SessionLifecycleService`；`E ActivityManager: ANR in com.futuremark.dmandroid.application`（21:49:45）。窗口内 app CPU 只有 **7.4%**（4.8% user + 2.5% kernel），faults 75257 minor / 42 major → 不是忙等，是阻塞。
- 主线程栈（/data/anr/anr_2026-10-25-21-49-26-778，pid 3060 段）：`artQuickResolutionTrampoline` → `androidx.constraintlayout.core.widgets.analyzer.Direct.horizontalSolvingPass` → `Direct.solvingPass` → `ConstraintWidgetContainer.layout` → `BasicMeasure.solveLinearSystem` → `ConstraintLayout.resolveSystem` → `ConstraintLayout.onMeasure` → `View.measure` → `FrameLayout.onMeasure` → `ConstraintLayout$Measurer.measure` → `Direct.verticalSolvingPass` → `solveVerticalMatchConstraint` …（多层嵌套求解）。**关键：栈顶是 ART 的 resolution trampoline，即这些 UI 代码没被 AOT 编译、在解释器里跑。**
- 内存压力（ANR 报告附带）：`/proc/pressure/memory` some avg10=22.34 full avg10=14.00。
- 结果：没有产生任何结果 zip；3DMark 未进入 workload。

## 判读与下一步

1. 这不是「venus 渲染慢」的证据，而是「app 的启动器 UI 在本容器里慢到 ANR」。同类现象在 glibc-shim 也出现过（13:42 那次 `MainActivity Input dispatching timed out`，主线程卡在 `BaseBenchmarkLauncherActivity.onCreate:304 → LayoutInflater.inflate → AppCompatImageView.<init>` >5 s；13:45 还有 data_app_crash NPE）。
2. **便宜的修法**：给 3DMark 做一次 AOT 编译，把 UI 代码从解释器里拿出来：`cmd package compile -m speed -f com.futuremark.dmandroid.application`（对 Geekbench 同理：com.primatelabs.parkdale）。这是纯 guest 侧动作，不涉及 HAL，且能同时降低「跑到一半被 UI ANR 打断」的概率。
3. venus 的 GPU 结论必须等 UI 能稳定出启动器之后再取；届时把 bench3d 的首次 tap 延迟拉长（现在 ~25 s 就 tap，venus 下不够），并用「橙色圆盘 + 期望位置 1844,376 附近」作双重判据，避免假阳性 tap。

## 产物

- artifacts/bench3d-20261025T213951-venus-t1/：00-guest-env.txt、01-before.png、02-launcher.png、03-dumpsys-activity.txt、03-surfaceflinger-list.txt、03-window-displays.txt、04-after-tap1.png、04-after-tap2.png、04-tap-decision.txt、dmon-t.csv、dmon-p.csv、gpu.csv、gpu-before-q.txt、logcat.txt（1.32 MB）
- ANR trace（guest）：/data/anr/anr_2026-10-25-21-49-26-778（736649 B，pid 3060 段 148 行）