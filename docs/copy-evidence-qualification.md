# GPU copy evidence and qualification

FluxCap reports copy evidence as a level, not as a universal “zero-copy”
boolean. Every result is scoped to one adapter LUID, driver, OS build, capture
backend and output, rotation, format, geometry, transform backend, codec,
encoder MFT, and observation window. A passing row cannot be transferred to a
different tuple by inference.

| Level | Required evidence | What it does not prove |
|---|---|---|
| L1 | FluxCap uses external encoder submission and reports no explicit encoder-input `Copy*` in that path. | Capture ingress and format conversion may still contain a required copy/transform. The MFT or driver may allocate/copy privately. |
| L2 | L1 plus exact `IMFDXGIBuffer` texture/subresource identity immediately before `ProcessInput`; every external input carries `D3D11_BIND_VIDEO_ENCODER`; capture ingress, source/desktop presentation, bus publish, and encoder-copy counters satisfy the lifetime gates below. | Private MFT surfaces, driver layout conversion, DMA, and caches remain unobservable. |
| L3 | L2 plus one hash-bound WPR/WPA artifact set whose reviewed workload window contains no target-process full-frame D3D11 `CopyResource`/`CopySubresourceRegion` and no attributable full-frame Copy-engine packet. | Absence of an ETW event is not proof that private firmware/driver work did not occur. |
| L4 | A vendor tool or vendor/D3D12 encoder qualification adds documented visibility beyond L3 for the exact tested tuple. | It proves only what that tool and qualification explicitly observe. Driver-private surfaces, hardware DMA, or caches may still remain opaque. |

Runtime API fields stop at L2:

- `GpuEncoderSupport::runtime_copy_evidence_capability` describes what a
  successful external submission could prove on the current device.
- `GpuEncoderStats::external_submission_copy_evidence` is the highest level
  proven for every successful external submission in that encoder instance.
- `external_identity_verified_submissions` and
  `external_video_encoder_bound_submissions` make mixed runs auditable.
- `GpuEncoderSupport::mft_internal_copy_observable` remains `false` by design,
  including after an offline L3 or L4 qualification. The public runtime API
  cannot observe the private MFT boundary.

L3 and L4 are hash-bound offline results. They never promote
`mft_internal_copy_observable`, `driverPrivateSurfaceCopyObservable`, or
`hardwareDmaCopyObservable` to `true`.

Damage correctness is a separate contract. The deterministic plane-RTV backend
uses the same integer taps as the CPU damage mapper for scaled NV12/P010.
VideoProcessor scaling is driver-defined, so scaled VP frames publish full
damage. Synchronous Desktop Duplication native moves may publish one exact
replayable interior plus at most four aligned dirty strips when the scaled
displacement is integral and chroma-even. Fractional/odd displacement and WGC
asynchronous inferred moves remain dirty/full/fail-closed. A copy-evidence
level does not upgrade damage precision.

## Machine-readable contracts

The current contract versions are:

| Artifact | Schema version | Binding role |
|---|---:|---|
| Workload runtime evidence | 2 | Exact PID/executable, runner nonce, adapter, normalized qualification tuple and tuple hash, lifetime counters, selected MFT, and private-observation boundary. |
| WPR capture manifest | 3 | Pre-bound expected workload, nonce, spawned PID/lifetime, executable and artifact hashes, OS/GPU inventory, profiles, ETL, and export-manifest reference. |
| WPA export manifest | 1 | ETL, WPA profile, exporter executable, and exported Generic Events CSV hashes. |
| Analyzer qualification result | 2 | Screening, runtime-L2, manual-review, hash-binding, and final L3 status. |

The repository does not currently contain a normative `*.schema.json` file.
The workload serializer and `Test-FluxCapL3Trace.ps1` strict validators are the
normative implementation; the JSON examples are illustrative and must never
be copied into a qualification run. The current runtime shape is shown in
[`runtime-l2-evidence.example.json`](../tools/qualification/runtime-l2-evidence.example.json).

Runtime schema v2 binds these capture distinctions:

