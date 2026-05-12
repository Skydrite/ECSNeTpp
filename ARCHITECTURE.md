# ECSNeTpp — Architecture Overview & Change Map

This document describes the simulator architecture and annotates every component
that was modified during the thesis project. Diagrams use [Mermaid](https://mermaid.js.org/)
(rendered natively in GitHub, GitLab, and most modern Markdown viewers).

---

## 1. Physical Network Topology

The simulator models a **star topology** with three tiers. All edge devices share
a single WiFi uplink through `area1AP1` → `albacore5`.

```mermaid
graph TD
    subgraph EDGE["Edge Tier  (Pi3Bs / Pi2Bs)"]
        p0["Pi3B[0]\nWiFi"]
        p1["Pi3B[1]\nWiFi"]
        pn["Pi3B[n]\nWiFi"]
        p2b["Pi2B[k]\nWiFi"]
    end

    AP["area1AP1\n(WiFi Access Point)"]
    R["albacore5\n(Router)"]

    subgraph TRANSIT["🆕 Transit Tier  (added 2026-03-27)"]
        t0["transitNodes[0]\nCloudNodeA"]
        t1["transitNodes[1]\nCloudNodeA"]
    end

    subgraph CLOUD["Cloud Tier  (was singleton 'stargazer3', 🆕 array 2026-03-23)"]
        c0["cloudNodes[0]\nCloudNodeA"]
        c1["cloudNodes[1]\nCloudNodeA"]
    end

    p0 -- "WiFi (802.11b)" --> AP
    p1 -- "WiFi (802.11b)" --> AP
    pn -- "WiFi (802.11b)" --> AP
    p2b -- "WiFi (802.11b)" --> AP

    AP -- "E2C_low\n$0.00/GB · 0 ms\n🆕 costPerGB param" --> R

    R -- "🆕 E2Trans_link\n$0.08/GB · linkDelay[i] ms" --> t0
    R -- "🆕 E2Trans_link\n$0.08/GB · linkDelay[i] ms" --> t1

    R -- "E2C_high  🆕 costPerGB\n$0.20/GB · eToCDelayMean ms" --> c0
    R -- "E2C_high  🆕 costPerGB\n$0.20/GB · eToCDelayMean ms" --> c1

    style TRANSIT fill:#fff3cd,stroke:#f0ad4e
    style CLOUD fill:#d4edda,stroke:#28a745
    style EDGE fill:#d1ecf1,stroke:#17a2b8
```

**Key files:** `src/networks/simpleedgecloudenvironment.ned`, `src/configs/ec_sim_mirror.xml`

---

## 2. Node Module Internals

### 2a. CloudNodeA — used for both `cloudNodes[]` and `transitNodes[]`

```mermaid
graph TD
    subgraph CNA["CloudNodeA  (extends StandardHost)"]
        ethg["ethg[] port\n(Ethernet — wired to albacore5)"]
        tcp["TCP stack"]
        sup["StreamingSupervisor\n(routes msgs to tasks)"]
        sched["RoundRobinCpuCoreScheduler"]
        cores["cpuCore[0..cores-1]\n(CPUCore)"]
        acker["Acker\n(if hasAcker)"]
        ld["🆕 linkDelay @unit(s)\n= default(0s)\nper-node WAN override"]
    end

    ethg --> tcp --> sup --> sched --> cores
    sup --> acker

    style ld fill:#fff3cd,stroke:#f0ad4e
```

`linkDelay` is read by `simpleedgecloudenvironment.ned` at connection time:
```ned
transitNodes[i].ethg++ <--> E2Trans_link { delay = transitNodes[i].linkDelay; } <--> albacore5.ethg++;
```
Set per-node in `omnetpp.ini`: `*.transitNodes[0].linkDelay = 2ms`

**Key files:** `src/host/CloudNodeA.ned`

### 2b. RaspberryPiModel3B — edge device

```mermaid
graph TD
    subgraph Pi3B["RaspberryPiModel3B  (extends WirelessHost)"]
        wlan["wlan[] port\n(WiFi — to area1AP1)"]
        tcp2["TCP stack"]
        sup2["StreamingSupervisor"]
        sched2["RoundRobinCpuCoreScheduler"]
        cores2["cpuCore[0..cores-1]"]
        note["⚠ No ethg gate!\n(no Ethernet port)\noperator cost = $0.0"]
    end

    wlan --> tcp2 --> sup2 --> sched2 --> cores2

    style note fill:#f8d7da,stroke:#dc3545
```

Pi3Bs have **no `ethg` gate** — the gate guard added in `StreamingOperator` prevents a crash
when trying to read the channel cost for WiFi-hosted operators.

---

## 3. Streaming Task Class Hierarchy

```mermaid
classDiagram
    class ISTask {
        <<abstract>>
        +double costPerGB
        +double totalBytesReceived
        # simsignal_t linkCostSignal 🆕
        # simsignal_t e2eP99Signal 🆕
        +sendAck(msg)
        +getNextProcessorCoreIndex()
    }

    class StreamingSource {
        +double fixedSourceEventRate
        +generateEvent()
        stamps creationTime on each msg
    }

    class StreamingOperator {
        +double selectivityRatio
        +double productivityRatio
        +double totalBytesForwarded 🆕
        +initialize()
        reads costPerGB from ethg$o[0] 🆕
        guarded: isGateVector(ethg) 🆕
        +finish()
        emits linkCost signal 🆕
    }

    class StreamingSink {
        +vector~double~ e2eLatencies 🔧
        +double totalBytesReceived 🆕
        +double costPerGB 🆕
        +handleMessage()
        e2e = simTime() - creationTime() 🔧
        push to e2eLatencies 🔧
        +finish()
        sort → P99 at 99th percentile 🔧
        emits e2eP99 signal 🆕
        emits linkCost signal 🆕
    }

    ISTask <|-- StreamingSource
    ISTask <|-- StreamingOperator
    ISTask <|-- StreamingSink
```

**Legend:** `🆕` = newly added &nbsp;|&nbsp; `🔧` = fixed/changed

**Key files:** `src/stask/ISTask.h/.cc`, `src/stask/StreamingOperator.h/.cc/.ned`,
`src/stask/StreamingSink.h/.cc/.ned`

---

## 4. Message Flow Through the System

This shows how a single `StreamingMessage` travels from source to sink, and where
the thesis changes intercept it.

```mermaid
sequenceDiagram
    participant Src as StreamingSource<br/>(Pi3B edge)
    participant Net1 as WiFi → E2C_low<br/>(area1AP1 → albacore5)
    participant Net2 as E2Trans_link<br/>$0.08/GB · linkDelay
    participant Op as StreamingOperator<br/>(transitNode)
    participant Net3 as E2C_high<br/>$0.20/GB · eToCDelayMean
    participant Sink as StreamingSink<br/>(cloudNode)

    Src->>Net1: StreamingMessage<br/>🆕 creationTime = simTime()
    Net1->>Op: deliver (0ms + WiFi delay)
    Op->>Op: apply selectivity & productivity
    Op->>Op: 🆕 totalBytesForwarded += byteLength
    Net2->>Sink: deliver (linkDelay ms)
    Op->>Net3: forward message
    Net3->>Sink: deliver (eToCDelayMean ms)
    Sink->>Sink: 🔧 e2e = simTime() − creationTime()
    Sink->>Sink: 🆕 e2eLatencies.push_back(e2e)
    Sink->>Sink: 🆕 totalBytesReceived += byteLength

    Note over Op,Sink: finish() — end of simulation
    Op-->>Op: 🆕 emit linkCost = (bytesForwarded/1e9) × costPerGB
    Sink-->>Sink: 🔧 sort e2eLatencies → P99 = [⌈0.99×N⌉]
    Sink-->>Sink: 🆕 emit e2eP99
    Sink-->>Sink: 🆕 emit linkCost = (bytesReceived/1e9) × costPerGB
```

---

## 5. Results Pipeline

```mermaid
graph LR
    SIM["ECSNeTpp\nsimulator"]
    SCA[".sca file\n(scalar results)"]
    VCI[".vci / .vec\n(vector results)"]
    PY["parse_results.py 🆕"]
    CSV["results_log.csv\n(appended per run)"]
    MD["reports/<timestamp>_<config>.md\n(per-run markdown report)"]

    SIM --> SCA
    SIM --> VCI
    SCA --> PY
    VCI --> PY
    PY --> CSV
    PY --> MD

    style PY fill:#fff3cd,stroke:#f0ad4e
    style CSV fill:#d4edda,stroke:#28a745
    style MD fill:#d4edda,stroke:#28a745
```

**Parser changes (🆕 written from scratch 2026-03-24, 🔧 fixed 2026-03-27):**
- `find_latest_run()` — detects which config ran from newest `.sca` filename
- `parse_ini(target_config)` — scopes to the right `[Config X]` section; falls back to `defaultplan`
- Sums `linkCost` across ALL device labels (operators + sinks) for true multi-hop total
- Reports: per-sink E2E breakdown, P99, link cost (USD), throughput

---

## 6. Annotated Change Map

Summary of every file touched and why.

```mermaid
graph TD
    subgraph NED["NED / Network Definition"]
        ned1["simpleedgecloudenvironment.ned\n🆕 cloudNodes[] array (was stargazer3)\n🆕 transitNodes[] + E2Trans_link channel\n🆕 costPerGB on all channel types\n🆕 edgeLinkCostPerGB / transitLinkCostPerGB / cloudLinkCostPerGB params\n🆕 transitNodes[i].linkDelay per-node connection"]
        ned2["CloudNodeA.ned\n🆕 linkDelay @unit(s) = default(0s)"]
        ned3["StreamingSink.ned\n🆕 e2eP99 signal + statistic\n🆕 linkCost signal + statistic"]
        ned4["StreamingOperator.ned\n🆕 linkCost signal"]
    end

    subgraph CPP["C++ Source"]
        cpp1["ISTask.h/.cc\n🆕 linkCostSignal registered\n🆕 costPerGB field\n🆕 totalBytesReceived field"]
        cpp2["StreamingSink.cc\n🔧 P99 uses simTime()−creationTime()\n🆕 reads costPerGB from ethg$o[0]\n🆕 emits e2eP99 + linkCost in finish()"]
        cpp3["StreamingOperator.cc\n🆕 reads costPerGB from ethg$o[0]\n🆕 gate guard: isGateVector(ethg)\n🆕 totalBytesForwarded accumulator\n🆕 emits linkCost in finish()"]
    end

    subgraph CFG["Config / INI"]
        cfg1["omnetpp.ini\n🆕 numTransitNodes / numCloudNodes params\n🆕 edgeLinkCostPerGB / transitLinkCostPerGB / cloudLinkCostPerGB\n🆕 transitNodes[*].linkDelay\n🆕 AggNet-Simple config block\n🆕 SAFA-Default + SAFA-DCM config blocks"]
        cfg2["ec_sim_mirror.xml\n🆕 transitNodes[] IP subnet (10.0.1.x)\n🆕 cloudNodes[] array addressing"]
        cfg3["src/configs/aggnet/\n🆕 1.txt topology\n🆕 1.xml placement"]
        cfg4["src/configs/safa/\n🆕 topology.txt\n🆕 default.xml (cloud placement)\n🆕 dcm.xml (transit placement)"]
    end

    subgraph PY["Python / Results"]
        py1["simulations/parse_results.py\n🆕 written from scratch\n🔧 config scoping fix\n🔧 find_latest_run() auto-detection\n🆕 multi-device linkCost aggregation"]
    end

    style ned1 fill:#fff3cd,stroke:#f0ad4e
    style ned2 fill:#fff3cd,stroke:#f0ad4e
    style ned3 fill:#fff3cd,stroke:#f0ad4e
    style ned4 fill:#fff3cd,stroke:#f0ad4e
    style cpp2 fill:#f8d7da,stroke:#dc3545
    style cpp3 fill:#f8d7da,stroke:#dc3545
    style cfg3 fill:#d4edda,stroke:#28a745
    style cfg4 fill:#d4edda,stroke:#28a745
    style py1 fill:#d1ecf1,stroke:#17a2b8
```

**Colour key:**
- Yellow = NED / structural changes
- Red = C++ logic changes (fixes + new behaviour)
- Green = new scenario configs
- Blue = tooling / parser

---

## 7. Scenarios at a Glance

```mermaid
graph LR
    subgraph ETL["ETL-Pi3B-1-Plan\nAll operators on edge (Pi3Bs)"]
        e_src["so1..so4\n(Pi3Bs)"] --> e_op["op1/op2\n(Pi3Bs)"] --> e_sink["si\n(cloudNodes)"]
    end

    subgraph AGGNET["AggNet-Simple\n3-tier: edge → transit → cloud"]
        a_src["so1/so2\n(Pi3Bs)"] --> a_eop["edgeOp1/2\n(Pi3Bs)"] --> a_top["transitOp\n(transitNodes[0])"] --> a_sink["si\n(cloudNodes[0])"]
    end

    subgraph SAFA_DEF["SAFA-Default\nYARN-style: ops on cloud (far, $0.20/GB)"]
        sd_src["so1/so2\n(Pi3Bs)"] --> sd_op["op1/op2\n(cloudNodes)"] --> sd_sink["si\n(cloudNodes)"]
    end

    subgraph SAFA_DCM["SAFA-DCM\nDCM-style: ops on transit (near, $0.08/GB)"]
        sm_src["so1/so2\n(Pi3Bs)"] --> sm_op["op1/op2\n(transitNodes)"] --> sm_sink["si\n(cloudNodes[0])"]
    end

    style AGGNET fill:#fff3cd,stroke:#f0ad4e
    style SAFA_DEF fill:#f8d7da,stroke:#dc3545
    style SAFA_DCM fill:#d4edda,stroke:#28a745
```

---

## 8. Chronological Change Summary

| Date | What | Files |
|------|------|-------|
| 2026-03-23 | `cloudNodes[]` array (replaced singleton `stargazer3`) | `simpleedgecloudenvironment.ned`, all placement XMLs, `ec_sim_mirror.xml`, `omnetpp.ini` |
| 2026-03-24 | Link cost model — `costPerGB` on channels, tracked at `StreamingSink` | `simpleedgecloudenvironment.ned`, `StreamingSink.h/.cc/.ned`, `ISTask.h/.cc` |
| 2026-03-24 | Results parser written from scratch | `simulations/parse_results.py` |
| 2026-03-27 | Transit tier — `transitNodes[]` + `E2Trans_link` ($0.08/GB) | `simpleedgecloudenvironment.ned`, `ec_sim_mirror.xml`, `omnetpp.ini` |
| 2026-03-27 | AggNet-Simple scenario | `src/configs/aggnet/`, `omnetpp.ini` |
| 2026-03-27 | Operator link cost tracking + WiFi gate guard | `StreamingOperator.h/.cc/.ned`, `ISTask.h/.cc` |
| 2026-03-27 | P99 fix — use `simTime() − creationTime()` | `StreamingSink.cc` |
| 2026-03-27 | Parser config scoping fix + `find_latest_run()` | `simulations/parse_results.py` |
| 2026-03-29 | SAFA-Default and SAFA-DCM scenarios | `src/configs/safa/`, `omnetpp.ini` |
| 2026-04-01 | Per-node transit delay — `linkDelay` param on `CloudNodeA` | `CloudNodeA.ned`, `simpleedgecloudenvironment.ned`, `omnetpp.ini` |
