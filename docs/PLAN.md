# VMTL — virtio-gpu + Vulkan Metal driver for macOS guests

把本仓库从「NVIDIA 物理卡 Metal 驱动」改造成「virtio-gpu 虚拟卡 Metal 驱动」的蓝图。
目标形态:Linux 宿主 + QEMU/KVM(或 crosvm),macOS 15 作为 guest,通过 virtio-gpu
获得 Metal 加速(WindowServer 合成、Metal 应用),宿主端用真实 GPU 渲染。

```
macOS guest                                   Linux host
─────────────────────────────────────────    ─────────────────────
Metal app / WindowServer
   ↓
VMTLDriver.bundle   (plugin/, 改名复用)
   ↓ AIR → SPIR-V
libvmtl_translate.dylib (translator/, 原样保留)
   ↓ 标准 Vulkan ICD 接口
Venus ICD (Mesa src/virtio/vulkan, 移植内核接口层)
   ↓ 仿真 Linux virtgpu uapi 的 IOUserClient
VirtIOGPU.kext (新写) + VirtIOFB.kext (IOFramebuffer)
   ↓ virtio PCI: controlq/cursorq + hostmem BAR
virtio-gpu-pci 设备 ─────────────────────────►  vhost-user-gpu / QEMU 渲染
                                                   ↓
                                                宿主真 GPU (RADV/NVK)
```

## 0. 为什么可行 — 已验证的接缝

原项目把 Metal 插件和内核之间做成了纯标准接口,后端可整体替换:

1. **Metal 插件只吃标准 Vulkan**。`plugin/nvmtl_vk.c` 通过 `dlopen` 一个 ICD
   (`nvmtl_vk.c:573`),要求的 14 个扩展均有回退路径("ABSENT - estimated"、
   "private image + copies"、`NVMTL_NO_SURFACE_SHARE=1` 强制回退)。
   仅有的 5 个私有符号(`dlsym` 的 `nvmtl_nvk_*`)缺席时全部优雅降级。
2. **用户态/内核接缝模式已被验证**。`nvk-macos.patch` 中的 `nvrm_xnu.c`
   把 Linux ioctl ABI 逐字节搬到 IOUserClient(fd 即句柄、`SEL_BINDFD`、
   `IOConnectMapMemory64` 映 BAR),NVK 用户态几乎零改动跑通,浏览器
   沙箱兼容问题都趟过。Venus 用同样手法。
3. **Apple 框架对接代码可整体移植**。NVAccel(IOAccelerator nub +
   MetalPluginName 加载入口)、NVRMFB(IOFramebuffer)、NVRMAGDC(策略应答)
   里「面向 Apple 的那一半」与 GPU 无关,只换掉面向 NVIDIA 的那一半。
4. **Venus 的内核接口极小**。virtgpu uapi 十来个 ioctl,对比 NVIDIA RM 的
   整个 ioctl 面小一个数量级;SPIR-V 原样交宿主编译,guest 侧连 shader
   编译器都不需要(NVK 的 NAK/cubin/ELF 那 ~5k 行全部消失)。

## 1. 命名方案

前缀 **VMTL**(Virtual Metal)。类名必须一起改:`MetalPluginClassName`
加载全局 ObjC 类名,与 NullMoth 驱动共存时重复类名会崩。

| 现在 | 改为 |
|---|---|
| `NVMTLDriver.bundle` | `VMTLDriver.bundle` |
| ObjC 类 `NVMTLDevice` | `VMTLDevice` |
| `/Library/GPUBundles/nvmtl/` | `/Library/GPUBundles/vmtl/` |
| `libnvmtl_translate.dylib` | `libvmtl_translate.dylib` |
| 环境变量 `NVMTL_*` | `VMTL_*` |
| NVK 私有符号 `nvmtl_nvk_stage_import` 等 5 个 | `vmtl_*`(Venus 侧后期提供同名实现) |
| kext Bundle ID `com.nullmoth.*` | `io.github.<user>.*` |

改名涉及:`plugin/` 全部源码、`build/build_plugin.sh`、`build/build_xlate.sh`、
`kexts/NVRM/accel/Info.plist`(MetalPluginName/ClassName)、
`package/install.sh:193-194`、`package/uninstall.sh`、
`app/Sources/main.swift:165,425`、`tools/test_install_guard.py`、
`tools/test_support_install.py`、`plugin/.gitignore`。
`build_plugin.sh` 里查 NVK 符号的 marker 检查项标 TODO(接 Venus 后重写)。

## 2. 组件处置清单

