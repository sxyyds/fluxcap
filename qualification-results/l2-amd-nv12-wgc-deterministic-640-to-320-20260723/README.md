# Runtime L2 evidence — AMD / WGC / NV12 / 640→320

This directory preserves one real AMD Radeon 610M qualification workload.
The deterministic plane-RTV backend wrote a 320×320 NV12 bus from a 640×640
WGC source ROI and submitted those bus textures directly to `AMDh264Encoder`.

Measured counters:

- 59 external encoder submissions;
- 59 exact texture/subresource identity verifications;
- 59 `D3D11_BIND_VIDEO_ENCODER` submissions;
- 0 encoder copied submissions;
- 0 producer ingress copy submissions and 59 required planar transforms.

This is runtime L2 only. It has no elevated WPR ETL or manual GPU Hardware
Queue review and therefore makes no L3 claim. Private MFT/driver surfaces,
hardware DMA, and caches remain unobservable.
