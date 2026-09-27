# Legacy LOS23.2 Linux 4.4.177 BPF Bring-up Report

> Historical record only. This report describes the superseded 4.4.177 BPF
> bring-up from June 2026; it does not describe the current Android 17 kernel.

## Scope

This workspace is the corrected 4.4.177-based LOS23.2 eBPF bring-up for
Samsung SM-P205 / wisdom:

- Path: `/home/hajimi/lineageos23.2/los23_kernel`
- Branch: `kernel-4.4.177-los23.2-ebpf`
- Source base commit: `d6d61c0be431b3eea4c2f0c3a5c5e7e34a7c1ca9`
- Source version: `VERSION = 4`, `PATCHLEVEL = 4`, `SUBLEVEL = 177`

The earlier 4.4.302 attempt is not the active base. It was moved aside at:

- `/home/hajimi/lineageos23.2/los23_kernel-302-abandoned-20260627-025933`

The known-good LOS22.2 4.4.177 prebuilt remains untouched:

- Path: `/home/hajimi/lineageos22.2/device/samsung/wisdom/prebuilt/Image`
- SHA-256: `26a0e6a465becbb34e60a3bd1f9cf2d2dcb9e0fcc6107d3fee81485e900e2170`

## Config Source

`arch/arm64/configs/wisdom_defconfig` was generated from the known-good
4.4.177 prebuilt kernel's embedded IKCONFIG, then minimally adjusted for
LOS23.2 eBPF/cgroup compatibility.

Effective `out-gcc/.config` additions checked after `wisdom_defconfig`:

- `CONFIG_CGROUP_PIDS=y`
- `CONFIG_CGROUP_DEVICE=y`
- `CONFIG_CGROUP_BPF=y`
- `CONFIG_BPF=y`
- `CONFIG_BPF_SYSCALL=y`
- `CONFIG_NETFILTER_XT_MATCH_CGROUP=y`
- `CONFIG_NET_CLS_BPF=y`
- `CONFIG_NET_ACT_BPF=y`
- `CONFIG_CGROUP_NET_CLASSID=y`
- `CONFIG_BPF_JIT=y`
- `CONFIG_DM_VERITY_FEC=y`

`CONFIG_MALI_KUTF` is explicitly disabled because the Mali kernel unit-test
module trips old Samsung kbuild parsing in standalone `Image-dtb` builds and is
not needed for the boot image.

## eBPF Compatibility Changes

The current patch is a targeted Android 16 compatibility pass, not a wholesale
modern BPF subsystem backport.

Implemented:

- Add modern map enum values needed by Android userspace headers:
  `BPF_MAP_TYPE_LPM_TRIE`, `BPF_MAP_TYPE_DEVMAP`, `BPF_MAP_TYPE_DEVMAP_HASH`.
- Add `BPF_F_RDONLY_PROG`, `BPF_F_WRONLY_PROG`, and `BPF_OBJ_NAME_LEN`.
- Extend `union bpf_attr` enough to accept newer `BPF_MAP_CREATE` and
  `BPF_PROG_LOAD` payloads.
- Clear unsupported newer map/program fields before the old 4.4 verifier and
  attr checker run.
- Extend the BPF command enum and `union bpf_attr` layout for Android 16
  userspace wrappers, including id lookup, fd-info, test-run, BTF, and batch
  command payload shapes.
- Return explicit unsupported/empty results for modern commands this 4.4 tree
  does not implement, instead of letting newer nonzero payload fields fail the
  generic syscall tail check first.
- Add minimal `BPF_OBJ_GET_INFO_BY_FD` map/prog metadata for legacy
  compatibility checks.
- Add `BPF_MAP_LOOKUP_AND_DELETE_ELEM` as lookup followed by delete for map
  types that already support both operations.
- Downgrade `BPF_MAP_TYPE_DEVMAP_HASH` to `BPF_MAP_TYPE_HASH`, matching
  Android bpfloader behavior on kernels older than 5.4.
- Allow hash map creation to tolerate program-side readonly/writeonly flags.
- Add a minimal kernel-side `BPF_MAP_TYPE_LPM_TRIE` implementation for Android
  16 `local_net_access_map` semantics: create, update, longest-prefix lookup,
  delete, and key iteration. This is intentionally smaller than the S8
  universal8895 full upstream trie backport, but it is real kernel map support,
  not a userspace bypass.
- Add the Android userspace-visible BPF JIT sysctl
  `/proc/sys/net/core/bpf_jit_kallsyms`, matching the S8 LOS23.2 kernel shape.
  The 2026-06-29 09:07 TWRP log showed `NetBpfLoad` exiting because this node
  was missing, and init then rebooted with `bpfloader-failed`.

Not implemented in this stage:

- Full `DEVMAP`/`DEVMAP_HASH` datapath semantics.
- Real BPF id registries or fd-by-id lookup semantics; `GET_NEXT_ID` currently
  returns the empty-kernel result expected by Android's early sanity check.
- BTF, func-info, line-info, ringbuf, sk-storage, or cgroup sockopt backports.
- Batch map operations beyond explicit `-EOPNOTSUPP`.
- Tracepoint BPF enablement; this Samsung 4.4.177 arm64 tree does not select
  the kprobe/uprobes dependencies required for `CONFIG_BPF_EVENTS`.

## Build Verification

