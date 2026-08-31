# Runtime L2 evidence 鈥?nvidia / gdi-320

Preserved from a real `gdi` trace of
`fluxcap_gpu_dd_resilience_bench` on 20260831 (exit code 0, gates
`PASS`).

This is runtime L2 bus-publish copy evidence only: no encoder is involved,
no WPR ETL was captured, and no L3 claim is made. Driver-private surfaces,
hardware DMA, and caches remain unobservable. Idle heartbeat and session
rebuild counters are observations, not forced gates, because system desktop
activity is not controlled by the workload.

`manifest.json` binds the preserved executable, runtime JSON, and stdout
by SHA-256. Requalifying another adapter, format, ROI, or option set
requires a new artifact set.
