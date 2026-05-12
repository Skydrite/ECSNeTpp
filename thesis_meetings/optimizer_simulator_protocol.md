# Optimizer ↔ Simulator Communication Protocol

**Project:** SLAG — Geo-Distributed Streaming Aggregations based on Client Latency SLAs  
**Document scope:** Message protocol between the Java Optimizer (Windows) and the
ECSNeTpp Simulator (WSL/OMNeT++). Covers message types, structure, and direction for
both the reports (Simulator → Optimizer) and the action commands (Optimizer → Simulator).  
**Status:** Active — implement RE_ROUTE, SPLIT_LOAD, HOLD. FULL_REPLAN is future work only.

---

## Format Decision

**Hybrid format — chosen for simplicity:**

| Direction | Format | Rationale |
|---|---|---|
| Simulator → Optimizer | Newline-delimited JSON | Reports contain nested path data (hops); JSON handles this cleanly. Java parses JSON trivially. |
| Optimizer → Simulator | Plain space-delimited text | Commands are flat (3–4 fields max); no library needed on either side. |

All messages (both directions) are terminated with `\n`.

---

## Architecture Overview

```
Java Optimizer (Windows)              ECSNeTpp Simulator (WSL / OMNeT++)
─────────────────────────             ──────────────────────────────────
                                          ┌─────────────────────────┐
                              TCP 9998    │  Report stream thread    │
  connect() ───────────────────────────►  │  (POSIX blocking accept) │
  readLine() ◄─── JSON reports ────────── │  stores reportClientFd   │
                                          └─────────────────────────┘
                                                       ▲ emitToClient()
                                          ┌────────────┴────────────┐
                              TCP 9999    │  OMNeT++ sim thread      │
  [Text command] ──────────────────────►  │  pollMsg self-message    │
  [Text ACK]     ◄──────────────────────  │  dispatches command      │
                                          │  sends ACK               │
                                          │                          │
                                          │  snapshotMsg (10s)  ─────┼──► GLOBAL_SNAPSHOT
                                          │  sink window close  ─────┼──► TRIGGER_REPORT
                                          └─────────────────────────┘
                                          ┌─────────────────────────┐
                              TCP 9999    │  Command listener thread │
  [Text command] ──────────────────────►  │  (POSIX blocking accept) │
  [Text ACK]     ◄──────────────────────  │  sets cmdLine, wakes sim │
                                          └─────────────────────────┘
```

**Port summary:**
| Port | Direction | Content |
|------|-----------|---------|
| 9998 | Simulator → Optimizer | Newline-delimited JSON reports (TRIGGER_REPORT, GLOBAL_SNAPSHOT). Optimizer connects once, reads indefinitely. |
| 9999 | Optimizer → Simulator | One plain-text command per connection. Simulator echoes ACK, closes. |

### Threading model

The Simulator has three concurrent threads: the OMNeT++ **simulation thread**, the
**TCP report stream thread** (port 9998, push), and the **TCP command listener thread**
(port 9999, receive). Both background threads are detached POSIX threads started during
`initialize()`.

**TCP listener thread** (`tcpServerLoop`)
- Blocks on `accept()` waiting for the Optimizer to connect.
- Reads exactly one `\n`-terminated command line from the accepted socket.
- Acquires `cmdMutex`, deposits `cmdLine` and `cmdClientFd`, sets `cmdPending = true`.
- Wakes the sim thread by sending a `pollMsg` self-message via `scheduleAt`.
- Blocks on `cmdCv.wait()` until the sim thread signals it after the ACK is sent.
- Closes the client socket, loops back to `accept()`.

**Simulation thread** (`handleMessage` on `pollMsg`)
- Picks up `cmdLine` and `cmdClientFd` under the mutex.
- Calls `dispatchCommand()` which executes the placement action (SPLIT_LOAD / RE_ROUTE / HOLD).
- Sends the plain-text ACK via `::send(cmdClientFd, ...)`.
- Closes `cmdClientFd`, notifies `cmdCv` to release the TCP thread.

This handoff ensures all OMNeT++ state (routing tables, module parameters, queue depths)
is only touched on the simulation thread — which is required for correct discrete-event
semantics. The TCP thread never touches OMNeT++ state directly.

**TCP report stream thread** (`reportServerLoop`)
- Blocks on `accept()` on port 9998. When the Optimizer connects, stores the fd as
  `reportClientFd` (protected by `reportClientMutex`). Loops back to `accept()` to
  support reconnection.
- Never reads from the client — this is a push-only channel.

**JSON reports** are emitted on the simulation thread via `emitToClient()`:
- `TRIGGER_REPORT` — emitted via `sink->reportEmitter(json)` at the end of each 5-second
  measurement window in `StreamingSink::handleMessage`. The `reportEmitter` lambda is
  wired to `ECSBuilder::emitToClient()` during `executeAllocationPlan()`.
