# Experiment Plan — Optimizer vs AggNet vs SAFA

## Goal
Compare three placement strategies across topologies and event rates.
Results go into a shared results table at the bottom of this file (or a companion `results_log.csv`).

---

## Strategies Under Test

| ID | Strategy | How placement.xml is produced |
|----|----------|-------------------------------|
| **OPT** | This optimizer (GreedyScheduler) | `mvn exec:java ... -t <topology> -c <scenario> -s <sla>` → `output/placement.xml` |
| **AGG** | AggNet baseline: sources+edgeOps on pi3Bs, one aggregation op on transitNodes[0], sink on cloud | Manually authored placement.xml |
| **SAFA** | SAFA-DCM: each operator on the nearest transit node (lowest linkDelay) | Manually authored placement.xml |

---

## Topologies

### T1 — AggNet-Simple (2 sources → 1 op → 1 sink)
```
so1 op1 1
so2 op1 1
op1 si  1
```
File: `src/test/resources/aggnet_simple.txt`
Matches the AggNet-Simple omnetpp.ini scenario.

### T2 — AggNet (2 sources → 2 edge ops → 1 transit op → 1 sink)
```
so1 edgeOp1 1
so2 edgeOp2 1
edgeOp1 transitOp 1
edgeOp2 transitOp 1
transitOp si 1
```
File: `thesis/sample_topology.txt`
Matches the AggNet-Simple scenario with a richer operator chain.

### T3 — Linear chain (1 source → 3 chained ops → 1 sink)
```
so1 op1 1
op1 op2 1
op2 op3 1
op3 si  1
```
File: create as `thesis/topologies/chain3.txt`
Tests deeper pipelines where placement of intermediate ops matters.

### T4 — Filter chain (1 source → 2 ops with selectivity < 1 → 1 sink)
```
so1 op1 0.5
op1 op2 0.5
op2 si  1
```
File: create as `thesis/topologies/filter2.txt`
Tests the cost model's sensitivity to selectivity (rate drops across ops).

---

## Event Rates to Test

Use omnetpp.ini `fixedSourceEventRate` per source:

| Rate label | Events/s per source |
|------------|---------------------|
| Low        | 1                   |
| Medium     | 5 (default)         |
| High       | 10                  |
| Peak       | 20                  |

---

## SLA Thresholds

| SLA label | P99 target |
|-----------|------------|
| Tight     | 0.070 s    |
| Normal    | 0.100 s    |
| Relaxed   | 0.200 s    |

---

## Experiment Matrix

Priority: run **bold** rows first. Others are secondary.

| # | Topology | Strategy | Event rate | SLA    | Expected winner |
|---|----------|----------|------------|--------|-----------------|
| **E1** | T1 | OPT / AGG / SAFA | Medium (5) | Normal (0.1s) | OPT=AGG (same on simple DAG) |
| **E2** | T2 | OPT / AGG / SAFA | Medium (5) | Normal (0.1s) | OPT ≤ cost of AGG |
| **E3** | T2 | OPT / AGG / SAFA | High (10)  | Normal (0.1s) | Cost gap widens |
| **E4** | T2 | OPT / AGG / SAFA | Medium (5) | Tight (0.07s) | SAFA may fail SLA |
| E5 | T3 | OPT / AGG / SAFA | Medium (5) | Normal (0.1s) | OPT should beat AGG on chain |
| E6 | T4 | OPT / AGG / SAFA | Medium (5) | Normal (0.1s) | Filter reduces cost; OPT should reflect this |
| E7 | T1 | OPT / AGG / SAFA | Peak (20)  | Relaxed (0.2s)| Cost differences most visible at high rate |

---

## Metrics to Record per Run

For each simulation run, record:

- `topology` — T1/T2/T3/T4
- `strategy` — OPT/AGG/SAFA
- `event_rate` — events/s per source
- `sla_target_s` — SLA threshold used
- `p99_s` — measured P99 latency at sink (seconds)
- `total_cost_usd` — total link cost from simulation
- `cost_per_record_usd` — total_cost / event_count
- `sla_pass` — yes/no (p99 ≤ sla_target)
- `notes` — any anomalies

---

## Results Table (fill in after runs)

| # | Topology | Strategy | Rate | SLA | P99 (s) | Cost ($) | $/record | SLA pass |
|---|----------|----------|------|-----|---------|----------|----------|----------|
| E1 | T1 | OPT  | 5  | 0.1 | | | | |
| E1 | T1 | AGG  | 5  | 0.1 | | | | |
| E1 | T1 | SAFA | 5  | 0.1 | | | | |
| E2 | T2 | OPT  | 5  | 0.1 | | | | |
| E2 | T2 | AGG  | 5  | 0.1 | | | | |
| E2 | T2 | SAFA | 5  | 0.1 | | | | |
| E3 | T2 | OPT  | 10 | 0.1 | | | | |
| E3 | T2 | AGG  | 10 | 0.1 | | | | |
| E3 | T2 | SAFA | 10 | 0.1 | | | | |
| E4 | T2 | OPT  | 5  | 0.07 | | | | |
| E4 | T2 | AGG  | 5  | 0.07 | | | | |
| E4 | T2 | SAFA | 5  | 0.07 | | | | |

---

## Notes for the Simulator Session

- The optimizer writes its result to **`output/placement.xml`** (configurable via `--output`).
- For AGG and SAFA baselines, placement.xml must be authored manually — see schema in `thesis/optimizer_knowledge_transfer.md`.
- AggNet-Simple omnetpp.ini scenario: 4× pi3Bs (so1@[0], idle@[1], so2@[2], idle@[3]), 1× transitNode (linkDelay=2ms), 1× cloudNode.
- Use the same scenario config (same device counts, delays, costs) across all three strategies for a fair comparison.
- Simulation window: 50s total (5s warmup → measure last 45s).
- Store all placement.xml files alongside results (e.g. `results/<exp_id>_<strategy>_placement.xml`).
