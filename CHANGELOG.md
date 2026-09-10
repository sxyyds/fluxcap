# Changelog

本文件记录 FluxCap 的每个对外版本范围。

FluxCap 0.1 是一次性成型的开源初始版本：`61dc3d0` 之前的开发历史没有回填到
git 中，也不会重写。从 0.1 之后，本项目按小步提交演进，每个逻辑变更独立成
commit 并记录在此文件中。版本号遵循 [Semantic Versioning](https://semver.org/)，
条目格式参照 [Keep a Changelog](https://keepachangelog.com/)。

## [0.1] - 2026-08-31

首次开源发布。范围如下。

### 新增

- **CPU 捕获路径**：持久化 GDI/DIB 帧池捕获完整虚拟桌面或任意桌面矩形，
  输出 CPU 可读 BGRX8；同步 `capture()` 与异步 `acquire_latest()`；
  2-16 帧槽 latest-wins；光标合成、layered windows、显示器枚举；
  稳定 C ABI（版本 1）与仅头文件 C++ RAII 包装。
- **CPU 增量信息**：tile 哈希 dirty-region、相邻矩形合并、精确
  `dirty_base_sequence`、QPC 时间戳与捕获统计。
- **WGC GPU 捕获**：`HWND` 窗口或 `HMONITOR` 显示器（含遮挡/离屏窗口）、
  BGRA8/P709 与 scRGB FP16 纹理、预分配纹理环、最新帧租约
  （`WgcFrameLease`）、尺寸变化重建；bus-only ROI 直写
  `SharedFrameBus`。
- **Desktop Duplication GPU 捕获**：monitor-only 系统级 dirty/move rect、
  独立 pointer shape/position/hotspot、`DuplicateOutput1` 与驱动能力门控的
  scRGB FP16；`DXGI_ERROR_ACCESS_LOST` worker 内指数退避重建，
  recoverable 管线完整 epoch 重建。
- **韧性后端**：`DesktopDuplicationController` 多显示器虚拟桌面聚合、
  `GdiMonitorCapture` CPU GDI 兜底、WTS/电源会话事件、静止桌面
  idle republish、`IDXGIOutput6` HDR 自动探测。
- **GPU 处理**：`GpuCrop` 固定纹理 ROI；D3D11.3 plane-RTV 确定性
  BGRA8/P709 → NV12/P010 转换；VideoProcessor 兼容/HDR/旋转 fallback；
  limited/full-range YUV。
- **视频编码**：Media Foundation 硬件 MFT（H.264/HEVC/AV1）、
  `IMFDXGIBuffer` 外部提交、`IMFTrackedSample` 严格输入池管理、
  原始 Annex-B/OBU 码流输出。
- **异步实时管线**：`AsyncGpuPipeline`、2-32 槽原子 latest-wins 环、
  WGC lease 零额外复制交接、独立 transform/encode 工作者、完整吞吐/
  覆盖统计。
- **图片编码**：D3D11 纹理同步回读 + WIC 编码 PNG/JPEG（文件或内存）。
- **跨进程共享**：`SharedFrameBus` v5——2-8 槽 latest-wins GPU 广播、
  最多 8 个同 adapter 消费者、D3D11 timeline fence、目标进程句柄复制；
  单纹理 `IDXGIKeyedMutex` 接口。
- **证据模型**：L1-L4 GPU copy-evidence 分级、可复现 runtime-L2 认证
  工具（`tools/qualification/`）、hash 绑定离线 L3 资产契约。
- **测试与基准**：CPU/GPU/协议测试套件与纯逻辑策略单测、copy-topology
  与吞吐 benchmark、DD resilience 认证工作负载；GitHub Actions CI
  （MSVC x64 静态+DLL 配置、headless 测试子集、安装冒烟）。

### 边界

- 不包含 MP4/MKV muxer 与音频捕获；不做动态 HDR 元数据与自动 tone map。
- 安全桌面/UAC/DRM 保护内容不绕过（计入 `protected_content_frames`）。
- 共享纹理仅限同 adapter（LUID 校验）；C++ GPU API 不是稳定 ABI。

## [0.1.1] - 2026-09-10

推广前的信任基建与可发现性（不改变库行为）：

- 删除三个 0 字节失败残留 qualification JSON
  （`amd-nv12-av1-smoke` / `p010-hevc-amd-smoke` / `p010-hevc-smoke`）。
- README 双语新增项目状态、30 秒速览（绝对性能 TL;DR 与 L1-L4
  大众化解释）、Hello World（截图存 PNG）与实时预览演示 GIF；
  补搜索关键词与 `docs/comparison.md` 对比矩阵。
- 新增 `docs/benchmarks-20260909-rx5060.md`（RTX 5060 Laptop 绝对
  基准：GDI 全桌面 46.7 FPS、WGC→H.264 submit→packet P50 1.6 ms、
  4K 合成饱和与三 codec bus 数字）；GPU pipeline benchmark 支持任意
  测试窗口几何。
- 新增 tag 触发的 release workflow（x64 静态+共享打包 zip 附到
  GitHub Release）与 vcpkg port 草稿（`packaging/vcpkg/`）。