- `GLOBAL_SNAPSHOT` — emitted via `emitToClient()` inside `ECSBuilder::emitGlobalSnapshot()`,
  triggered by a recurring `snapshotMsg` self-message every 10 sim-seconds.

`emitToClient()` always writes to stdout (terminal visibility) and, if `reportClientFd >= 0`,
also pushes the same line over TCP. A broken pipe on send clears `reportClientFd`.

### Connection lifecycle

Each TCP connection carries exactly one command:

```
Optimizer                              Simulator (TCP thread)
─────────                              ──────────────────────
connect() ───────────────────────────► accept()
send("RE_ROUTE ...\n") ──────────────► read line
                                       → hand off to sim thread via mutex
                                       ← sim thread executes + sends ACK
recv("OK RE_ROUTE inflateOp 31.0\n") ◄─ ::send(ack)
close() ─────────────────────────────► close(clientFd)
```

Do not reuse a connection for a second command. Open a fresh TCP connection per command.

### WSL2 ↔ Windows port forwarding

WSL2 automatically forwards TCP ports bound on `0.0.0.0` inside WSL to the Windows host.
The Optimizer can reach the Simulator at `localhost:9999` from any Windows process without
any additional configuration (no `netsh` rules, no firewall exceptions needed). This is
the default behaviour on Windows 11 and recent Windows 10 builds.

If the Optimizer and Simulator run on the same Windows machine, the address to use is
`"127.0.0.1"`, port `9999`.

---

## Direction 1 — Simulator → Optimizer: JSON Reports (stdout)

All report messages are written to stdout as a single line of JSON terminated with `\n`.
Two message types: `TRIGGER_REPORT` and `GLOBAL_SNAPSHOT`.

---

### TRIGGER_REPORT

Emitted by each sink at the end of every measurement window (every 5 sim-seconds).
Always emitted regardless of SLA status — the Optimizer uses `trigger_reason` per path
to decide whether to act.

The report is **multi-path**: one sink window may observe several distinct routes
(e.g. `so1→inflateOp@transitNodes[0]→si` and `so2→inflateOp@cloudNodes[1]→si`).
Each route gets its own path object with independent P99, trend, and hop breakdown.

```json
{
  "type":           "TRIGGER_REPORT",
  "window_start_s": 1.0,
  "window_end_s":   6.0,
  "sink":           "si",
  "node":           "SimpleEdgeCloudEnvironment.cloudNodes[0]",
  "l_target_s":     2.000,
  "paths": [
    {
      "path_key":        "so1@pi3Bs[0]:inflateOp@transitNodes[0]:si@cloudNodes[0]",
      "events":          80,
      "p99_current_s":   2.295,
      "e2e_mean_s":      1.232,
      "p99_trend":       "STABLE",
      "budget_used_pct": 114.8,
      "trigger_reason":  "BREACH",
      "hops": [
        {
          "hop":           0,
          "to_op":         "inflateOp",
          "to_device":     "transitNodes[0]",
          "link_ms":       9.583,
          "processing_ms": 0.749,
          "cumulative_ms": 10.332,
          "queue_depth":   0,
          "bottleneck":    "OK"
        },
        {
          "hop":           1,
          "to_op":         "si",
          "to_device":     "cloudNodes[0]",
          "link_ms":       1171.893,
          "processing_ms": 0.163,
          "cumulative_ms": 1182.388,
          "queue_depth":   0,
          "bottleneck":    "NETWORK"
        }
      ]
    },
    {
      "path_key":        "so2@pi3Bs[1]:inflateOp@transitNodes[0]:si@cloudNodes[0]",
      "events":          76,
      "p99_current_s":   1.845,
      "e2e_mean_s":      1.103,
      "p99_trend":       "RISING",
      "budget_used_pct": 92.3,
      "trigger_reason":  "WARN",
      "hops": [
        {
          "hop":           0,
          "to_op":         "inflateOp",
          "to_device":     "transitNodes[0]",
          "link_ms":       9.102,
          "processing_ms": 0.749,
          "cumulative_ms": 9.851,
          "queue_depth":   0,
          "bottleneck":    "OK"
        },
        {
          "hop":           1,
          "to_op":         "si",
          "to_device":     "cloudNodes[0]",
          "link_ms":       982.400,
          "processing_ms": 0.163,
          "cumulative_ms": 992.414,
          "queue_depth":   0,
          "bottleneck":    "NETWORK"
        }
      ]
    }
  ]
}
```

#### Top-level field definitions

