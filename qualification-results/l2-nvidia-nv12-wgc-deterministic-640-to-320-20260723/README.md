# Runtime L2 evidence — NVIDIA / WGC / NV12 / 640→320

This directory preserves one real, same-process qualification workload from
2026-07-23. The deterministic plane-RTV backend wrote a 320×320 NV12 bus from
a 640×640 WGC source ROI and submitted those bus textures directly to the
selected NVIDIA H.264 hardware MFT.

Measured counters:

- 31 external encoder submissions;
- 31 exact texture/subresource identity verifications;
- 31 `D3D11_BIND_VIDEO_ENCODER` submissions;
- 0 encoder copied submissions;
- 0 producer ingress copy submissions and 31 required planar transforms.

This proves runtime L2 only. No WPR ETL was captured in an elevated session,
no GPU Hardware Queue review was performed, and no L3 claim is made. MFT,
driver-private surfaces, hardware DMA, and caches remain unobservable.

`manifest.json` binds the preserved executable, runtime JSON, and stdout by
SHA-256. Requalifying another driver, adapter, format, size, backend, or MFT
requires a new artifact set.