- WGC uses `captureTarget: "window"`, `rotation: "not_applicable"`, and
  `outputIdentity: "not_applicable"`. Supplying `--output-index` with WGC is
  rejected instead of being silently ignored.
- Desktop Duplication uses `captureTarget: "monitor"`, a physical rotation
  (`identity`, `rotate90`, `rotate180`, or `rotate270`), and an output identity
  containing output index, device name, desktop rectangle, and logical monitor
  size. DD qualification requires an explicit `--output-index`.
- WGC and DD are independent workloads. A WGC row never qualifies DD, and one
  DD output or rotation never qualifies another.
- NV12 and P010 evidence is explicitly
  `YCBCR_STUDIO_G22_LEFT_P709` with `hdr10StaticMetadata: false`. A P010 row in
  this contract is SDR BT.709 evidence, not an HDR, mastering-metadata,
  MaxCLL/MaxFALL, or bitstream-VUI claim.
- The normalized tuple hash includes adapter LUID, capture backend/target,
  rotation, output identity, transform backend, format/color space,
  capture-surface and ROI geometry, output geometry, codec, media subtypes, and
  encoder MFT CLSID.

The runner owns the 32-hex qualification nonce and injects it into the spawned
workload. Supplying `--qualification-run-nonce` manually is rejected. For DD,
the workload also re-enumerates the selected output at shutdown; a change in
adapter LUID, output identity, geometry, or rotation invalidates the evidence.
The workload must establish per-monitor-v2 DPI awareness before creating or
measuring its source window; evidence mode fails closed if that process context
cannot be established. This keeps the capture surface, logical source, ROI,
and DD output geometry in one explicit coordinate contract.

## L3 capture

Run capture from an elevated PowerShell session with a complete Windows
Performance Toolkit installation. The runner records the spawned workload PID
and lifetime, executable/profile/ETL/stdout/stderr hashes, OS/GPU/driver
inventory, provider configuration, and the actual WPAExporter version. It
preserves the exact WPR/WPA profiles and analyzer beside the ETL.

`fluxcap_gpu_wgc_bus_bench --runtime-evidence-json` supports one explicit,
in-process planar tuple per trace:

- `--bus-format nv12|p010`;
- `--codec h264|hevc|av1`;
- `--capture-backend wgc|desktop-duplication`;
- `--planar-backend deterministic-planar|video-processor`;
- `--size 320|640`, `--source-size 320|640`, and `--pairs 1`.

Evidence mode requires `--planar-backend` to be written explicitly; an
implicit default or `automatic` is rejected so the tuple cannot be relabeled
after capability selection.

The exact adapter/driver must support the chosen bus flags, plane views, and
D3D11-aware hardware encoder MFT. A failed resource or encoder capability probe
is an unsupported observation, not an L2 row.

For a scaled WGC/NV12/H.264 deterministic workload:

```powershell
$runner = "D:\截图库\tools\qualification\Invoke-FluxCapL3Qualification.ps1"
$workload = "D:\截图库\build\Release\fluxcap_gpu_wgc_bus_bench.exe"
$runtime = Join-Path $env:TEMP `
  ("fluxcap-runtime-l2-" + [guid]::NewGuid().ToString("N") + ".json")
& $runner `
  -Executable $workload `
  -ArgumentList @(
    "--size", "320",
    "--source-size", "640",
    "--duration-ms", "5000",
    "--warmup-ms", "1000",
    "--pairs", "1",
    "--consumer-mode", "inproc",
    "--bus-format", "nv12",
    "--codec", "h264",
    "--capture-backend", "wgc",
    "--planar-backend", "deterministic-planar",
    "--adapter-index", "0",
    "--runtime-evidence-json", $runtime) `
  -RuntimeEvidenceJson $runtime `
  -ExpectedAdapterLuid "0x00000000:0x00000000" `
  -ExpectedRotation "not_applicable" `
  -OutputDirectory "D:\截图库\qualification-results"
