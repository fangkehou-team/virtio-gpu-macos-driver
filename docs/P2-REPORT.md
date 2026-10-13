# P2 报告 — Venus ICD 上 macOS(vkcube 里程碑)

日期:2026-10-13。P2 全部产出已达成:

1. **vkcube 等价物在 VM 里跑通**:离屏旋转立方体,真实 graphics
   pipeline(顶点/片元着色器、深度测试、透视投影、push constant),
   8 帧旋转动画,42/42 检查全绿。工具:`vmtl-cube.c`。
2. **caps 普查**:插件要的 14 个扩展逐个核对,7/14 存在,
   每个缺席项都有已知原因和插件侧回退。工具:`vmtl-caps.c`。
3. **端到端 GPU 往返**:instance → 物理/逻辑设备 → 队列 →
   host3d blob → `vkCmdFillBuffer` → 回读,23 项冒烟测试
   连续 16 次全绿(单次开机内)。工具:`vmtl-icdtest.c`。

## 环境链路

```
macOS guest ── IOUserClient(VirtIOGPU.kext,P1 的 uapi shim)
    ── VirtIO GPU 队列 ── QEMU virtio-gpu Venus
    ── virgl_render_server(tmp/virgl-venus,host)── RADV(宿主 RENOIR gfx9)
```

Mesa `src/virtio/vulkan` 用 `vn_renderer_vmtl.c` 后端
(`-DVN_USE_VMTL`,darwin 专属),ICD 直接以
`libvulkan_virtio.dylib` 加载(guest 无 Vulkan loader,测试程序
dlopen ICD 入口点直驱)。

## caps 普查结果(RADV RENOIR 宿主)

设备:Virtio-GPU Venus / RADV,api 1.4.334,vendor 0x1002,
device 0x1638;暴露 151 个设备扩展。插件 `kWant[14]`
(plugin/vmtl_vk.c)逐项:

| 扩展 | 状态 | 原因 / 插件回退 |
|---|---|---|
| VK_EXT_extended_dynamic_state3 | ✅ | g_dyn3 开 |
| VK_EXT_depth_clip_enable | ✅ | g_clip 开 |
| VK_EXT_image_drm_format_modifier | ✅ | |
| VK_KHR_external_memory_fd | ✅ | |
| VK_EXT_mutable_descriptor_type | ✅ | |
| VK_EXT_calibrated_timestamps (KHR+EXT) | ✅ | |
| VK_EXT_sample_locations | ✅ | |
| VK_EXT_memory_budget | ❌ | Venus 侧 `VN_DEBUG(MEM_BUDGET)` 门控(vn_physical_device.c);插件有 ABSENT 回退 |
| VK_KHR_acceleration_structure | ❌ | gfx9 无 RT 硬件;g_rt 关 |
| VK_KHR_ray_query | ❌ | 同上 |
| VK_KHR_deferred_host_operations | ❌ | 随 RT 组一起缺席 |
| VK_EXT_external_memory_host | ❌ | Venus 已知缺口(plan §3);插件走 IOSurface copy-out |
| VK_KHR_cooperative_matrix | ❌ | gfx9 无 WMMA |
| VK_KHR_pipeline_executable_properties | ❌ | 无 Venus wire 协议支持 |

插件门控实测:g_dyn3 ON、g_clip ON、g_budget OFF、g_rt OFF;
`bufferDeviceAddress=1`、`accelerationStructure=0`、`rayQuery=0`。
所有缺口均有插件文档化回退,最差降级不崩溃(符合 plan §4 预判)。

WSI:venus 构建无 wayland/x11/kms 平台(实例层仅
`EXT_headless_surface`),因此 vkcube 里程碑采用离屏渲染路径
—— 与 guest 无窗口系统可用的事实一致。屏幕显示走 P1 的
2D scanout 路径(P4 打通真共享显存后合并)。

## 过程中的关键修复

- **kext 每 IOUserClient 上下文所有权**(fa01a58):之前所有
  submit/blob 都路由到"第一个用过的"context,第二个连接即坏。
  现在 client 连接持有 `fCtxIdx` 槽位,clientClose(含进程崩溃)
  释放该 session 的 bo 并发 CTX_DESTROY;context id 用空闲扫描
  分配并回收。
- **`vk_alloc(NULL)` 崩溃**(mesa 83be59d):本树 vk_alloc.h 无条件
  解引用 `pfnAllocation`;renderer 内部对象改用 calloc/free
  (与 Linux virtgpu 后端同款习惯)。
- **读回路径**:color attachment 是宿主 OPTIMAL tiling,
  直接 map blob 按 linear 读出来是花屏。必须
  `vkCmdCopyImageToBuffer` 到 linear host-visible buffer 再读
  (vmtl-cube.c 已改)。
- 构建注意:guest 端 ICD 增量构建
  `ninja src/virtio/vulkan/libvulkan_virtio.dylib`;
  测试程序 `clang -O1 -o vmtl-cube vmtl-cube.c -I ~/mesa/include
  ~/mesa/build-venus/src/virtio/vulkan/libvulkan_virtio.dylib`;
  着色器在宿主 `glslangValidator -V` 编译,`xxd -i` 生成头。

## P2 验收对照(PLAN.md)

| 计划项 | 状态 |
|---|---|
| Mesa virtgpu 后端换 IOUserClient shim(1.5-2k 行) | ✅ vn_renderer_vmtl.c |
| macOS 构建修法 | ✅ meson darwin 分支(b_lundef=false、dead_strip、去 vtest) |
| vkcube 在 VM 里跑 | ✅ 离屏 8 帧旋转立方体 42/42 |
| caps 普查 | ✅ 7/14,缺口与回退逐项文档化 |

## 遗留 / 后续

- QEMU 偶发优雅关机 SIGSEGV(写盘已 flush,kill -9 无损)。
- tmp/virgl-venus 的 VMTLDBG 插桩待稳定后移除。
- `EXT_memory_budget` 可用 `VN_DEBUG(MEM_BUDGET)` 打开验证插件
  budget 路径(可选)。
- P3:Metal 插件接入(VirtIOAccel nub + MetalPluginName 属性,
  插件 ICD 指向 libvulkan_venus.dylib)。