| Field | Type | Description |
|---|---|---|
| `type` | string | Always `"TRIGGER_REPORT"` |
| `window_start_s` | float | Simulation time at start of measurement window |
| `window_end_s` | float | Simulation time at end of measurement window |
| `sink` | string | Logical sink name (e.g. `"si"`) |
| `node` | string | Full OMNeT++ module path of the sink node |
| `l_target_s` | float | SLA latency target in seconds |
| `paths` | array | One object per distinct observed path through the network this window |

#### Path object field definitions

| Field | Type | Description |
|---|---|---|
| `path_key` | string | Full route identifier: `"sourceLabel:hop0Label:hop1Label:..."` where each label is `"taskName@deviceName[idx]"`. Source of truth for the complete physical route. |
| `events` | int | Event count on this path in this window |
| `p99_current_s` | float | P99 E2E latency for this path this window |
| `e2e_mean_s` | float | Mean E2E latency for this path this window |
| `p99_trend` | string | `"RISING"` / `"STABLE"` / `"FALLING"` — vs previous window (±5% threshold) |
| `budget_used_pct` | float | `(p99_current_s / l_target_s) × 100` |
| `trigger_reason` | string | `"OK"` / `"WARN"` (≥ 95% budget) / `"BREACH"` (> 100% budget) |
| `hops` | array | Per-hop breakdown along this path |

#### Hop field definitions

| Field | Type | Description |
|---|---|---|
| `hop` | int | Zero-indexed hop number |
| `to_op` | string | Logical operator/sink name at the destination of this hop |
| `to_device` | string | Physical device hosting `to_op` (e.g. `"transitNodes[0]"`) |
| `link_ms` | float | Mean one-way link delay arriving at `to_op` in milliseconds |
| `processing_ms` | float | Mean processing delay at `to_op` in milliseconds |
| `cumulative_ms` | float | Running sum of all `link_ms + processing_ms` up to and including this hop |
| `queue_depth` | int | Mean CPUCore queue depth at `to_op` over this window |
| `bottleneck` | string | `"OK"` / `"NETWORK"` / `"COMPUTE"` / `"BOTH"` |

**Note:** `from_op`/`from_device` are intentionally omitted from hops — the full route
is already encoded in `path_key`. The Optimizer can parse `path_key` to reconstruct the
complete source→op→sink chain when needed.

#### Bottleneck classification (implemented in Simulator)

```
fair_share_ms = (l_target_s * 1000) / num_hops

network_flag = link_ms     > fair_share_ms * 0.5
compute_flag = queue_depth >= 5

if network_flag AND compute_flag → "BOTH"
elif network_flag                → "NETWORK"
elif compute_flag                → "COMPUTE"
else                             → "OK"
```

---

### GLOBAL_SNAPSHOT

Emitted every N sim-seconds (configurable, default 10s) regardless of SLA status.
Gives the Optimizer a complete picture of all device load and link characteristics
so it can score candidate destinations for RE_ROUTE and SPLIT_LOAD.

```json
{
  "type":       "GLOBAL_SNAPSHOT",
  "sim_time_s": 23.4,
  "devices": [
    {
      "device":                "transitNodes[0]",
      "cores_total":           4,
      "queue_depth_total":     89,
      "link_delay_ms":         10.5,
      "observed_link_delay_ms": 16.2,
      "link_cost_per_gb":      0.08
    },
    {
      "device":                "transitNodes[1]",
      "cores_total":           4,
      "queue_depth_total":     2,
      "link_delay_ms":         10.4,
      "observed_link_delay_ms": 10.8,
      "link_cost_per_gb":      0.08
    },
    {
      "device":                "cloudNodes[0]",
      "cores_total":           8,
      "queue_depth_total":     0,
      "link_delay_ms":         8.6,
      "observed_link_delay_ms": 8.6,
      "link_cost_per_gb":      0.20
    }
  ]
}
```

#### Device field definitions

| Field | Type | Description |
|---|---|---|
| `device` | string | OMNeT++ array name + index (e.g. `"transitNodes[0]"`) |
| `cores_total` | int | Number of CPUCore submodules on this node (from ini config) |
| `queue_depth_total` | int | Sum of all CPUCore queue depths on this node at snapshot time. Use as saturation indicator: 0 = idle, high = overloaded. |
| `link_delay_ms` | float | WAN delay from the shared edge router to this node (static, from ini config) |
| `observed_link_delay_ms` | float | Average actual link delay to this node measured from live traffic in the most recent sink window. Includes queuing and transmission effects not captured by the static value. **Use this for RE_ROUTE candidate scoring — it is more accurate than `link_delay_ms`.** Falls back to `link_delay_ms` if no traffic has passed through yet. |
| `link_cost_per_gb` | float | Egress cost in USD/GB (static: transit = 0.08, cloud = 0.20, edge = 0.00) |