```

Obtain the current adapter index/LUID and attached-output list with
`--list-adapters`; do not reuse an index or LUID from an earlier boot. For an
identity DD workload, use a fresh runtime path and a separately pre-bound run:

```powershell
& $runner `
  -Executable $workload `
  -ArgumentList @(
    "--size", "320",
    "--source-size", "640",
    "--duration-ms", "5000",
    "--warmup-ms", "1000",
    "--pairs", "1",
    "--consumer-mode", "inproc",
    "--bus-format", "nv12",
    "--codec", "h264",
    "--capture-backend", "desktop-duplication",
    "--planar-backend", "deterministic-planar",
    "--adapter-index", "0",
    "--output-index", "0",
    "--runtime-evidence-json", $runtime) `
  -RuntimeEvidenceJson $runtime `
  -ExpectedAdapterLuid "0x00000000:0x00000000" `
  -ExpectedRotation "identity"
```

Use the actual `rotate90`, `rotate180`, or `rotate270` value for a physically
rotated output. Non-identity DD evidence must use `video-processor`; the
workload rejects a rotated deterministic-planar qualification. Each output and
rotation needs a separate trace.

The runtime path must not exist before the run. The runner copies it only after
the exact spawned PID exits, preventing a stale file from being rebound to a
recycled PID. The schema-v3 capture manifest pre-binds adapter, backend,
capture target, DD output index, expected rotation, transform backend, format,
codec, and geometry before WPR starts.

Current L2 acceptance requires all of the following over the entire
capture/encoder instance lifetime, including warmup:

- schema v2, evidence level 2, exact PID/executable hash/runner nonce, canonical
  LUID, and a valid normalized tuple SHA-256;
- runtime tuple equals the manifest's pre-bound expected workload; DD output
  identity begins with the pre-bound output index;
- external submissions are nonzero and
  `direct == identity-verified == VIDEO_ENCODER-bound == external`;
- encoder copied submissions, producer ingress copies, and bus copied
  publishes are all zero;
- producer ingress transforms and bus publishes are nonzero, with every bus
  publish direct;
- WGC reports `desktopPresentFrames == measuredDesktopPresentFrames == 0` and
  `measuredSourcePresentations > 0`; DD reports all three counters nonzero and
  `measuredDesktopPresentFrames <= desktopPresentFrames <= busPublishedFrames`;
- packet codec mismatches are zero and input/output media subtypes match the
  requested format/codec;
- MFT CLSID is non-null, its friendly/combined identity is consistent, and all
  private copy-observability fields remain `false`.

Do not hand-edit runtime JSON into evidence. The workload must serialize its
own final `GpuEncoderStats`, capture, bus, and pipeline counters.

The runner exports a Generic Events CSV for candidate discovery. Automated
screening cannot determine whether a `CopySubresourceRegion` is full-frame and
cannot reliably attribute every kernel packet/context. A string match is never
proof of “no Copy-engine work”. Open the ETL in WPA and review the entire
spawned-process lifetime, including initialization and warmup:

1. **Generic Events**: filter the manifest PID and
   `Microsoft-Windows-Direct3D11`. Inspect every `CopyResource`,
   `CopySubresourceRegion`, `ResourceCopy`, or `ResourceCopyRegion` candidate,
   including resource, subresource, box, and dimensions. Confirm that no
   prohibited full-frame copy occurred.
2. **GPU Hardware Queue**: correlate the target process, D3D context, adapter,
   and engine. Confirm that no attributable full-frame packet ran on a Copy
   engine. Do not classify the two plane-RTV graphics draws as copies.
3. **Media Foundation**: confirm the same interval contains encoder input and
   output activity and is not an empty or truncated trace.
4. **Trace health**: inspect lost-event/trace diagnostics and reject any run
   with event loss in the reviewed interval.
5. **Runtime L2**: verify that the hash-bound runtime JSON belongs to this PID
   and every external submission satisfies the L2 gates above.

The current animated source and capture/bus/encoder workload run in the same
PID. That is sufficient for runtime L2, but it is not a clean attribution
boundary for strict DD L3: source-window rendering events can appear under the
same target PID and false-fail the D3D11/queue review. Before claiming DD L3,
move the animation source to a child PID, bind both PIDs in the capture
manifest, and make the analyzer explicitly exclude the source PID while still
requiring capture/encoder activity from the workload PID.

