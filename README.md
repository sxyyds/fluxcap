# FluxCap 0.1

[![CI](https://github.com/sxyyds/fluxcap/actions/workflows/ci.yml/badge.svg)](https://github.com/sxyyds/fluxcap/actions/workflows/ci.yml)

**中文** | [English](README.en.md)

![FluxCap GPU 预览示例](docs/assets/demo.gif)

实时预览：`fluxcap_gpu_preview --window HWND` 直接消费 WGC GPU 纹理并显示源 FPS。动图为屏幕录制缩样，非库输出质量标尺。

FluxCap 是面向 Windows 的 C++20 低延迟捕获库。它同时提供两条互补路径：

* CPU 路径：使用持久化 GDI/DIB 帧池捕获虚拟桌面或桌面 ROI，输出 CPU 可读的 BGRX8 像素，并提供稳定 C ABI 与仅头文件的 C++ RAII 包装。

* GPU 路径：使用 Windows.Graphics.Capture（WGC）捕获窗口或显示器，或使用 Desktop Duplication 捕获显示器，直接处理 `ID3D11Texture2D`；同一 GPU 设备上可继续完成中心 ROI 裁切、缩放、BGRA8/scRGB FP16 到 NV12/P010 转换、硬件视频编码和跨进程纹理共享。

GPU 路径已覆盖遮挡和离屏窗口捕获。仓库中的真实 GPU 测试会创建一个红色目标窗口，用蓝色顶层窗口完全遮挡并验证捕获纹理仍是红色；随后把目标完全移出虚拟桌面，在离屏状态重新建立 WGC session，并再次验证其现有 GPU surface。

## Hello World：截一张图存成 PNG

```cpp
#include <fluxcap/gpu.hpp>
#include <winrt/base.h>
#include <windows.h>

namespace gpu = fluxcap::gpu;

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    gpu::WgcCapture capture;
    auto created = gpu::WgcCapture::create_for_monitor(
        MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY),
        nullptr, gpu::WgcCaptureOptions{}, capture);
    if (!created || !(created = capture.start())) return 1;

    gpu::WgcFrameLease frame;
    if (!capture.acquire_latest(1000, frame)) return 1;

    gpu::WicImageEncoder png;
    if (!png.initialize(capture.device())) return 1;
    return png.encode_texture_to_file_sync(frame.texture(), L"screenshot.png")
        ? 0 : 1;
}
```

链接 `fluxcap_gpu` 及其系统依赖（d3d11/dxgi/dwmapi/gdi32/mf/mfplat/mfuuid/ole32/user32/windowsapp/windowscodecs/wtsapi32，CMake 目标已自动处理）。CPU 路径的等价入口见 `examples/grab.cpp` 与 [C ABI](#cpu-api)。

## 项目状态

FluxCap 0.1 是一次性成型的开源初始版本：0.1 之前的开发历史没有回填到 git，也不会重写。从 0.1 开始，本项目按小步提交演进，每个逻辑变更独立成 commit，版本范围记录在 [CHANGELOG.md](CHANGELOG.md)。

## 30 秒速览

实测机器：RTX 5060 Laptop / Ryzen 9 8945HX / Windows 11（build 26200）/ 2560x1600 单屏。数字仅对该组合有效，复现命令与完整分布见 [docs/benchmarks-20260909-rx5060.md](docs/benchmarks-20260909-rx5060.md)。

| 路径 | 结果 |
|---|---|
| CPU GDI 捕获完整虚拟桌面 2560x1600 | 46.7 FPS（P50 21.0 ms），平均 0.41 核 |
| WGC 窗口捕获 → NV12 → H.264 硬编（1080p 级 surface） | 捕获提交到编码 packet P50 **1.6 ms**；240 fps 时基零丢帧 |
| 同上，H.264/HEVC/AV1 三种硬编 | 全部 `encoder copied = 0`，600/600 packet 覆盖 |
| 4K（3840x2160）合成转换/硬编饱和 | BGRA→NV12 5 896 ops/s；单线程顺序硬编 194 packets/s |
| CPU 占用 | 以上所有 GPU 路径平均 1.1 - 1.5 核（32 核机器） |

**"0 拷贝直达编码器"是什么意思？** 在被测组合上，捕获纹理一路走进硬件编码器，FluxCap 自己的统计显示：捕获入口拷贝 0 次、bus 发布拷贝 0 次、编码器输入拷贝 0 次、纹理身份核验 121/121 全部通过。这不是一句营销口号，而是带等级的证据体系：

| 等级 | 大众版解释 |
|---|---|
| L1 | "我们交出去了"：纹理作为外部资源提交给编码器，统计里没有显式拷贝调用 |
| L2 | "每帧都有身份证"：提交瞬间核对纹理指针身份与编码器绑定标志，确认交给编码器的就是捕获那张纹理 |
| L3 | "行车记录仪"：系统级 ETW 跟踪审查窗口内没有全帧 GPU 拷贝事件，离线 hash 绑定 |
| L4 | "厂家盖章"：厂商工具/认证提供的额外可见性 |

诚实的边界：编码器 MFT 与驱动内部的行为对运行时 API 不可观测，任何等级都不证明固件内部没有私有操作；证据只对被测的 adapter/驱动/OS/几何组合有效，换组合必须重新测量。完整定义与工具链见 [docs/copy-evidence-qualification.md](docs/copy-evidence-qualification.md)。

## 当前能力

| 模块                  | 已实现                                                                                                                                                                      |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| CPU 桌面捕获            | 完整虚拟桌面或任意桌面矩形、同步/异步 latest-frame、2 到 16 个持久 DIB 帧槽、光标合成、layered windows、显示器枚举                                                                                            |
| CPU 增量信息            | tile 哈希 dirty-region、相邻矩形合并、精确的 `dirty_base_sequence`、QPC 时间戳和捕获统计                                                                                                       |
| WGC 捕获              | `HWND` 窗口或 `HMONITOR` 显示器、遮挡窗口、BGRA8/P709 或 scRGB FP16 GPU 纹理、预分配纹理环、最新帧租约、尺寸变化重建；固定 ROI 也可在回调中直接发布到 `SharedFrameBus`，不分配私有 WGC 输出环                                      |
| Desktop Duplication | monitor-only 系统级 dirty/move rect、独立 pointer shape/position/hotspot、BGRA8，以及由 `DuplicateOutput1` 与驱动能力门控的 scRGB FP16；`DXGI_ERROR_ACCESS_LOST` 可由 recoverable 管线重建完整 epoch |
| GPU 处理              | `GpuCrop` 固定纹理 ROI；D3D11.3 plane-RTV 确定性 BGRA8/P709 到 NV12/P010；VideoProcessor 兼容/HDR fallback；limited/full-range YUV                                                    |
| 视频编码                | Media Foundation 硬件 MFT 探测与编码，支持 H.264、HEVC、AV1；`IMFTrackedSample` 严格管理 free/in-flight 输入 surface，避免高速下提前复用                                                              |
| 异步实时管线              | `AsyncGpuPipeline`、2 到 32 槽原子 latest-wins 环、WGC lease 零额外复制交接、多生产者非阻塞提交、独立 transform/encode 工作者和完整吞吐/覆盖统计                                                                |
| 图片压缩                | 从 D3D11 纹理同步回读并使用 WIC 编码到 PNG/JPEG 文件或内存                                                                                                                                 |
| 跨进程共享               | `SharedFrameBus` 2 到 8 槽 latest-wins GPU 广播、最多 8 个同 adapter 消费者、D3D11 timeline fence、目标进程句柄复制；另保留单纹理 `IDXGIKeyedMutex` 接口                                                |

视频编码器输出原始码流包：H.264/HEVC 通常是 Annex-B，AV1 是 OBU 数据。当前不包含 MP4/MKV muxer，也不包含音频捕获。

## 架构

```text
窗口 / 显示器
      |
      v
Windows.Graphics.Capture
      |
      v
free-threaded frame pool -> 最新源 surface
             |                    |
             |                    +-- ROI CopySubresourceRegion --> SharedFrameBus 槽
             |                                                   （bus-only 直写）
             v
预分配 D3D11 BGRA 私有纹理环
             |
             +----------------------+----------------------+----------------------+
             |                      |                      |                      |
             v                      v                      v                      v
 plane-RTV shader / VP       SharedFrameBus publish   keyed mutex 共享       WIC 图片编码
 缩放 / BGRA / NV12 / P010   （已有普通纹理）         （单纹理）             同步回读 + PNG/JPEG
             |
             v
 D3D11-aware hardware MFT
 H.264 / HEVC / AV1 packets
```

高吞吐模式在 WGC 纹理环和编码器之间增加一条有界实时通道：

```text
WgcFrameLease --move--> free/writing/ready/reading 原子槽环
                              |
                    只选择最新 ready 帧
                    覆盖过期帧而不阻塞生产者
                              |
                              v
             plane-RTV shader / VP -> hardware MFT
                              |
                   IMFTrackedSample 释放回调
                              |
                       in-flight -> free
```

`submit_frame()` 移动 WGC lease，因此不会为交接再复制一张 BGRA 纹理。`submit_texture()` 面向普通 D3D11 纹理，会复制到预分配输入环。工作线程一次只处理最新 ready 帧；已经 reading 的帧不会被覆盖。硬编输入槽只有在 MFT 最终释放 tracked sample 后才重新进入 free 状态，不能用 `METransformNeedInput` 或简单的 modulo 下标推测资源已经可复用。

WGC 回调会排空源 frame pool，只保留其中最新的一帧。普通模式把内容复制到固定数量的私有 GPU 帧槽，消费者通过不可复制、可移动的 `WgcFrameLease` 持有纹理；未被租出的旧 ready 帧可以被覆盖，已租出的 reading 帧不会被改写。bus-only 模式不创建这些私有帧槽，而是在同一个回调中取得可写 bus 槽、复制源 ROI 并提交 ready fence。

CPU 后端使用相同的 latest-frame 思路，但目标是立即可由 CPU 读取的像素：

```text
desktop DC -> BitBlt -> 持久 top-down DIB 帧池 -> 可选光标/dirty tracker
                                                     |
                                                     v
                                  同步 frame lease / 异步 acquire_latest
```

## DXGI 边界

FluxCap 同时提供两类 GPU 捕获后端：WGC 用于窗口和显示器；`DesktopDuplicationCapture` 仅用于显示器，并通过 `IDXGIOutputDuplication` 提交系统级 dirty/move rect 与独立 pointer metadata。`RecoverableGpuPipelineConfig::monitor_backend` 可选择 `automatic`、`windows_graphics_capture` 或 `desktop_duplication`。自动模式只在能够满足 cursor 契约时尝试 Desktop Duplication，能力或会话不支持时回退 WGC；显式模式 fail-closed。

Desktop Duplication bus 必须位于目标显示器的 adapter。recoverable 管线会在每个 epoch 重新解析 monitor adapter，并创建全新的 D3D11 device、duplication/WGC capture、bus、consumer、transform 和 encoder。`DXGI_ERROR_ACCESS_LOST` 触发完整 epoch 重建，不会被误判为 planar 能力失败。Desktop Duplication 会把 90/180/270 度输出的 surface、ROI、dirty/move rect 映射到显示器逻辑方向，并在驱动支持时用一次 VideoProcessor 提交把旋转与 crop/scale/color conversion 融合到最终 bus 槽；不支持对应 rotation/format/color-space 组合时 fail-closed，`automatic` 后端可回退 WGC。旋转几何已有确定性契约检查，但当前测试机没有实际旋转显示器，因此尚未完成真实 rotated-monitor 端到端验证。

独立 cursor 有两种形态：`include_cursor=true` 时由 FluxCap 在 GPU 上把光标合成进 bus 帧（多一次全表面 copy，DD 原生脏区语义保留）；`include_cursor_metadata=true` 时发布 position、visibility、shape kind、尺寸和 hotspot 而不烧录像素。`screen_x/y` 与 `frame_x/y` 统一表示热点而非 shape 左上角；DD native 的 top-left 会在发布前加回原始 hotspot，hidden、溢出或缺少可用 shape/hotspot 时不标记 position valid。驱动提供 DXGI pointer shape 时直接使用；驱动把 pointer 合成进 desktop image、没有 shape buffer 时，用 Win32 cursor shape/hotspot 兜底，并仅把位置标记为 `wgc_cursor_position_estimated`。shape 在对应纹理 commit 成功后才发布，失败帧不会淘汰旧 shape。masked-color、普通 BGRA 和 monochrome AND/XOR 三种形状都保留各自语义；GPU 合成同样按 GDI 语义处理 monochrome XOR/AND 与 masked-color。

### Desktop Duplication 生命周期韧性与后端扩展

`WgcCaptureOptions` 中的 Desktop Duplication/GDI 专用字段（WGC 忽略）控制 worker 内的自动恢复与协商行为，默认值保持旧的 fail-closed 语义：

* `frame_timeout_ms`（默认 50）：`AcquireNextFrame` 轮询超时。

* `access_lost_retry_limit`（默认 0 = 无限）：`DXGI_ERROR_ACCESS_LOST` 后在 worker 内按指数退避（`access_lost_retry_initial_ms` → `access_lost_retry_max_ms`）重建 duplication 会话、transform 与光标中间纹理，恢复后强制全帧；显示器被拔出时置位 `target_closed()`。上层 epoch 重建仍是最终防线。

* `session_retry_limit` / `session_retry_interval_ms`（默认 3 / 100ms）：`DuplicateOutput1` 遇到 `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`（会话数上限、独占全屏切换）时的延时重试。

* `idle_republish_interval_ms`（默认 0 = 关闭）：静止桌面时按间隔重发最后帧（sidecar 为 valid 空 damage，并刷新 Win32 光标位置），维持编码器帧率；启用时每次真实发布多一次 bus 槽到私有纹理的 copy。

* `auto_detect_color`：用 `IDXGIOutput6::GetDesc1` 探测桌面 PQ/BT.2020 HDR，自动选择 scRGB FP16 或 SDR BGRA8；重建时若色彩空间变化则停止会话并要求上层以新格式重建 epoch。`desktop_color_state()` 暴露探测结果。

* `allow_format_fallback`：驱动拒绝 FP16 duplication 时接受 `DuplicateOutput1` 多格式协商中的 BGRA8 降级（计入 `format_fallbacks`）。

* `monitor_session_events`：消息窗口线程监听 WTS 锁屏/解锁/RDP 连接切换与电源挂起/恢复，事件触发全帧刷新与光标缓存重置（计入 `session_events`）。

* `ProtectedContentMaskedOut` 帧计入 `protected_content_frames`。

两个新后端补齐 OBS 级能力面：

* `DesktopDuplicationController`：把 bus adapter 上所有 attached 输出聚合为一张虚拟桌面纹理发布（等价 OBS `DxgiDuplicatorController`）。damage 按显示器原点偏移映射；旋转输出与拓扑变化（显示器增删、虚拟边界变化）不支持——会话以可恢复错误停止，由上层重建 epoch；mailbox 必须为 `full_frame`。

- `GdiMonitorCapture`：CPU GDI 兜底（`BitBlt` + `CAPTUREBLT`），要求 BGRA8 SDR、mailbox 尺寸与 bus 一致（不做缩放）；每帧全帧 damage（无原生 metadata），`include_cursor` 用 `DrawIcon` 烧录，`gdi_poll_interval_ms` 控制节奏。适合 DD/WGC 都不可用的环境。

可运行示例见 `examples/resilient_monitor_capture.cpp`：DD 韧性采集（心跳 + 会话事件 + 无限退避重建）→ 失败时 GDI 兜底，进程内消费 SharedFrameBus 并每秒打印韧性统计。另外 WGC 的 best-effort 会话属性（border/secondary windows/min update interval/cursor/dirty-region）设置失败时不再静默：`WgcCaptureStats::property_failure_mask` 按 `wgc_property_failure_*` 位暴露，`property_failures` 为累计次数。

monitor 后端通过公开 recoverable API 选择；`automatic` 会记录最终激活的后端，显式 `desktop_duplication` 则在能力不足时直接返回错误：

```cpp
gpu::RecoverableGpuPipelineConfig config;
config.monitor_backend = gpu::RecoverableMonitorCaptureBackend::automatic;
config.capture.include_cursor = false;
config.capture.include_cursor_metadata = true;
config.capture.damage_mode = gpu::WgcDamageMode::native_report_only;
config.mailbox.mode = gpu::WgcMailboxMode::centered_region;
config.mailbox.width = config.mailbox.height = 320;
config.pipeline.transform.input_width = 320;
config.pipeline.transform.input_height = 320;
config.pipeline.transform.output_width = 320;
config.pipeline.transform.output_height = 320;
config.pipeline.transform.output_format = gpu::GpuPixelFormat::nv12;
config.pipeline.encoder.width = 320;
config.pipeline.encoder.height = 320;
config.pipeline.encoder.input_format = DXGI_FORMAT_NV12;

gpu::RecoverableGpuPipeline pipeline;
auto created = gpu::RecoverableGpuPipeline::create_for_monitor(
    monitor, config, packet_callback, callback_context, pipeline);
if (!created || !pipeline.start()) return false;
const auto active_backend = pipeline.snapshot().active_monitor_backend;
```

### 零冗余复制与色彩能力矩阵

这里的“FluxCap 可观测范围内零冗余复制”严格指：`0` 次 CPU pixel copy、`0` 次冗余 `CopyResource`、最多一次源格式到最终 encoder 格式所必需的 GPU copy/transform，以及一次 external encoder submission。该契约只覆盖 FluxCap 显式提交并由统计计数器观测到的操作，不担保 Windows 捕获栈、显示驱动或硬件 MFT 内部没有复制。系统捕获 surface 本身不等于可无限持有的 bus 槽；capture-native bus 因此仍需要一次必要的 GPU copy，不能称为“零 GPU 操作”。

| 捕获输入与 bus                            | capture ingress        | Async 段                       | encoder 输入                            | 色彩契约                                         |
| ------------------------------------ | ---------------------- | ----------------------------- | ------------------------------------- | -------------------------------------------- |
| WGC/DD BGRA8 -> BGRA8 bus            | 1 次必要 ROI GPU copy     | 1 次 BGRA->NV12/P010 transform | tracked encoder surface               | RGB P709 -> 显式 YUV contract                  |
| WGC/DD BGRA8 -> NV12 planar bus      | 1 次必要 transform，0 copy | 0 copy，0 transform            | external NV12                         | P709 studio/full range 显式匹配                  |
| WGC/DD scRGB FP16 -> P010 planar bus | 1 次必要 transform，0 copy | 0 copy，0 transform            | capability-gated external P010 Main10 | scRGB/P709 -> PQ/P2020                       |
| 已是 NV12/P010 的 bus                   | 0 copy                 | 0 copy，0 transform            | external planar texture               | bus metadata、transform、MFT media type 必须完全一致 |

`GpuEncoder` 会把 primaries、transfer、matrix、nominal range、chroma siting 与 Main/Main10 profile 写入 MF input/output media type 和可用的 codec attributes，并用 `mft_identity()` 暴露实际选中 activation 的 friendly name/CLSID。显式 `video_processor` 在创建阶段通过 `CheckVideoProcessorFormatConversion` 探测；`deterministic_planar` 在创建阶段验证 D3D11.3 plane RTV，并在提交时严格要求 source SRV；`automatic` 只在 epoch 首帧选择一次 shader 或 VP，不会运行中反复换核。完全 typed-SRGB source 无法在所有驱动上直接作为 UNORM VP input，因此 automatic/VP 兼容分支会先复制到一个复用的 UNORM 输入；`compatibility_copy_submissions()` 会如实计数，不能把该分支列为零 copy。`input_copy_submissions`、`transform_submissions`、`encoder_external_submissions` 和 `encoder_copied_submissions` 用于验证上述矩阵，而不是只依赖架构推断。

recoverable planar 路径还要求最终 NV12/P010 bus 槽同时带 `D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER`：前者允许 Y/UV plane RTV 或 VP 直接写最终槽，后者消除“输入纹理根本不能创建 encoder input view”这一类必然 staging 原因。`require_video_encoder_input_bind=true` 可对普通 `GpuEncoder` 输入启用同样的 fail-closed 门禁；`video_encoder_input_bind_supported` 反映当前 device/format/dimensions 能否创建这种纹理。该 bind flag 只能消除一个可观测的内部复制诱因，仍不能证明 MFT/驱动/硬件实现中没有其他私有表面或复制。

L1-L4 是逐级、逐硬件 tuple 的证据模型；benchmark 的严格模式可输出绑定 PID、EXE hash、adapter LUID、精确 MFT、identity/bind/copy 计数的 runtime L2 JSON。当前机器已保留 NVIDIA 与 AMD 两条 WGC/NV12/H.264 640→320 runtime-L2 工件，见 [`qualification-results/hardware-matrix-20260723.json`](qualification-results/hardware-matrix-20260723.json)。L3 还要求同一次运行的哈希绑定 ETL 和人工 GPU Hardware Queue 审阅；仓库当前没有 L3/L4 通过工件，这两条 L2 也不能外推到其他 tuple，详见 [`docs/copy-evidence-qualification.md`](docs/copy-evidence-qualification.md)。

P010 + PQ/P2020 的 HEVC/AV1 可以通过公开配置提交 HDR10 静态 mastering-display、MaxCLL 和 MaxFALL，并要求最终码流提供证据：

```cpp
gpu::GpuEncoderConfig hdr;
hdr.codec = gpu::VideoCodec::hevc;
hdr.width = width;
hdr.height = height;
hdr.input_format = DXGI_FORMAT_P010;
hdr.input_color_space = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
hdr.hdr10.enabled = true;
// 结构体默认值为 BT.2020/D65、1000 nit、MaxCLL 1000、MaxFALL 400；
// 它们不会从捕获像素、EDID、ICC 或显示器 profile 自动推导，应按内容覆盖。
hdr.require_bitstream_color_metadata = true;
hdr.require_bitstream_hdr10_metadata = true;

gpu::GpuEncoderSupport support;
auto probed = gpu::GpuEncoder::probe(device, hdr, support);
if (!probed || !support.supported
    || !support.hdr10_static_metadata_media_type) return false;
```

`hdr10_static_metadata_media_type` 只证明 MFT 接受带这些属性的 media type；它不证明编码器把属性写进了码流。开启任一 `require_bitstream_*` 后，每个输出 packet 会在进入用户 callback 前被解析并累计 metadata；格式损坏或不受支持时该 packet 不会交给 callback，当前编码操作返回 `not_supported`。`GpuEncoder::drain()` 会处理剩余输出并最终检查 presence/value，VUI/color description 或 HDR10 静态 SEI/OBU 缺失、值不匹配同样返回 `not_supported`。此前已经交付 callback 的 packet 不会回滚；需要严格 HDR10 文件或网络流时，应暂存 packet，只有 `drain()` 成功后才提交。`flush()`/`close()` 不代替这次终验。自行管理 packet 的调用方也可以逐包调用 `inspect_encoded_packet_metadata()`，最后用 `validate_encoded_video_metadata()` 校验累积结果；inspector 接受 H.264/HEVC Annex-B 与 AV1 low-overhead OBU。

## 构建

要求：

* Windows 10/11，且当前交互式会话支持 Windows.Graphics.Capture。

* CMake 3.24 或更高版本。

* 支持 C++20 的编译器。当前工程使用 MSVC 和较新的 Windows SDK；AV1 Media Foundation 声明需要足够新的 SDK。

* GPU 转换和编码能力由显卡、驱动、像素格式、分辨率及已安装的 Media Foundation 硬件 MFT 决定，运行前应调用 `probe`。

```powershell
cmake -S . -B build -A x64 `
  -DFLUXCAP_BUILD_GPU=ON `
  -DFLUXCAP_BUILD_TESTS=ON `
  -DFLUXCAP_BUILD_EXAMPLES=ON `
  -DFLUXCAP_BUILD_BENCHMARKS=ON
cmake --build build --config Release --parallel
```

默认构建静态库。添加 `-DBUILD_SHARED_LIBS=ON` 可构建 DLL。主要选项如下：

| CMake 选项                   |   默认值 | 作用                                           |
| -------------------------- | ----: | -------------------------------------------- |
| `FLUXCAP_BUILD_GPU`        |  `ON` | 构建 WGC/D3D11 GPU 库和 GPU 测试                   |
| `FLUXCAP_BUILD_TESTS`      |  `ON` | 构建 C、CPU 和 GPU 测试                            |
| `FLUXCAP_BUILD_EXAMPLES`   |  `ON` | 构建 CPU BMP 示例和 WGC GPU 管线示例                  |
| `FLUXCAP_BUILD_BENCHMARKS` |  `ON` | 构建 CPU、真实 WGC 管线及 synthetic GPU 饱和 benchmark |
| `BUILD_SHARED_LIBS`        | `OFF` | 在静态库与 DLL 之间切换                               |

安装头文件、库和 CMake package：

```powershell
cmake --install build --config Release --prefix install
```

下游 CMake 工程：

```cmake
find_package(FluxCap CONFIG REQUIRED)

# 稳定 C ABI / CPU 捕获
target_link_libraries(your_cpu_target PRIVATE FluxCap::fluxcap)

# C++ GPU API；只有 FLUXCAP_BUILD_GPU=ON 时才会安装
target_link_libraries(your_gpu_target PRIVATE FluxCap::gpu)
```

## GPU 快速开始

下面的核心流程从指定窗口取得 BGRA GPU 纹理，缩放并转换为 NV12，然后送入 D3D11-aware H.264 硬件 MFT。调用线程必须先初始化 COM/WinRT apartment。传入空 D3D11 device 时，`WgcCapture` 会创建带 BGRA/video support 的硬件设备；也可以传入应用自己的 `ID3D11Device`，让后续模块共享同一设备。

```cpp
#include <fluxcap/gpu.hpp>
#include <winrt/base.h>

#include <cstdint>
#include <fstream>

namespace gpu = fluxcap::gpu;

void write_packet(void* context, const gpu::EncodedPacket& packet) {
    auto& output = *static_cast<std::ofstream*>(context);
    output.write(
        reinterpret_cast<const char*>(packet.data),
        static_cast<std::streamsize>(packet.size));
    // packet.data 只在本次回调期间有效；需要保留时必须在这里复制。
}

bool capture_and_encode(HWND window) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    gpu::WgcCapture capture;
    gpu::WgcCaptureOptions capture_options;
    capture_options.buffer_count = 3;
    capture_options.include_cursor = false;

    auto result = gpu::WgcCapture::create_for_window(
        window, nullptr, capture_options, capture);
    if (!result || !(result = capture.start())) {
        return false;
    }

    gpu::WgcFrameLease frame;
    result = capture.acquire_latest(1000, frame);
    if (!result) {
        return false;
    }

    gpu::GpuTransformConfig transform_config;
    transform_config.input_width = frame.info().width;
    transform_config.input_height = frame.info().height;
    transform_config.output_width = frame.info().width & ~1u;
    transform_config.output_height = frame.info().height & ~1u;
    transform_config.output_format = gpu::GpuPixelFormat::nv12;
    transform_config.frame_rate_numerator = 60;
    transform_config.frame_rate_denominator = 1;

    gpu::GpuTransform transform;
    if (!gpu::GpuTransform::create(
            capture.device(), transform_config, transform)
        || !transform.process(frame.texture())) {
        return false;
    }

    gpu::GpuEncoderConfig encoder_config;
    encoder_config.codec = gpu::VideoCodec::h264;
    encoder_config.width = transform_config.output_width;
    encoder_config.height = transform_config.output_height;
    encoder_config.frame_rate_numerator = 60;
    encoder_config.frame_rate_denominator = 1;
    encoder_config.bitrate = 8'000'000;
    encoder_config.input_format = DXGI_FORMAT_NV12;
    encoder_config.low_latency = true;

    gpu::GpuEncoderSupport support;
    if (!gpu::GpuEncoder::probe(capture.device(), encoder_config, support)
        || !support.supported
        || !support.d3d11_aware) {
        return false;
    }

    std::ofstream bitstream("capture.h264", std::ios::binary);
    gpu::GpuEncoder encoder;
    if (!encoder.initialize(
            capture.device(), encoder_config, write_packet, &bitstream)) {
        return false;
    }

    constexpr std::int64_t duration_100ns = 10'000'000 / 60;
    if (!encoder.encode_texture(
            transform.output_texture(), 0, duration_100ns, true)
        || !encoder.drain()) {
        return false;
    }

    frame.reset();
    capture.stop();
    return true;
}
```

实际视频循环中，每次 `acquire_latest` 后调用 `transform.process` 和 `encode_texture`，并按帧递增 100 ns 单位的时间戳。窗口尺寸改变时，WGC 会重建内部 frame pool；调用方发现 `WgcFrameInfo` 尺寸变化后，也应按新尺寸重建 `GpuTransform` 和 `GpuEncoder`。

要生成 P010，把 `GpuTransformConfig::output_format` 改为 `GpuPixelFormat::p010`，并把编码器输入格式改为 `DXGI_FORMAT_P010`。NV12/P010 输出宽高必须为偶数。本节示例使用默认的 8 位 BGRA WGC 源，转换到 P010 不会凭空增加源画面的色彩精度；需要保留 HDR 精度时，应把 `WgcCaptureOptions::pixel_format` 设为 `WgcPixelFormat::rgba16_float`，并按能力探测结果使用 scRGB FP16 -> P010/PQ 路径。

需要 AiMod 风格的屏幕中心固定尺寸纹理时，使用 `GpuCrop`。下面会输出精确的 320×320 BGRA GPU 纹理；把 `size` 改为 640 即可得到 640×640。输出纹理只初始化一次，稳态每帧只提交一次 `CopySubresourceRegion`，不会把整屏拉伸成正方形：

```cpp
constexpr std::uint32_t size = 320;
if (frame.info().width < size || frame.info().height < size) {
    return false;
}

gpu::GpuCropConfig crop_config;
crop_config.input_width = frame.info().width;
crop_config.input_height = frame.info().height;
crop_config.x = (frame.info().width - size) / 2;
crop_config.y = (frame.info().height - size) / 2;
crop_config.width = size;
crop_config.height = size;

gpu::GpuCrop crop;
if (!gpu::GpuCrop::create(capture.device(), crop_config, crop)
    || !crop.process(frame.texture())) {
    return false;
}
ID3D11Texture2D* square_texture = crop.output_texture();
```

## 融合 ROI Mailbox

如果消费者从一开始就只需要固定区域，优先让 WGC mailbox 直接输出 ROI，而不是先把整帧复制到 FluxCap 纹理环、再调用 `GpuCrop`。这把 FluxCap 内部的“整帧 ingress copy + ROI post-copy”融合为一次 `CopySubresourceRegion`：

```cpp
gpu::WgcMailboxConfig mailbox;
mailbox.mode = gpu::WgcMailboxMode::centered_region;
mailbox.width = 320;
mailbox.height = 320;

gpu::WgcCapture roi_capture;
auto created = gpu::WgcCapture::create_for_window(
    window, nullptr, capture_options, mailbox, roi_capture);
if (!created || !roi_capture.start()) {
    return false;
}

gpu::WgcFrameLease roi_frame;
auto acquired = roi_capture.acquire_latest(1000, roi_frame);
if (!acquired) {
    return false;
}

// texture/info() 已经是 320 x 320；这里可直接送入转换、编码或共享。
ID3D11Texture2D* roi_texture = roi_frame.texture();
gpu::WgcMailboxFrameInfo region = roi_frame.mailbox_info();
```

`absolute_region` 使用显式 `x/y/width/height`，`centered_region` 在每个源尺寸上重新计算中心原点。`set_mailbox_config()` 可以运行中切换；每次成功切换都会增加 generation 并丢弃旧 generation 中尚未租出的帧，已经交给调用方的旧 lease 仍然有效。固定 ROI 暂时大于源尺寸时，`acquire_latest()` 返回 `WgcStatus::region_unavailable`，源重新变大后自动恢复，不会偷偷改变输出尺寸。切换 320/640 会改变纹理契约，固定尺寸的转换器、编码器和共享纹理应同步重建。

该模式不会减少 DWM/WGC 生成完整 capture surface 的成本，只减少 FluxCap 自己的纹理环与后处理带宽。以 2560×1600 BGRA、240 个新帧/s 为例，整帧 mailbox copy 的理论载荷约为 3.93 GB/s；直接 320×320 ROI 约为 98 MB/s，640×640 约为 393 MB/s，尚未计入旧路径额外的 post-copy。

## 异步 Latest-Wins 管线

捕获线程不应同步等待硬件编码器。`AsyncGpuPipeline` 接受移动的 WGC lease，在独立 MTA 工作者上执行转换与硬编。队列满时覆盖最旧 ready 帧，保持画面新鲜度：

```cpp
gpu::AsyncGpuPipelineConfig pipeline_config;
pipeline_config.transform = transform_config;
pipeline_config.encoder = encoder_config;
pipeline_config.queue_depth = 8;

gpu::AsyncGpuPipeline pipeline;
if (!gpu::AsyncGpuPipeline::create(
        capture.device(), pipeline_config, write_packet, &bitstream, pipeline)) {
    return false;
}

constexpr std::int64_t duration = 10'000'000 / 60;
for (std::int64_t index = 0; index < 1000; ++index) {
    gpu::WgcFrameLease latest;
    auto acquired = capture.acquire_latest(1000, latest);
    if (!acquired) {
        continue;
    }
    auto submitted = pipeline.submit_frame(
        std::move(latest), index * duration, duration, index == 0);
    if (!submitted) {
        break;
    }
}

auto drained = pipeline.drain(30'000); // terminal; no more submissions
auto stats = pipeline.stats();
```

`submit_frame()` 是 lease 所有权转移，成功后原 lease 为空。`submit_texture()` 会把普通 BGRA 纹理复制进预分配环。packet callback 在管线工作线程执行，必须快速返回，不能从回调中重入、`drain`、`close` 或销毁管线；需要保留 packet 时必须在回调返回前复制字节。

`AsyncGpuPipelineStats` 分开报告 submission attempts、accepted、overwritten、rejected、processed、failed、packets、bytes 和最大 ready 深度。`accepted` 表示成功进入有界调度器，不等于最终被编码；正常 terminal drain 后满足 `accepted = processed + overwritten + failed`。拥塞时 `overwritten` 会增加，这是 latest-wins 延迟策略的预期行为。

## PNG/JPEG

`WicImageEncoder` 接受与初始化 device 相同设备上的 BGRA/BGRX/RGBA 8 位纹理。编码是同步操作，内部包含 GPU 到 CPU 的 staging readback，随后由 WIC 压缩：

```cpp
gpu::WicImageEncoder images;
if (!images.initialize(capture.device())) {
    return false;
}

gpu::ImageEncodeOptions png_options;
png_options.format = gpu::ImageFormat::png;
png_options.preserve_png_alpha = true;
auto png_result = images.encode_texture_to_file_sync(
    frame.texture(), L"capture.png", png_options);

gpu::ImageEncodeOptions jpeg_options;
jpeg_options.format = gpu::ImageFormat::jpeg;
jpeg_options.jpeg_quality = 0.90F;
std::vector<std::uint8_t> jpeg;
auto jpeg_result = images.encode_texture_to_memory_sync(
    frame.texture(), jpeg, jpeg_options);
```

对持续高帧率视频不要在捕获回调链路中同步执行 WIC 编码，否则 readback 和图片压缩会阻塞该线程。视频流应优先使用 NV12/P010 加硬件 MFT。

## 跨进程纹理共享

### 多槽广播：SharedFrameBus

`SharedFrameBus` 是固定尺寸、固定格式、固定 GPU adapter 的 latest-wins 广播环。生产者可配置 2 到 8 个共享纹理槽，并向最多 8 个**可信的同 adapter 进程**注册独立消费者。一个共享的 ready timeline fence 表示每个发布 sequence 何时可由 GPU 读取；每个消费者有自己的 done timeline fence，表示该消费者何时完成对应 sequence。生产者只在槽没有 writer/reader、且相关 done fence 已覆盖租约后复用它：

```text
                         +-> consumer 0: pin latest -> wait ready(N) -> signal done0(N)
source -> bus slot ring -+-> consumer 1: pin latest -> wait ready(N) -> signal done1(N)
                         +-> consumer n: pin latest -> wait ready(N) -> signal donen(N)
             |
             +-> publish(): CopyResource 到可写槽
             +-> begin_publish()/commit(): 上游直接写可写槽
             +-> WGC bus-only: 源 surface 的 ROI 直接 CopySubresourceRegion 到可写槽
```

每个消费者独立选择比自己上次所见 sequence 更新的最新槽，可以跳过中间帧。慢消费者只固定自己正在读取的槽；只要还有可复用槽，生产者和其他消费者继续前进，不会为它排队。若所有可用槽都正被持有，零超时发布返回 timeout 并增加 `no_slot`，调用方可丢帧或重试，而不是积累无界延迟。

复制发布适用于已有普通 D3D11 纹理的上游：

```cpp
gpu::SharedFrameBusConfig config;
config.width = frame.info().width;
config.height = frame.info().height;
config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
config.bind_flags = D3D11_BIND_SHADER_RESOURCE
    | D3D11_BIND_RENDER_TARGET; // 兼容 D2D 与 VideoProcessor 直接读取
config.slot_count = 4;       // 允许 2..8
config.max_consumers = 4;   // 允许 1..8

gpu::SharedFrameBusPublisher bus;
if (!gpu::SharedFrameBusPublisher::create(
        capture.device(), config, bus)) {
    return false;
}

gpu::SharedFrameBusRegistration registration;
if (!bus.register_consumer(target_process_handle, registration)) {
    return false;
}
// 通过应用自己的 pipe/socket/RPC 把 registration 发给这个目标进程。

// timeout=0：没有可复用槽时立即返回，实时链路可选择丢弃本帧。
if (!bus.publish(frame.texture(), 0)) {
    // 检查 GpuError；timeout 对应 no_slot，其他状态需要中止或重建。
}
```

若自定义 GPU 上游能直接把结果写入 bus 槽，可消除 `publish()` 的那次复制。所有写命令必须先排入创建 bus 的同一设备 immediate context；执行 deferred command list 后也要在 `commit()` 前提交到该 context：

```cpp
gpu::SharedFrameBusWriteLease write;
if (bus.begin_publish(0, write)) {
    render_or_convert_directly_into(write.texture());
    if (!bus.commit(std::move(write))) {
        return false;
    }
}
```

WGC 固定 ROI 可以直接发布到 bus。先在目标 adapter 的 D3D11 device 上创建固定尺寸的 bus，再用 bus-only 工厂创建捕获；显示器版本只需把最后一段换成 `create_for_monitor_to_bus(monitor, bus, ...)`：

```cpp
gpu::SharedFrameBusConfig bus_config;
bus_config.width = 320;
bus_config.height = 320;
bus_config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
bus_config.bind_flags = D3D11_BIND_SHADER_RESOURCE
    | D3D11_BIND_RENDER_TARGET;
bus_config.slot_count = 4;

gpu::SharedFrameBusPublisher bus;
if (!gpu::SharedFrameBusPublisher::create(device, bus_config, bus)) {
    return false;
}

gpu::WgcMailboxConfig mailbox;
mailbox.mode = gpu::WgcMailboxMode::centered_region;
mailbox.width = 320;
mailbox.height = 320;

gpu::WgcCapture capture;
auto created = gpu::WgcCapture::create_for_window_to_bus(
    window, bus, capture_options, mailbox, capture);
if (!created || !capture.start()) {
    return false;
}
```

稳态发布拓扑是：

```text
WGC 完整 capture surface
          |
          | CopySubresourceRegion(resolved ROI)
          v
SharedFrameBusWriteLease.texture() -> commit/ready fence -> 多进程消费者
```

这条路径不分配 WGC 私有输出环，也不会先产生 `WgcFrameLease` 再调用 `bus.publish()`，因此消除了“源 ROI -> WGC 私有 ROI 槽 -> bus 槽”中剩余的第二次发布复制。WGC/DWM 仍然生成完整 capture surface；这里的“直写”仅指 FluxCap 从该 surface 到最终 bus 槽只提交一次 ROI copy，不表示 Windows 捕获源本身是 ROI surface 或整条链路没有 GPU 复制。

bus-only 捕获没有可供本进程租用的私有帧，`capture.acquire_latest(...)` 会立即返回 `WgcStatus::invalid_state`，即使传入 `INFINITE` 也不会等待。进程内或跨进程读者统一通过 `SharedFrameBusConsumer::acquire_latest()` 取得 `SharedFrameBusFrameLease`。

当前 bus 协议版本是 v5。纹理 sequence 与 `SharedFrameBusFrameMetadata`、damage/cursor sidecar 原子配对；native move 与 inferred move provenance 互斥，move 必须引用严格更早的 `base_sequence`。异步 GPU move inference 通过独立的 `{epoch, nonce, sequence}` 结果环发布，迟到结果不会黏到后续纹理。consumer 会校验 metadata/sidecar commit stamp、颜色契约、ROI、damage 边界和 cursor payload；损坏槽被 quarantine。新 consumer 仍可打开 v1-v4 registration，旧协议缺失的字段显式清零。

bus 是固定尺寸、格式、色彩空间和 adapter 的资源 epoch。bus-only 捕获可直接写 BGRA8/scRGB，或把 source mailbox 直接写入 NV12/P010 render-target bus；direct RGB bus 仍要求 mailbox 与 bus 同尺寸。BGRA8/P709 的确定性 planar 后端用同一目标纹理的 Y/UV plane RTV 各 draw 一次，无中间像素纹理和 `Copy*`；shader 与 CPU damage mapper共用整数 center-point tap，UV 使用固定 2x2 box，因此 scaled dirty 可以按最终字节支持域精确映射并对齐 4:2:0。若 source SRV/plane RTV 不可用，`automatic` 在 epoch 首帧固定回退 VP；scaled VP 帧强制 full damage。Desktop Duplication 的同步 native scaled move 在输出位移可整除且保持偶数 chroma phase 时发布可重放内部 move，并把最终字节支持域的剩余部分拆成最多四条 dirty strip；非整/奇数位移、空内部区域或容量不足继续 fail-closed。WGC 的异步 scaled inferred move 仍发布 fail-closed，因为迟到结果不能缩减已随纹理提交的 dirty。独立 cursor 仍按稳定、可表示的几何映射。运行中允许在 `centered_region` 与 `absolute_region` 之间切换或改变 `x/y`，只要 bus 输出契约不变；成功切换会递增 mailbox generation。尺寸、格式、颜色或 adapter 变化需要创建新的 capture/bus/consumer/encoder epoch。

`create_for_window_to_bus()` / `create_for_monitor_to_bus()` 在创建成功时独占该 bus 的生产者端；在对应捕获 `stop()` 或析构前，第二个 bus-only WGC producer、`bus.publish()` 和公开的 `bus.begin_publish()` 都会失败，避免两条写入链交错 sequence。捕获内部以共享所有权保留 bus 状态，因此移动或销毁外层 `SharedFrameBusPublisher` wrapper 不会让正在运行的捕获悬空；实际应用仍应保留 publisher wrapper，以便注册/注销消费者和读取统计。停止捕获会释放生产者独占，之后 bus 可再次由其他 producer 使用。

统计口径保持可核对：bus-only 模式下 `capture.stats().published_frames` 只统计成功的 bus commit；零超时取不到可复用槽，或 bus 管理锁正忙时，都会丢弃该源帧并增加 `capture.stats().skipped_no_buffer`。成功取得管理锁的尝试会体现在 `bus.stats().publish_attempts` 中，槽耗尽还会增加 `bus.stats().no_slot`；管理锁忙时不会等待，也不会修改受该锁保护的 bus 计数。成功提交增加 `bus.stats().published_frames` 和 `direct_publishes`，`copied_publishes` 不会因 WGC 直写增加。`received_frames` / `dropped_at_source` 描述 WGC frame-pool 入口；`capture.stats().overwritten_frames` 是私有 WGC 环指标，在 bus-only 模式下不表示 bus 槽复用，后者应结合 `bus.stats().reused_ready_slots` 与各消费者的 sequence gap 分析。非 timeout 的 bus begin/commit 或 device-lost 错误会写入 `capture.last_error()`。

消费者在 registration 记录的 adapter LUID 上创建设备，然后打开、取得最新租约并显式释放：

```cpp
gpu::SharedFrameBusConsumer consumer;
if (!gpu::SharedFrameBusConsumer::open(
        consumer_device, registration, true, consumer)) {
    return false;
}

gpu::SharedFrameBusFrameLease lease;
if (consumer.acquire_latest(1000, lease)) {
    ID3D11Texture2D* texture = lease.texture();
    const std::uint64_t sequence = lease.info().sequence;
    const gpu::SharedFrameBusFrameMetadata metadata = lease.metadata();

    // 先把对共享纹理的 VideoProcessor 读取排入 consumer immediate context。
    if (!transform.process(texture)) {
        lease.reset();
        return false;
    }
    // done fence 排在 transform 的共享纹理读取之后；随后编码独立输出纹理。
    if (!consumer.release(lease)) {
        return false;
    }
    if (!encoder.encode_texture(
            transform.output_texture(),
            metadata.source_timestamp_100ns,
            frame_duration_100ns)) {
        return false;
    }
}

// 必须先 release 当前租约，再关闭；随后通知生产者 unregister。
if (!consumer.close()) {
    return false;
}
```

持续编码可把 consumer 所有权直接交给 `AsyncGpuPipeline`。worker 自己执行 `acquire latest -> transform(lease.texture) -> release -> encode transform output`，因此不会把“每个 consumer 同时只能持有一个 lease”的对象塞进外部多帧队列，也没有 consumer 侧 `CopyResource`：

```cpp
gpu::AsyncGpuPipeline pipeline;
auto created = gpu::AsyncGpuPipeline::create_from_shared_bus(
    consumer_device,
    std::move(consumer),
    pipeline_config,
    packet_callback,
    callback_context,
    pipeline);
if (!created) return false;

// terminal drain 会处理当时最新帧、drain 编码器并关闭 moved consumer。
if (!pipeline.drain(15'000)) return false;
pipeline.close();

// registration 仍由 producer 侧持有；worker 关闭 consumer 后才能注销。
if (!bus.unregister_consumer(registration, 5'000)) return false;
```

若 metadata 带 source timestamp，bus-source worker 用它生成编码时间线并保证单调；旧协议/普通发布没有该字段时按编码帧率生成 fallback 时间戳。输入尺寸、格式、颜色或 D3D11 device 不匹配会使 worker fail-closed，但在返回错误前仍释放当前 bus lease。BGRA bus 槽建议保留默认的 `D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET`；planar bus 必须带 `D3D11_BIND_RENDER_TARGET`。

`SharedFrameBusRegistration::texture_handles` 是 legacy DXGI shared texture identifiers，**不能**调用 `CloseHandle`。`control_mapping_handle`、`publisher_process_handle`、`ready_fence_handle`、`done_fence_handle` 才是复制到目标进程的 4 个 NT handle；`open(..., true, ...)` 会在成功或失败路径接管并关闭这 4 个 handle。使用 `false` 时它们仍由调用方负责。registration 与可写控制页不是面向不可信进程的安全边界，只能通过已经完成身份认证和访问控制的 IPC 交给可信消费者。

正常生命周期是 publisher `create -> register_consumer -> publish`，consumer `open -> acquire_latest/release -> close`，最后 publisher `unregister_consumer`。`unregister_consumer` 会立即阻止新的 acquire，但只有在并发 acquire 已离开且 done fence 证明所有 reader 完成后才回收消费者索引；应使用有限超时。生产者进程退出会唤醒正在等待的消费者。若消费者异常退出，生产者会检查其进程状态和 done fence：能证明完成的槽直接回收，无法证明 GPU 已完成的槽会在当前 bus epoch 永久 quarantine，绝不冒险复用。quarantine 使容量不足或所有槽被隔离时，应销毁并重建 bus，开始新的资源/控制页/fence epoch。

`bus.stats()` 提供 `publish_attempts`、`published_frames`、`copied_publishes`、`direct_publishes`、`no_slot`、`reused_ready_slots`、消费者注册/回收/异常回收数、`quarantined_slots` 和 `active_consumers`。这些计数用于区分成功发布、实时背压、consumer churn 与故障隔离，不应只看 publisher API-return rate。

### 单纹理 Keyed Mutex

生产者创建固定尺寸/格式的共享纹理，发布时在 producer key 上获取 mutex、复制源纹理并以 consumer key 释放。随后把 NT handle 复制到目标进程，并通过应用自己的 IPC 传递 `SharedTextureExport` 元数据：

```cpp
gpu::SharedTextureConfig config;
config.width = frame.info().width;
config.height = frame.info().height;
config.format = DXGI_FORMAT_B8G8R8A8_UNORM;
config.bind_flags = D3D11_BIND_SHADER_RESOURCE;
config.producer_key = 0;
config.consumer_key = 1;

gpu::SharedTexturePublisher publisher;
if (!gpu::SharedTexturePublisher::create(
        capture.device(), config, publisher)
    || !publisher.publish(frame.texture(), 1000)) {
    return false;
}

gpu::SharedTextureExport metadata;
if (!publisher.export_to_process(target_process_handle, metadata)) {
    return false;
}
// 通过你自己的 pipe/socket/RPC 把 metadata 发送给目标进程。
```

消费者必须在 metadata 中 LUID 对应的同一 GPU adapter 上创建设备：

```cpp
gpu::SharedTextureConsumer consumer;
if (!gpu::SharedTextureConsumer::open(
        consumer_device, metadata, true, consumer)
    || !consumer.acquire(1000)) {
    return false;
}

ID3D11Texture2D* texture = consumer.texture();
// 在持有 consumer key 期间读取或复制 texture。

if (!consumer.release()) {
    return false;
}
```

`take_handle_ownership=true` 表示 `open` 会关闭已复制到消费者进程的 NT handle。库只定义 GPU 资源和同步协议，不负责传输 metadata、进程发现、访问控制或断线恢复。每次成功 acquire 必须对应一次 release；生产者和消费者应使用有限超时，避免对端退出后永久等待。

## CPU API

CPU API 适合必须立即访问像素、只需要桌面/ROI，或目标机器不支持 WGC 的场景。C ABI 版本为 1：

```cpp
#include <fluxcap/fluxcap.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main() {
    auto config = fluxcap_config_default();
    config.target_fps = 60;
    config.flags |= FLUXCAP_FLAG_INCLUDE_CURSOR;
    config.flags |= FLUXCAP_FLAG_DETECT_DIRTY_REGIONS;

    fluxcap::Session session(config);
    session.start();

    if (auto frame = session.acquire_latest(100ms)) {
        std::cout << frame->width() << 'x' << frame->height()
                  << ", sequence=" << frame->sequence()
                  << ", dirty=" << frame->dirty_regions().size() << '\n';
    }
}
```

`fluxcap::Frame` 不可复制、可以移动，并在析构或 `reset()` 时归还帧槽。C 调用方必须零初始化 `fluxcap_frame`、只释放一次租约，并在 `fluxcap_destroy` 前释放该 session 的所有帧。复制 C 结构不会复制租约。

当前仓库的 CPU 示例会捕获一帧并写出 32 位 BMP：

```powershell
.\build\Release\fluxcap_grab.exe .\capture.bmp
```

完整 GPU 示例会捕获前台窗口（也可显式传入十进制或 `0x` 开头的 HWND），把首帧分别编码为内存 PNG/JPEG，并运行 BGRA -> NV12 -> H.264 硬件管线：

```powershell
# 前台窗口，默认 120 帧
.\build\Release\fluxcap_gpu_pipeline.exe

# 指定 HWND 和帧数
.\build\Release\fluxcap_gpu_pipeline.exe 0x0000000000012345 300
```

该示例在控制台报告图片字节数、H.264 packet/字节数、管线 FPS 以及 WGC received/published/overwritten/source-dropped 统计。示例把编码结果保存在内存中，不生成 MP4 文件。

GPU 屏幕预览默认捕获主显示器。WGC 回调把整帧或中心 ROI 直接写入 `SharedFrameBusWriteLease`，同进程消费者取得 `SharedFrameBusFrameLease.texture()` 后由 D2D 直接缩放并绘制到 flip-model 交换链，不做 consumer 侧 `CopyResource`，同时叠加实时 FPS 和各阶段耗时：

```powershell
# 主显示器
.\build\Release\fluxcap_gpu_preview.exe

# 主显示器中心 320 x 320 或 640 x 640
.\build\Release\fluxcap_gpu_preview.exe --size 320
.\build\Release\fluxcap_gpu_preview.exe --size 640

# 查看显示器编号并选择其他屏幕
.\build\Release\fluxcap_gpu_preview.exe --list-monitors
.\build\Release\fluxcap_gpu_preview.exe --monitor 1 --size 320

# 可选：改为捕获指定窗口
.\build\Release\fluxcap_gpu_preview.exe --window 0x0000000000012345
```

预览窗的 `Capture size` 菜单可以在 Native、320×320 和 640×640 之间实时切换。`SharedFrameBus` 是固定尺寸协议，因此尺寸改变会建立新的 bus/WGC epoch；320/640 epoch 让 WGC 在回调入口直接写中心 ROI，Native epoch 直接写当前源尺寸。预览窗会从整屏捕获中排除，避免递归镜像；WGC `MinUpdateInterval` 保持为 0，不设置 240/1000 FPS 或其他人为限速。交换链使用 frame-latency 信号和非阻塞 `Present`。

叠加层把帧率拆成三项：`WGC FPS` 是 WGC `received_frames` 的滚动速率，`Bus FPS DIRECT` 是消费者实际取得的不同 bus sequence 速率，`Preview FPS` 是交换链成功接受的 `Present` 调用率。三者都不应解释为 GPU synthetic operation/s 或超出内容源的物理 scanout 速率。`Acquire` 包含等待新 sequence 的时间，`Release` 是把 done-fence 排在本帧全部 GPU 读取之后并提交的 CPU 调用时间，`Draw`、`Present` 和 `Total` 是本轮预览提交耗时。Native 和固定 ROI 都在 D2D `EndDraw`/`Present` 已排队读取 bus texture 后才释放 lease；每轮 release 后才允许 WGC 复用对应槽。

## 测试

运行全部测试：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

只构建并运行真实 GPU 管线测试：

```powershell
cmake --build build --config Release --target fluxcap_gpu_capture_tests --parallel
.\build\Release\fluxcap_gpu_capture_tests.exe
```

独立运行真实子进程 `SharedFrameBus` 协议测试；`build-shared` 需要以 `-DBUILD_SHARED_LIBS=ON` 配置：

```powershell
# 静态库配置
cmake --build build --config Release --target fluxcap_shared_frame_bus_tests --parallel
.\build\Release\fluxcap_shared_frame_bus_tests.exe

# DLL 配置
cmake --build build-shared --config Release --target fluxcap_shared_frame_bus_tests --parallel
.\build-shared\Release\fluxcap_shared_frame_bus_tests.exe
```

GPU 测试覆盖：

* WGC 对完全遮挡窗口的 GPU 纹理捕获，以及窗口完全移出虚拟桌面后重新建立捕获。

* 融合 ROI mailbox 的中心/绝对区域像素、输出尺寸、generation 切换、旧 lease、区域不可用与恢复。

* WGC bus-only 的 320×320/640×640 ROI 跨进程像素、v2 时间戳/源尺寸/ROI/generation/色彩空间、直写统计、无私有 lease、同尺寸重配置、full-frame 固定 bus 的尺寸变化保护、源缩小后的 `region_unavailable` 与原 bus 恢复、两槽全部被 reader 固定时的 `no_slot` 丢帧与恢复，以及 publisher wrapper 销毁后的保留生命周期。

* 320×320/640×640 GPU 中心裁切的输出尺寸和坐标映射、BGRA 缩放、BGRA 到 NV12、BGRA 到 P010。

* deterministic plane-RTV scaled planar 的逐字节 damage 支持域、与 shader 共用的整数 tap、DD native scaled move 的内部 move/最多四条 edge dirty、非整/奇数位移与容量不足时的原子 fail-closed、VP fallback 强制 full damage、WGC 异步 scaled inferred move fail-closed，以及 cursor position/hotspot/shape 缩放与不可表示时的 fail-closed 行为。

* 合成 bitstream fixture 中的 H.264/HEVC/AV1 color description 解析，以及 HEVC/AV1 HDR10 mastering-display、MaxCLL/MaxFALL 跨 packet 合并与严格值校验；真实 P010 硬编测试不据此宣称所有 MFT 都会传播 HDR10 metadata。

* D3D11-aware H.264 硬件编码并收到有效包。

* 仅 2 个硬编输入槽连续编码 64 帧，验证 tracked-sample free/in-flight 回收不会提前覆盖。

* HEVC/AV1 的可选硬件 probe；硬件声明支持时继续初始化和编码。

* 把 shared NT handle 复制到真实子进程；子进程按 adapter LUID 创建设备，使用非零 keyed-mutex key 打开、同步并读取纹理。

* L3 qualification analyzer 的合成工件门禁：完整 hash/tuple/provider 工件必须到 L3，runtime JSON 篡改必须降级，ETL 篡改必须在 screening 阶段失败。

* GPU 纹理到内存 PNG/JPEG，并验证文件签名。

* 4 个并发生产者提交到异步原子槽环，并以移动 lease 验证零额外交接、覆盖统计和 terminal drain。

* `AsyncGpuPipeline` 的 worker-owned bus consumer 直接变换/硬编并产生 packet；metadata 时间戳传递、尺寸不匹配、同 adapter 不同 D3D11 device、失败/drain/close 后的 lease 释放和 producer unregister。

* packet callback 故意失败时立即回收所有 ready lease；4 个生产者与 `close()` 并发时验证公开 pImpl 生命周期安全。

`fluxcap_shared_frame_bus_tests` 覆盖真实跨进程 copy/direct publish、v5 metadata/sidecar、v1-v4 兼容、迟到 move 结果精确匹配、16 槽覆盖淘汰、损坏结果/槽隔离、masked-color 与 monochrome cursor shape、颜色契约、latest sequence、慢/异常消费者回收和 slot quarantine。子进程会在相同 adapter 上真实打开共享纹理与 fence，不是进程内模拟。

真实捕获测试必须运行在已登录、可访问交互式桌面的 Windows 会话中。GPU 测试要求 D3D11 video processing 和硬件 H.264 MFT；HEVC/AV1 不可用时会跳过对应编码验证。

CPU 测试还覆盖并发消费者、latest-frame 新鲜度、帧槽耗尽、旧租约 ABA、防消费者饥饿、有限超时、dirty 基线序号、异步启停、统计以及由 C 编译器构建的公共头/ABI 冒烟测试。

## Benchmark 与性能声明

一台真实笔记本（RTX 5060 Laptop / Ryzen 9 8945HX / Windows 11）上的绝对数字与复现命令见 [docs/benchmarks-20260909-rx5060.md](docs/benchmarks-20260909-rx5060.md)。

FluxCap 的 GPU 架构目标是降低“窗口/显示器捕获 -> 缩放/色彩转换 -> 编码/共享”的端到端延迟：

* WGC 直接交付 GPU surface，常规视频链路不做 CPU 像素回读。

* 固定区域可在 WGC 回调入口直接写入 ROI mailbox，避免整帧 ring copy 和后续 crop copy；需要跨进程广播时也可直接写最终 `SharedFrameBus` 槽，再消除私有 ROI 槽到 bus 的一次发布复制。

* latest-frame 策略主动丢弃过期源帧，避免队列延迟无限增长。

* 捕获纹理环、VideoProcessor 输出和 tracked-sample 编码输入池均在稳态复用。

* `AsyncGpuPipeline` 用有界原子槽环隔离捕获与硬编背压，拥塞时丢旧保新。

* BGRA8/P709 到 NV12/P010 优先使用确定性 D3D11.3 plane-RTV 双 draw；HDR、旋转或能力不足时使用 VP fallback。

* 视频编码只枚举 hardware MFT，并使用 D3D11 device manager 传递 GPU surface。

* 跨进程传输共享 GPU 资源句柄，不逐帧传送 CPU 像素。

这些设计可以在窗口捕获、ROI 后处理、硬件编码和跨进程消费等特定工作负载中减少复制和排队，但当前仓库**没有**与 DXGI Desktop Duplication 在同机器、同分辨率、同内容、同输出契约下的 GPU 对照 benchmark。因此，当前版本不声称在所有硬件和场景中普遍快于 DXGI，也不应把功能测试结果当作性能结论。

GPU benchmark 会创建持续变化的 1280 x 720 窗口，执行 WGC -> BGRA -> NV12 -> H.264，并报告各阶段的 min、p50、p95、p99 和 mean：

```powershell
.\build\Release\fluxcap_gpu_bench.exe --frames 1000 --warmup 100 --media-fps 240

# 相同 320 x 320 输出契约：旧的整帧 mailbox + 后裁切
.\build\Release\fluxcap_gpu_bench.exe --frames 600 --warmup 100 --post-crop-size 320

# 相同 320 x 320 输出契约：融合 ROI mailbox
.\build\Release\fluxcap_gpu_bench.exe --frames 600 --warmup 100 --mailbox-size 320

# 只运行随窗口所在显示器 VSync 的 D3D 测试源；关闭测试窗口即可停止
# 刷新率由运行时显示器决定
.\build\Release\fluxcap_gpu_bench.exe --source-only
```

指标包括 latest-frame 等待、已发布帧的 mailbox age、可选 post-crop CPU 提交、VideoProcessor CPU 提交、硬编调用、提交到编码 packet，以及 acquire + process + encode 调用总时间。它还验证 `sequence` 和 WGC `SystemRelativeTime` 严格递增，报告 consumed unique presentations/s、sequence gap 与 received/published/overwritten/source-dropped/no-buffer。每个 measured submission 都必须得到一个可按时间戳关联的编码 packet 延迟样本；覆盖不是 `frames/frames` 时 benchmark 会打印实际覆盖并失败。capture counters 在最后一次 measured 编码提交后、`encoder.drain()` 前冻结，避免 drain 时间扩张统计窗口。crop/transform CPU submission 不代表 GPU 已完成执行；`mailbox age` 从 FluxCap 排队 WGC mailbox 复制后开始，因此不要把两者误读为显示扫描到编码完成的绝对端到端延迟。`--media-fps` 只设置转换/编码媒体类型与时间戳，不会改变 D3D 测试窗口跟随的显示器刷新率。

synthetic 饱和 benchmark 不进入 WGC，用预生成 GPU 纹理分别测量 CPU 提交、D3D11 event query 批完成、顺序硬编、定速异步管线和瞬时洪泛：

```powershell
# 验证 240 fps 媒体时基、按 240 ops/s 定速喂入的持续处理
.\build\Release\fluxcap_gpu_throughput_bench.exe `
  --operations=5000 --encode-operations=1000 `
  --media-fps=240 --paced-fps=240

# 探测单路千帧时基的饱和点
.\build\Release\fluxcap_gpu_throughput_bench.exe `
  --operations=5000 --encode-operations=1000 `
  --media-fps=1000 --paced-fps=1000
```

中心裁切 benchmark 使用 2560×1600 合成源，专门测量 `GpuCrop` 的 320/640 固定纹理复制提交和 D3D11 event query 批完成吞吐：

```powershell
.\build\Release\fluxcap_gpu_crop_bench.exe --size=320
.\build\Release\fluxcap_gpu_crop_bench.exe --size=640
```

融合 copy-topology microbenchmark 不进入 WGC、DWM 或编码器；它使用同一张 2560×1600 合成源和相同的固定 ROI 输出，对比 raw D3D11 的“整帧复制到 mailbox + ROI post-copy”与“源 ROI 直接复制到 mailbox”。它会逐像素验证两条路径的输出，并分别报告 CPU 提交、D3D11 event-query 批完成和每操作 API copy 载荷：

```powershell
.\build\Release\fluxcap_gpu_ingress_bench.exe `
  --size=320 --operations=10000 --warmup=1000 --rounds=8
.\build\Release\fluxcap_gpu_ingress_bench.exe `
  --size=640 --operations=10000 --warmup=1000 --rounds=8
```

当前测试机每个尺寸使用 8 轮、每轮 10,000 次操作，交替 AB/BA 顺序并取配对比值中位数，以降低升频和固定先后顺序偏差。320×320 的融合路径相对旧路径 CPU 提交为 `2.17x`、event-query GPU 批完成为 `24.72x`、API copy 载荷减少 `41x`；640×640 分别为 `2.73x`、`9.55x` 和 `11x`。这些比值只归因于该合成 raw D3D11 copy topology：两条路径都复用源纹理和输出纹理，未包含 WGC 新帧生产、DWM、跨进程共享、色彩转换或编码，因此既不是 WGC 新帧率，也不是完整捕获入口或 FluxCap 相对其他截图库的加速比。载荷按每个 API copy 计一次，也不等于考虑缓存后的实际显存总线读写。

上面的 raw-copy benchmark 仍不能代表本次 WGC direct-bus 改动的加速比，因为 Path A 的第一步复制整张 2560×1600 source，且两侧都没有 bus fence。严格的 bus 发布拓扑对照由 `roi-publish-compare` 模式提供：staged 路径执行 `source ROI -> private ROI -> bus.publish()` 两次 ROI copy，direct 路径执行 `begin_publish -> source ROI -> lease.texture() -> commit` 一次 ROI copy。两条路径复用同一 producer device 和源纹理，每次使用相同配置的新 bus epoch；真实子进程先完成 registration，producer 的 event-query 测量窗口保持零背压，随后消费者通过同一 ready/done fence 协议取得最终 sequence 并逐像素验证。

```powershell
foreach ($size in 320, 640) {
  .\build\Release\fluxcap_gpu_shared_bus_bench.exe `
    --mode=roi-publish-compare --size=$size --consumers=1 `
    --frames=10000 --warmup=1000 --rounds=8
}
```

当前 RTX 5060 Laptop GPU 的单次 Release 样本如下。时间比值是每轮交替 AB/BA 后取 staged/direct 配对比值中位数，因此不必等于两项独立中位速率的商；`80,000 / 0` 表示每条路径 8 轮合计 80,000 个 measured publish、零 no-slot。它只衡量 synthetic bus publish，不是 WGC 新画面率：

| ROI     | staged CPU publish/s | direct CPU publish/s | CPU 配对比值 | event-query 配对比值 |         copy 命令/载荷 |
| ------- | -------------------: | -------------------: | -------: | ---------------: | -----------------: |
| 320×320 |               10,985 |               15,395 |   1.250x |           1.250x | 2 -> 1 / 2.000x 减少 |
| 640×640 |               16,420 |               16,803 |   1.042x |           1.041x | 2 -> 1 / 2.000x 减少 |

两种尺寸都完成真实子进程最终全纹理坐标与 sequence marker 校验。确定性结论是 producer copy 命令和按 API copy 计算的载荷减半；时间收益受驱动调度、GPU 频率、纹理尺寸以及每帧 ready-fence `Signal+Flush1` 固定成本影响，不能外推为固定倍数，更不能当作相对其他截图库的加速比。

真实 WGC bus A/B benchmark 持久复用同一个 D3D11 动画 HWND 和 producer device，在固定时间窗内按 `staged,direct,direct,staged` 执行 AB/BA：staged 为 `WGC 私有 ROI lease -> bus.publish(metadata)`，direct 为 `WGC -> SharedFrameBusWriteLease`。两条路径的 consumer 都直接执行 `bus texture -> NV12 transform -> release -> H.264`，没有 consumer copy；D3D11 event query 排在 transform 后并由 release 一并提交：

```powershell
cmake --build build --config Release --target fluxcap_gpu_wgc_bus_bench --parallel

# 默认：consumer 与 WGC/bus producer 位于同一进程
.\build\Release\fluxcap_gpu_wgc_bus_bench.exe `
  --size=320 --duration-ms=2000 --warmup-ms=500 --pairs=2

# 真实跨进程：子进程直接消费共享纹理并完成转换与硬编
.\build\Release\fluxcap_gpu_wgc_bus_bench.exe `
  --size=320 --duration-ms=2000 --warmup-ms=500 --pairs=2 `
  --consumer-mode=child --ipc-timeout-ms=15000
```

`--consumer-mode=inproc` 是默认模式：producer 和 consumer 共用 benchmark 进程及 D3D11 device。`--consumer-mode=child` 会为每次 topology run 启动真实子进程，producer 把 registration 句柄复制给该进程；子进程按 registration 的 adapter LUID 创建设备，直接执行 `SharedFrameBusFrameLease.texture() -> NV12 transform -> event query -> release -> H.264`。两种模式都没有 consumer 侧 `CopyResource`，父进程仍按相同的 staged/direct AB/BA 顺序统计 WGC、bus 和源 presentation。帧级路径不发送 pipe 消息；measurement 结束后，sequence/packet/GPU 覆盖计数先通过 `ResultHeader` 返回，随后仅把 source age、capture age、GPU completion 和 packet latency 四类逐帧样本按不超过 4 KiB 的消息分块回传。

子进程的创建、正确 LUID device、consumer、transform、event-query ring 和 encoder 初始化都在 `setup -> ready` 阶段完成，位于共享绝对 QPC 测量窗口之外；输出的 `setup -> ready` 只用于观察启动/初始化成本。`Measure -> Armed RTT` 与 `Close -> Closed RTT` 是控制管道往返时间，`ResultHeader delivery/pickup` 是 child 写 header 到 parent 取走它的同机 QPC 年龄，`parent result-chunk drain` 是 parent 排空四类样本 chunk 的时间；这些控制面/结果传输数字**不是**共享纹理数据面延迟，也不是 GPU 完成时间。

child 与 parent 使用同一组绝对 `measure_begin_qpc` / `measure_end_qpc`。每轮输出中的 measured seconds 和所有 `/s` 指标固定以这段 scheduled duration（即 `--duration-ms`）为分母，不会把 child 启动、encoder 初始化、warmup、drain 或结果传输时间算进去；`parent start/end` 与 `child start/end` 另行报告 actual 相对 scheduled boundary 的 skew，任一边界偏差超过 250 ms 时该轮无效。benchmark 还执行严格覆盖 gate：consumer 必须至少取得一帧和一个不同 source timestamp，sequence span 必须由 consumed frames 与 gap 精确覆盖；`packet submissions == consumer frames == packets == packet-latency samples`，且总 packet bytes 必须非零；source/capture age 样本必须逐帧齐全；GPU query samples 加 dropped 必须等于 consumer frames。四类 child 样本的声明数量还会按 `max(64, duration_ms * 10)` 限制，且 Header 与全部 chunks 共用一个绝对 IPC deadline。任一计数不一致、样本非有限值或越界都会让该 run 失败，而不是只打印不完整结果。

benchmark 分别报告源 swapchain 提交率、WGC received/published、bus publish、consumer frame、不同 `SystemRelativeTime`、sequence gap、`no_slot`、control contention、source/capture age、`transform -> GPU complete` 和 `submit -> packet` p50/p95/p99。child 模式中的 `capture/pub -> child age` 是 metadata QPC 到子进程 acquire 的跨进程交付年龄，包含 callback/mailbox、共享 fence 和消费者调度等成本；当前 metadata 仍没有独立的 bus-ready QPC。

下面是默认 `inproc` 模式的历史短 smoke 样本：320×320、500 ms 测量窗、1 个 AB/BA block 中，源 swapchain 约 `240 present/s`，但当前 60 Hz DWM/WGC 只交付约 `59-60 distinct presentation/s`；两条路径均为零 gap、零 `no_slot`、零 control contention。staged/direct 的 `transform -> GPU complete` p50 约 `0.79/1.00 ms`，`submit -> packet` p50 约 `0.53/0.60 ms`。这是短功能/口径样本，不是 child 模式结果或稳定性能排名；尤其不能用 `240 present/s` 冒充 WGC 的 240 个不同画面。

生产 soak 工具默认运行 30 分钟且不加入 CTest。`churn` profile 每 5 秒动态注册/注销第二 consumer，并让测试窗口在可容纳 ROI、缩小到 ROI 不可用、恢复之间循环；主 consumer 使用独立 D3D11 device 直接变换、释放并硬编。指标使用固定大小直方图，每 10 秒输出 heartbeat：

```powershell
cmake --build build --config Release --target fluxcap_gpu_bus_soak --parallel

# 30 分钟窗口 resize + consumer churn
.\build\Release\fluxcap_gpu_bus_soak.exe `
  --soak-minutes=30 --profile=churn --size=320

# 真实显示器直写稳态；只观察外部 device loss，不会主动诱发 TDR
.\build\Release\fluxcap_gpu_bus_soak.exe `
  --soak-minutes=30 --profile=steady --size=640 --monitor=0
```

工具会拒绝 sequence/packet timestamp 回退、10 秒 ROI 可用时间内无发布/消费/非空编码覆盖、零帧 churn 生命周期、恢复超时、slot quarantine、残留 consumer 和未预期 device loss。`bus_no_slot` 严格要求为零；window/churn 因注册/注销持有管理锁而产生的非阻塞 control-contention drop 必须同时不超过 16 帧和 WGC received 的 0.01%，steady profile 则要求为零。`--expect-device-lost` 只用于观察外部触发的移除；D3D11 没有安全的公开主动移除接口，因此工具不会制造 TDR。fence 提交优化与显式异步 deadline-batching 设计见 `docs/shared-frame-bus-fence-batching.md`；现有同步 `commit/release` 只改用语义等价的 `Flush1(ALL)`，没有偷偷改成延迟可见。

2026-07-15 在当前 RTX 5060 Laptop GPU 上完成了一次加固 gate 后的 320x320 window/churn Release 运行（`elapsed=1800.023 s`）：WGC 直写发布 `53,144` 帧，主 consumer 消费并获得 `53,132/53,132` 个非空编码 timestamp 覆盖，完成 `180/180` 次第二 consumer 注册/注销和 `178/178` 次 ROI 不可用/恢复。最终 `bus_no_slot=0`、control contention `1/10`、sequence regression `0`、zero-byte/unmatched packet `0/0`、packet timestamp regression `0`、recovery timeout `0`、quarantined slot `0`、device lost `0`、active consumer `0`。该 profile 约一半时间故意让 ROI 不可用，因此约 `29.52 presentation/s` 的有效消费率是压力状态机结果，不是捕获或 GPU 吞吐上限；12 个 sequence gap 符合 latest-wins consumer 契约且没有回退或编码覆盖缺口。

跨进程 bus benchmark 会启动 1、2 或 4 个真实消费者子进程，在固定 8 槽 `SharedFrameBus` 上无节流 copy-publish 320×320/640×640 BGRA8。每个消费者独立 latest-wins，复制到自己的私有纹理，并用唯一坐标像素和全局 sequence marker 验证实际内容：

```powershell
foreach ($size in 320, 640) {
  foreach ($consumers in 1, 2, 4) {
    .\build\Release\fluxcap_gpu_shared_bus_bench.exe `
      --size=$size --consumers=$consumers --frames=2000
  }
}
```

该 benchmark 不进入 WGC/DWM，也没有目标 FPS 或定速等待；仅当所有槽忙时立即重试。publisher 数字是 CPU 观察到的 `publish()` API-return 窗口，消费者最终私有纹理验证才是 GPU 完成边界。各消费者允许跳过中间 sequence，因此 distinct-sequence rate/coverage 不能解释为所有消费者逐帧处理，也不能解释为屏幕新帧率。

当前 RTX 5060 Laptop GPU 的单次 Release 样本使用 1,000 帧 warmup 和 10,000 个成功 measured publish。消费者速率一栏给出各真实子进程的 distinct-sequence rate 范围；无节流 publisher 故意比消费者快得多，所以测试同时如实报告了大量 sequence gap：

| 尺寸      | 消费者 | copy-publish API-return/s | no-slot 重试 | 每消费者 distinct sequence/s |
| ------- | --: | ------------------------: | ---------: | -----------------------: |
| 320×320 |   1 |                    24,198 |      2,497 |                    1,010 |
| 320×320 |   2 |                    18,164 |        945 |              1,099–1,487 |
| 320×320 |   4 |                    19,738 |      3,682 |                  568–630 |
| 640×640 |   1 |                    21,281 |      1,755 |                      852 |
| 640×640 |   2 |                    23,119 |      2,654 |                  785–948 |
| 640×640 |   4 |                    12,432 |      3,702 |                  672–818 |

这只是 synthetic 无节流广播压力样本，不是 WGC FPS、显示刷新率、物理显存带宽或相对其他截图库的加速比。1 个 publisher bus copy 替代 N 个逐消费者发布 copy 的 API 载荷比例是 `N:1`；消费者复制到自己的私有输出属于两种拓扑共有的下游工作，不计入这项发布载荷差异。

本仓库当前测试机的 Release/H.264 样本如下。`direct encoder` 是顺序硬编饱和吞吐，`paced pipeline` 是 BGRA copy + VideoProcessor + encoder 的定速异步管线：

| 尺寸          | media/paced 时基 |    direct encoder |     paced pipeline |             覆盖 |
| ----------- | -------------: | ----------------: | -----------------: | -------------: |
| 320 x 320   |   1000/1000 Hz | 2747.86 packets/s |    972.21 frames/s |        51/2000 |
| 640 x 640   |     240/240 Hz | 1824.71 packets/s |    239.82 frames/s |         0/1000 |
| 640 x 640   |   1000/1000 Hz | 1831.17 packets/s |    891.51 frames/s |       213/2000 |
| 1280 x 720  |     240/240 Hz |  约 1200 packets/s |   约 239.9 frames/s |              0 |
| 1280 x 720  |   1000/1000 Hz |  约 1200 packets/s | 约 790-800 frames/s | 约 196-206/1000 |
| 1920 x 1080 |     240/240 Hz |  238.95 packets/s |    235.02 frames/s |       约 12/600 |
| 3840 x 2160 |     240/240 Hz |  169.20 packets/s |    164.48 frames/s |       约 74/240 |

720p VideoProcessor 批完成约 `23k-27k ops/s`，非阻塞 API 瞬时接受能力超过百万次/s。1080p 使用 1000 fps 饱和媒体类型时测得约 `607 packets/s`，按 240 ops/s 喂入可处理约 `239.53 frames/s` 且零覆盖；这使用的是 1000 fps 编码时基，不能冒充标准 240 fps 文件时序。

当前 RTX 5060 Laptop GPU 上，2560×1600 到 320×320 的 `GpuCrop` event-query 批完成约 `582,482 ops/s`（有效载荷约 238.58 GB/s），到 640×640 约 `198,374 ops/s`（约 325.02 GB/s）。这些数字使用同一张合成源纹理和同一个输出纹理测饱和吞吐，不是对应数量的 WGC 新画面。

真实捕获 benchmark 使用 D3D11 flip-discard 测试窗口和 240 fps 编码媒体时间线，在当前 240 Hz 显示链上消费 `240.032` 个严格递增 presentation/s；最近一次运行 `received=867`、`published=867`、`source_dropped=0`、`sequence_gaps=1`。240 Hz 屏幕最多只能提供约 240 个不同的扫描呈现；达到 1000 operation/s 说明 GPU 处理有余量，不代表物理屏幕凭空产生 1000 个不同画面。

融合 ROI mailbox 的相同 240 Hz WGC 到编码提交吞吐样本：320×320 为 `239.637` 个严格递增 presentation/s，`source_dropped=0`、`sequence_gaps=0`；640×640 为 `239.018`，同样零 source drop 和 sequence gap。该口径在 `encode_texture` 返回时停止，不代表编码 packet 已完成或显示扫描到编码完成的绝对端到端延迟。旧的 full-mailbox + post-crop 路径在相同输出契约下也能达到显示链上限，因此 240 Hz 测试用于验证真实新帧、稳定性和提交延迟；融合路径的合成 copy-topology 饱和吞吐差异由上面的 event-query microbenchmark 单独衡量。

当前预览已经移除“对同一纹理无限重复裁切”产生的 synthetic `Process FPS`。窗口只报告真实 WGC received、不同 bus sequence 和成功 preview submit 的滚动速率，以及 acquire/release/draw/present/total 耗时。需要测量 GPU 对同一纹理的饱和操作能力时应使用独立 throughput/crop benchmark，不能把该数字放进实时屏幕预览冒充新画面率。

`fluxcap_bench` 比较 CPU `FluxCap steady-state` 与“每帧重建 GDI 资源”的基线：

```powershell
.\build\Release\fluxcap_bench.exe --frames 1000 --warmup 100
.\build\Release\fluxcap_bench.exe --frames 1000 --warmup 100 --no-dirty
```

CPU benchmark 不能证明 GPU 管线超过 Desktop Duplication。严谨的跨实现比较还需要在相同源内容、输出纹理格式、缩放、编码参数和同步点下加入 Desktop Duplication 基线，并同时报告分辨率、刷新率、GPU/驱动、电源模式与 dropped/overwritten 统计。

## 限制与安全边界

* `240/1000 FPS` 不是跨硬件、分辨率、编码器和源刷新率的最低保证。`AsyncGpuPipeline` 保证的是有界内存和不阻塞捕获；处理跟不上时会增加 `overwritten_frames`，而不是积压延迟。

* WGC 的唯一 presentation 速率受目标应用、DWM/compositor 和显示链限制。synthetic GPU operation/s 不能当作真实捕获 FPS。

* WGC 可以捕获被其他普通窗口遮挡的目标窗口。移出屏幕或最小化后的持续更新取决于目标应用和 Windows compositor；库不保证所有应用都继续提交内容。

* DRM/受保护内容、设置了排除捕获属性的窗口、硬件 overlay、独占全屏内容可能为黑色、缺失或行为依赖系统。

* FluxCap 不绕过 secure desktop、UAC、锁屏、用户会话隔离、进程权限或 Windows 捕获策略。

* WGC 支持 BGRA8/P709 与 scRGB FP16；Desktop Duplication 仅在 `DuplicateOutput1` 和驱动接受 FP16 时提供同一 HDR 输入。严格的 `P010 + DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 + HEVC/AV1` 契约可把 HDR10 静态 mastering-display、MaxCLL/MaxFALL 作为 MFT media-type 属性提交，并可在 `drain()` 时验证最终码流中的 VUI/SEI/OBU；库不会替驱动补写缺失的码流 metadata，现有真实硬编测试也不证明所有 MFT 都会传播它。动态 HDR metadata、任意 ICC/display profile 的自动转换和内容自适应 tone mapping 不在当前范围。

* 系统级 native move rect 只存在于 monitor-only Desktop Duplication；WGC window/monitor 仍明确标记 `wgc_damage_native_move_unavailable`，可选 GPU hash inference 仍标记为 inferred，二者不会混淆。

* Desktop Duplication 支持 90/180/270 度输出的逻辑 ROI 与 damage/move 映射，并在 VideoProcessor 支持时把旋转融合进最终 bus 写入；当前只有几何契约与非旋转真实 DD 覆盖，尚未在实际 rotated monitor 上完成端到端验证。baked-cursor 请求仍 fail-closed；驱动不提供独立 DXGI shape buffer 时使用带 estimated 标志的 Win32 fallback。

* 确定性 plane-RTV scaled planar 的 dirty 与 shader 共用整数 tap，已有实际最终字节覆盖测试；同步 DD native scaled move 只在位移可精确缩放且为偶数时发布“内部 move + 最多四边 dirty strip”。VP fallback 的 scaled 帧始终 full damage；WGC 异步 inferred move、非整/奇数位移、空内部区域与容量溢出继续 dirty/full/fail-closed，不能输出无法证明的增量契约。fractional、位置相关而无法稳定缓存的 cursor shape 会拒绝 planar 路径或让 automatic 模式选择 capture-native。

* VideoProcessor 的 rotation、格式、色彩空间和分辨率支持取决于驱动。NV12/P010 宽高必须为偶数。

* H.264/HEVC/AV1 是否可用、接受 NV12/P010/BGRA 的组合、异步行为和码流细节由硬件 MFT 决定，必须以 `GpuEncoder::probe` 和实际初始化结果为准。

* `external_surface_identity_verifiable` 只能证明传入 MFT 前的 `IMFDXGIBuffer` 对应调用方的原 texture/subresource；`D3D11_BIND_VIDEO_ENCODER` 门禁可排除“资源无法直接作为 encoder input”这一必然 staging 原因，`mft_identity()` 只绑定实际选中的 activation。公开 API 仍无法观察或证明 MFT、驱动及硬件内部是否使用其他私有表面，`mft_internal_copy_observable` 因此保持为 `false`，离线 L3/L4 也不会把它改成 `true`。L1-L4 证据定义和 WPR/WPA L3 流程见 [`docs/copy-evidence-qualification.md`](docs/copy-evidence-qualification.md)。

* `low_latency=true` 的异步 MFT 事件泵会主动 pause/yield 来避免固定 1 ms 等待台阶，可能占用更多 CPU；离线任务可关闭低延迟模式。

* 图片压缩是同步 GPU readback 加 CPU/WIC 编码，适合截图而非零阻塞视频热路径。

* 共享纹理只支持同一 GPU adapter；消费者会用 LUID 拒绝错误 adapter。跨 adapter 或跨机器传输需要另一套复制/编码方案。

* CPU GDI 后端捕获的是桌面合成结果，不提供独立的遮挡窗口离屏内容；需要窗口语义时使用 WGC 后端。

* CPU dirty region 是 tile 哈希推断，不是操作系统提供的精确 damage metadata，并存在理论上的哈希碰撞概率。

* GPU C++ API 当前不是稳定 C ABI。需要长期二进制兼容的 CPU 集成应使用 `fluxcap/fluxcap.h`。


## 许可证

MIT License，见 [LICENSE](LICENSE)。
