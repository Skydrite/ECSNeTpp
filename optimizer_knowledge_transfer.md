# Link Cost Optimizer — Knowledge Transfer

This file is the single source of truth for implementing the Java optimizer.
Read this at the start of every optimizer session. It describes the interface contract,
the cost/latency model, and the optimization problem — everything needed without diving
into the simulator source code.

---

## What the Optimizer Does

Given:
- A **streaming DAG** (operators, sources, sinks and their connections)
- A **device inventory** (available edge/transit/cloud nodes with their cost and delay)
- A **P99 latency SLA** (hard constraint, in seconds)

Produce:
- A **placement XML file** assigning each task to a specific device instance

The simulator (ECSNeTpp) reads this XML at startup, deploys the tasks, runs the
simulation, and reports P99 latency and total link cost. The optimizer's goal is to
**minimise total link cost while keeping P99 ≤ SLA threshold**.

---

## The Three-Tier Network Model

```
Pi3Bs (edge) ──WiFi──► area1AP1 ──► albacore5 (router)
                                          │
                     ┌────────────────────┼──────────────────┐
                     ▼                    ▼                   ▼
             transitNodes[]          cloudNodes[]         (future)
             "near" tier             "far" tier
             $0.08/GB                $0.20/GB
             linkDelay = Xms         eToCDelayMean = 8.56ms
```

**Important:** This is a **star topology**. All edge devices share the same path through
albacore5. The `linkDelay` on a transit node is the WAN delay from the shared edge router
to that transit DC — not a source-specific delay.

### Device Types

| Array name | Tier | Link cost | Link delay | Notes |
|------------|------|-----------|------------|-------|
| `pi3Bs[i]` | Edge | $0.00/GB | WiFi only (~1-2ms transmission) | Sources always here; operators can be here too |
| `transitNodes[i]` | Transit | $0.08/GB | `transitNodes[i].linkDelay` (configurable per node) | Near-edge DCs; operators placed here reduce final-hop cost |
| `cloudNodes[i]` | Cloud | $0.20/GB | `eToCDelayMean` = 8.56ms | Destination tier; sinks always here |

### Link Cost Accounting

Cost is charged on the **outgoing link of each device that forwards events**:
- Operator on `pi3Bs`: $0.00/GB (WiFi, no ethernet cost)
- Operator on `transitNodes[i]`: $0.08/GB × bytes forwarded
- Operator on `cloudNodes[i]`: $0.20/GB × bytes forwarded
- Sink on `cloudNodes[i]`: $0.20/GB × bytes received

**Total link cost = sum across all operators and sinks.**

### Latency Accounting

End-to-end latency for one event = processing time + all link traversal times.

| Hop | Latency |
|-----|---------|
| Pi3B → Pi3B (same device, different task) | ~0ms channel delay + WiFi transmission |
| Pi3B → transitNode | `transitNodes[i].linkDelay` + transmission |
| Pi3B → cloudNode | `eToCDelayMean` (8.56ms) + transmission |
| transitNode → cloudNode | `eToCDelayMean` (8.56ms) + transmission |
| Processing at any device | Task-specific (see below) |

**Processing delays (nanoseconds, as used in existing configs):**

| Task type | `processingdelay.measuredtime` (ns) |
|-----------|--------------------------------------|
| StreamingSource | 50,000,000 ns (50ms) |
| StreamingOperator | 748,722 ns (~0.75ms) |
| StreamingSink | 195,424 ns (~0.2ms) |

The P99 SLA is measured at the sink as the 99th percentile of
`simTime() - event.creationTime()` across all received events.

---

## Input to the Optimizer

### 1. Topology file (`.txt`)

Describes the streaming DAG. One edge per line: `sender receiver selectivity`

```
so1 op1 1
so2 op1 1
op1 si  1
```

- `selectivity`: ratio of output events per input event (1 = pass-through, 0.5 = filters half)
- Sources have no incoming edges; sinks have no outgoing edges
- The optimizer reads this to understand data flow and which operators are on the critical path

### 2. Device inventory (configured in `omnetpp.ini`)

The optimizer needs to know (or be given as config) what devices are available:
```
numPiModel3Bs   = N    → pi3Bs[0..N-1] available for sources/operators
numTransitNodes = T    → transitNodes[0..T-1] available for operators
numCloudNodes   = C    → cloudNodes[0..C-1] available for sinks/operators
```

And their per-node properties:
```
transitNodes[i].linkDelay    = Xms   (WAN delay from router to this transit DC)
transitLinkCostPerGB         = 0.08
cloudLinkCostPerGB           = 0.20
eToCDelayMean                = 8.56ms
```

### 3. SLA threshold

