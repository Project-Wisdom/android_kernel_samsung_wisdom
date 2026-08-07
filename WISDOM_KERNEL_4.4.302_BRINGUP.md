# SM-P205 / wisdom Linux 4.4.302 Bring-up

> Android 17 migration note (2026-08-07): the current repository is
> `Project-Wisdom/android_kernel_samsung_wisdom`, the source path is
> `kernel/samsung/wisdom`, and the development branch is `android-17`.
> References to universal7904 paths and LineageOS 23.2 branches below document
> the historical 4.4.302 bring-up and are intentionally retained.

## 1. 目标

在保留可回退的 LineageOS 23.2 / wisdom 4.4.177 全量包与 boot 备份的前提下：

1. 将现有 universal7904 内核合并到官方 Linux `v4.4.302`。
2. 保留 SM-P205 / wisdom 的板级支持、Android 兼容补丁和已经验证的设备修复。
3. 从 Eureka Kernel 中按风险分批移植可验证的性能或正确性修复。
4. 保留 `exynos7904-perfd` 0.4.2 独立仓库作为候选，但当前不做原生 ROM 集成。
5. 使用独立构建的内核替换 ROM device tree 中的 prebuilt kernel，并分批完成设备验证。

这不是从 Eureka 整树移植 wisdom，也不是只修改 `Makefile` 的 `SUBLEVEL`。

截至 2026-07-29，纯 4.4.302 bring-up 已完成编译、boot 打包、TWRP 刷入和 Android
完整启动。当前工作进入 Eureka 小批次审查与运行时内存基线审计阶段；
`exynos7904-perfd` 原生 ROM 集成已按决定完整回退。
功能回归矩阵尚未全部完成，因此当前 prebuilt 仍是测试基线，不是最终发布结论。

## 2. 当前基线

### 2.1 工作区与分支

| 项目 | 值 |
| --- | --- |
| 原有 LOS23.2 内核工作区 | `/home/hajimi/lineageos23.2/kernel/samsung/universal7904` |
| 新 4.4.302 工作区 | `/home/hajimi/android_kernel_samsung_universal7904-4.4.302` |
| 原分支 | `lineage-23.2` |
| 新分支 | `lineage-23.2-302` |
| 新分支起点 | `72aae3c51a516405605a483dbe9732d498ec961a` |
| 起点提交 | `media: mfc: requeue pending encoder work` |
| 起点源码版本 | `4.4.177` |
| 合并后源码版本 | `4.4.302` |
| 官方 `v4.4.177` commit | `6b50202a4d53bf527c640467bcff68b50a5e38a2` |
| 官方 `v4.4.302` commit | `a09b2d8f61ea0e9ae735c400399b97966a9418d6` |
| Google Android 4.4.302 merge | `875c0cc8115381f702b12d41de293807f47cdac9` |
| universal7904 同源 bring-up | `b128d69019a8c1dd05b10b2879c9eaa0de3207c6` |
| 官方 stable 分支 | `linux-4.4.y` |

新工作区使用独立 clone，避免把旧工作区内未跟踪的 `KernelSU-Next/`、`out-gcc/`
或其他生成物带入 4.4.302 分支。

### 2.2 Git 关系

当前 wisdom 分支是官方 `v4.4.177` 的后代：

- 当前分支相对 `v4.4.177` 有约 851 个下游提交。
- 官方 `v4.4.302` 相对 `v4.4.177` 有约 7783 个 stable 提交。
- 当前分支和 `v4.4.302` 的共同祖先是官方 `v4.4.177`。

直接把官方 `linux-4.4.y` 合并进当前分支的试运行产生了 92 个冲突。继续手工
猜测这些 Android/Samsung 下游冲突的语义，不如复用同一 universal7904 源码已经
完成并长期使用的 Android 4.4.302 bring-up：

- `b128d69019` 的第一父提交 `86c332d77b` 只比当前双方共同基点 `d6d61c0be4`
  多一个 flip-cover 修复。
- 第二父提交 `875c0cc811` 是 Google `android-4.4-p` 合并官方 `v4.4.302`
  的提交，且其第二父提交就是 `a09b2d8f61`。
- `b128d69019` 已经处理 universal7904 与 Android 4.4.302 的冲突，提交说明还
  记录了 Samsung MTP gadget 的 NULL 检查及多 gadget 兼容处理。
- 当前 `lineage-23.2` 只在共同基点之后增加 18 个设备/Android 16 修复，因此
  将 `b128d69019` 合并进当前分支只产生 1 个真实冲突。

采用这条同源 merge ancestry，仍然完整包含官方 `v4.4.302` 历史，同时不会带入
Samar 分支在 `b128d69019` 之后的 CIP、BPF、LZ4、vmalloc 或性能修改。

不采用以下方法：

