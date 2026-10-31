# ORB 决定性验证：SYNC_FD 修复无效，失败根因收敛（2026-10-31）

## 本轮第一次真正跑成 Geekbench

用 MCP portal 在 GPU 标签点 Run，Geekbench 7 GPU 基准完整跑完（Background Blur -> Video Filter -> Fluid Simulation ...）。

此前十几轮都卡在点击：原因是下面第「新增环境事实」一节里的遮挡层。

## 结果：ORB 仍然失败

```
10-31 19:09:58.589  I geekbench: [1031/190958:ERROR:orb_gpu_workload.cpp(182)] GPU:186, CPU:162: difference is > 10%
10-31 19:09:58.590  I geekbench: [1031/190958:ERROR:gpu_workload.cpp(190)] workload 503 failed validation
```

CPU 参考值仍然是 162（与 10-18 的 208/162、10-25 的 201/162 与 203/162 完全一致）。GPU 侧 186，偏高 14.8%。

## 这排除了什么

| 假设 | 状态 |
|---|---|
| 上游 c8c9047 的 SYNC_FD 修复（580 的 sync_file 泄漏） | 已合入 6a1948d、已部署、仍失败 => 不是根因 |
| 设备身份切换 | 排除（Vulkan Device Name 始终 NVIDIA GeForce GTX 1080，driver 580.196.7.0） |
| 跑分 API 选错 | 排除（GPU API 下拉框显示 Vulkan） |
| NVK 独有 | 排除（这是 glibc-shim + 闭源 580） |

## 仍然成立的规律

CPU 参考值恒为 162，GPU 侧在 186 / 201 / 203 / 208 之间浮动，永远偏高 14.8%-28.4%。
这不像纯随机污染（那样应双向、幅度更大），更像 GPU 侧系统性多算一批特征点 + 抖动。

- H3（确定性数值语义）要求 GPU 值恒定；实测在变 => 部分是随机的。
- 但方向恒定偏高 => 混合了系统性多算与抖动，倾向 H1d（重复累加 / 多算）而不是纯 H3。

## 新增的环境事实（解释了一直以来的点击失效）

```
W InputDispatcher: Untrusted touch due to occlusion by /1000
  * package=/1000, id=227, mode=BLOCK_UNTRUSTED, alpha=0.32,
    window={Dim Layer for - Task=65#227}, inputConfig={NO_INPUT_CHANNEL}
```

一个 uid 1000 的 Dim Layer（alpha 0.32）盖住整个屏幕，Android 防点击劫持因此拦截所有触摸。
这就是本容器里 input tap 与 portal 点击看起来成功却没反应的真因（历史上多次误判为 ANR 吞掉点击）。

修法（已生效）：settings put secure untrusted_touch_filter 0（镜像为 userdebug，允许）。
修复后 GPU 标签与 Run 按钮一次点击即响应。

## 下一步

1. glibc-shim 下连跑 3 次，记录每次 GPU:x, CPU:162：
   - 3 次 GPU 值完全相同 => 转 H3，查 ORB shader 的阈值 / min-max / NaN；
   - 每次不同但都偏高 => 转 H1d，查 vkCmdDispatchIndirect 的 groupCount 来源与 barrier 缺失导致的重算；
   - 已有数据（186/201/203/208）倾向后者。
2. 探针 v2（tests/orb-probe/ 已有 993 行基建）：把 storage buffer/image 换成 AHB/gralloc 导入，覆盖唯一还没测的层。

## 修复建议（写进跑分脚本准备阶段）

dev/gebench 与 dev/bench3d 的开跑前都应执行：

    settings put secure untrusted_touch_filter 0

否则任何点击都会被 BLOCK_UNTRUSTED 静默吞掉。