A P99 latency budget in seconds. Events delivered to the sink must satisfy:
`P99(simTime - creationTime) ≤ SLA_threshold`

---

## Output: The Placement XML

This is the **most critical contract**. The simulator reads this file exactly as specified.

### Schema

```xml
<?xml version="1.0" ?>
<devices>
    <device>
        <name>DEVICE_ARRAY_NAME</name>       <!-- pi3Bs | transitNodes | cloudNodes -->
        <index-range>INDEX</index-range>     <!-- integer index into the array -->
        <tasks>
            <task>
                <name>TASK_NAME</name>               <!-- unique, e.g. "op1" -->
                <category>TASK_CATEGORY</category>   <!-- same as name, used for routing -->
                <type>FULLY_QUALIFIED_TYPE</type>     <!-- see type strings below -->
                <processingdelay>
                    <measuredtime>NANOSECONDS</measuredtime>
                </processingdelay>
                <!-- additional fields depend on task type, see below -->
            </task>
        </tasks>
    </device>
    <!-- more <device> blocks ... -->
</devices>
```

### Task type strings

| Task | `<type>` value |
|------|----------------|
| Source | `ecsnetpp.stask.StreamingSource` |
| Operator | `ecsnetpp.stask.StreamingOperator` |
| Sink | `ecsnetpp.stask.StreamingSink` |

### Source-specific fields

```xml
<msgsize>6880</msgsize>   <!-- message size in BITS (6880 bits = 860 bytes) -->
<sourceevdistribution>
    <name>FixedSourceEventRateDistribution</name>
    <type>ecsnetpp.model.source.eventrate.FixedSourceEventRateDistribution</type>
</sourceevdistribution>
```

The actual event rate is set separately in `omnetpp.ini`:
`*.pi3Bs[i].fixedSourceEventRate = 5` (events per second)

### Operator-specific fields

```xml
<selectivity>1</selectivity>     <!-- output events per input event (1 = pass-through) -->
<productivity>1</productivity>   <!-- output message size ratio (1 = same size) -->
```

### Sink-specific fields

No additional fields beyond the base schema.

### Full example (two pipelines, one sink)

```xml
<?xml version="1.0" ?>
<devices>
    <device>
        <name>pi3Bs</name>
        <index-range>0</index-range>
        <tasks>
            <task>
                <name>so1</name>
                <category>so1</category>
                <type>ecsnetpp.stask.StreamingSource</type>
                <processingdelay><measuredtime>50000000</measuredtime></processingdelay>
                <msgsize>6880</msgsize>
                <sourceevdistribution>
                    <name>FixedSourceEventRateDistribution</name>
                    <type>ecsnetpp.model.source.eventrate.FixedSourceEventRateDistribution</type>
                </sourceevdistribution>
            </task>
        </tasks>
    </device>
    <device>
        <name>transitNodes</name>
        <index-range>0</index-range>
        <tasks>
            <task>
                <name>op1</name>
                <category>op1</category>
                <type>ecsnetpp.stask.StreamingOperator</type>
                <processingdelay><measuredtime>748722</measuredtime></processingdelay>
                <selectivity>1</selectivity>
                <productivity>1</productivity>
            </task>
        </tasks>
    </device>
    <device>
        <name>cloudNodes</name>
        <index-range>0</index-range>
        <tasks>
            <task>
                <name>si</name>
                <category>si</category>
                <type>ecsnetpp.stask.StreamingSink</type>
                <processingdelay><measuredtime>195424</measuredtime></processingdelay>
            </task>
        </tasks>
    </device>
</devices>
```

---

## Placement Rules & Constraints

| Rule | Detail |
|------|--------|
| Sources are **fixed** | Always on `pi3Bs[i]`. The optimizer does not move them. |
| Sinks go to **cloudNodes** | Destination tier. Index chosen by optimizer. |
| Operators are **free to place** anywhere | Pi3B (free), transitNode (cheap), cloudNode (expensive). This is the optimizer's decision space. |
| One device block per `(device_name, index)` pair | Multiple tasks can share a device — just list multiple `<task>` elements inside one `<device>` block. |
| `index-range` must be within `numXxx` bounds | Set in `omnetpp.ini` for the config being run. |
| `<name>` and `<category>` should match | Both are used internally for routing lookup. |

---

## How the Optimizer Integrates with the Simulator

1. Optimizer reads topology `.txt` and device inventory, runs algorithm, writes `placement.xml`.
2. `omnetpp.ini` config block points to the output:
   ```ini
   *.taskbuilder.allocationPlanFile = "../src/configs/myrun/placement.xml"
   *.taskbuilder.dspTopologyFile    = "../src/configs/myrun/topology.txt"
   ```
