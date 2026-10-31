# 纠正：崩溃循环来自 podman/redroid，与 Waydroid 无关（2026-10-31）

> 本文替代我先前的误判记录（那份草稿已删除）。

## 误判经过（保留以供后人避免）

我在 workload 期看到宿主 `ps` 里有多个 `system_server` 与 `systemd-coredump`，且 
`coredumpctl list | grep -c SIGABRT` = **1487**，于是推断「Waydroid 的 Android 运行时在崩溃循环」，
并据此怀疑前面所有 GPU 测量都不可信。**这个推断是错的。**

## 真正的情况

崩溃的全部 cgroup 都形如：

```
Control Group: /user.slice/.../libpod-1f8d5e51...scope/container/uid_1002/pid_263690
```

即 **podman 的容器**。`podman ps -a` 显示这台机器上有一批 `docker.io/redroid/redroid:12.0.0_64only-latest` 容器
（`redroid-dbg`、`redroid-att`、`rdk`、`rdkit` 等），多数是 `Exited (129)`，但**其中几个仍在反复拉起 system_server**
（`app_process64` → `com.android.bluetooth`、`system_server`，Signal 6 / ABRT）。

这些是**别的实验残留**，与 Waydroid（LXC，`/var/lib/waydroid/lxc/waydroid`）完全无关。

## Waydroid 侧的实测健康度（一手）

```
$ lxc-attach ... 'pidof system_server; ps -A -o PID,ETIME,ARGS | grep system_server'
312
   87    02:36:22 zygote64
   88    02:36:22 zygote
  312    02:36:19 system_server      <-- 已连续运行 2 小时 36 分
$ lxc-attach ... 'logcat -b crash -d | tail'
(空)                                <-- crash 缓冲区 0 字节
```

⇒ **Waydroid 的 Android 运行时是健康的**：system_server 自上次 `dev/restart` 起没有崩溃过。

## 那 workload 期的 467% user / 259% sys 是什么？

那是**宿主整机**的 CPU 统计（`top` 在 guest 里读的是宿主 cgroup 视图），包含了 podman 那些崩溃循环的 redroid 容器。
它解释了「5.1 核被吃掉」和「GPU 只有 5%」的表面矛盾 —— **不是我们的负载在抢 CPU，是别的容器在抢。**

## 对本轮结论的影响

| 结论 | 是否受影响 |
|---|---|
| Run D：显存压力假设被证伪 | **不受影响**（对比的两条跑分都在同一宿主噪声下） |
| DRAW-DIST：每 draw 10–30 ms、与像素覆盖无关 | 可能被宿主噪声放大，需要安静环境复测 |
| guest 内同构基准 0.228 µs/draw | **不受影响**（单进程、短时，但值得在安静环境复核） |
| 宿主原生 0.599 µs/draw | **不受影响** |

## 下一步（必须先做）

1. **清掉 podman 的 redroid 残留容器**（`podman stop/rm` 那些 running 的），让宿主 CPU 回到空闲，
   然后重跑一条 3DMark 基线 + DRAW-DIST。这一步不做，任何性能数字都没有可比性。
2. 重测后再判断「每 draw 10–30 ms」是否成立。
3. ORB 的 SYNC_FD 修复验证也在同一安静环境下做。

## 教训

- 宿主 `ps` / `coredumpctl` 是**跨容器共享**的，看到 `system_server` 不能假定是自己的 LXC；
  必须用 `Control Group` 里的 `libpod-*` vs `/var/lib/waydroid/lxc/` 区分。
- guest 内 `top` 的 cpu 行反映宿主 cgroup，不是 guest 独占，容易被别的容器污染。