- cherry-pick 7783 个 stable 提交；
- 使用 Eureka 源码覆盖当前树；
- 把 Eureka 的 A20/A30 defconfig 改名成 `wisdom_defconfig`。

### 2.3 ROM 打包现状

`device/samsung/wisdom/BoardConfig.mk` 当前包含：

```make
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/Image
TARGET_FORCE_PREBUILT_KERNEL := true
TARGET_KERNEL_CONFIG := wisdom_defconfig
```

所以修改 kernel 源码本身不会进入 ROM。必须先独立构建、上机验证，然后明确替换：

```text
/home/hajimi/lineageos23.2/device/samsung/wisdom/prebuilt/Image
```

替换后还要验证源码构建输出、device prebuilt 和 ROM `out/.../kernel` 的 SHA-256
完全一致。

`device/samsung/universal7904-common/BoardConfigCommon.mk` 还有一个后续注意点：

```make
TARGET_KERNEL_ADDITIONAL_FLAGS += LD=ld.lld AR=llvm-ar ...
TARGET_KERNEL_ADDITIONAL_FLAGS := HOSTCFLAGS="..."
```

第二行的 `:=` 会覆盖第一行。4.4.302 的
`include/linux/compiler-gcc.h` 明确要求 GCC 5.1 及以上，直接沿用 Android
GCC 4.9 会在编译入口处失败，因此不能通过删除版本检查来硬绕过。第一阶段实际
验证通过的组合是 LineageOS kernel Clang r416183b（Clang 12.0.5）加 Android
GCC 4.9 提供的 GNU binutils。

这仍然是独立构建后走 `TARGET_PREBUILT_KERNEL`，没有切换 ROM 内源码构建或启用
LTO。以后如果改成 ROM 内编译，需要单独修正上述 `:=` 覆盖，并复现同一套工具链
参数。

### 2.4 当前设备状态

第一次 4.4.302 boot 在启动约 12.585 秒时于
`kmem_cache_alloc() -> anon_vma_fork()` panic。它不是 ramdisk、AVB 或 init 问题，
而是旧 Android/Samsung SLUB 与 anon-vma 代码缺少已知稳定版修复。已分开合入：

- `cdce2a216a`：SLUB deactivation 路径补齐 TID 更新；
- `603c9d99cb`：修复 anon-vma degree 歧义导致的 double reuse；
- `f1b54a1874`：串行化 out-of-tree firmware blob 生成，消除并行构建竞争。

重新构建并替换 prebuilt 后，设备经 TWRP 刷入启动到可用 Android，验证到：

- `uname` / `/proc/version` 为 `4.4.302+`；
- `sys.boot_completed=1`；
- ROM `out/target/product/wisdom/kernel` 与 device prebuilt SHA-256 均为
  `4fc5e9d8da2d9df5c5af630b407a516270dd4c76288a67682bdfbb5696569869`；
- 已刷 boot image SHA-256 为
  `2a6291a8e2721249c59342b4bf32ef6d8bd128ffe4df236a632b46d54fc5f41e`。

这是启动和打包证据，不等同于完整功能回归。相机录像问题随后已在当前设备上确认
恢复，但 Type-C 音频、2.4G 接收器、S Pen、长时间录像和待机仍应在新批次内核刷入
后重新逐项验证。

## 3. 总体策略

### 阶段 A：纯 4.4.302 bring-up（已完成）

1. 从 `72aae3c` 创建 `lineage-23.2-302`。
2. 获取官方 `linux-4.4.y` 到 `a09b2d8`，只用于核验版本和 ancestry。
3. 获取同源 universal7904 的 `b128d69019` Android 4.4.302 bring-up。
4. 将 `b128d69019` 合并进当前分支，保留当前 18 个 LOS23.2/设备修复。
5. 处理当前分支与同源 bring-up 的少量冲突。
6. 保持 `wisdom_defconfig` 和现有 DTS/驱动，不导入 Eureka defconfig。
7. 先让 4.4.302 Android baseline 编译并启动，不加入 Eureka 性能修改，也不开
   perfd。

### 阶段 B：恢复并验证 wisdom ABI（启动通过，功能矩阵进行中）

必须保留或重新验证：

- P205 DTS、面板、触摸和 Wacom/S Pen。
- S2MU004 MUIC、USB/Type-C 音频和 2.4G 接收器相关路径。
- Exynos MFC 与录像修复。
- Android 16 所需 BPF/netd、cgroup、namespace、binder 和 SELinux 兼容代码。
- KernelSU 现有集成。
- 电池、充电、休眠和 Wi-Fi/WLBT 路径。
- HMP `selective_boost`。
- cpufreq `policy0` / `policy6` 以及现有 OPP 表。
- perfd 使用的 thermal zone、cpuset 和 sysfs ABI。