**Note:** `cores_used` is intentionally omitted. In OMNeT++'s discrete-event model,
`queue_depth_total` is the meaningful saturation metric — it directly reflects how many
events are waiting to be processed. High queue depth = compute bottleneck.

---

## Direction 2 — Optimizer → Simulator: Text Commands (TCP port 9999)

Commands are plain space-delimited text, one per TCP connection, terminated with `\n`.
The Optimizer connects, sends one command line, waits for the ACK line, then closes.

### Pre-creation constraint (applies to RE_ROUTE and SPLIT_LOAD)

Both RE_ROUTE and SPLIT_LOAD require a **dormant replica** to already exist on the
target device. Replicas are pre-deployed at simulation startup by `ECSBuilder` when
it generates the extended placement XML. The set of valid `to_device` candidates is
therefore bounded by where replicas were placed at startup — it cannot be extended
at runtime.

If the Optimizer sends a command targeting a device with no pre-deployed replica, the
Simulator returns an ERROR ACK (`ERROR RE_ROUTE inflateOp no replica pre-deployed on transitNodes[1]`).
The Optimizer's device scoring logic must filter candidates against the known replica
locations before sending any command — an empty candidate list after filtering means
no action is possible and a HOLD should be sent instead.

The Optimizer learns replica locations implicitly: any device that appears in a
`path_key` as a hop destination for a given operator is confirmed active; any device
not appearing in any path is either a replica candidate or unused. The definitive
replica map is established when the Simulator starts (logged to stdout during startup).

### Command types

---

#### SPLIT_LOAD

Activate a pre-created dormant replica and begin round-robin load sharing with the original.

```
SPLIT_LOAD <operator> <replica> <strategy>
```

| Token | Description |
|---|---|
| `operator` | Logical name of the original operator (e.g. `inflateOp`) |
| `replica` | Logical name of the pre-created dormant replica (e.g. `inflateOp_r2`) |
| `strategy` | Load distribution strategy. Currently only `ROUND_ROBIN` supported. |

**Example:**
```
SPLIT_LOAD inflateOp inflateOp_r2 ROUND_ROBIN
```

**Simulator behaviour:** Resolves `replica` to an IP address, adds it to the upstream
supervisor's round-robin destination list for `operator`. In-flight messages on the
original path complete normally (soft cutover). New messages are distributed evenly
across both destinations from this point forward.

---

#### RE_ROUTE

Move a logical operator from its current device to a different device by activating
the pre-created replica there and stopping traffic to the original.

```
RE_ROUTE <operator> <from_device> <to_device>
```

| Token | Description |
|---|---|
| `operator` | Logical name of the operator to move (e.g. `inflateOp`) |
| `from_device` | Current physical device in `name[index]` format (e.g. `transitNodes[0]`) |
| `to_device` | Target physical device (e.g. `transitNodes[1]`) |

**Example:**
```
RE_ROUTE inflateOp transitNodes[0] transitNodes[1]
```

**Simulator behaviour:** Activates the pre-created replica on `to_device`, updates all
upstream supervisor routing tables to exclusively use the new device, and deactivates
the original (removes it from routing). Soft cutover — in-flight messages on the old
path complete naturally. No hard queue drain.

---

#### HOLD

The Optimizer evaluated the trigger report and decided no action is warranted.

```
HOLD <reason>
```

| Token | Description |
|---|---|
| `reason` | One-word reason. Suggested values: `COMPUTE_SATURATED_NO_CAPACITY`, `TREND_SELF_CORRECTING`, `NO_BETTER_DEVICE` |

**Example:**
```
HOLD TREND_SELF_CORRECTING
```

**Simulator behaviour:** Logs the reason with current sim_time. No placement change.

---

### ACK response (Simulator → Optimizer, plain text)

Every command receives a single-line ACK before the TCP connection closes.

**Success:**
```
OK <action> <operator> <sim_time>
```

**Failure:**
```
ERROR <action> <operator> <reason...>
```

| Token | Description |
|---|---|
| `OK` / `ERROR` | Result status |
| `action` | Echoes the action keyword |
| `operator` | Echoes the operator name (use `-` for HOLD) |
| `sim_time` | Current simulation time in seconds (OK only) |
| `reason...` | Error description, may contain spaces (ERROR only) |

**Examples:**
```
OK SPLIT_LOAD inflateOp 23.4
OK RE_ROUTE inflateOp 31.0
OK HOLD - 18.0
ERROR SPLIT_LOAD inflateOp no dormant replica found for inflateOp
ERROR RE_ROUTE inflateOp no replica pre-deployed on transitNodes[1]
```

---

## Optimizer Decision Logic (Java side)

On receiving a `TRIGGER_REPORT`, iterate over each path object:

