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
    "processing_mean_s", "link_latency_mean_s", "total_latency_mean_s"
]


# ─────────────────────────────────────────────────────────────────────────────
# 1. Parse omnetpp.ini
# ─────────────────────────────────────────────────────────────────────────────

def parse_ini(ini_path):
    """
    Extract from omnetpp.ini:
      - active config name (last [Config ...] block that isn't 'defaultplan')
      - description
      - freq value from ${freq=...}
      - allocationPlanFile path
      - dspTopologyFile path
      - per-device fixedSourceEventRate values
    """
    result = {
        "config": None,
        "description": None,
        "freq": None,
        "allocation_file": None,
        "topology_file": None,
        "event_rates": {}
    }

    current_config = None

    with open(ini_path, "r") as f:
        for line in f:
            line = line.strip()

            # Detect config section headers
            m = re.match(r'^\[Config\s+(.+?)\]', line)
            if m:
                current_config = m.group(1).strip()
                # Use the last non-default config as the active one
                if current_config.lower() != "defaultplan":
                    result["config"] = current_config
                continue

            # Skip comments and empty lines
            if not line or line.startswith("#"):
                continue

            # description
            m = re.match(r'^description\s*=\s*(.+)', line)
            if m and current_config == result["config"]:
                result["description"] = m.group(1).strip()
                continue

            # freq from ${freq=...}
            if result["freq"] is None:
                m = re.search(r'\$\{freq=(\d+)\}', line)
                if m:
                    result["freq"] = m.group(1)
                    continue

            # allocationPlanFile
            m = re.match(r'^\*\.taskbuilder\.allocationPlanFile\s*=\s*"?(.+?)"?\s*$', line)
            if m:
                path = m.group(1).strip().strip('"')
                # resolve relative to simulations/
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

            # per-device fixedSourceEventRate
            # e.g. *.pi3Bs[0].fixedSourceEventRate = 5
            m = re.match(r'^\*\.(\w+)\[(\d+)\]\.fixedSourceEventRate\s*=\s*(\d+)', line)
            if m:
                device = f"{m.group(1)}[{m.group(2)}]"
                rate   = int(m.group(3))
                result["event_rates"][device] = rate
                continue

            # global fixedSourceEventRate (wildcard)
            m = re.match(r'^\*\.(\w+)\[\*\]\.fixedSourceEventRate\s*=\s*(\d+)', line)
            if m:
                key  = f"{m.group(1)}[*]"
                rate = int(m.group(2))
                result["event_rates"][key] = rate
                continue

    return result


# ─────────────────────────────────────────────────────────────────────────────
# 2. Parse .sca file
# ─────────────────────────────────────────────────────────────────────────────