### 阶段 C：Eureka 按批移植（当前阶段）

Eureka R24U 的源码版本为 `4.4.302-p6`，但其主要目标是 A10/A20/A20e/A30/A30s/
A40/M20 等机型，没有可直接使用的 P205/wisdom 板级 defconfig。

Eureka 仅用于：

- 参考其 4.4.302 冲突处理；
- 选择有明确原因、范围较小、可独立测试的修复；
- 对比最终代码，而不是按提交标题盲目 cherry-pick。

## 4. Eureka 候选分组

### 4.1 第一批：正确性修复

这些提交必须先与本树的对象生命周期和 Android common 实现对照，不能只按 Eureka
提交标题移植：

| Commit | 作用 | 处理 |
| --- | --- | --- |
| `4ca9d60a459a` | 合并 cpufreq time-in-state 的 init/alloc | **拒绝**：会撤销 AOSP 为 idle task 泄漏修复而拆分的 init/alloc 路径；当前树已在 `_do_fork()` 分配并在 `free_task()` 释放 |
| `ed13ccc89174` | 删除 cpuidle 错误路径中的显式 `kfree()` | **拒绝**：原上游提交后来被回退，后续修复也明确指出它会造成 kobject 内存泄漏；本树 release callback 不释放外层对象 |
| `53d523204806` | devfreq governor resume 后重新更新频率 | **已适配**为 `010915fdef`，独立提交，待构建和 suspend/resume 实机 A/B |
| `85320521da60` | 非法 CPU 编号不触发 WARN | **跳过**：只隐藏 WARN，不修复调用方，也不是性能或内存优化 |

这里保留当前 Android common 的 cpufreq 设计：

- `copy_process()` 只调用 `cpufreq_task_times_init()` 清空新 task 指针；
- `_do_fork()` 在 task 即将对外可见前调用 `cpufreq_task_times_alloc()`；
- 所有失败和退出路径最终由 `free_task()` 调用 `cpufreq_task_times_exit()`。

对应 AOSP 原始修复见
<https://android.googlesource.com/kernel/common/+/47bbcd6bf8f926e4e009c12b18f349ffa41bafd4^!/>。
cpuidle 后续正确修复见
<https://github.com/torvalds/linux/commit/e5f5a66c9aa9c331da5527c2e3fd9394e7091e01>。

### 4.2 第二批：需要 A/B 测试的性能修改

| Commit/方向 | 风险 |
| --- | --- |
| `5401370b886c`：负载均衡时始终更新 CPU capacity | 代码范围小，但会取消 100 ms 限流；保持为独立实验，需测启动延迟、jank、功耗和温度 |
| scheduler latency/min-granularity 调整 | 会同时影响吞吐、交互和功耗 |
| scheduler migration cost 调整 | Eureka 提交标题和实际数值不一致，禁止直接照抄 |
| power-efficient workqueue | 可能省电，也可能增加交互延迟 |
| ZRAM LZ4 默认值 | 先确认 Android userspace 实际选择的压缩算法 |
| devfreq input boost | 与 perfd 的触摸/选择性 boost 叠加后可能增加温度 |

调度参数如果最终采用，应根据 R24U 最终代码重新整理成一个 wisdom 专用提交，
不能照搬中间状态的多次 tweak/revert。

### 4.3 暂缓

- `f007733cb2d7`：MFC 录像状态联动 devfreq boost。
  - 该提交直接修改刚恢复稳定的 MFC 录像路径。
  - 它使用单个无锁全局开关，没有多实例引用计数；一个 decoder 关闭时可能提前恢复
    boost。
  - 不进入当前批次；只有录像回归稳定后，重新设计成引用计数并独立 A/B 才考虑。
- Mali GPU threshold、GPU 驱动升级。
- CPU/GPU 超频、降压或替换 OPP/电压表。
- 新 I/O scheduler 和大量第三方 governor。

### 4.4 明确拒绝

以下修改不进入第一轮 bring-up：

- 30 秒后强制杀 wakelock。
- 停止周期性脏页回写。
- 全局 always-overcommit。
- 关闭 TTWU_QUEUE。
- 将所有 SCHED_FIFO 改成 SCHED_RR。
- 为减少开销关闭 arm64 hardware breakpoint。
- 没有可审计说明的 “gotta go fast” 类修改。
- 默认 CPU offlining、强制 Doze、修改 thermal trip。

## 5. exynos7904-perfd 状态（原生 ROM 集成已回退）

当前不把 `exynos7904-perfd` 接入 ROM。已回退：

- `device/samsung/universal7904-common` 中的 package、init 和 SEPolicy 接线；
- `.repo/local_manifests/wisdom.xml` 中的 perfd project；
- `/home/hajimi/lineageos23.2/hardware/samsung/exynos7904-perfd` 集成工作树及其
  `lineage-23.2` 集成分支。