```
for each path in paths:

    if path.trigger_reason == "OK" and path.p99_trend != "RISING":
        → skip, no action needed

    if path.p99_trend == "FALLING":
        → send HOLD TREND_SELF_CORRECTING, skip

    identify the worst hop (highest link_ms or queue_depth in path.hops):
        worst_hop = hops.max_by(link_ms if NETWORK, queue_depth if COMPUTE)
        operator  = parse to_op from worst_hop
        current_device = parse to_device from worst_hop

        if worst_hop.bottleneck == "NETWORK":
            → consult latest GLOBAL_SNAPSHOT
            → rank candidate devices by observed_link_delay_ms ascending
            → filter: device must have a pre-deployed replica of operator
            → filter: wouldAddHop(candidate, sink_device) == false  // Finding 3: skip cloud nodes that place operator past the sink
            → if candidate found: send RE_ROUTE operator current_device candidate
            → else: send HOLD NO_BETTER_DEVICE

        if worst_hop.bottleneck == "COMPUTE":
            → consult latest GLOBAL_SNAPSHOT
            → rank candidate devices by queue_depth_total ascending
            → filter: device must have a pre-deployed replica of operator
            → if candidate found: send SPLIT_LOAD operator replica ROUND_ROBIN
            → else: send HOLD COMPUTE_SATURATED_NO_CAPACITY

        if worst_hop.bottleneck == "BOTH":
            → score by combined weighted objective (link_delay_ms + queue_depth_total)
            → same filtering and dispatch logic as NETWORK case above
```

**Note:** If multiple paths in one report trigger an action, process the worst one
(highest `budget_used_pct`) first. Send one command per TCP connection.

---

## Empirical Findings from Initial Runs (2026-05-09)

Four controlled runs were executed against the `MinimalNetTest` config (4 Pi3B sources,
inflateOp on transitNodes[0], si sink on cloudNodes[0], so4→si2 direct to cloudNodes[1]).
Each command was injected at ~t=25s via the test scripts in `simulations/`.

### Summary table (si path — so4→si2 is identical across all runs: 4350 events, P99 ~0.086s)

| Run | Events (si) | P99 (si) | Link Latency Mean | Link Cost (total) |
|-----|-------------|----------|-------------------|-------------------|
| Baseline | 7 215 | 66.06s | 66.64s | $0.0205 |
| SPLIT_LOAD | 10 326 | 54.03s | 55.85s | $0.0302 |
| RE_ROUTE | 5 076 | 88.98s | 90.39s | $0.0274 |
| HOLD | 7 215 | 66.06s | 34.30s | $0.0205 |

### Finding 1 — HOLD is a true no-op ✅

HOLD result is **bit-for-bit identical to Baseline**: same event counts, same P99, same link
cost. Confirmed: the Simulator correctly discards HOLD with no side effects.

### Finding 2 — SPLIT_LOAD helps compute bottlenecks but cannot overcome a network bottleneck ✅

Throughput at si increased +43% (7215 → 10326 events). P99 improved from 66s to 54s (-18%).
However, the dominant cost is WAN link delay, not CPU — even with two operator instances the
P99 remains very high. **Implication for the optimizer:** if `bottleneck == "NETWORK"` on the
hop containing the operator, prefer RE_ROUTE over SPLIT_LOAD. SPLIT_LOAD only delivers
meaningful P99 gains when the bottleneck classification is `"COMPUTE"`.

### Finding 3 — RE_ROUTE target scoring must account for the full downstream path ⚠️

The command `RE_ROUTE inflateOp transitNodes[0] cloudNodes[1]` made things **significantly
worse**: P99 rose from 66s to 89s (+35%), events processed dropped -30%.

**Root cause:** the sink `si` sits on cloudNodes[0]. After RE_ROUTE, the path became:

```
pi3B → [WAN] → cloudNodes[1] (inflateOp) → [WAN] → albacore5 → [WAN] → cloudNodes[0] (si)
```

Two inter-cloud WAN hops instead of one. Moving the operator "past" the sink in the
network topology created an extra expensive crossing.

**Rule for the optimizer's RE_ROUTE candidate scorer:**

> Do not rank a candidate device by its own `observed_link_delay_ms` alone.
> Compute the **total path cost** from the sources through the candidate operator node
> through to the sink. In a star topology, placing an operator on a node that is
> "further" from the sink than the current node will add, not remove, latency.
>
> Concretely: if the sink is on `cloudNodes[0]`, a candidate on `cloudNodes[1]`
> incurs cloudNodes[1]→albacore5→cloudNodes[0] as an additional hop. Prefer candidates
> on the same node tier as (or closer to) the sink, or candidates that are topologically
> between the sources and the sink.

