# Dynamic Re-Scheduling Feasibility Analysis

## Context

ECSNeTpp runs on OMNeT++ 5.5.2 + INET 3.6.5. The goal is online re-scheduling:
the optimizer observes per-window P99 output from the simulator, decides a new
placement, and pushes it back to the simulator mid-run.

---

## Requirement 1 — Simulator outputs networking values periodically ✅ DONE

`StreamingSink` prints one block per **distinct observed path** every 5 sim-seconds.
Each path is identified by a key built from the stamped labels on each message:
`sourceLabel:hop0Label:hop1Label:...` (e.g. `so1@pi3Bs[0]:inflateOp@transitNodes[0]:si@cloudNodes[0]`).

Example output:

```
[WINDOW t=21-26]
  sink:   si
  node:   SimpleEdgeCloudEnvironment.cloudNodes[0]
  l_target_s: 2.000
  paths:  6
  ---
  path_key:        so1@pi3Bs[0]:inflateOp@cloudNodes[1]:si@cloudNodes[0]
  events:          6
  p99_current_s:   0.594
  e2e_mean_s:      0.460
  p99_trend:       STABLE
  budget_used_pct: 29.7
  trigger_reason:  OK
  hops:
    - hop: 0  (inflateOp @ cloudNodes[1])
      link_ms:       32.689
      processing_ms: 0.749
      queue_depth:   0
      bottleneck:    OK
    - hop: 1  (si @ cloudNodes[0])
      link_ms:       375.960
      processing_ms: 0.163
      queue_depth:   0
      bottleneck:    OK
```

Implementation details:
- Each message carries `sourceLabel` (stamped at source creation) and `hopNodeLabel[h]`
  (stamped by CPUCore at each hop using `sender + "@" + nodeName`).
- `StreamingSink` accumulates per-path `PathWindowStats` keyed by the path string.
  `prevP99` is preserved across windows for trend tracking; all other accumulators reset.
- Bottleneck classification: NETWORK if `link_ms > 50% of fair-share budget`; COMPUTE if
  `queue_depth ≥ 5` or `proc_ms > 30% of fair-share`.
- Window interval: `StreamingSink::WINDOW_INTERVAL_S = 5.0` (compile-time constant).
- Multi-sink topologies: each sink emits its own independent window block.

---

## Requirement 2 — Change placement while simulation is running

### What OMNeT++ 5.5 supports

| Operation | Supported? |
|-----------|-----------|
| Change scalar module parameters at runtime | Yes — `par("x").setDoubleValue(v)` |
| Reschedule self-messages | Yes |
| Add/remove modules mid-simulation | **No** — module graph is fixed after `initialize()` |
| Reconnect gates / channels mid-simulation | **No** — structural, set at network setup |
| Move a `StreamingOperator` from one host to another | **No** — it is a submodule of a fixed host |

### Feasible workaround — Logical re-routing (no structural change)

Message routing in ECSNeTpp is driven by the `StreamingSupervisor` routing table
(next-hop IP per task category), NOT by hard-wired gate connections.
This means a re-schedule = **update the routing table**, not move modules.

**Design:**

```
Startup:
  - ECSBuilder deploys operators on ALL candidate nodes (edge, transit, cloud)
    based on a "full deployment" plan, but marks non-active ones as inactive
  - Only the initial plan's operators are in the supervisor routing tables

Re-schedule event (mid-sim):
  - Optimizer sends a ReplacementPlan control message to the simController
  - simController injects it to each affected supervisor
  - Supervisor updates next-hop table entries
  - In-flight messages complete on old path; new messages follow new path
```

This mirrors how real stream processors (Storm, Flink) handle plan migration.

### Components to build / status

| Component | File(s) | Status |
|-----------|---------|--------|
| Pre-deploy all operators at startup (mark inactive) | `ECSBuilder.cc` — `generateReplicaPlacement()` | ✅ DONE |
| Routing-table update handler in supervisor | `StreamingSupervisor.cc` — `activateReplica()` | ✅ DONE |
| Replica slot selection (excludes downstream-node co-location) | `ECSBuilder.cc` — `generateReplicaPlacement()` | ✅ DONE |
| Simulation controller trigger | `ECSBuilder.cc` — hardcoded t=25s, **commented out** | ⏳ STUB — needs optimizer signal |
| `ReplacementPlan` message type (external wire format) | New `.msg` + handler in `ECSBuilder` or `simController` | ❌ TODO |
| External communication channel | stdout pipe / shared file / socket | ❌ TODO |

### Data flow for online re-scheduling

```
[Simulator]  every 5s: print [WINDOW] P99 to stdout
     │
     ▼
[External optimizer process]  parses stdout, runs placement algorithm
     │
     ▼  (TCP socket / stdin pipe / shared file)
[simController module]  receives new plan, injects ReplacementPlan message
     │
     ▼
[StreamingSupervisor on each node]  updates routing table
     │
     ▼
[StreamingOperator / StreamingSink]  next messages follow new path
```

### Key constraint

The window output currently goes to **stdout** of the OMNeT++ process.
The optimizer must either:
- (a) run as a subprocess reading the simulator's stdout (simplest), or
- (b) read a shared file the simulator writes each window to, or
- (c) use OMNeT++'s built-in `cSocketRTScheduler` for real-time external comms (complex).

Option (a) is easiest for a thesis prototype.

---

## Open Questions / Next Steps

1. **External communication channel** — decide how the optimizer signals the simulator.
   Option (a) stdout pipe (simplest for thesis prototype), (b) shared file polled each window,
   or (c) OMNeT++ `cSocketRTScheduler` for real-time socket comms.

2. **ReplacementPlan wire format** — define the message the optimizer sends to trigger
   activation. Simplest: a single line `ACTIVATE <category>` on the channel. Richer: a JSON
   or XML block specifying the full new routing plan (for multi-operator migrations).

3. **Multi-step migration** — currently `activateFirstReplica` does a 50/50 round-robin
   split. Future: drain old path (stop routing to it), activate new path fully, then
   optionally deactivate the old replica.

4. **Deactivation** — no deactivation mechanism yet. Once a replica is activated it stays
   active. For the optimizer to reassign an operator, a deactivate path is needed too.

5. **Multiple replicas per operator** — `replicasPerOperator` is configurable (default 2).
   Each additional replica gives the optimizer another migration target. `activateFirstReplica`
   pops from the front of `inactiveReplicaNodePaths[category]`, so activations are sequential.