独立开发仓库 `/home/hajimi/exynos7904-perfd` 保留且工作区干净；设备上原有的
Magisk perfd 安装未被本次源码回退修改。下面仅保留将来重新评估时的策略边界，
不代表当前 ROM 会包含或启动 perfd。

0.4.2 已验证过的策略：

- 屏幕开启使用 `fast`。
- 屏幕关闭使用 `powersave`。
- 管理 `policy0` 和 `policy6` 的最低频率 lease。
- 按触摸、应用切换、持续 BIG 核负载申请短期 boost。
- 使用 HMP `selective_boost`。
- 85 摄氏度 thermal guard 及 5 摄氏度 hysteresis。
- 不修改 governor、thermal trip、GPU、MIF、CPU online 或 HMP threshold。
- 退出或异常恢复时归还所管理的频率和 affinity。

不要恢复旧版本中的这些内容：

- interactive `timer_rate` / `go_hispeed_load` / `hispeed_freq`。
- `dirty_writeback_centisecs` 动态修改。
- 全局 read-ahead 修改。
- GPU/MIF/thermal 节点写入。

如果以后明确决定恢复原生集成，再按以下顺序重新验证：

1. 纯 4.4.302，perfd 关闭。
2. 纯 4.4.302，perfd 0.4.2 开启。
3. 加入 Eureka 正确性修复，perfd 开启。
4. 每次只加入一个或一组可独立回退的性能实验。

## 6. 运行时内存占用与压力优化

### 6.1 当前设备基线

在已启动的 4.4.302 设备上做一次空闲态只读采样：

| 项目 | 当前值 |
| --- | --- |
| 可用物理内存 | `MemTotal 2789448 kB`，约 2.66 GiB |
| 两次采样可用内存 | `MemAvailable 1170020` 至 `1202556 kB` |
| `dumpsys meminfo` | Used RAM `1517766` 至 `1602292K`；状态均为 `normal` |
| Slab / 不可回收 Slab | `155384 kB` / `95164 kB` |
| Kernel stack / Page tables | `26528 kB` / `48436 kB` |
| CMA | `172032 kB`，最新采样 `CmaFree 0 kB` |
| ZRAM | 物理内存的 `55%`，约 1.46 GiB |
| 压缩算法 | `lzo [lz4] deflate`，实际已选择 LZ4 |
| ZRAM 使用量 | `360 kB`，当前几乎没有换页压力 |
| VM 参数 | `swappiness=100`，`page-cluster=0` |
| LMKD | `ro.lmk.use_psi=false`，`ro.lmk.use_minfree_levels=true` |
| CachedAppOptimizer | 配置为启用 compaction，但累计执行次数为 0 |
| Cached app freezer | `use_freezer=false` |
| KSM | 内核支持，但 `run=0` |
| ION noncontig | 当前分配约 249.5 MiB，其中 debugfs 标记 orphaned 约 27 MiB |

两次采样中 `dumpsys meminfo` 报告约 0.85 至 0.97 GiB free RAM，ZRAM 只使用
360 kB，LMKD kill 计数为 0，并不处于内存压力。
因此不能用单次空闲态 RSS 排名直接证明某个服务“泄漏”，也不应为了看起来 free RAM
更多而盲目缩小缓存。

社区所谓 “GrapheneOS GSI 占用 1.6G” 也不是当前基线的优势：同口径下本机
`Used RAM 1602292K`，约 1.53 GiB / 1.64 GB。GrapheneOS 官方明确不支持作为 GSI
使用，而且 GSI 继续依赖设备原有 kernel、vendor 和 device support；第三方镜像的
少服务、少应用或功能缺失不能归因成 GrapheneOS 内核优化。只有在相同 kernel/vendor、
安装应用、开机静置时间和统计命令下才值得做 A/B。官方说明见
<https://grapheneos.org/faq#device-support>。

### 6.2 可执行优先级

1. **先建立可重复的压力与生命周期对照。** 使用同一套应用切换、录像、返回桌面和
   30 分钟静置流程，分别记录 `dumpsys meminfo`、`/proc/meminfo`、ZRAM
   `mm_stat`、LMKD kill、CMA 和 `/d/ion/heaps/*`。只有多轮结束后仍不回落的对象
   才进入泄漏定位。
2. **保持现有 ZRAM 配置。** 55% + LZ4 + `page-cluster=0` 已是合理基线；当前没有
   证据支持改算法或继续放大。ZRAM 接口与统计方法见
   <https://docs.kernel.org/admin-guide/blockdev/zram.html>。