A simple safe heuristic for the current star topology:
- Transit nodes are always "closer" to the sink than cloud nodes (one WAN hop from the router vs. one WAN hop out and one back).
- Only RE_ROUTE operator → cloud if the **sink is also on a cloud node on the same physical machine** (i.e. same cloudNodes index).
- Otherwise RE_ROUTE to a transit node with lower `observed_link_delay_ms` than the current one.

---

## Java Optimizer Integration Guide

This section is the definitive reference for how the Java Optimizer should interact with
the Simulator. All items below are implemented and stable on the Simulator side.

---

### Step 1 — Connect to the Simulator's report stream (port 9998)

Start the Simulator manually in WSL (or from a terminal). Once running, connect to
port 9998 from Java — the Simulator will push all `TRIGGER_REPORT` and
`GLOBAL_SNAPSHOT` messages to this connection as newline-delimited JSON.

```java
Socket reportSocket = new Socket("127.0.0.1", 9998);  // WSL2 auto-forwards to Windows
InputStream reportStream = reportSocket.getInputStream();

// Read the report stream on a dedicated thread — never blocks the command path
Thread reportReader = new Thread(() -> readSimulatorOutput(reportStream));
reportReader.setDaemon(true);
reportReader.start();
```

**Why not a process pipe?** The Simulator lives in WSL; the Optimizer in Windows.
Cross-environment `ProcessBuilder` spawning is fragile (paths, env vars, WSL lifetime
coupling). TCP on port 9998 is forwarded automatically by WSL2 — no configuration
needed — and lets both sides start and restart independently.

Reports are also always written to the Simulator's stdout, so you can observe the stream
with `nc localhost 9998` from PowerShell while debugging.

---

### Step 2 — Parse the JSON stream (line-buffered)

Every JSON message is a single line terminated with `\n`. Buffer input by line and
dispatch by the `"type"` field.

```java
void readSimulatorOutput(InputStream in) {
    try (BufferedReader reader = new BufferedReader(new InputStreamReader(in))) {
        String line;
        while ((line = reader.readLine()) != null) {
            if (line.isBlank()) continue;
            try {
                JSONObject msg = new JSONObject(line);   // org.json or Gson equivalent
                String type = msg.getString("type");
                switch (type) {
                    case "TRIGGER_REPORT"  -> handleTriggerReport(msg);
                    case "GLOBAL_SNAPSHOT" -> handleGlobalSnapshot(msg);
                    default                -> log.warn("Unknown message type: " + type);
                }
            } catch (Exception e) {
                log.warn("Failed to parse simulator line: " + line, e);
            }
        }
    }
}
```

Use any JSON library (`org.json`, Gson, Jackson). The structure matches the schemas
in the sections above exactly.

---

### Step 3 — Cache GLOBAL_SNAPSHOT

Cache the most recent `GLOBAL_SNAPSHOT` in a thread-safe map. You will consult it when
scoring candidate devices for RE_ROUTE and SPLIT_LOAD.

```java
// Thread-safe snapshot cache
private volatile Map<String, DeviceInfo> latestSnapshot = Collections.emptyMap();

void handleGlobalSnapshot(JSONObject msg) {
    Map<String, DeviceInfo> snap = new HashMap<>();
    JSONArray devices = msg.getJSONArray("devices");
    for (int i = 0; i < devices.length(); i++) {
        JSONObject d = devices.getJSONObject(i);
        DeviceInfo info = new DeviceInfo(
            d.getString("device"),
            d.getInt("cores_total"),
            d.getInt("queue_depth_total"),
            d.getDouble("link_delay_ms"),
            d.getDouble("observed_link_delay_ms"),  // use this for actual latency scoring
            d.getDouble("link_cost_per_gb")
        );
        snap.put(info.device, info);
    }
    latestSnapshot = Collections.unmodifiableMap(snap);
    log.info("GLOBAL_SNAPSHOT cached: {} devices at sim_time={}", snap.size(), msg.getDouble("sim_time_s"));
}
```

**Use `observed_link_delay_ms` (not `link_delay_ms`) for latency scoring.** The static
`link_delay_ms` reflects the ini configuration value; `observed_link_delay_ms` is derived
from actual traffic measurements and includes any queuing or transmission delay not
captured by the static parameter. When the values differ, the observed value is more
accurate.

---

### Step 4 — React to TRIGGER_REPORT

On receiving a TRIGGER_REPORT, iterate over `paths`, apply the decision logic, and
send at most one command per report (for the worst-breaching path).