After review, finalize the JSON:

```powershell
& "<run>\trace-analyzer.ps1" `
  -Manifest "<run>\capture-manifest.json" `
  -GenericEventsCsv "<run>\wpa-export\fluxcap_FluxCapCopyEvents.csv" `
  -RuntimeEvidenceJson "<run>\runtime-l2-evidence.json" `
  -Output "<run>\qualification.json" `
  -ConfirmReviewedWorkloadInterval `
  -ConfirmNoFullFrameD3D11Copies `
  -ConfirmNoCopyEnginePackets `
  -ConfirmMediaFoundationActivity `
  -ConfirmNoLostEvents `
  -Reviewer "name-or-CI-attestation-id"
```

The analyzer uses four terminal states:

- `failed-screening`: manifest hashes, export binding, time range, provider
  coverage, trace health, or workload exit status failed;
- `insufficient-runtime-l2-evidence`: ETW screening passed but the exact-run
  runtime JSON is absent or invalid;
- `needs-manual-wpa-review`: automated screening and L2 passed, but the manual
  WPA attestations are incomplete;
- `passed`: all gates passed. Only `status: "passed"` with
  `evidenceLevel: 3` is an L3 result.

The result binds the manifest, ETL, Generic Events CSV, runtime evidence,
profiles, and exporter by SHA-256. It continues to state that private
MFT/driver surfaces, hardware DMA, and caches are unobservable.

`wpaexporter.exe -help` is not a qualification test. The currently inspected
machine prints the 11.7.395.48728 help text and exits `-1` because
`wpaexporter.deps.json` is missing; no alternate complete installation was
found. The current session is also not elevated. No successful administrator
ETL/export/manual review has been produced on this machine, so no L3 claim is
present.

## Current hardware matrix

The repository index is
[`qualification-results/hardware-matrix-20260723.json`](../qualification-results/hardware-matrix-20260723.json).
It now separates passing runtime observations, unsupported attempts, and
pending external coverage.

The current-contract schema-v2 runtime-L2 set is:

| Vendor | Backend / format / codec | Geometry | Artifact state |
|---|---|---|---|
| NVIDIA RTX 5060 Laptop | WGC / NV12 / H.264 | 1284×767 surface, ROI 322,63,640,640 → 320×320 | Nonce- and tuple-hash-bound lifetime L2. |
| NVIDIA RTX 5060 Laptop | WGC / NV12 / HEVC | 1284×767 surface, ROI 322,63,640,640 → 320×320 | Nonce- and tuple-hash-bound lifetime L2. |
| NVIDIA RTX 5060 Laptop | WGC / NV12 / AV1 | 1284×767 surface, ROI 322,63,640,640 → 320×320 | Nonce- and tuple-hash-bound lifetime L2. |
| AMD Radeon 610M | WGC / NV12 / H.264 | 1284×767 surface, ROI 322,63,640,640 → 320×320 | Nonce- and tuple-hash-bound lifetime L2. |
| AMD Radeon 610M | WGC / NV12 / HEVC | 1284×767 surface, ROI 322,63,640,640 → 320×320 | Nonce- and tuple-hash-bound lifetime L2. |
| NVIDIA RTX 5060 Laptop | DD output 0 identity / NV12 / H.264 | 2560×1600 surface, ROI 960,480,640,640 → 320×320 | Output-identity-, nonce-, and tuple-hash-bound lifetime L2. |
| NVIDIA RTX 5060 Laptop | DD output 0 identity / NV12 / HEVC | 2560×1600 surface, ROI 960,480,640,640 → 320×320 | Output-identity-, nonce-, and tuple-hash-bound lifetime L2. |
| NVIDIA RTX 5060 Laptop | DD output 0 identity / NV12 / AV1 | 2560×1600 surface, ROI 960,480,640,640 → 320×320 | Output-identity-, nonce-, and tuple-hash-bound lifetime L2. |

The matrix also retains five older observations for audit history: preserved
schema-v1 NVIDIA/AMD WGC NV12/H.264 bundles, and pre-hardening schema-v2
NVIDIA NV12/HEVC, NVIDIA NV12/AV1, and AMD NV12/HEVC standalone smokes. They
remain honest runtime-L2 observations under their recorded contract, but they
cannot be used as current schema-v3 L3 inputs or upgraded in place.