3. **优先审计 ION / dma-buf 生命周期，而不是先缩 CMA。** 当前较大的持有者来自
   graphics composer、Launcher、SurfaceFlinger、FIMC camera 和 system_server。
   ION 源码把 `handle_count == 0` 但仍被 dma-buf 引用的 buffer 显示为
   `orphaned allocations`；这只是“最后已知 client”的调试提示，不等于已经证明
   泄漏。应对照正常关闭截图、相机和应用后的 heap 总量，确认哪一类 buffer 不释放。
4. **把 CachedAppOptimizer 内核支持视为独立的中高风险 backport。** 当前 4.4.302
   已回移 `pidfd_open()` / `CLONE_PIDFD`，但仍缺 `process_madvise()`、供该路径
   安全解析 pidfd 的 helper，以及 `MADV_COLD` / `MADV_PAGEOUT` reclaim 语义。
   现代 framework 会组合使用这些接口；不能只补一个 syscall 或只开属性。若推进，
   必须独立分支补 UAPI、arm64 syscall、权限/LSM、SELinux、seccomp 和回归测试。
   AOSP 实现参考
   <https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/services/core/jni/com_android_server_am_CachedAppOptimizer.cpp>。
5. **暂不启用 cached app freezer。** 本内核虽有 cgroup freezer，却没有
   `BINDER_FREEZE` / `BINDER_GET_FROZEN_INFO` 内核实现；只打开 framework 属性会
   留下 binder transaction 与进程冻结不同步的风险。Android 的 freezer 与 binder
   协作要求见 <https://source.android.com/docs/core/perf/cached-apps-freezer> 和
   <https://source.android.com/docs/core/architecture/ipc/binder-freezer>。
6. **PSI/现代 LMKD 是长期方向，不是当前小补丁。** Android 推荐 PSI LMKD，但官方
   文档给出的旧内核 backport 路径从 4.9 起；4.4 没有 PSI，本树移植规模和风险较高。
   当前先保留 minfree LMKD，避免只改 `ro.lmk.use_psi` 导致错误配置。参考
   <https://source.android.com/docs/core/perf/lmkd>。

当前明确不做：

- 不强制设置 `ro.config.low_ram=true`，因为它会改变功能和 UI 策略，不只是减少内存；
- 不全局启动 KSM；没有 `MADV_MERGEABLE` 工作集时收益有限，还会增加扫描 CPU 与
  COW 成本，接口参考 <https://docs.kernel.org/admin-guide/mm/ksm.html>；
- 不把 Dalvik heap 模板从当前 2048 档改成 4096 档；后者保留的空闲 heap 更大，
  不是这台设备的减内存方案；
- 不缩减 CMA / ION reserved memory；相机和显示正在使用这些 heap，且录像链路刚
  恢复稳定；
- 不在 4.4 上移植 MGLRU；它是现代内核的大型 reclaim 子系统，不适合作为当前
  bring-up 的低风险优化；
- 不因静态 RSS 较大就裁剪 SystemUI、Launcher 或 framework cache；
- 不同时修改 ZRAM、LMKD、Dalvik heap 和 freezer，避免无法归因。

## 7. 实际采用的 4.4.302 合并命令

新工作区初始创建命令：

```bash
git clone --filter=blob:none --depth=1000 --single-branch \
  -b lineage-23.2 \
  https://github.com/xuanyayi/android_kernel_samsung_universal7904.git \
  /home/hajimi/android_kernel_samsung_universal7904-4.4.302

cd /home/hajimi/android_kernel_samsung_universal7904-4.4.302
git switch -c lineage-23.2-302
```

拉取官方 stable，用于核对官方目标和共同祖先：

```bash
git remote add linux-stable \
  https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git

git fetch --filter=blob:none --shallow-exclude=v4.4.176 linux-stable \
  +refs/heads/linux-4.4.y:refs/remotes/linux-stable/linux-4.4.y
```

检查共同祖先：

```bash
git merge-base --is-ancestor \
  6b50202a4d53bf527c640467bcff68b50a5e38a2 HEAD

git merge-base HEAD linux-stable/linux-4.4.y
```

预期共同祖先：

```text
6b50202a4d53bf527c640467bcff68b50a5e38a2
```

纯 stable 直接合并只作为冲突规模试运行：

```bash
git merge --no-ff --no-commit linux-stable/linux-4.4.y
git diff --name-only --diff-filter=U
git merge --abort
```

该试运行得到 92 个冲突，没有提交。

实际采用同源 universal7904 Android 4.4.302 merge：

```bash
git remote add samar \
  https://github.com/SamarV-121/android_kernel_samsung_universal7904.git

git fetch --depth=1000 samar \
  +refs/heads/lineage-22.2:refs/remotes/samar/lineage-22.2

git merge-base HEAD b128d69019a8c1dd05b10b2879c9eaa0de3207c6
git show -s --format=raw b128d69019a8c1dd05b10b2879c9eaa0de3207c6
git merge --no-ff --no-commit \
  b128d69019a8c1dd05b10b2879c9eaa0de3207c6
```

