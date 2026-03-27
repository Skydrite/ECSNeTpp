#!/usr/bin/env python3
"""
ECSNeT++ Results Parser
=======================
Reads simulation outputs and produces:
  - results_log.csv        (appended each run)
  - reports/<ts>_<cfg>.md  (one detailed report per run)

Usage: python3 parse_results.py
Place in: ECSNeTpp/simulations/

22/03/2026 Andreas Efstathiou
"""

import os
import re
import csv
import xml.etree.ElementTree as ET
from datetime import datetime

# ── Paths (relative to simulations/) ─────────────────────────────────────────
INI_FILE    = "omnetpp.ini"
RESULTS_DIR = "results"
REPORTS_DIR = os.path.join(RESULTS_DIR, "reports")
CSV_FILE    = os.path.join(RESULTS_DIR, "results_log.csv")

# ── CSV columns ───────────────────────────────────────────────────────────────
CSV_COLUMNS = [
    "timestamp", "description",
    "freq_hz",
    "events_completed", "e2e_mean_s", "e2e_stddev_s",
    "e2e_min_s", "e2e_max_s", "e2e_p99_s",
    "processing_mean_s", "link_latency_mean_s", "total_latency_mean_s",
    "link_cost_usd"
]


# ─────────────────────────────────────────────────────────────────────────────
# 1. Parse omnetpp.ini
# ─────────────────────────────────────────────────────────────────────────────

def find_latest_run(results_dir):
    """
    Scan results/ for the most recently modified .sca file and extract
    (config_name, freq) from its filename.
    Filename pattern: {config}-freq={freq}-#0.sca
    Returns (config_name, freq_str) or (None, None) if nothing found.
    """
    pattern = re.compile(r'^(.+)-freq=(\d+)-#\d+\.sca$')
    candidates = []

    if not os.path.isdir(results_dir):
        return None, None

    for fname in os.listdir(results_dir):
        m = pattern.match(fname)
        if m:
            fpath = os.path.join(results_dir, fname)
            candidates.append((os.path.getmtime(fpath), m.group(1), m.group(2)))

    if not candidates:
        return None, None

    candidates.sort(reverse=True)  # newest first
    _, config_name, freq = candidates[0]
    return config_name, freq


def parse_ini(ini_path, target_config=None):
    """
    Extract from omnetpp.ini for a specific config (target_config).
    If target_config is None, falls back to the last non-default config in the file.

    Reads:
      - description, allocationPlanFile, dspTopologyFile, fixedSourceEventRate
        only from the target config section.
      - freq from ${freq=...} anywhere in the file (defined globally in defaultplan).
    """
    result = {
        "config": target_config,
        "description": None,
        "freq": None,
        "allocation_file": None,
        "topology_file": None,
        "event_rates": {}
    }

    current_config = None
    in_target = False  # True when we're inside the target config section

    with open(ini_path, "r") as f:
        for line in f:
            line = line.strip()

            # Detect config section headers
            m = re.match(r'^\[Config\s+(.+?)\]', line)
            if m:
                current_config = m.group(1).strip()
                if target_config is None and current_config.lower() != "defaultplan":
                    result["config"] = current_config
                in_target = (current_config == result["config"])
                continue

            # Skip comments and empty lines
            if not line or line.startswith("#"):
                continue

            # freq from ${freq=...} — defined globally, read from anywhere
            if result["freq"] is None:
                m = re.search(r'\$\{freq=(\d+)\}', line)
                if m:
                    result["freq"] = m.group(1)
                    continue

            # Everything below is scoped to the target config section only
            if not in_target:
                continue

            # description
            m = re.match(r'^description\s*=\s*(.+)', line)
            if m:
                result["description"] = m.group(1).strip()
                continue

            # allocationPlanFile
            m = re.match(r'^\*\.taskbuilder\.allocationPlanFile\s*=\s*"?(.+?)"?\s*$', line)
            if m:
                path = m.group(1).strip().strip('"')
                result["allocation_file"] = os.path.normpath(
                    os.path.join(os.path.dirname(ini_path), path)
                )
                continue

            # dspTopologyFile
            m = re.match(r'^\*\.taskbuilder\.dspTopologyFile\s*=\s*"?(.+?)"?\s*$', line)
            if m:
                path = m.group(1).strip().strip('"')
                result["topology_file"] = os.path.normpath(
                    os.path.join(os.path.dirname(ini_path), path)
                )
                continue

            # per-device fixedSourceEventRate  e.g. *.pi3Bs[0].fixedSourceEventRate = 5
            m = re.match(r'^\*\.(\w+)\[(\d+)\]\.fixedSourceEventRate\s*=\s*(\d+)', line)
            if m:
                device = f"{m.group(1)}[{m.group(2)}]"
                result["event_rates"][device] = int(m.group(3))
                continue

            # wildcard fixedSourceEventRate
            m = re.match(r'^\*\.(\w+)\[\*\]\.fixedSourceEventRate\s*=\s*(\d+)', line)
            if m:
                result["event_rates"][f"{m.group(1)}[*]"] = int(m.group(2))
                continue

    return result


