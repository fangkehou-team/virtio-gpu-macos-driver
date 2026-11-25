# VirtIO-GPU / VMTL 归档状态（2026-11-25）

本提交保存当前研究和实现现场，**不是稳定版本或渲染修复版本**。项目目标已调整为直接评估、使用 `reims-vgpu` 获得可用的 macOS 虚机；当前自研渲染栈暂停继续开发。

## 已经打通并实测的部分

- macOS 15.8.1 guest 可以启动到桌面。
- WindowServer 已加载 VMTL Metal 设备并执行 Metal 合成工作。
- P4.2(e) 的 Metal 窗口合成验收通过，逐帧拷回计数 G13=0。
- P4.3 的真实 display-pipe transaction 已能把 IOSurface 内容复制到 virtio scanout，并产生宿主 `TRANSFER_TO_HOST_2D` / `RESOURCE_FLUSH`。
- OpenCore 冷启动时可通过 boot-args 和 AGDC 启动回退进入上述呈现路径。

历史验收与逆向过程见：

- `docs/P4.2e-MACOS15-REPORT.md`
- `docs/P4.3-DISPLAY-COMPARE.md`

这些文档保留了攻关过程中的历史判断；当前状态以各自标明的最新章节为准。

## 尚未解决的问题

- 快速移动鼠标时仍有品红/红色色块和轨迹残留。
- 窗口、菜单、Dock、Launchpad、通知和 vibrancy 等区域仍可能出现白色、黄色、青色或品红背景。
- 鼠标轨迹经过后，部分区域会暂时恢复正常。
- Launchpad、菜单和目录首次打开时仍可能异常并卡顿，多次操作后有所改善。
- UI 性能仍未达到可用目标。
- YCbCr matrix 微输出仍有按 command buffer 分组的错误；实际 descriptor 绑定、完整 Vulkan 工作负载和呈现同步尚未完成最终定位。
- kext 的提交完成语义仍需正确的异步设计，不能把提前完成视为已解决问题。

因此不能把“桌面能够显示”或单次截图正常当作渲染验收通过。

## 最后一项同步 fence 实验

曾给非零 Venus ring 的 `SUBMIT_3D` 增加宿主 fence，并等待真实 GPU 完成后才返回控制响应：

- 2508 个受测提交均收到匹配响应，未出现控制超时。
- 该改动没有稳定修复 matrix 微输出。
- 实机 UI 出现明显性能回归。

相关源码实验已经撤回，guest 恢复了实验前备份的 kext 二进制并重建 auxiliary kernel collection。不要重新把“每次提交同步等待宿主 GPU”当作最终修复；后续若恢复该方向，应设计真正的异步 fence/timeline 退休。

## 归档内容边界

本提交包含当前产品源码、构建辅助脚本和验收文档，也保留了源码中的若干临时诊断路径，未宣称完成生产化清理。

以下内容不属于本提交：

- `tmp/` 中的研究脚本、运行磁盘、截图、日志、第三方 `reims-vgpu` 和其他实验材料；
- 未跟踪的第三方 `nvk/mesa/`；
- 仓库根目录的临时 PNG/PPM 诊断图像。

当前归档环境没有 macOS SDK，因此提交前只执行了差异/空白检查，没有重新执行 kext 或 Metal 插件的完整构建。此前各阶段构建与实测结果不能替代对这个最终归档快照的一次干净构建验收。
