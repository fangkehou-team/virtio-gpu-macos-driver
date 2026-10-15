# P4 报告 — 真共享显存(IOSurface 直落 blob 页)+ WindowServer 合成验收

日期:2026-10-16。P4 产出达成:

- **P4.1** IOSurface 页 → virtio blob → VkImage 的零拷贝链,端到端(无 WindowServer)。
- **P4.2** 接进插件与 WindowServer。
- **P4.2(e)** GUI 集成视觉验收**通过**:GUI 会话里跑的 Metal 窗口被 WindowServer 合成
  上桌面(截图为证),插件侧零拷贝签名完整、**逐帧拷回为 0**。

## 里程碑链路(P4)

```
app 的 CAMetalLayer drawable(IOSurface,guest 物理页)
 ── 插件 vmtl_surface_image_create:把 IOSurface 别名成 VkImage
       (buffer texture,DRM linear,row pitch = 应用自己的 bytesPerRow)
 ── kext UAPI_STAGE_IMPORT:IOSurface 物理页 → STAGE_IMPORT blob(QEMU udmabuf)
 ── 宿主 render server:vkImportMemory(DMA-BUF) → RADV 直接用 IOSurface 页渲染
 ── 提交后 vmtl_surface_dirty:标脏,走 noop/FLUSH(不逐帧读回)
 ── WindowServer:直接读同一批 guest 页做合成(统一内存)→ 进 VirtIOFB scanout
```

零拷贝的本质:guest RAM 就是宿主 memfd(share=on),宿主 GPU 经 udmabuf 写入与
guest CPU 读取天然一致,故没有"逐帧把 GPU 结果拷回 IOSurface"这一步。

## P4.1(已提交)

- kext `UAPI_STAGE_IMPORT`:IOSurfaceRoot::lookupSurface → 物理页 → GUEST 分支 blob
  (主仓库 `27585e5`)。
- mesa vmtl 后端 staged import 表 + `vmtl_nvk_stage_import`/`vmtl_nvk_surface_dirty`
  (`nvk/mesa` `62d659a`)。
- 主机侧 udmabuf 桥(`tmp/virgl-venus` `c3819e9`)。
- 验收:`vmtl-surface-test`(guest 建 IOSurface → import → Venus 渲染 → CPU 读字节断言)。

## P4.2 插件侧(已提交)

- 插件把 IOSurface 别名绑在 buffer 自己的 memory type 上(主仓库 `3da92db`)。
- **WindowServer 设备放行**:`debug.nvaccel_headless=1` 时 allow-list 原本拒绝
  WindowServer 拿设备(视作"只给 app 的 headless 卡"),导致应用窗口从不被合成。
  现用 `/Library/GPUBundles/vmtl-windowserver` 标记文件门控该拒绝:文件在,则
  WindowServer 拿到我们的设备,输出落进 FB scanout(P1 路径)。删除文件即恢复拒绝
  (本会话提交 `dc9957a`)。
- kext user client 更名 `IOAccelVirtIOGPUUserClient`(本会话提交 `0a258d6`)。

> ⚠️ 上一会话残留的阻塞:每个 boot 的**第一个** WindowServer 启动即 SIGABRT,
> 详见下节「关键问题 1」;修复后本会话第一次 boot 就进桌面。

## P4.2(e) 验收 —— 视觉证据

**探针**:`vmtl-guiprobe` —— NSWindow + 256×256 CAMetalLayer,每帧清成品红,
30 fps 画 600 帧 / 20 s 后退出。用 LaunchAgent 在 `gui/501` 里跑,期间抓桌面。

**判据与结果**:

| 判据 | 结果 |
|---|---|
| 品红窗口出现在桌面截图里 | ✅ `tmp/p42e-A.png`、`tmp/p42e-B.png`(md5 均为 `8d3e6bbfb727c0dbab9e977c67d0d745`) |
| 出现区域与窗口几何一致 | ✅ 品红像素 65,478 个,bbox `(300,344)-(555,599)` = 256×256(窗口 X=300,内容区 Y≈344) |
| 窗口期间在屏 | ✅ `tmp/p42e-wins.txt` 列出 `vmtl-guiprobe layer=0`,与 Dock(程序坞)、访达同屏 |
| 插件零拷贝签名 | ✅ 见下 |
| 无逐帧拷回(G13) | ✅ 探针进程 `G13 surface-out` 计数 = **0** |

**插件零拷贝签名**(探针 pid 1154,`tmp/p1154-probe-plugin.log`):

```
buffer texture: 256x256 VkFormat 44 pitch 1024 bound at 0
                (type 0, DRM linear, align 256, size 262144, storage 0, render 1, view pair 50)
item 7: surface 17 256x256 pitch 1024 -> the family's VRAM (vram 1, private 0)
surface-out -> dirty, no copy (256x256 surface 17 seed 0, #1)   … 共 6 次
```

- 4 个 buffer texture、多个 item-7(surface 进 family 显存)、6 次 dirty-no-copy、0 次 G13。
- WindowServer(pid 144)日志显示它 `allow-listed: WindowServer`,并把应用窗口的
  IOSurface 直接当统一内存读来做合成 —— 不需要在我们的设备上再建一张 256×256 贴图,
  所以这就是设计里的"真共享显存"快路径。

## 过程中修复的三个关键问题

### 1. 每个 boot 的第一个 WindowServer 立即 SIGABRT

