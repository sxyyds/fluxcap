# FluxCap 与其他 Windows 捕获方案对比

本文帮助搜索 "dxcam vs windows-capture" 或寻找 screen capture library 的人快速定位。
对比基于各项目官方 README（访问日期 2026-09-10）；特性矩阵描述"有无该能力"，
不评判实现质量。FluxCap 侧如有未覆盖项，如实标注"无"。

## 特性矩阵

| 能力 | FluxCap 0.1 | [DXcam](https://github.com/ra1nty/DXcam) | [windows-capture](https://github.com/NiiightmareXD/windows-capture) | OBS（应用，非库） |
|---|---|---|---|---|
| 语言/形态 | C++20 库（稳定 C ABI + C++ API） | Python 库（Cython 内核） | Rust 库 + Python 绑定 | C++ 应用（不可嵌入的完整程序） |
| 捕获后端 | WGC + Desktop Duplication + DD 多屏聚合 + GDI 兜底 | Desktop Duplication 为主，WGC 可选（`winrt` extra） | WGC + Desktop Duplication（2.0 起） | WGC + DD + GDI 等（内置源） |
| 窗口捕获 | WGC 窗口/显示器，含遮挡与离屏窗口（有契约测试） | 否（monitor 像素区域近似） | 是（WGC） | 是 |
| 显示器/多屏 | 单屏 + 虚拟桌面聚合 | 多显示器/多 GPU（device/output 索引） | 是 | 是 |
| 帧输出位置 | **默认全程 GPU 纹理**（`ID3D11Texture2D`），CPU 像素可选 | numpy ndarray（CPU，含 GPU→CPU 回读） | 帧缓冲 CPU 访问 + GPU 编码直通 | 内部渲染管线 |
| GPU 处理链 | ROI 裁切、缩放、BGRA8/scRGB FP16→NV12/P010（确定性 plane-RTV 或 VP） | 无（定位是取像素给 CV/AI 用） | 帧数据转换 | 完整滤镜/混流管线 |
| 硬件视频编码 | H.264/HEVC/AV1 硬件 MFT，外部纹理提交，原始码流输出 | 无（输出帧，不含编码） | 硬件编码器，MP4 容器 + 音频时基 | 完整推流/录制 |
| 跨进程共享 | 同 adapter GPU 纹理广播（SharedFrameBus v5，timeline fence，多消费者） | 无 | 无 | 无（进程内） |
| HDR | scRGB FP16 / P010 + PQ/BT.2020 自动探测 | 无 | 无 | 是 |
| 脏区/dirty | 原生 DD dirty/move rect + 精确 base_sequence 契约 | 帧"有无更新"语义 | dirty region 设置 | 内部优化 |
| 复制行为证据 | L1-L4 分级证据体系 + 认证工具链 | 无 | 无 | 无 |
| 语言绑定 | C ABI（任何能调 C 的语言可桥接） | Python 原生 | Rust 原生 + Python 绑定 | 脚本（Lua/Python via obs-websocket 等） |
| 容器/muxer | 无（原始 Annex-B/OBU 码流） | 无 | MP4 | 是 |

## 选型建议（大众版）

* **要在 Python 里拿 numpy 像素喂 CV/模型** → DXcam。这就是它存在的意义，且做得好。
* **要在 Rust 里捕获 + 一行 API 存 MP4** → windows-capture。硬件编码 + 容器开箱即用。
* **要嵌入 C++ 应用、帧留在 GPU 上直达编码器/另一个进程、要求可认证的复制行为证据、HDR、原生脏区语义** → FluxCap。
* **要一个现成的录屏/直播程序** → OBS。

## 性能数字

FluxCap 的单机绝对数字（吞吐/延迟/CPU 占用，含 1080p 与 4K 合成负载）见
[benchmarks-20260909-rx5060.md](benchmarks-20260909-rx5060.md)。各项目 README
中的性能声明（如 DXcam "240+ fps on 1080p"、windows-capture "240 FPS"）
测试条件各异，**不能直接横比**；跨库同机同内容对比尚未进行，方法论如下：

### 同机对比方法论（待执行）

1. 同一台机器、同一显示器、同一动态内容源（如相同动画窗口）。
2. 相同输出契约：同为 CPU 像素时比"捕获→CPU 可读"；同为 GPU 编码时比
   "捕获→编码 packet"。
3. 报告 P50/P95/P99 延迟、吞吐、进程 CPU 核数、丢帧/覆盖数。
4. 每个库使用其 README 推荐的默认配置与最优配置各跑一轮。

## 许可证

FluxCap（MIT）、DXcam（MIT）、windows-capture（MIT）。