```java
void handleTriggerReport(JSONObject msg) {
    JSONArray paths = msg.getJSONArray("paths");
    
    // Find worst path (highest budget_used_pct) that needs action
    JSONObject worst = null;
    double worstBudget = 0;
    for (int i = 0; i < paths.length(); i++) {
        JSONObject path = paths.getJSONObject(i);
        String reason = path.getString("trigger_reason");
        String trend  = path.getString("p99_trend");
        if (reason.equals("OK") && !trend.equals("RISING")) continue;
        if (trend.equals("FALLING")) continue;  // self-correcting
        double budget = path.getDouble("budget_used_pct");
        if (budget > worstBudget) { worstBudget = budget; worst = path; }
    }

    if (worst == null) {
        sendCommand("HOLD TREND_SELF_CORRECTING");
        return;
    }

    // Identify worst hop
    JSONObject worstHop = findWorstHop(worst.getJSONArray("hops"));
    String operator     = worstHop.getString("to_op");
    String currentDev   = worstHop.getString("to_device");
    String bottleneck   = worstHop.getString("bottleneck");

    String command = decideCommand(operator, currentDev, bottleneck);
    sendCommand(command);
}

String decideCommand(String operator, String currentDev, String bottleneck) {
    Map<String, DeviceInfo> snap = latestSnapshot;
    if (snap.isEmpty()) return "HOLD NO_SNAPSHOT_YET";

    if (bottleneck.equals("NETWORK") || bottleneck.equals("BOTH")) {
        // Find candidate with lowest observed link delay that has a replica of operator
        return snap.values().stream()
            .filter(d -> !d.device.equals(currentDev))
            .filter(d -> hasReplica(operator, d.device))          // filter against known replica map
            .filter(d -> !wouldAddHop(d.device, sinkDevice))      // Finding 3: don't place op past the sink
            .min(Comparator.comparingDouble(d -> d.observedLinkDelayMs))
            .map(d -> "RE_ROUTE " + operator + " " + currentDev + " " + d.device)
            .orElse("HOLD NO_BETTER_DEVICE");
    }

    if (bottleneck.equals("COMPUTE")) {
        // Find candidate with lowest queue depth that has a replica of operator
        return snap.values().stream()
            .filter(d -> !d.device.equals(currentDev))
            .filter(d -> hasInactiveReplica(operator, d.device))
            .min(Comparator.comparingInt(d -> d.queueDepthTotal))
            .map(d -> "SPLIT_LOAD " + operator + " " + replicaName(operator, d.device) + " ROUND_ROBIN")
            .orElse("HOLD COMPUTE_SATURATED_NO_CAPACITY");
    }

    return "HOLD NO_ACTION_MATCHED";
}

// Finding 3 guard — returns true if placing the operator on candidateDev would add an
// extra WAN hop relative to the sink. In the current star topology this happens when
// the candidate is a cloud node and the sink is on a *different* cloud node: traffic
// would have to cross albacore5 twice (edge→cloudNodes[i]→albacore5→cloudNodes[j]).
boolean wouldAddHop(String candidateDev, String sinkDev) {
    boolean candidateIsCloud = candidateDev.startsWith("cloudNodes[");
    boolean sinkIsCloud      = sinkDev.startsWith("cloudNodes[");
    return candidateIsCloud && sinkIsCloud && !candidateDev.equals(sinkDev);
}
```

---

### Step 5 — Send a command via TCP

Open a fresh TCP connection, send the command line (with `\n`), read the ACK line, close.

```java
void sendCommand(String commandLine) {
    String host = "127.0.0.1";
    int    port = 9999;

    try (Socket socket = new Socket()) {
        socket.connect(new InetSocketAddress(host, port), 2000);  // 2s timeout
        socket.setSoTimeout(5000);                                 // 5s read timeout

        PrintWriter out = new PrintWriter(socket.getOutputStream(), true);
        BufferedReader in = new BufferedReader(
            new InputStreamReader(socket.getInputStream()));

        out.println(commandLine);     // sends commandLine + "\n"
        String ack = in.readLine();   // blocks until "\n"

        if (ack != null && ack.startsWith("OK")) {
            log.info("Command accepted: {} → ACK: {}", commandLine, ack);
        } else {
            log.warn("Command rejected: {} → ACK: {}", commandLine, ack);
        }
    } catch (IOException e) {
        log.error("Failed to send command '{}': {}", commandLine, e.getMessage());
    }
}
```

**Timing note:** Send commands only after the simulation has had time to start and reach
steady state. In practice, wait until you have received at least one `GLOBAL_SNAPSHOT`
before sending your first command. The Simulator emits the first snapshot ~10 sim-seconds
in (roughly 5–8 wall-clock seconds at normal simulation speed).

---

### Step 6 — Track replica locations

The Simulator does not send an explicit "here are all replicas" message at startup. You
derive the replica map from two sources:

1. **Active operators** — any `to_device` that appears in a `path_key` for a given
   operator is confirmed active and running.
2. **Inactive replicas** — the `GLOBAL_SNAPSHOT` lists all transit and cloud nodes.
   If a device appears in the snapshot but does NOT appear in any `path_key` for a given
   operator, it may host an inactive replica for that operator.