| 组件 | 处置 | 说明 |
|---|---|---|
| `translator/` (LGPL) | 原样保留 | SPIR-V 是可移植 IR,零改动 |
| `plugin/` (PolyForm 非商业) | 改名后复用 | 本项目同样非商业开源,许可兼容;显存导入段(P4)改 Venus blob mmap |
| `nvk/` NVK 本体 | 丢弃 | guest 侧不编译机器码;只参考其 macOS 构建修法清单 |
| `kexts/NVRM` | 替换为 VirtIOGPU.kext | 新写:virtio PCI + virtqueue + capset + hostmem BAR + uapi 仿真 IOUserClient |
| `kexts/NVRMFB` | 移植为 VirtIOFB.kext | IOFramebuffer 骨架保留;NVKMS head 查询换 capset;display info + EDID;vblank 定时器模拟 |
| `kexts/NVAccel` | 移植为 VirtIOAccel.kext | 加速器 nub + Metal 插件加载入口保留;surfshare/pageoff 换 blob/hostmem 路径(P4) |
| `kexts/NVRMAGDC` | 照抄 | 纯静态策略应答 |
| `kexts/NMHDA`、`NMIntelFB` | 丢弃 | 音频/Intel 显示,VM 无关 |
| `app/` (1401) | 后期裁剪 | 变成 VM 安装器;VM 里无需 OpenCore 引导处理 |
| NVIDIAShared.bundle / GSP 固件 | 丢弃 | 插件 dlopen 失败已优雅回退 |

## 3. 路线图

### P0 — 改名 + 宿主环境(1-2 周)
- 执行第 1 节改名,重建 repo 结构:`kexts/{VirtIOGPU,VirtIOFB,VirtIOAccel}/ venus/ vmtl-plugin/`
- Linux 宿主:QEMU/KVM + OVMF + `virtio-gpu-pci`(venus=true,带 hostmem BAR)装 macOS 15
- 验证:VM 跑到固件屏/安装器;virtio-gpu 的 BAR 布局能过 macOS PCI 设置

### P1 — VirtIOGPU.kext + VirtIOFB.kext(4-8 周)
- virtio 设备初始化、controlq 协议(GET_DISPLAY_INFO、GET_CAPSET、
  RESOURCE_CREATE_2D、TRANSFER_TO_HOST_2D、FLUSH 等 dumb 路径)、EDID capset
- IOUserClient 逐字节仿真 Linux virtgpu uapi(ioctl 号 + 结构体)
- VirtIOFB:从 NVRMFB 剥离 NVKMS,接 capset
- **产出:桌面软件合成可用** — 本身就是可发布的独立成果
- 参照:Linux `drivers/gpu/drm/virtio/`、NVRMFB 源码

### P2 — Venus ICD 上 macOS(4-8 周)
- Mesa `src/virtio/vulkan`:virtgpu DRM 后端换成 P1 的 IOUserClient shim(约 1.5-2k 行)
- Mesa on macOS 构建修法抄 NVK patch 趟过的坑(无 libdrm、无 memfd 等)
- **产出:vkcube 在 VM 里跑**(无需 Metal,独立验证)
- caps 普查:插件要的 14 个扩展逐个核对(宿主用 RADV 起步,NVK 宿主验证 RT)
- 参照:`nvk-macos.patch` 的 `nvrm_xnu.c` / `nvkmd/nvrm/nvkmd_nvrm_*.c`

### P3 — Metal 插件接入(4-8 周)
- VirtIOAccel 最小版:加速器 nub + `MetalPluginName`/`MetalPluginClassName` 属性
- 插件 ICD 指向 `libvulkan_venus.dylib`(默认路径常量或 `VMTL_ICD_LIB`)
- 5 个私有符号缺席 → 走插件现成回退(sample locations 关、IOSurface copy-out)
- **产出:Xcode Metal 示例在 VM 里渲染**;MPS/NVIDIA 编译器路径静默失败,无碍

### P4 — 真共享显存(6-12 周)
- virtio blob resource + hostmem BAR:IOSurface 页面落 guest 物理页,
  宿主经 udmabuf/DMA-BUF 导入真 GPU(Venus blob 模型)
- kext 侧 `IOMemoryDescriptor` 映 BAR 页,替换 surfshare/pageoff 拷贝语义
- Venus shim 实现 `vmtl_surface_dirty`(→ resource flush)、
  `vmtl_stage_import`(→ blob 创建提示),插件快路径复活
- **产出:WindowServer GPU 合成,IOSurface 不再逐帧拷出**

### P5 — 打磨发布
- vhost-user-gpu 性能、多队列;1401 app 裁剪成 VM 安装器;
  CI:宿主 headless QEMU + SSH 进 VM 跑回归;文档

**总量**:新写 + 移植约 8-10k 行;复用 16k 行插件 + 2k 行 translator。
单人全职:P3(Metal app 能跑)约 3-5 个月;P4(桌面全加速)再 2-3 个月。

## 4. 风险与对策

| 风险 | 对策 |
|---|---|
| Venus caps 缺口(`external_memory_host`、`sample_locations`、`calibrated_timestamps`) | P2 caps 普查;插件每个缺口都有 "ABSENT" 回退,最差降级不崩溃 |
| RT / cooperative matrix 透传 | 短期 Metal RT API 返回 nil,应用自降级;宿主 NVK 组合实测 |
| IOAccelerator 家族隐式契约 | 参照实现即源码;调试大头在 P3/P4 |
| vblank 缺失 | 定时器模拟;NVRMAGDC 平滑策略有现成答案 |
| macOS 15 guest + OVMF GOP 组合 | P0 验证;有坑退回 VGA 循环-blit(性能差,不影响 P2/P3) |
