# P4 设计 — 真共享显存(IOSurface 直落 blob 页)

日期:2026-10-13。基于全链路代码勘察(plugin/、kexts/、nvk/mesa、tmp/virgl-venus)。

## 0. 现状一句话

插件现有的 IOSurface 快路径(`vmtl_nvk_stage_import` / `vmtl_nvk_surface_dirty`,
dlsym 自 ICD)是给 NVRM kext + NVIDIA 真卡设计的:内核把 IOSurface 的
RM 句柄共享进调用方 client(`nvAccelSurfVram`),翻页时 CE 把 VRAM 拷回
IOSurface 页(`NVResource::pageoff`)。我们的部署里 `VMTL_ICD_LIB` 就是
Venus ICD,这五个符号缺席 → 插件退到 `vmtlSurfaceOut/In` 逐帧 CPU 拷出
(`plugin/VMTLObjects.m:1052-1076`,IOSurfaceLock + 全图 readback)。

## 1. P4 的核心洞察:方向反过来

NVIDIA 方案:IOSurface 页在系统内存,GPU 在 VRAM 渲染,翻页拷回。
virtio 方案天然相反——**IOSurface 页本身就是共享资源**:

```
IOSurface(guest 普通内存,物理页列表)
  → kext 注册为 VIRTIO_GPU blob(BLOB_MEM_GUEST,mem_entry=物理页)
  → QEMU glue: guest iov → virtio_gpu_init_udmabuf → DMA-BUF fd
  → render server: vkImportMemory(DMA-BUF)→ RADV 直接对 IOSurface 页渲染
  → guest CPU(WindowServer)通过 IOSurface 直接读同一批页
```

零拷贝:guest RAM 本来就是宿主 memfd(16G vgpu-mem,share=on),
宿主 GPU 经 udmabuf 写入与 guest CPU 读取天然一致(x86 DMA 一致性)。
这与现有 `qemu-virtio-gpu.c` 已实现的 `BLOB_MEM_GUEST + SHAREABLE`
(`virtio_gpu_init_udmabuf`,qemu-virtio-gpu.c:373-374)严丝合缝——
只是该路径目前只吃 ATTACH_BACKING 的内核缓冲,不吃任意 IOSurface 页。

## 2. 分片实施

### P4.1 — IOSurface 页 → blob → VkImage(本阶段核心,无 WindowServer)

1. **kext**(`VirtIOGPU` UAPI):新增 `UAPI_STAGE_IMPORT(surfaceID, plane, size)`
   → `IOSurfaceRoot::lookupSurface`(NVRM accel 已用同款私有符号,
   kexts/NVRM/accel/nvrm-accel.cpp:1184,复用其解析手法)→ 取
   `IOSurface::getMemoryDescriptor` 的物理页 → 走
   `uapiBlobCreateLocked` 的 GUEST 分支(把 `mem_entry.addr` 从
   `IOBufferMemoryDescriptor` 泛化为任意物理页)→ 返回 blob_id/res_id。
2. **ICD**(mesa vmtl 后端):实现 `vmtl_nvk_stage_import(surfaceID, plane, need)`
   → kext ioctl → 记 `surfaceID → resourceId`;随后插件的
   `vkAllocateMemory`(OPAQUE_FD + /dev/null 占位 fd)在导入路径命中
   staged 表 → `VkImportMemoryResourceInfoMESA(resourceId)`
   (mesa 已有该导入扩展,vn_device_memory.c:142-153)。
3. **`vmtl_nvk_surface_dirty`**:令 `vmtlSurfaceOut` 的 `_vramOn` 快路径
   生效(标脏→ 空 noop 或 RESOURCE_FLUSH 提示),消除逐帧拷出。
4. **验收测试**(不依赖插件):guest 程序建 IOSurface → stage_import →
   Venus 在该 VkImage 上渲染四边形 → CPU 读 IOSurface 字节断言像素。
   这是"真共享显存"的最小端到端证明。

### P4.2 — 接进插件与 WindowServer

- 插件 `vmtl_surface_image_create`(`VMTLDevice.m:699-724`)对
  CAMetalLayer 表面走新快路径;`vmtlRecordDrawable` 提交后
  `vmtl_surface_dirty`。
- 翻页/合成:VIOAccelSurface 目前 copyFrom/ToBuffer=false、
  swapCopy 忽略(kexts/VirtIOGPU/VirtIOAccel.cpp:208-227)——
  WindowServer GPU 合成要补 IOAccelerator 家族的 surface 方法
  (参照 NVRM accel 的 NVResource 语义),或先走 RESOURCE_FLUSH
  到 VirtIOFB scanout(P1 路径)保证桌面可见。

## 3. 勘察结论(复用/缺口清单)

| 组件 | 已有 | P4 缺口 |
|---|---|---|
| kext blob/MAP_BLOB/UAPI | uapiBlobCreate + MAP_BLOB→shmPa + clientMemoryForType(P3 验证) | STAGE_IMPORT + GUEST 分支泛化 |
| QEMU glue | BLOB_MEM_GUEST+SHAREABLE → udmabuf(qemu-virtio-gpu.c) | mem_entry 泛化核验;virtio 变体(qemu-virtio-gpu-virgl.c)同步 |
| render server | DMA-BUF 导出/导入、OPAQUE blob(P3 验证) | udmabuf fd_type 与导入路径贯通 |
| mesa ICD | blob 创建/映射、resource 导入 | stage_import/dirty 两个 dlsym + staged 表 |
| 插件 | 快路径代码全在,等符号 | 5 私有符号 ABI 对齐(vmtl_vram_rec{3,surfaceID,plane,0,need}) |
| NVRM accel | IOSurface 内核机制(lookupSurface/deviceCacheForIOSurface/pageoff) | 仅作参照,不改(src/asahi 式禁区?) |

## 4. 未决问题(P4.1 期间回答)

1. `IOSurfaceRoot::lookupSurface` 在 VirtIOGPU kext 里的解析方式
   (NVRM accel 怎么链接私有符号——抄它的)。
2. IOSurface 页的物理地址获取:`getMemoryDescriptor` + 
   `IOMemoryDescriptor` 页遍历(wired? wire 需要否)。
3. udmabuf 数量限制 / QEMU `init_udmabuf` 对多散页的 tile 尺寸约束
   (IOSurface stride/tiling 对齐 vs VkImage DRM modifier)。
4. seed-bump:`IOSurfaceLock` 会推 seed 触发重传(G17 noseed 实验,
   VMTLObjects.m:907);P4.1 用只读锁,插件层另行处理。
5. headless 门(`vmtl_gpu_headless`,vmtl_vk.c:1695)在纯 virtio
   环境下应关(无 NVAccel 竞争者),fast path 才会开。