# ─────────────────────────────────────────────────────────────────────────────
# 2. Parse .sca file
# ─────────────────────────────────────────────────────────────────────────────

def parse_sca(sca_path):
    """
    Extract per-sink metrics from .sca, then aggregate across all sinks.
    Returns (aggregated_metrics, per_sink_dict) where per_sink_dict maps
    a short sink label (e.g. 'cloudNodes[0]') to its metric dict.
    """
    per_module = {}   # full_module_path -> metric dict
    current_module    = None
    current_statistic = None

    with open(sca_path, "r") as f:
        for line in f:
            line = line.strip()

            # e2eP99 scalar  — keyed by module
            m = re.match(r'^scalar\s+(\S+)\s+e2eP99:last\s+([\d.eE+\-]+)', line)
            if m:
                mod = m.group(1)
                per_module.setdefault(mod, {})["e2e_p99"] = float(m.group(2))
                continue

            # linkCost scalar — keyed by module
            m = re.match(r'^scalar\s+(\S+)\s+linkCost:last\s+([\d.eE+\-]+)', line)
            if m:
                mod = m.group(1)
                per_module.setdefault(mod, {})["link_cost"] = float(m.group(2))
                continue

            # statistic block header  — captures module + stat name
            m = re.match(r'^statistic\s+(\S+)\s+(\S+)', line)
            if m:
                current_module    = m.group(1)
                current_statistic = m.group(2)
                per_module.setdefault(current_module, {})
                continue

            # fields inside a statistic block
            if current_statistic and current_module:
                m = re.match(r'^field\s+(\w+)\s+([\d.eE+\-]+)', line)
                if m:
                    fname = m.group(1)
                    fval  = float(m.group(2))
                    if current_statistic == "endToEndDelay:stats":
                        if fname == "count":  per_module[current_module]["count"]      = int(fval)
                        if fname == "mean":   per_module[current_module]["e2e_mean"]   = fval
                        if fname == "stddev": per_module[current_module]["e2e_stddev"] = fval
                        if fname == "min":    per_module[current_module]["e2e_min"]    = fval
                        if fname == "max":    per_module[current_module]["e2e_max"]    = fval
                    continue

            # scalar latency means  — keyed by module
            m = re.match(r'^scalar\s+(\S+)\s+processingLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                per_module.setdefault(m.group(1), {})["processing_mean"] = float(m.group(2))
                continue

            m = re.match(r'^scalar\s+(\S+)\s+transmissionLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                per_module.setdefault(m.group(1), {})["network_mean"] = float(m.group(2))
                continue

            m = re.match(r'^scalar\s+(\S+)\s+totalLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                per_module.setdefault(m.group(1), {})["total_latency_mean"] = float(m.group(2))
                continue

    # Normalize full module path to just the device[index] label.
    # e.g. "SimpleEdgeCloudEnvironment.cloudNodes[0].supervisor" -> "cloudNodes[0]"
    # e.g. "SimpleEdgeCloudEnvironment.cloudNodes[0].si10"       -> "cloudNodes[0]"
    def short_label(full_path):
        parts = full_path.split(".")
        return parts[1] if len(parts) >= 2 else full_path

    # Merge all sub-module data into one dict per device label
    per_device = {}
    for mod, data in per_module.items():
        label = short_label(mod)
        if label not in per_device:
            per_device[label] = {}
        per_device[label].update({k: v for k, v in data.items() if v is not None})

    # Total link cost = sum across ALL devices (sinks + operators)
    all_link_cost = sum(
        d["link_cost"] for d in per_device.values() if d.get("link_cost") is not None
    ) or None

    # Keep only entries that have sink-level data (count) for per-sink breakdown
    per_sink = {label: data for label, data in per_device.items() if "count" in data}

    agg = _aggregate_metrics(per_sink)
    agg["link_cost"] = all_link_cost  # override: include operator hop costs
    return agg, per_sink


def _aggregate_metrics(sink_modules):
    """Aggregate per-sink dicts into one dict (sum counts, weighted means)."""
    empty = {k: None for k in [
        "count", "e2e_mean", "e2e_stddev", "e2e_min", "e2e_max",
        "e2e_p99", "processing_mean", "network_mean", "total_latency_mean",
        "link_cost"
    ]}
    if not sink_modules:
        return empty

    total_count = sum(d.get("count", 0) or 0 for d in sink_modules.values())

    def weighted_mean(key):
        pairs = [(d.get("count", 0) or 0, d[key])
                 for d in sink_modules.values() if d.get(key) is not None]
        w = sum(c for c, _ in pairs)
        return sum(c * v for c, v in pairs) / w if w else None

    def safe_min(key):
        vals = [d[key] for d in sink_modules.values() if d.get(key) is not None]
        return min(vals) if vals else None

    def safe_max(key):
        vals = [d[key] for d in sink_modules.values() if d.get(key) is not None]
        return max(vals) if vals else None

    return {
        "count":              total_count,
        "e2e_mean":           weighted_mean("e2e_mean"),
        "e2e_stddev":         weighted_mean("e2e_stddev"),
        "e2e_min":            safe_min("e2e_min"),
        "e2e_max":            safe_max("e2e_max"),
        "e2e_p99":            weighted_mean("e2e_p99"),
        "processing_mean":    weighted_mean("processing_mean"),
        "network_mean":       weighted_mean("network_mean"),
        "total_latency_mean": weighted_mean("total_latency_mean"),
        "link_cost":          sum(d.get("link_cost") or 0 for d in sink_modules.values()) or None,
    }


# ─────────────────────────────────────────────────────────────────────────────
# 3. Parse .vci file for vector means (fallback if not in .sca)
# ─────────────────────────────────────────────────────────────────────────────

def parse_vci(vci_path):
    """
    Fallback: read vector means from .vci when not present in .sca.
    Returns (aggregated_means, per_sink) matching the parse_sca shape,
    but only for the three latency means (no count/e2e stats here).
    """
    empty_agg = {
        "processing_mean": None, "network_mean": None, "total_latency_mean": None
    }
    empty = (empty_agg, {})

    if not os.path.exists(vci_path):
        return empty

    # Map vecId -> (module, title)
    vec_info = {}
    current_vec_id = None

    # per-module accumulation
    per_module = {}   # module -> {processing_mean, network_mean, total_latency_mean}

    with open(vci_path, "r") as f:
        for line in f:
            line = line.strip()

            # vector declaration: "vector <id> <module> <name> TV"
            m = re.match(r'^vector\s+(\d+)\s+(\S+)\s+(\S+)\s+TV', line)
            if m:
                current_vec_id = int(m.group(1))
                vec_info[current_vec_id] = {"module": m.group(2), "title": m.group(3).lower()}
                continue

            # attr title override
            m = re.match(r'^attr\s+title\s+"(.+)"', line)
            if m and current_vec_id is not None:
                vec_info[current_vec_id]["title"] = m.group(1).lower()
                continue

            # data summary line
            parts = line.split()
            if len(parts) >= 9:
                try:
                    vid   = int(parts[0])
                    mean  = float(parts[7])
                    info  = vec_info.get(vid, {})
                    title = info.get("title", "")
                    mod   = info.get("module", "unknown")

                    per_module.setdefault(mod, {})
                    if "processing" in title and "total" not in title:
                        per_module[mod]["processing_mean"] = mean
                    elif "transmission" in title or "network" in title:
                        per_module[mod]["network_mean"] = mean
                    elif "total" in title:
                        per_module[mod]["total_latency_mean"] = mean
                except (ValueError, IndexError):
                    pass

    if not per_module:
        return empty

    def short_label(full_path):
        parts = full_path.split(".")
        return parts[1] if len(parts) >= 2 else full_path

    per_sink = {}
    for mod, data in per_module.items():
        label = short_label(mod)
        if label not in per_sink:
            per_sink[label] = {}
        per_sink[label].update({k: v for k, v in data.items() if v is not None})

    # Aggregate (no counts available here, so plain average)
    def plain_mean(key):
        vals = [d[key] for d in per_module.values() if d.get(key) is not None]
        return sum(vals) / len(vals) if vals else None

    agg = {
        "processing_mean":    plain_mean("processing_mean"),
        "network_mean":       plain_mean("network_mean"),
        "total_latency_mean": plain_mean("total_latency_mean"),
    }
    return agg, per_sink


# ─────────────────────────────────────────────────────────────────────────────
# 4. Parse topology .txt
# ─────────────────────────────────────────────────────────────────────────────

def parse_topology(topo_path):
    edges = []

    if not os.path.exists(topo_path):
        return edges, "topology file not found"

    with open(topo_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 2:
                edges.append((parts[0], parts[1]))

    if not edges:
        return edges, "empty topology"

    # Build adjacency
    successors   = {}
    predecessors = {}
    all_nodes    = set()

    for src, dst in edges:
        all_nodes.add(src)
        all_nodes.add(dst)
        successors.setdefault(src, []).append(dst)
        predecessors.setdefault(dst, []).append(src)

    # Find sources (no predecessors)
    sources = [n for n in all_nodes if n not in predecessors]

    # For each source, trace ALL paths to sinks independently
    # No visited tracking — each source gets its own full traversal
    def trace_paths(node):
        """Return list of all paths from node to a sink as strings."""
        if node not in successors:
            return [[node]]  # sink node
        paths = []
        for succ in successors[node]:
            for subpath in trace_paths(succ):
                paths.append([node] + subpath)
        return paths

    chain_lines = []
    for source in sorted(sources):
        for path in trace_paths(source):
            chain_lines.append(" → ".join(path))

    return edges, "\n".join(chain_lines)


# ─────────────────────────────────────────────────────────────────────────────
# 5. Parse placement .xml
# ─────────────────────────────────────────────────────────────────────────────

def parse_placement(xml_path):
    """
    Parse placement XML into list of (task_name, task_type, device, index_range).
    Returns both raw data and a summary string.
    """
    placements = []

    if not os.path.exists(xml_path):
        return placements, "placement file not found"

    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()
    except ET.ParseError as e:
        return placements, f"XML parse error: {e}"

    for device in root.findall("device"):
        name_el  = device.find("name")
        range_el = device.find("index-range")
        tasks_el = device.find("tasks")

        if name_el is None or tasks_el is None:
            continue

        device_name  = name_el.text.strip()
        index_range  = range_el.text.strip() if range_el is not None else "0"

        for task in tasks_el.findall("task"):
            task_name_el = task.find("name")
            task_type_el = task.find("type")

            if task_name_el is None:
                continue

            task_name = task_name_el.text.strip()
            task_type = task_type_el.text.strip().split(".")[-1] if task_type_el is not None else "Unknown"

            placements.append({
                "task":    task_name,
                "type":    task_type,
                "device":  device_name,
                "range":   index_range
            })

    # Build summary string: "task(type)@device[range]"
    summary_parts = [
        f"{p['task']}({p['type']})@{p['device']}[{p['range']}]"
        for p in placements
    ]
    summary = " | ".join(summary_parts)

    return placements, summary


# ─────────────────────────────────────────────────────────────────────────────
# 6. Write CSV row
# ─────────────────────────────────────────────────────────────────────────────

def write_csv(row, csv_path):
    """Append one row to the CSV log, creating headers if file is new."""
    file_exists = os.path.exists(csv_path)

    with open(csv_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        if not file_exists:
            writer.writeheader()
        writer.writerow(row)

    print(f"  → CSV appended: {csv_path}")


# ─────────────────────────────────────────────────────────────────────────────
# 7. Write Markdown report
# ─────────────────────────────────────────────────────────────────────────────

def write_markdown(ini_data, metrics, topo_string, placements,
                   timestamp, md_path, total_rates=None, sources_per_device=None,
                   per_sink=None):
    """Write a detailed Markdown report for this run."""

    os.makedirs(os.path.dirname(md_path), exist_ok=True)

    def fmt(value, decimals=4):
        if value is None:
            return "N/A"
        return f"{value:.{decimals}f}"

    if total_rates:
        event_rates_str = "\n".join(
            f"  - `{device}`: {ini_data['event_rates'].get(device, 0)} ev/s per source"
            f" × {sources_per_device.get(device, 0)} sources"
            f" = **{total_rates.get(device, 0)} ev/s total**"
            for device in sorted(
                {f"{p['device']}[{p['range'].split('..')[0]}]"
                 for p in placements}
            )
        ) or "  - (none found)"
    else:
        event_rates_str = "\n".join(
            f"  - `{device}`: {rate} ev/s"
            for device, rate in sorted(ini_data["event_rates"].items())
        ) or "  - (none found)"

    placement_table = "| Task | Type | Device | Index Range |\n|------|------|--------|-------------|\n"
    for p in placements:
        placement_table += f"| {p['task']} | {p['type']} | {p['device']} | {p['range']} |\n"

    # Per-sink breakdown table (only shown when there are 2+ sinks)
    per_sink_section = []
    if per_sink and len(per_sink) > 1:
        per_sink_section = [
            f"## Results per Sink",
            f"",
            f"| Sink | Events | E2E Mean | E2E P99 | Processing Mean | Link Latency Mean | Total Latency Mean | Link Cost |",
            f"|------|--------|----------|---------|-----------------|-------------------|--------------------|-----------|",
        ]
        for label, sm in sorted(per_sink.items()):
            per_sink_section.append(
                f"| {label} "
                f"| {sm.get('count') or 'N/A'} "
                f"| {fmt(sm.get('e2e_mean'))} s "
                f"| {fmt(sm.get('e2e_p99'))} s "
                f"| {fmt(sm.get('processing_mean'))} s "
                f"| {fmt(sm.get('network_mean'))} s "
                f"| {fmt(sm.get('total_latency_mean'))} s "
                f"| ${fmt(sm.get('link_cost'), 6)} |"
            )
        per_sink_section += [f"", f"---", f""]

    lines = [
        f"# Run Report: {ini_data['config']}",
        f"",
        f"**Date & Time:** {timestamp}  ",
        f"**Description:** {ini_data.get('description') or 'N/A'}  ",
        f"**CPU Frequency:** {ini_data['freq']} Hz  ",
        f"",
        f"---",
        f"",
        f"## Source Event Rates",
        f"",
        event_rates_str,
        f"",
        f"---",
        f"",
        f"## Topology",
        f"",
        f"```",
        topo_string,
        f"```",
        f"",
        f"---",
        f"",
        f"## Placement",
        f"",
        placement_table,
        f"---",
        f"",
        *per_sink_section,
        f"## Results (Aggregated)",
        f"",
        f"| Metric | Value |",
        f"|--------|-------|",
        f"| Events Completed | {metrics['count'] or 'N/A'} |",
        f"| E2E Mean | {fmt(metrics['e2e_mean'])} s |",
        f"| E2E Std Dev | {fmt(metrics['e2e_stddev'])} s |",
        f"| E2E Min | {fmt(metrics['e2e_min'])} s |",
        f"| E2E Max | {fmt(metrics['e2e_max'])} s |",
        f"| E2E P99 | {fmt(metrics['e2e_p99'])} s |",
        f"| Processing Mean | {fmt(metrics['processing_mean'])} s |",
        f"| Link Latency Mean | {fmt(metrics['network_mean'])} s |",
        f"| Total Latency Mean | {fmt(metrics['total_latency_mean'])} s |",
        f"| Link Cost (Total) | ${fmt(metrics['link_cost'], 6)} USD |",
        f"",
        f"---",
        f"",
        f"*Generated by parse_results.py*",
    ]

    with open(md_path, "w") as f:
        f.write("\n".join(lines))

    print(f"  → Markdown report: {md_path}")

def compute_device_total_rates(ini_data, placements):
    # Count sources per device from placement XML
    sources_per_device = {}
    for p in placements:
        key = f"{p['device']}[{p['range'].split('..')[0]}]"
        if p["type"] == "StreamingSource":
            sources_per_device[key] = sources_per_device.get(key, 0) + 1

    # Multiply rate × source count per device
    total_rates = {}
    for device, rate in ini_data["event_rates"].items():
        source_count = sources_per_device.get(device, 0)
        total_rates[device] = rate * source_count

    return total_rates, sources_per_device


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    ts_file   = datetime.now().strftime("%Y%m%d_%H%M%S")

    print(f"\n{'='*50}")
    print(f"  ECSNeT++ Results Parser")
    print(f"  {timestamp}")
    print(f"{'='*50}\n")

    # ── 1. Parse ini ──────────────────────────────────────────────────────────
    print(f"[1/5] Reading {INI_FILE}...")
    if not os.path.exists(INI_FILE):
        print(f"ERROR: {INI_FILE} not found. Run from simulations/ directory.")
        return

    # Detect which config was actually run from the newest .sca file
    latest_config, latest_freq = find_latest_run(RESULTS_DIR)
    if latest_config:
        print(f"      Detected last run: config={latest_config}, freq={latest_freq}")

    ini_data = parse_ini(INI_FILE, target_config=latest_config)

    # If freq was found in the sca filename, prefer that (it's authoritative)
    if latest_freq:
        ini_data["freq"] = latest_freq

    print(f"      Config:      {ini_data['config']}")
    print(f"      Description: {ini_data['description']}")
    print(f"      Freq:        {ini_data['freq']} Hz")
    print(f"      Topology:    {ini_data['topology_file']}")
    print(f"      Placement:   {ini_data['allocation_file']}")

    if not ini_data["config"] or not ini_data["freq"]:
        print("ERROR: Could not extract config name or freq from ini file.")
        return

    # ── 2. Construct and parse .sca ───────────────────────────────────────────
    sca_name = f"{ini_data['config']}-freq={ini_data['freq']}-#0.sca"
    sca_path = os.path.join(RESULTS_DIR, sca_name)
    vci_name = f"{ini_data['config']}-freq={ini_data['freq']}-#0.vci"
    vci_path = os.path.join(RESULTS_DIR, vci_name)

    print(f"\n[2/5] Reading {sca_name}...")
    if not os.path.exists(sca_path):
        print(f"ERROR: {sca_path} not found.")
        return

    metrics, per_sink = parse_sca(sca_path)

    # Fill in means from vci if not found in sca
    if any(v is None for v in [
        metrics["processing_mean"],
        metrics["network_mean"],
        metrics["total_latency_mean"]
    ]):
        print(f"      (reading vector means from {vci_name}...)")
        vci_agg, vci_per_sink = parse_vci(vci_path)
        metrics["processing_mean"]    = metrics["processing_mean"]    or vci_agg["processing_mean"]
        metrics["network_mean"]       = metrics["network_mean"]       or vci_agg["network_mean"]
        metrics["total_latency_mean"] = metrics["total_latency_mean"] or vci_agg["total_latency_mean"]
        # merge per_sink latency means if sca didn't have them
        for label, sm in vci_per_sink.items():
            per_sink.setdefault(label, {})
            for key in ("processing_mean", "network_mean", "total_latency_mean"):
                if per_sink[label].get(key) is None:
                    per_sink[label][key] = sm.get(key)

    print(f"      Events: {metrics['count']}  |  E2E mean: {metrics['e2e_mean']}s  |  P99: {metrics['e2e_p99']}s")
    if len(per_sink) > 1:
        print(f"      Sinks detected: {', '.join(sorted(per_sink.keys()))}")

    # ── 3. Parse topology ─────────────────────────────────────────────────────
    print(f"\n[3/5] Reading topology...")
    edges, topo_string = parse_topology(ini_data["topology_file"])
    print(f"      {topo_string}")

    # ── 4. Parse placement ────────────────────────────────────────────────────
    print(f"\n[4/5] Reading placement XML...")
    placements, placement_summary = parse_placement(ini_data["allocation_file"])
    for p in placements:
        print(f"      {p['task']} ({p['type']}) → {p['device']}[{p['range']}]")

    # ── 5. Write outputs ──────────────────────────────────────────────────────
    print(f"\n[5/5] Writing outputs...")
    os.makedirs(REPORTS_DIR, exist_ok=True)

    # CSV row
    event_rates_str = "; ".join(
        f"{d}={r}" for d, r in sorted(ini_data["event_rates"].items())
    )
    csv_row = {
        "timestamp":            timestamp,
        "description":          ini_data.get("description") or "",
        "freq_hz":              ini_data["freq"],
        "events_completed":     metrics["count"],
        "e2e_mean_s":           metrics["e2e_mean"],
        "e2e_stddev_s":         metrics["e2e_stddev"],
        "e2e_min_s":            metrics["e2e_min"],
        "e2e_max_s":            metrics["e2e_max"],
        "e2e_p99_s":            metrics["e2e_p99"],
        "processing_mean_s":    metrics["processing_mean"],
        "link_latency_mean_s":  metrics["network_mean"],
        "total_latency_mean_s": metrics["total_latency_mean"],
        "link_cost_usd":        metrics["link_cost"]
    }
    write_csv(csv_row, CSV_FILE)

    # Markdown report
    md_filename = f"{ts_file}_{ini_data['config']}.md"
    md_path     = os.path.join(REPORTS_DIR, md_filename)
    total_rates, sources_per_device = compute_device_total_rates(ini_data, placements)
    write_markdown(ini_data, metrics, topo_string, placements,
                   timestamp, md_path, total_rates, sources_per_device,
                   per_sink=per_sink)

    print(f"\n{'='*50}")
    print(f"  Done!")
    print(f"{'='*50}\n")


if __name__ == "__main__":
    main()