这次只冲突 `kernel/locking/rtmutex.c`。解析方式是同时保留：

- 4.4.302 的 `lock->wait_lock` IRQ 语义；
- 当前修复使用 `waiter->task`，而不是错误使用 `current`；
- `rt_mutex_adjust_prio_chain()` 最后一个参数继续传递该 waiter task。

完成的 merge commit：

```text
228624978b Merge Android 4.4.302 bring-up into lineage-23.2-302
```

此外，旧树的
`drivers/gpu/arm/b_r26p0/platform/devicetree/Kbuild`
本来就提交了残留冲突标记；在独立提交 `4383ad3f2c` 中清理，避免 Make 解析失败。

## 8. 已验证的第一阶段构建命令

以下命令已在新工作区完整执行成功。Clang 负责 C/汇编编译，Android GCC 4.9
目录只提供 AArch64/AArch32 GNU binutils：

```bash
KERNEL_SRC=/home/hajimi/android_kernel_samsung_universal7904-4.4.302
KERNEL_OUT=/home/hajimi/kobj-wisdom-4.4.302-clang12
CLANG_BIN=/home/hajimi/lineageos23.2/prebuilts/clang/kernel/linux-x86/clang-r416183b/bin
GCC64_BIN=/home/hajimi/lineageos23.2/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin
GCC32_BIN=/home/hajimi/lineageos23.2/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin

export PATH="$CLANG_BIN:$GCC64_BIN:$GCC32_BIN:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

make -C "$KERNEL_SRC" O="$KERNEL_OUT" \
  ARCH=arm64 SUBARCH=arm64 \
  CC=clang CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=aarch64-linux-android- \
  CROSS_COMPILE_ARM32=arm-linux-androideabi- \
  wisdom_defconfig

make -C "$KERNEL_SRC" O="$KERNEL_OUT" \
  ARCH=arm64 SUBARCH=arm64 \
  CC=clang CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=aarch64-linux-android- \
  CROSS_COMPILE_ARM32=arm-linux-androideabi- \
  olddefconfig

make -C "$KERNEL_SRC" O="$KERNEL_OUT" \
  ARCH=arm64 SUBARCH=arm64 \
  CC=clang CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=aarch64-linux-android- \
  CROSS_COMPILE_ARM32=arm-linux-androideabi- \
  -j"$(nproc)" Image dtbs
```

首次完整构建退出码为 0；随后用相同参数重复执行，`Image` 保持 up to date，
退出码仍为 0。构建中仍有 Samsung 旧驱动的 Clang warning 和 3 条 section
mismatch warning，必须结合启动日志继续审查，不能把“编译通过”等同于“设备
运行通过”。

第一阶段没有同时启用：

- Eureka Vortex/Proton Clang。
- LTO。
- 新 governor。
- 新 I/O scheduler。
- 超频/降压。

## 9. 验证矩阵

### 9.1 静态与构建

- `Makefile` 必须显示 `VERSION=4`、`PATCHLEVEL=4`、`SUBLEVEL=302`。
- `wisdom_defconfig` 能完成 `olddefconfig`。
- 对比旧/新生成 `.config`，逐项审查新 Kconfig 默认值。
- `Image` 和配置选择的 DTB/DTBO 成功生成；如 ROM 使用可加载模块，再单独验证
  `modules` 目标和模块打包。
- 记录 compiler、UTS version、Image size、SHA-256。
- 检查 boot partition 32 MiB 大小限制。

### 9.2 首次启动

- `uname -a` 和 `/proc/version` 显示实际 4.4.302。
- Android UI 可用，不能仅以 `adbd started` 判定成功。
- 无 kernel panic、Oops、连续 WARN、RCU stall 或 watchdog。
- init、vold、netd、zygote、system_server 正常。
- BPF/netd 网络初始化正常。

### 9.3 设备功能

- 相机预览、拍照和持续录像。
- Type-C 耳机播放、录音和拔插。
- 2.4G USB 接收器。
- USB MTP/ADB。
- S Pen 书写、悬浮、侧键和唤醒。
- Wi-Fi、蓝牙、移动网络、GPS。
- 充电、电量显示和 thermal。
- 内置/外置存储。
- KernelSU。
- suspend/resume、Doze 和待机。

### 9.4 性能 A/B

每次使用同一 ROM、同一 userspace 和同一环境：

| 组 | 内核 | perfd | Eureka |
| --- | --- | --- | --- |
| A | 4.4.177 | 关闭/开启 | 无 |
| B | 纯 4.4.302 | 关闭 | 无 |
| C | 纯 4.4.302 | 0.4.2 | 无 |
| D | 4.4.302 | 0.4.2 | 正确性修复 |
| E | 4.4.302 | 0.4.2 | 单个性能实验 |