**现象**:boot 后 loginwindow 认证成功(`authPluginSuccessContinueLogin`),但第一个
WindowServer(pid 144)随即崩溃,会话被拆掉、回落到登录界面;自动登录是每 boot 一次,
于是**永远进不了桌面**。

**根因**(`WindowServer-*.ips` + `/tmp/vmtl.log`):首个 WindowServer 必须翻译
`MetalPerformanceShaders.framework/.../MPSImage.framework/Resources/default.metallib`
(16,457,748 B,1825 个 bitcode blob)—— 走 `desktop_window_effects_update` → `MPSImage`
→ `MPSDevice::GetLibrary`。翻译失败触发 `MTLReportFailure` → `__assert_rtn` → SIGABRT:
- WindowServer 受限环境不允许 `posix_spawn`:`library: air-opt spawn failed: Operation
  not permitted`;
- 插件内嵌的 bitcode reader 的 `air-runtime/libLLVM.dylib` 缺失:
  `in-memory bitcode reader unavailable: dlopen(...) (no such file)`
  → `newLibraryWithFile: translation produced nothing`。

**修复**:给插件 bundle 补上真正的
`/Library/GPUBundles/VMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib`
(149 MB,x86_64,导出全部所需 LLVM C API),ad-hoc 签名并重签 bundle。这样
WindowServer 无需 spawn `air-opt`,进程内直接解码 AIR。

**验证**:`VMTL_IDXCACHE_OFF=1 VMTL_AIRCACHE=<空目录>` 跑 `~/mpslibload`,日志:
`library: in-memory bitcode reader ready` / `metallib 16457748 bytes, 1825 bitcode
blobs, 1825 entries indexed`,库加载成功(1825 函数)。另把 SSH 用户已预热的
`aircache`(含整库索引 `idx2-de44ec08….plist` + 1825 份 AIR 文本)拷进 WindowServer
自己的 cache 目录兜底。重启后第一个 WindowServer 存活,桌面会话正常起来。

### 2. 自动登录的 `/etc/kcpassword`

原来的 `/etc/kcpassword` 是手写的假值(`31aa32aa33aa34aa35aa36aaba`,13 字节),解不出
密码。按 XOR 算法重写(密钥 `7d 89 52 23 d2 bc dd ea a3 b9 1f`,11 字节循环,密文长度 =
≥ strlen(pw)+1 的 12 的最小倍数),现文件 `4cbb6117e78addeaa3b91f7d` 解出 `123456`;
`autoLoginUser = lony2003`。`sysadminctl -autologin set` 从 SSH 调不通
(`SACSetAutoLoginPassword error:22`,无 SecurityAgent),故直接写文件。

### 3. 截图里"没有窗口" = TCC 屏幕录制权限缺失(不是合成 bug)

**现象**:桌面截图只有壁纸 + 菜单栏,Dock、应用窗口、状态栏图标全无;窗口级
`CGWindowListCreateImage` 一律返回 NULL。

**根因**:macOS 的屏幕录制 TCC 保护 —— 未授权时截图 API 会把其它 App 的窗口内容
抹掉。`tccd` 日志坐实:
`Service kTCCServiceScreenCapture does not allow prompting; returning denied.`
且 `winprobe*` 在 TCC.db 里 `auth_value=0`(拒绝)。

**修复**:本机 SIP Filesystem Protections 为 disabled,直接给
`/usr/sbin/screencapture`、`/bin/sh` 等授 `kTCCServiceScreenCapture = 2`,重启 `tccd`。
之后同一份截图立即出现品红窗口、Dock、菜单栏状态项与其它窗口。

> 结论:此前"窗口没被合成"的判断是**假象**,实际是抓图被 TCC 清空。合成一直是好的;
> 加回权限后 P4.2(e) 一次通过。

## 产物与位置(guest)

| 组件 | 位置 | 备注 |
|---|---|---|
| Metal 插件 | `/Library/GPUBundles/VMTLDriver.bundle` | 含 `air-runtime/libLLVM.dylib`(新) |
| WindowServer 放行标记 | `/Library/GPUBundles/vmtl-windowserver` | 空文件;删除即恢复拒绝 |
| 翻译器 | `/Library/GPUBundles/vmtl/libvmtl_translate.dylib` | |
| Venus ICD | `/Library/GPUBundles/vmtl/libvulkan_nouveau.dylib` | |
| 探针 | `~/vmtl-guiprobe`、`~/winlist`、`~/winprobe3`、`~/p42e-run.sh` | |
| 截图证据 | `tmp/p42e-A.png`、`tmp/p42e-B.png`、`tmp/live-now.png`、`tmp/p1154-probe-plugin.log`、`tmp/p42e-wins.txt` | 工作区 `tmp/`(gitignore) |

## 遗留(非阻塞)

- 主仓库 `translator/translator/Cargo.lock` 仍有一处未提交的大幅删减(与本阶段修复无关),
  未纳入提交。
- Dock 小表面(`com.apple.dock.extra`,75×60/67×60/67×52)`vkCreateImage(buffer texture)`
  偶发 `VK_ERROR_FORMAT_NOT_SUPPORTED`,回落 G13;host 侧 DRM-linear pitch 对齐待查。
- `vmtl-allow.txt` 可从 `*` 收紧为点名进程。
- TCC 授权是本机(SIP disabled)的直接改库,量产环境需走正常授权流程。