3. Run simulation: `../ECSNeTpp -u Cmdenv ... -c MyConfig`
4. Run parser: `python3 parse_results.py`
5. Parser reports P99 and total link cost → optimizer evaluates constraint satisfaction.

For an iterative optimizer, steps 1–5 form one evaluation loop.

---

## Observed Baseline Results (reference points)

All at 50s sim-time, 5s warmup, 10 ev/s total, 8.56ms cloud delay.

| Scenario | Placement | P99 | Link Cost |
|----------|-----------|-----|-----------|
| ETL-Pi3B-1-Plan | All operators on edge (Pi3Bs) | ~0.064s | $0.000101 |
| AggNet-Simple | Edge ops + transit aggregator | ~0.082s | $0.000118 |
| SAFA-Default | Operators on cloud | higher | higher |
| SAFA-DCM | Operators on transit (2ms) | lower than default | $0.000118 range |

**Key insight from results:** Placing operators on Pi3Bs (edge) gives the lowest link cost
and good latency. Transit placement adds operator forwarding cost ($0.08/GB) that can
outweigh the savings vs cloud. The optimizer must account for ALL hops, not just the final
sink hop.

---

## Online / Runtime Scheduling (Replica Activation)

The simulator supports **runtime operator re-placement** without restarting the simulation.
This is the mechanism the optimizer will use for online scheduling decisions.

### How it works

At startup, `ECSBuilder` reads the placement XML and auto-generates
`placement_with_replicas.xml`. For each active operator, one dormant replica is
pre-deployed on a separate node (transit → cloud → edge priority, excluding nodes that
host downstream tasks of that operator). These replicas are wired into the network but
receive no traffic until explicitly activated.

When the optimizer signals a split, `activateFirstReplica(category)` is called:
1. The first dormant replica of `category` is resolved to an IP address.
2. Every upstream supervisor that routes to `category` has the replica's IP added to its
   round-robin destination list (key: the sender's own task category).
3. From that point, each upstream supervisor distributes load evenly across all
   destinations (original node + replica node), one message per destination per round.

### Activation interface (TODO: wire to optimizer)

```cpp
// In ECSBuilder — call this when the optimizer decides to split operator `category`
void ECSBuilder::activateFirstReplica(const std::string& category);
```

Currently this is triggered by a hardcoded `scheduleAt(SimTime(25), ...)` (commented out).
The optimizer will replace this with an external signal — e.g. a TCP/UDP message received
by ECSBuilder, or a shared-memory flag polled each window.

### Replica placement constraints

- Replicas are placed on nodes that do **not** already host an active instance of the
  same category and do **not** host any downstream task of that category.
- Slot priority: `transitNodes` → `cloudNodes` → `pi3Bs`.
- Number of replicas per operator is controlled by `replicasPerOperator` (default 2,
  meaning 1 active + 1 dormant).

### Per-path telemetry available to the optimizer

Each sink window report (printed to stdout every 5s) includes one block per observed
path through the network. The optimizer can parse these to make activation decisions.

Key fields per path:

| Field | Meaning |
|-------|---------|
| `path_key` | `sourceLabel:hop0Label:hop1Label:...` — identifies the exact physical route |
| `p99_current_s` | P99 latency over this window for this path |
| `p99_trend` | `RISING` / `FALLING` / `STABLE` (±5% threshold vs previous window) |
| `trigger_reason` | `BREACH` / `WARN` / `OK` vs the SLA budget |
| `hop[h].link_ms` | Mean WAN/queue delay on the link arriving at hop h |
| `hop[h].processing_ms` | Mean CPU processing time at hop h |
| `hop[h].queue_depth` | Mean CPUCore queue depth at hop h |
| `hop[h].bottleneck` | `NETWORK` / `COMPUTE` / `BOTH` / `OK` |

**Activation heuristic (example):** If `p99_trend = RISING` and
`trigger_reason = BREACH` and `hop[last-1].bottleneck = NETWORK`, activate a replica
on a node closer to the sources to bypass the congested WAN link.

---

## Open Questions / Future Decisions for the Optimizer

- **Algorithm:** Greedy? ILP? Heuristic search? SAFA's DCM is a natural starting point
  (pick nearest/cheapest node for each operator greedily).
- **Input format:** Will the optimizer read the `.txt` + `.ini` directly, or receive a
  structured config (JSON/YAML)?
- **SLA estimation:** The optimizer needs to estimate P99 without running the simulation
  (a model), then verify by running the simulation. Or it runs simulation for each candidate.
- **Multi-operator topologies:** With fan-in (multiple upstream operators feeding one
  downstream), cost accumulates across all paths. The optimizer must sum across all edges.
- **Source rate as input:** Higher source rates → more bytes → higher cost. The optimizer
  should receive this as a parameter.