记录：

- 应用启动中位数和 P95。
- SurfaceFlinger/gfxinfo 卡顿。
- sustained load 下的频率、温度和降频。
- 录像时的帧率、编码稳定性和温度。
- 屏幕关闭 10 至 30 分钟的真实断电待机。
- perfd runtime state、频率 lease 和 selective boost。

## 10. Prebuilt 替换与发布门槛

当前 device tree prebuilt 已替换为可启动并完成 ROM payload、TWRP 刷入和正常
启动验证的 4.4.302 内核。当前发布候选 Image 为：

```text
Size: 26,094,912 bytes
SHA-256: c555821d3df1099f966f6ff70e7b78072d7289fb593ae71f7084040380b1b694
Build: Android Clang 12.0.5 / r416183b
Runtime: Linux 4.4.302+ #1 SMP PREEMPT Thu Jul 30 16:41:52 CST 2026
```

要把该 prebuilt 冻结为长期发布基线，仍须满足：

1. 新内核实际启动到可用 Android UI。
2. 录像、USB 音频、2.4G 接收器和 S Pen 全部回归通过。
3. 网络、充电、存储和待机没有明显回退。
4. ROM 不声明、不打包且不启动已回退的原生 perfd 服务。
5. 没有未解释的 panic、Oops、stall 或高频 WARN。
6. 记录构建输出 SHA-256。

每次新批次替换后验证：

```bash
sha256sum \
  /home/hajimi/kobj-wisdom-4.4.302-preferidle-20260730-164032/arch/arm64/boot/Image \
  /home/hajimi/lineageos23.2/device/samsung/wisdom/prebuilt/Image \
  /home/hajimi/lineageos23.2/out/target/product/wisdom/kernel
```

三者哈希必须一致，且 ROM ZIP 中的实际 payload 还要再次验证版本字符串。

## 11. Bring-up 日志

### 2026-07-29

- [x] 核对远端 `lineage-23.2` HEAD：
  `72aae3c51a516405605a483dbe9732d498ec961a`
- [x] 核对官方 `linux-4.4.y` HEAD：
  `a09b2d8f61ea0e9ae735c400399b97966a9418d6`
- [x] 创建独立工作区：
  `/home/hajimi/android_kernel_samsung_universal7904-4.4.302`
- [x] 创建分支：
  `lineage-23.2-302`
- [x] 拉取可连接到 `v4.4.177` 的 stable 历史。
- [x] 验证官方 merge base：
  `6b50202a4d53bf527c640467bcff68b50a5e38a2`
- [x] 试运行官方 stable 直接 merge，记录 92 个冲突后撤销。
- [x] 找到同源 universal7904 Android 4.4.302 merge：
  `b128d69019a8c1dd05b10b2879c9eaa0de3207c6`
- [x] 合并同源 4.4.302 bring-up，仅产生并解决 1 个 `rtmutex` 冲突。
- [x] 创建 merge commit：
  `228624978b`
- [x] 清理旧树 Mali r26p0 `Kbuild` 残留冲突标记：
  `4383ad3f2c`
- [x] 运行 `wisdom_defconfig` 和 `olddefconfig`。
- [x] 确认 GCC 4.9 被 4.4.302 的最低版本检查拒绝，没有删除检查硬绕过。
- [x] 用 Clang r416183b（12.0.5）加 GNU 4.9 binutils 完成
  `Image dtbs` 构建。
- [x] 修复合并后暴露的源码/构建问题：
  - `8536d898c9`：删除 SELinux netlink 重复初始化。
  - `e55c6c7d06`、`99b506e7fe`：修正 TFA9872 enum 返回值及无效自赋值。
  - `4035e8c299`：删除 MobicoreDriver 的 `kref_read()` 重定义。
  - `0828708e8d`：修正 out-of-tree firmware blob 生成路径。
  - `f7989b9505`：futex requeue PI 使用现有 `put_pi_state()`。
- [x] 验证 `v4.4.302` 和原 LOS23.2 起点都在当前 HEAD ancestry 中。
- [x] 验证源码无 `<<<<<<<` 残留，`git diff --check` 无报错。
- [x] 记录首个可构建产物：

  | 项目 | 值 |
  | --- | --- |
  | Kernel release | `4.4.302+` |
  | Compiler | Android Clang `12.0.5` / r416183b |
  | UTS | `#1 SMP PREEMPT Wed Jul 29 17:38:19 CST 2026` |
  | Image | `25,863,752` bytes |
  | Image SHA-256 | `ba11e38816d624c9fe2147280ec30b7240d2b6d48d3e53754768ff3c5ace81bc` |
  | `.config` SHA-256 | `872226bf6ed1f1eb2fcc42a90593afd5cb33be261f78a038baa7412721721683` |