To get a definitive replica map, inspect the startup XML placement file. It lists all
`<tasks>` elements; any task instance that does not appear in a path is a dormant replica.

**Practical approach for early development:** hard-code the known replica map from the
placement XML rather than inferring it dynamically. The Simulator will return an `ERROR`
ACK if you target a device with no pre-deployed replica, so the fallback is safe.

---

### Step 7 — Handle simulation end

When the Simulator finishes, it closes port 9998. The Optimizer's `reader.readLine()`
returns `null` — the normal TCP EOF signal. Shut down cleanly:

```java
// At end of readSimulatorOutput loop:
if (line == null) {
    log.info("Simulator report stream closed — simulation complete");
    optimizer.onSimulationComplete();
}
```

Both port 9998 and port 9999 stop accepting connections when the Simulator exits. Any
pending `connect()` on port 9999 will fail with connection refused. Build in retry
logic if you may send commands near the very end of the simulation run.

---

### Quick reference — command send pattern

```
FOR EACH trigger report received:
  1. Find worst-breaching path
  2. Identify bottleneck hop
  3. Query cached GLOBAL_SNAPSHOT for candidate devices
  4. Filter candidates against known replica map
  5. Select best candidate (min link_delay or min queue_depth)
  6. Build command string
  7. Open TCP connection to localhost:9999
  8. Write "COMMAND ...\n"
  9. Read ACK line
  10. Close connection
  11. Log command + ACK
```

---

### Simulator (ECSNeTpp / C++) — all done

- [x] TCP listener: parse space-delimited command, dispatch to handler, send text ACK
- [x] SPLIT_LOAD handler: activate named replica, update upstream routing (round-robin)
- [x] RE_ROUTE handler: activate replica on `to_device`, deactivate original, update routing
- [x] HOLD handler: log reason + sim_time, send ACK
- [x] TRIGGER_REPORT: multi-path JSON to stdout (`paths` array, each with `path_key`, per-path P99/trend/trigger, `hops` array)
- [x] TRIGGER_REPORT: `cumulative_ms` running sum per hop
- [x] TRIGGER_REPORT: `observed_link_delay_ms` updated per window in `latestLinkDelayPerDevice`
- [x] GLOBAL_SNAPSHOT: periodic self-message (10s), queries CPUCore queue depths, emits `observed_link_delay_ms`
- [x] Deactivation: `StreamingSupervisor::deactivateNode` removes IP from routing, resets round-robin counter

### Optimizer (Java / Windows) — pending

- [ ] Connect to TCP port 9998, read newline-delimited JSON stream (see Step 1 + Step 2)
- [ ] Distinguish `TRIGGER_REPORT` vs `GLOBAL_SNAPSHOT` by `type` field
- [ ] Cache latest `GLOBAL_SNAPSHOT` — use `observed_link_delay_ms` for latency scoring (see Step 3)
- [ ] On `TRIGGER_REPORT`: iterate paths, apply decision logic above (see Step 4)
- [ ] Parse `path_key` to extract operator name and device at each hop when needed
- [ ] Connect to TCP port 9999, send text command, read ACK line, close (see Step 5)
- [ ] Track replica locations from path observations or startup XML (see Step 6)
- [ ] Log all received reports and sent commands with timestamps

---

## Future Work — FULL_REPLAN (do not implement now)

A `FULL_REPLAN` command is the natural extension for atomic multi-operator migrations.
Recommended wire format when added:

```
FULL_REPLAN <base64-encoded-placement-xml>
```

Inline base64 avoids cross-OS filesystem sharing. At current topology scale (7–13 nodes)
the placement XML is 3–5 KB, well within TCP message limits after encoding.

---

*Document version: 2.3 — 2026-05-09*  
*Format decision: JSON (Simulator → Optimizer reports), plain text (Optimizer → Simulator commands)*  
*v2.1 changes: multi-path TRIGGER_REPORT structure; pre-creation constraint section added; decision logic updated for per-path iteration; `from_op`/`from_device` removed from hops (redundant with `path_key`)*  
*v2.2 changes: expanded Architecture section (threading model, connection lifecycle, WSL↔Windows path); added Java Optimizer Integration Guide (Steps 1–7 with full code skeleton); checklist updated — all Simulator items now done*  
*v2.3 changes: replaced stdout/pipe approach with TCP port 9998 push stream; Simulator now has three threads (sim + report server + command server); Step 1 updated to Socket connect; `emitToClient()` writes to both stdout and TCP client; added port summary table*  
*v2.4 changes: added Empirical Findings section — HOLD confirmed no-op, SPLIT_LOAD validated, RE_ROUTE scoring rule derived from observed path-cost blowup when operator is placed "past" the sink in the star topology*