Clang/LLVM was tried first but this old Samsung 4.4.177 tree fails under the
newer clang toolchain on existing driver warnings promoted to errors. The
successful verification used the GCC 4.9 aarch64 toolchain, matching the
known-good 4.4.177 prebuilt lineage more closely.

Command:

```sh
PATH=/home/hajimi/lineageos23.2/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin:$PATH \
make O=out-gcc ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- Image-dtb -j$(nproc)
```

Previous result:

- Output: `out-gcc/arch/arm64/boot/Image-dtb`
- Size: `25394952` bytes
- Timestamp: `2026-06-29 06:22:24.959921664 +0800`
- SHA-256: `eda8a0e9ceb2ab6a9762579d9ce75f0c34f797504bb2c4a6892aeb94cdee2ca0`

The payload was copied to:

- `/home/hajimi/lineageos23.2/device/samsung/wisdom/prebuilt/Image`

Then `m bootimage` completed successfully in the LOS23.2 tree. Result:

- `out/target/product/wisdom/kernel` SHA-256:
  `eda8a0e9ceb2ab6a9762579d9ce75f0c34f797504bb2c4a6892aeb94cdee2ca0`
- `out/target/product/wisdom/boot.img` SHA-256:
  `7fc32f2c490886f47d9db663f750be3fa8370cf4b2f85dd8c9e5ec31ecfd9ea9`
- The flashable zip was not rebuilt during this kernel-only verification.

Previous result after adding minimal `BPF_MAP_TYPE_LPM_TRIE` map support:

- Output: `out-gcc/arch/arm64/boot/Image-dtb`
- Size: `25427720` bytes
- Timestamp: `2026-06-29 07:08:56.935356859 +0800`
- SHA-256: `e63d1297352d957fc8acab2d53d9f5c75e5a1fe2897ea264b171419106c91dd8`
- The payload was copied to
  `/home/hajimi/lineageos23.2/device/samsung/wisdom/prebuilt/Image`.
- `m bootimage -j"$(nproc --all)"` completed successfully.
- Kernel payload SHA-256 in `Image-dtb`, `device/.../prebuilt/Image`,
  and `out/target/product/wisdom/kernel`:
  `e63d1297352d957fc8acab2d53d9f5c75e5a1fe2897ea264b171419106c91dd8`
- `out/target/product/wisdom/boot.img` SHA-256:
  `219e68ecd15f8f38d18142c2c1aeb04395acc40f90fb6b31f4fca6c14035e822`
- Unpacked `boot.img` kernel matches the same `e63d1297...` payload and reports:
  `Linux version 4.4.177+ ... #8 SMP PREEMPT Mon Jun 29 07:08:47 CST 2026`.
- The flashable zip was not rebuilt during this kernel-only verification.

Current result after adding the `bpf_jit_kallsyms` sysctl:

- Output: `out-gcc/arch/arm64/boot/Image-dtb`
- Size: `25427720` bytes
- Timestamp: `2026-06-29 09:11:23.593856404 +0800`
- SHA-256: `875805d184a3c2ef551e73fa6823ad451c77bcb7b09acbec2fbcfe2f60bb21de`
- The payload was copied to
  `/home/hajimi/lineageos23.2/device/samsung/wisdom/prebuilt/Image`.
- `m bootimage -j"$(nproc --all)"` completed successfully.
- Kernel payload SHA-256 in `Image-dtb`, `device/.../prebuilt/Image`,
  `out/target/product/wisdom/kernel`, and unpacked `boot.img` kernel:
  `875805d184a3c2ef551e73fa6823ad451c77bcb7b09acbec2fbcfe2f60bb21de`
- `out/target/product/wisdom/boot.img` SHA-256:
  `5c6bfcd0c2bf34ca7a4f278447c2a6f50342d1ccded859a5564bc55ba7769bc7`
- Unpacked `boot.img` kernel matches the same `875805d1...` payload and reports:
  `Linux version 4.4.177+ ... #9 SMP PREEMPT Mon Jun 29 09:11:11 CST 2026`.
- The flashable zip was not rebuilt during this kernel-only verification.

## Remaining Risk

This confirms that eBPF-related kernel config, loader-facing ABI shims, and the
minimal LPM trie map build on the 4.4.177 base. It also clears the known
`/proc/sys/net/core/bpf_jit_kallsyms` ENOENT blocker from the latest
`bpfloader` crash. It does not prove LOS23.2 can boot.

ROM-side follow-up performed after this kernel build:

- `packages/modules/Connectivity/bpf/loader/NetBpfLoad.cpp` now relaxes the
  global kernel-version gate only for SM-P205/exynos7904 bring-up.
- Because the 4.4.177 kernel now has real `BPF_MAP_TYPE_LPM_TRIE` support, the
  loader also allows that specific backported map type through the declared
  4.9/4.14 map gates on wisdom. This is intentionally limited to LPM trie maps.
- `m netbpfload` and `m com.android.tethering` completed successfully. The
  rebuilt `com.android.tethering.apex` embeds the updated `netbpfload`.

Remaining risk: BPF program `min_kver` gates are still preserved. Programs
whose metadata says `min_kver=4.9` are not forced into the old 4.4 verifier
unless the matching kernel-side program/helper work is actually backported.
This keeps the current policy aligned with the real 4.4.177 source base instead
of pretending it is a 4.9+ kernel.
