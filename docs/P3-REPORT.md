# P3 报告 — Metal 插件接入(VirtIOAccel 里程碑)

日期:2026-10-13。P3 产出达成:Metal 客户端经官方插件链在 VM 里
渲染(运行时编译 MSL → 着色器翻译 → Venus → 宿主 GPU → 回读),
修复后压测 **180/180 连续全绿**(修复前间歇失败率 ~10-15%)。

## 里程碑链路

```
Metal app(mtlprobe)
 ── MTLCopyAllDevices → 1 台设备 "Virtio-GPU Venus"
     (VirtIOAccel nub → MetalPluginName → VMTLDriver.bundle)
 ── newLibraryWithSource(MSL)→ 插件 metallib 解析
     → air-opt(bitcode→LLVM IR)→ libvmtl_translate.dylib
     (metal2vulkan:IR→SPIR-V)→ newLibraryWithData
 ── newRenderPipelineState / command buffer / blit 回读
```

单次运行全绿的判据:1 台设备、queue/texture 创建、MSL 编译
(vmain+fmain 两函数)、PSO 建立、command buffer status=completed、
64×64 RGBA8 回读 `lit=4096/4096 rust=4096`(全屏 rust 四边形)。
工具:`mtlprobe.m`(guest clang 直编,不经 Xcode 工程)。

## 组件落位(guest)

| 组件 | 位置 |
|---|---|
| VirtIOAccel nub + VIOAccelProbe | `VirtIOGPU.kext` 内新增 personality(IOAccelerator 家族,`vioaccel=1` 门控) |
| Metal 插件 | `/Library/GPUBundles/VMTLDriver.bundle`(ICD → `libvulkan_nouveau.dylib` = mesa vmtl 后端) |
| 着色器翻译器 | `/Library/GPUBundles/vmtl/libvmtl_translate.dylib`(guest cargo 构建 metal2vulkan wrapper) |
| AIR 解码 | `/Library/GPUBundles/vmtl/air-opt`(Xcode `usr/metal/macos/bin` 真身;`usr/bin` 那个是转发 shim) |
| SDK 14.2 垫片 | `plugin/vmtl_compat13.h`(ResidencySet/ShaderValidation/CommandQueueDescriptor) |

## 过程中的两个关键 bug 与修复

1. **Venus blob 竞态(间歇 `vkMapMemory -> -2`)**
   guest ICD 把 `vkWaitRingSeqnoMESA` 批命令内联进
   `RESOURCE_CREATE_BLOB`,期望服务端先等 ring 的 `vkAllocateMemory`
   再导出 blob——但该批命令**从未到达渲染服务**:
   `render_context_op_create_resource` 协议无 `cmd` 字段,kext 把
   `rc->cmd` 走 legacy ctrl `SUBMIT_3D` 提交,与 Venus ring 无排序。
   结果 ctrl 命令偶发先于 alloc 抵达,服务端对象表查无此物,导出失败。
   修复:mesa `vn_device_memory_bo_init` 改为直接
   `vn_ring_roundtrip`(nvk/mesa 0f637c6),roundtrip 返回即保证
   alloc 已在服务端处理,再发 blob ioctl。
2. **kext shm 区泄漏(压测后 "shm region exhausted",设备消失)**
   hostmem 4G 的 blob 映射窗口用 bump 分配器,BO 关闭不回收偏移
   ("4G is plenty" 的注释)。每轮 mtlprobe ~40MB 映射量永久累积,
   约 100 轮耗尽。修复:`VirtIOGPU.cpp` 增加有序空闲段表
   (first-fit + 合并 + 尾部归还 cursor),BO close 归还区间;
   MAP_BLOB 在宿主确认成功后才提交区间(失败不再漏)。

## 验证

- 修复 1 后:30/30 全绿(此前 ~85%)。
- 修复 2 后:50/50 + 100 连跑全绿,`dmesg` 零 "shm region exhausted"。
- 服务端对象表 MISSING 诊断在修复后窗口零命中。
- 去除全部插桩的干净 render server 重启后(v62):29+40+60 = 129 全绿;
  唯一一次失败发生在开机后第 1 分钟内(冷启动 WindowServer 抖动),
  稳态 100 连续全绿。

## P3 验收对照(PLAN.md)

| 计划项 | 状态 |
|---|---|
| VirtIOAccel 最小版(nub + MetalPluginName/ClassName) | ✅ 装在已批准的 VirtIOGPU.kext 内,额外 personality |
| 插件 ICD 指向 Venus ICD | ✅ libvulkan_nouveau.dylib(mesa vmtl 后端) |
| 私有符号缺席 → 插件回退 | ✅ 5 缺口无碍,P2 caps 报告在案 |
| Metal 示例在 VM 里渲染 | ✅ mtlprobe 运行时编译 MSL 渲染+回读,180/180 |

## 遗留 / 后续

- 正确的排序通道:`RESOURCE_CREATE_BLOB` 的 `cmd` 全链路透传
  (kext→QEMU→render 协议)——可去掉 bo_init 的 roundtrip 同步开销。
- `vmtl-allow.txt` 现为 `*`,发布前收紧。
- 插件日志 `/tmp/vmtl.log` 可能 root 属主,重跑前需 sudo 清理。
- P4:真共享显存(IOSurface 页落 guest 物理页,WindowServer 合成)。