def parse_sca(sca_path):
    """
    Extract from .sca:
      - endToEndDelay stats (count, mean, stddev, min, max)
      - e2eP99:last
      - processingLatency mean
      - transmissionLatency mean
      - totalLatency mean
    """
    metrics = {
        "count":              None,
        "e2e_mean":           None,
        "e2e_stddev":         None,
        "e2e_min":            None,
        "e2e_max":            None,
        "e2e_p99":            None,
        "processing_mean":    None,
        "network_mean":       None,
        "total_latency_mean": None,
    }

    # We parse the scalar section and statistic section
    # Track which statistic block we are in
    current_statistic = None

    with open(sca_path, "r") as f:
        for line in f:
            line = line.strip()

            # e2eP99 scalar
            m = re.match(r'^scalar\s+\S+\s+e2eP99:last\s+([\d.eE+\-]+)', line)
            if m:
                metrics["e2e_p99"] = float(m.group(1))
                continue

            # statistic block header
            m = re.match(r'^statistic\s+\S+\s+(\S+)', line)
            if m:
                current_statistic = m.group(1)
                continue

            # fields inside a statistic block
            if current_statistic:
                m = re.match(r'^field\s+(\w+)\s+([\d.eE+\-]+)', line)
                if m:
                    field_name  = m.group(1)
                    field_value = float(m.group(2))

                    if current_statistic == "endToEndDelay:stats":
                        if field_name == "count":  metrics["count"]      = int(field_value)
                        if field_name == "mean":   metrics["e2e_mean"]   = field_value
                        if field_name == "stddev": metrics["e2e_stddev"] = field_value
                        if field_name == "min":    metrics["e2e_min"]    = field_value
                        if field_name == "max":    metrics["e2e_max"]    = field_value
                    continue

            # vector summary lines for latency means
            # format: vector <id> <module> <name>:vector TV
            # data lines: <id> <count_idx> <...> <count> <min> <mean> <sum> <sumsq>
            # We read mean from the processingLatency, transmissionLatency, totalLatency vectors
            # These appear as inline data in .sca when scalar-recording is on

            # scalar for processingLatency mean
            m = re.match(r'^scalar\s+\S+\s+processingLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                metrics["processing_mean"] = float(m.group(1))
                continue

            m = re.match(r'^scalar\s+\S+\s+transmissionLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                metrics["network_mean"] = float(m.group(1))
                continue

            m = re.match(r'^scalar\s+\S+\s+totalLatency:mean\s+([\d.eE+\-]+)', line)
            if m:
                metrics["total_latency_mean"] = float(m.group(1))
                continue

    # If mean scalars weren't recorded separately, compute from vci data
    # (handled in parse_vci below if needed)

    return metrics


# ─────────────────────────────────────────────────────────────────────────────
# 3. Parse .vci file for vector means (fallback if not in .sca)
# ─────────────────────────────────────────────────────────────────────────────

def parse_vci(vci_path):
    """
    The .vci file contains vector metadata including min/max and
    the inline summary stats embedded in the data lines.

    Line format in .vci data section:
    <vecId> <startEventNum> <startOffset> <startTime> <endTime>
            <count> <min> <mean> <sum> <sumSq>

    We identify vectors by their title from attr lines.
    """
    vector_means = {
        "processing_mean":    None,
        "network_mean":       None,
        "total_latency_mean": None,
    }

    if not os.path.exists(vci_path):
        return vector_means

    # Map vecId → title
    vec_titles = {}
    current_vec_id = None

    with open(vci_path, "r") as f:
        for line in f:
            line = line.strip()

            # vector declaration: "vector <id> <module> <name> TV"
            m = re.match(r'^vector\s+(\d+)\s+\S+\s+(\S+)\s+TV', line)
            if m:
                current_vec_id = int(m.group(1))
                vec_titles[current_vec_id] = m.group(2)
                continue

            # attr title line
            m = re.match(r'^attr\s+title\s+"(.+)"', line)
            if m and current_vec_id is not None:
                vec_titles[current_vec_id] = m.group(1).lower()
                continue

            # data summary line: starts with a number (vecId)
            # format: vecId  startEvt  offset  startT  endT  count  min  mean  sum  sumsq
            parts = line.split()
            if len(parts) >= 9:
                try:
                    vid  = int(parts[0])
                    mean = float(parts[7])
                    title = vec_titles.get(vid, "").lower()

                    if "processing" in title and "total" not in title:
                        vector_means["processing_mean"] = mean
                    elif "transmission" in title or "network" in title:
                        vector_means["network_mean"] = mean
                    elif "total" in title:
                        vector_means["total_latency_mean"] = mean
                except (ValueError, IndexError):
                    pass

    return vector_means


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
                   timestamp, md_path, total_rates=None, sources_per_device=None):
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
        f"## Results",
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

    ini_data = parse_ini(INI_FILE)
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

    metrics = parse_sca(sca_path)

    # Fill in means from vci if not found in sca
    if any(v is None for v in [
        metrics["processing_mean"],
        metrics["network_mean"],
        metrics["total_latency_mean"]
    ]):
        print(f"      (reading vector means from {vci_name}...)")
        vci_means = parse_vci(vci_path)
        metrics["processing_mean"]    = metrics["processing_mean"]    or vci_means["processing_mean"]
        metrics["network_mean"]       = metrics["network_mean"]       or vci_means["network_mean"]
        metrics["total_latency_mean"] = metrics["total_latency_mean"] or vci_means["total_latency_mean"]

    print(f"      Events: {metrics['count']}  |  E2E mean: {metrics['e2e_mean']}s  |  P99: {metrics['e2e_p99']}s")

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
        "link_latency_mean_s":       metrics["network_mean"],
        "total_latency_mean_s": metrics["total_latency_mean"]
    }
    write_csv(csv_row, CSV_FILE)

    # Markdown report
    md_filename = f"{ts_file}_{ini_data['config']}.md"
    md_path     = os.path.join(REPORTS_DIR, md_filename)
    total_rates, sources_per_device = compute_device_total_rates(ini_data, placements)
    write_markdown(ini_data, metrics, topo_string, placements,
                   timestamp, md_path, total_rates, sources_per_device)

    print(f"\n{'='*50}")
    print(f"  Done!")
    print(f"{'='*50}\n")


if __name__ == "__main__":
    main()