Known unsupported attempts are recorded separately, never as passing rows:

- NVIDIA and AMD both returned `E_INVALIDARG` (`0x80070057`) while creating the
  exact P010 bus resource with
  `D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER`; the requested evidence
  files remained empty. This is exact-path evidence, not a universal claim that
  every P010 mechanism on those GPUs is impossible.
- AMD exposed no eligible D3D11-aware hardware AV1 encoder MFT for the tested
  NV12 tuple; no L2 file was produced.

The remaining external qualification matrix is still open:

- Intel hardware has not been tested.
- WGC and DD remain independent workloads. NVIDIA output 0 identity now has
  dedicated DD H.264/HEVC/AV1 L2 rows; no AMD DD row exists because the current
  AMD adapter has no attached output.
- No physical DD runs exist for 90°, 180°, or 270° rotation. Synthetic tests
  are not substitutes for this matrix.
- P010/HEVC/AV1 needs a hardware/driver tuple that can create the exact bus
  resource and expose the requested D3D11-aware encoder.
- Administrator WPR/WPA L3 capture and manual review are pending; strict DD L3
  additionally needs child-PID source isolation and analyzer exclusion to avoid
  same-PID attribution false failures.
- Intel, NVIDIA, and AMD vendor/D3D12 L4 artifacts are pending.

Qualify separate rows for every adapter/driver, WGC or DD output/rotation,
NV12/P010 format, 1:1 or scaled geometry, 320/640 ROI, H.264/HEVC/AV1 codec,
transform backend, and MFT. Record source SRV and plane-RTV results, exact bus
flags, runtime/ETL/export/qualification hashes, reviewer identity, and the
additional boundary observed by any L4 tool.

A missing vendor, backend, rotation, format, or codec row means “not
qualified”, not an implicit pass. The schema-v2 matrix template is
[`hardware-matrix.example.json`](../tools/qualification/hardware-matrix.example.json).

## Resilience/extension feature evidence status (2026-08-31)

The Desktop Duplication lifecycle work (worker-side ACCESS_LOST rebuild,
idle heartbeat republish, `IDXGIOutput6` color auto-detection, format
negotiation fallback, session-limit retry, WTS/power session events,
`ProtectedContentMaskedOut` counting), the GPU cursor compositor
(`include_cursor = true`), `DesktopDuplicationController`, and
`GdiMonitorCapture` are covered by the runtime smoke assertions in
`fluxcap_gpu_capture_tests` plus the pure policy unit tests
(`fluxcap_desktop_duplication_policy_tests`).

`fluxcap_gpu_dd_resilience_bench --runtime-evidence-json` (driver script:
`tools/qualification/Invoke-FluxCapDdResilienceL2.ps1`) produces
runtime-L2 bus-publish copy evidence for four traces: `dd-identity`,
`dd-baked-cursor`, `dd-controller`, and `gdi`. Gates: non-zero publishes,
strict consumer sequence monotonicity, valid damage on every consumed
frame, >=30 measured consumer frames, final drain at/after the last
observed sequence, worker liveness, and per-trace copy contracts
(identity `copies == 2 x published` with heartbeat capture enabled; baked
cursor `copies == 3 x published`; controller `copies >= published`; GDI
`copies == published` with full-frame damage). Passing rows exist for the
NVIDIA adapter only (`qualification-results/hardware-matrix-20260831.json`,
artifact directories `l2-nvidia-*-20260831`); AMD/Intel rows are missing
and follow the "missing row = not qualified" rule.

Note that GPU cursor compositing intentionally adds one full-surface copy
per frame, and the idle heartbeat adds one bus-slot copy per real publish
plus one per republish; all of them are counted in
`ingress_copy_submissions`/`idle_republished_frames` and must be included
when reasoning about the zero-redundant-copy contract. Idle heartbeat and
session-rebuild counters are observations in this evidence, not forced
gates, because system desktop activity is not controlled by the workload.