- [x] 生成 universal7904 DTB/DTBO：
  `exynos7904-universal7904_P_Treble.dtbo` 和
  `exynos7904-universal7904_FHD_P_Treble.dtbo`。
- [x] 完成首次上机测试并定位启动约 12.585 秒的
  `kmem_cache_alloc() -> anon_vma_fork()` panic。
- [x] 合入并验证新的启动修复：
  - `cdce2a216a`：SLUB deactivation TID；
  - `603c9d99cb`：anon-vma double reuse；
  - `f1b54a1874`：firmware blob 并行生成竞争。
- [x] 通过 TWRP 刷入新 boot，Android 完整启动，确认
  `4.4.302+` 和 `sys.boot_completed=1`。
- [x] 替换测试用 device prebuilt；prebuilt 与 ROM kernel SHA-256：
  `4fc5e9d8da2d9df5c5af630b407a516270dd4c76288a67682bdfbb5696569869`。
- [x] 审查 Eureka 第一批候选，拒绝会倒退 AOSP cpufreq 生命周期的
  `4ca9d60a459a` 和会造成 cpuidle 泄漏的 `ed13ccc89174`。
- [x] 独立适配 devfreq resume 修复：
  `010915fdef`。
- [x] 按决定回退 `exynos7904-perfd` 的 manifest、package、init 与 SEPolicy
  原生 ROM 集成；保留独立开发仓库和设备现有 Magisk 安装。
- [x] 完成空闲态内存、ZRAM、LMKD、CachedAppOptimizer、CMA 和 ION 只读基线；
  当前未观察到内存压力，下一步先做可重复生命周期 A/B。
- [ ] 构建并上机验证 `010915fdef` 的 suspend/resume。
- [ ] 对 `5401370b886c` 做独立 scheduler A/B；尚未合入。
- [ ] 完成 Type-C 音频、2.4G 接收器、S Pen、持续录像和待机回归后，冻结发布
  prebuilt。

### 2026-07-30

- [x] 从 Android 4.4 SchedTune 实现回移完整 cgroup runnable accounting、
  `schedtune.boost`、`schedtune.prefer_idle` 和 8 个 boost group，并在
  `wisdom_defconfig` 启用 `CONFIG_SCHED_TUNE` 与
  `CONFIG_CGROUP_SCHEDTUNE`。
- [x] 将 SchedTune per-task boost 应用于 Samsung HMP 的
  `hmp_load_avg`，让 Android top-app/foreground 分组能被 legacy HMP
  placement 路径实际消费。
- [x] 增加有时限的 HMP `selective_boostpulse`，并保护 boosted 或
  `prefer_idle` 任务不被 offload/down-migration 立即迁回慢核；修正
  idle-pull 使用旧候选任务判断的问题。
- [x] 为 Zinitix 触摸 IRQ 增加短时 CPU DMA latency PM QoS 请求，避免处理
  触摸 I2C 事务时进入深 idle；请求在所有加锁后的退出路径释放。
- [x] 完成 `wisdom_defconfig -> olddefconfig -> Image dtbs` 构建。最终
  Image 为 26,094,912 bytes，SHA-256：
  `c555821d3df1099f966f6ff70e7b78072d7289fb593ae71f7084040380b1b694`。
- [x] 验证 ROM ZIP 内 `boot.img` 的 kernel 与上述 Image 完全一致，并通过
  TWRP sideload 刷入；设备正常启动到
  `sys.boot_completed=1`，运行内核时间戳与本次构建一致。
- [x] 运行时确认 `/dev/stune`、top-app/foreground 分组、PowerHAL interaction
  boost 和 HMP placement 生效；`exynos7904-perfd` daemon 未运行。
- [x] 用户实测整体和普通应用滑动明显改善。现有对照不支持全局固定 CPU6-7、
  永久锁满 MIF 或绕过 CAL/ECT 将 GPU 提到 845 MHz，因此这些实验没有进入
  发布候选。
- [ ] 继续验证长时间待机、2.4G 接收器和跨应用负载；这些项目通过前不把当前
  prebuilt 标记为最终冻结版本。

## 12. 参考

- Linux stable：<https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git>
- Linux v4.x 发布目录：<https://www.kernel.org/pub/linux/kernel/v4.x/>
- 当前 wisdom kernel：
  <https://github.com/Project-Wisdom/android_kernel_samsung_wisdom>
- 同源 universal7904 4.4.302：
  <https://github.com/SamarV-121/android_kernel_samsung_universal7904>
- Google Android 4.4 common：
  <https://android.googlesource.com/kernel/common/+/deprecated/android-4.4-p>
- Eureka R24U：
  <https://github.com/eurekadevelopment/Eureka-Kernel/tree/R24U>
- exynos7904-perfd：
  <https://github.com/xuanyayi/exynos7904-perfd>
