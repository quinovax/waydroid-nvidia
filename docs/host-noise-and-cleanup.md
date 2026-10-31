# 宿主噪声量化与清理（2026-10-31）

## 发现

本机 `podman` 上残留 **11 个 `redroid` Android 容器**（rdb7…rdb17）全部在运行，
每个都在跑自己的 `system_server` 并反复 ABRT，把宿主 CPU 吃满：

```
$ cat /proc/loadavg
35.56 27.49 21.34        # 8 核机器上的 4.4 倍过载

$ ps -eo pcpu,pid,comm --sort=-pcpu | head
46.2  system_server
42.0  system_server
41.3  system_server      # 多个容器堆叠
```

它们的 cgroup 都形如 `.../libpod-<hash>.scope/container/uid_*/pid_*`，
与 Waydroid 的 LXC（`/var/lib/waydroid/lxc/waydroid`）无关。

Waydroid 自身是健康的：`system_server` ETIME **02:36:19**（连续运行 2 小时 36 分），
`logcat -b crash` 为空。

## 清理与效果

```
$ podman ps -q | wc -l      # before
11
$ for c in $(podman ps -q); do podman stop -t 2 $c; done
$ /proc/loadavg             # after
4.00 18.11 19.35
$ coredumpctl list --since "90 seconds ago" | grep -c SIGABRT
0
```

## 干净环境 vs 噪声环境的 3DMark（同一 HAL、同一驱动、同一模式）

| 环境 | 分数 | GraphicsTest1 (fps) | zip |
|---|---|---|---|
| 噪声（load 22–35） | 190 / 193 | 1.41 / 1.43 | 18.5 / 22.9 KB |
| **干净（load ~4）** | **205** | **1.52** | **15.0 KB** |

**结论：宿主噪声值 +6%**（1.43 → 1.52 fps）。
所以：

1. 清理宿主残留容器是**必须做**的纪律，否则数字不可比、且会误导归因；
2. 但它**不是 15 倍差距的原因** —— 干净环境下仍然只有 205 分 / 1.52 fps，
   而目标 >3000 需要 ~22.3 fps。

## 对先前结论的修订

| 结论 | 修订 |
|---|---|
| 「Android 运行时在崩溃循环」 | **错的**，那是 podman/redroid 的残留容器，Waydroid 自身健康（见 `docs/runtime-crash-loop.md`） |
| 「workload 期 467% user + 259% sys 是 3DMark 在烧 CPU」 | **错**，主要是别的容器在抢；干净环境下需要重新采样 |
| Run D（显存压力假设被证伪） | **成立**（对比双方都在同等噪声下） |
| 逐 draw 10–30 ms（DRAW-DIST） | 待干净环境复测 |
| guest 内同构基准 0.228 µs/draw | **成立**，且干净环境应更好 |

## 下一步

1. 在**干净环境**下重跑一次 DRAW-DIST（`debug.nvhal.gputime=2`），确认每 draw 成本是否仍为毫秒级。
2. 若仍是毫秒级 → 真瓶颈确实是每 draw 的固定成本，继续查 ANGLE/descriptor/pipeline。
3. 若降到微秒级 → 说明先前所有“每 draw 10–30 ms”都是宿主噪声造成的假象，
   那 205→3000 的差距需要另找解释（更可能是真实的渲染工作量 + 分辨率）。

## 纪律（写进团队规则）

**跑任何基准前先做**：

```
cat /proc/loadavg                                   # 期望 < 8
podman ps -q | wc -l                                # 期望 0
coredumpctl list --since "60 seconds ago" | grep -c SIGABRT   # 期望 0
```

三项任一不满足就不要开跑，否则数字没有意义。
## 干净环境复测（2026-10-31 17:39–17:44）

清理 11 个 redroid 残留容器后（load 35.6 → 3–4，SIGABRT 归零），同一 HAL/驱动/模式重测：

| 跑分 | 环境 | 分数 | fps | 备注 |
|---|---|---|---|---|
| base-580196（10-30） | 噪声 load 22–35 | 193 | 1.43 | |
| drawdist-t3（13:44） | 噪声 load 22–35 | 190 | 1.41 | gputime=2 |
| clean-base（17:31） | 干净 load ~3 | 205 | 1.52 | 无插桩 |
| clean-drawdist（17:39） | 干净 load ~3 | 195 | — | gputime=2 |

结论：宿主噪声影响约 +6%（1.43 → 1.52 fps），不是 15 倍差距的主因。

### 干净环境下的逐 draw 成本

busy 562.6 ms/帧 | timed regions 554.2 | draws(level 2) 421.2（噪声环境是 744 / 729 / 585）

| 每帧 draw 数 | 单 draw 平均 | p50 | p90 | max |
|---|---|---|---|---|
| 1040 | 22.4 us | 0-16 us | 16-32 us | 4002.8 us |
| 628 | 251.0 us | 16-32 us | 80-96 us | 33257.5 us |

对比宿主原生 0.6 us/draw，仍是 40-400 倍。

所以：宿主噪声解释了约 1/3 的 busy 差异（744 → 562 ms），
但「每 draw 比原生贵 1-2 个数量级」在干净环境下依然成立。