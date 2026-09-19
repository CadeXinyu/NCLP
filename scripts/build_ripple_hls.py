#!/usr/bin/env python3
"""Synthesize the ripple detector and require fresh, cycle-budgeted artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "programmable_logic/hls/ripple_detector"
BUILD = SOURCE / "build"
SOLUTION = BUILD / "solution1"
BUDGET = 3333


def tool_path(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    elif os.environ.get("VITIS_RUN"):
        candidates.append(Path(os.environ["VITIS_RUN"]).expanduser())
    else:
        found = shutil.which("vitis-run")
        if found:
            candidates.append(Path(found))
        candidates.append(Path.home() / "Xilinx/2025.1/Vitis/bin/vitis-run")
        for prefix in (Path.home() / "Xilinx", Path("/opt/Xilinx"), Path("/tools/Xilinx")):
            if prefix.is_dir():
                candidates.extend(sorted(prefix.glob("*/Vitis/bin/vitis-run"), reverse=True))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise RuntimeError("Vitis vitis-run not found; supply --vitis /path/to/Vitis/bin/vitis-run")


def stamp(path):
    return path.stat().st_mtime_ns if path.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vitis", help="Explicit qualified vitis-run executable")
    args = parser.parse_args()
    tool = tool_path(args.vitis)
    tool_root = tool.parent.parent
    runtime_dirs = [tool_root / "lib/lnx64.o/Ubuntu/24", tool_root / "lib/lnx64.o/Ubuntu",
                    tool_root / "lib/lnx64.o/Rhel/9", tool_root / "lib/lnx64.o/SuSE"]
    runtime = next((path for path in runtime_dirs if (path / "libtinfo.so.5").is_file()), None)
    environment = os.environ.copy()
    if runtime:
        environment["LD_LIBRARY_PATH"] = str(runtime) + (
            ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    rtl = SOLUTION / "syn/verilog/nclp_ripple_hls.v"
    report = SOLUTION / "syn/report/nclp_ripple_hls_csynth.xml"
    before = {path: stamp(path) for path in (rtl, report)}
    log_path = SOURCE / "hls_build.log"
    hls_tcl = ROOT / "scripts/lib/build_ripple_hls.tcl"
    command = [str(tool), "--mode", "hls", "--tcl", str(hls_tcl)]
    print(f"Building fixed-point ripple HLS with {tool}; target100MHz, "
          f"budget{BUDGET}cycles/sample", flush=True)
    excessive_threads = False
    started = time.monotonic()
    with log_path.open("w") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in process.stdout:
            log.write(line)
            if re.search(r"error|PASS:|Estimated Fmax|Finished Command csynth",
                         line, re.IGNORECASE):
                print(line.rstrip(), flush=True)
            match = re.search(r"Using (\d+) slave threads", line)
            if match:
                print(line.rstrip(), flush=True)
                excessive_threads |= int(match.group(1)) > 4
        status = process.wait()
    if status:
        raise RuntimeError(f"HLS exited{status}; full log: {log_path}")
    for artifact in (rtl, report):
        if not artifact.is_file() or stamp(artifact) == before[artifact]:
            raise RuntimeError(f"HLS failed to generate fresh {artifact}; inspect {log_path}")
    tree = ET.parse(report)
    latency_text = tree.findtext(".//Worst-caseLatency")
    if latency_text is None or not latency_text.strip().isdigit():
        raise RuntimeError("HLS report has no bounded worst-case latency")
    maximum = int(latency_text)
    if maximum + 1 > BUDGET:
        raise RuntimeError(f"HLS transaction interval{maximum+1} exceeds {BUDGET}cycles/sample")
    resources = {name: int(tree.findtext(f".//AreaEstimates/Resources/{name}"))
                 for name in ("DSP", "LUT", "FF", "BRAM_18K", "URAM")}
    available = {name: int(tree.findtext(f".//AreaEstimates/AvailableResources/{name}"))
                 for name in resources}
    if any(resources[name] > available[name] for name in resources):
        raise RuntimeError(f"HLS resources exceed target capacity: {resources}")
    if excessive_threads:
        raise RuntimeError("HLS exceeded the repository four-thread limit")
    build_inputs = [SOURCE / "ripple_detector.cpp", SOURCE / "ripple_detector.h",
                    SOURCE / "coefficients.h", SOURCE / "ripple_default_initializer.inc",
                    SOURCE / "iir_default_initializer.inc",
                    hls_tcl]
    summary = {"tool": str(tool), "target_clock_ns": 10, "input_samples_per_second": 30000,
               "cycle_budget": BUDGET, "hls_max_latency_cycles": maximum,
               "hls_max_interval_cycles": maximum + 1,
               "elapsed_seconds": round(time.monotonic() - started, 2),
               "source_sha256": hashlib.sha256((SOURCE / "ripple_detector.cpp").read_bytes()).hexdigest(),
               "rtl_sha256": hashlib.sha256(rtl.read_bytes()).hexdigest(), "csynth_pass": True,
               "supported_filter_kinds": ["fir", "fourth_order_butterworth_iir"],
               "max_fir_taps": 256, "iir_biquad_sections": 2, "live_k_update": True,
               "arithmetic": "integer_fixed_point",
               "configuration_register_format": "ieee754_binary32_bits",
               "fir_coefficient_format": "signed18_q1_17",
               "iir_coefficient_format": "signed18_q2_16",
               "iir_state_format": "signed40_q20",
               "power_accumulator_format": "uint64_exact",
               "power_output_format": "uint32_rne_mean_square_adc_counts_squared",
               "threshold_output_format": "ieee754_binary32_sum_threshold_bits",
               "hls_resources": resources, "device_resources": available,
               "input_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in build_inputs}}
    summary_path = BUILD / "validation_summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"PASS: HLS max latency{maximum}cycles; artifacts {rtl.parent}", flush=True)
    print(f"HLS estimated resources: {resources['DSP']}/{available['DSP']} DSPs, "
          f"{resources['LUT']} LUTs, {resources['FF']} FFs", flush=True)
    print(f"Validation summary: {summary_path}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ET.ParseError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
