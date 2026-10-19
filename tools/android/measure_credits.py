#!/usr/bin/env python3
"""Bounded Honor/emulator credits capture with opt-in asynchronous renderer timings."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = "org.openrealm.wc3"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--apk", type=Path, help="Install this APK with -r, preserving imported archives")
    parser.add_argument("--adb", type=Path, default=ROOT / "build/android/sdk/platform-tools/adb")
    parser.add_argument("--scale", type=int, choices=(50, 67, 75, 85, 100), default=67)
    parser.add_argument("--profile", type=int, choices=(0, 1, 2), default=2)
    parser.add_argument("--particles", type=int, choices=(0, 1), default=1)
    parser.add_argument("--warmup", type=float, default=40)
    parser.add_argument("--duration", type=float, default=15)
    args = parser.parse_args()
    if args.duration <= 0 or args.warmup < 0:
        parser.error("duration must be positive and warmup nonnegative")
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    if (out / "summary.json").exists():
        parser.error("output already contains a capture; select a new directory")
    adb = [str(args.adb), "-s", args.serial]

    def call(*cmd):
        return subprocess.run([*adb, *map(str, cmd)], check=True, capture_output=True).stdout

    def log():
        return call("exec-out", "run-as", PACKAGE, "cat", "files/engine.log").decode()

    result = {"serial": args.serial, "scale": args.scale, "profile": args.profile,
              "particles": args.particles, "warmup": args.warmup, "duration": args.duration}
    call("shell", "am", "force-stop", PACKAGE)
    if args.apk:
        result["apk_sha256"] = hashlib.sha256(args.apk.read_bytes()).hexdigest()
        call("install", "--no-incremental", "-r", args.apk)
    try:
        call("shell", "run-as", PACKAGE, "rm", "-f", "files/engine.log")
        call("shell", "am", "start", "-n", PACKAGE + "/.LauncherActivity",
             "--ei", "frames", "2000000", "--ei", "scale", args.scale,
             "--ei", "profile", args.profile, "--ei", "particles", args.particles)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                text = log()
            except subprocess.CalledProcessError:
                text = ""
            if "G_ClientBegin:" in text:
                break
            time.sleep(.25)
        else:
            raise RuntimeError("Credits did not reach client begin within 120 seconds")
        print("World ready; warming up", args.warmup, "seconds", flush=True)
        time.sleep(args.warmup)
        (out / "battery-before.txt").write_bytes(call("shell", "dumpsys", "battery"))
        first = len(log().splitlines())
        started = time.monotonic()
        print("Recording", args.duration, "seconds", flush=True)
        time.sleep(args.duration)
        text = log()
        result["elapsed_seconds"] = time.monotonic() - started
        (out / "engine.log").write_text(text)
        window = text.splitlines()[first:]
        (out / "window.log").write_text("\n".join(window) + "\n")
        (out / "scene.png").write_bytes(call("exec-out", "screencap", "-p"))
        (out / "battery-after.txt").write_bytes(call("shell", "dumpsys", "battery"))
        rows = {}
        for line in window:
            match = re.match(r"\[(R_STATS|R_CPU|R_GPU|R_PARTICLES)\] ", line)
            if match:
                values = {k: float(v) for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line)}
                if values:
                    rows.setdefault(match[1], []).append(values)
        result["windows"] = rows
        result["medians"] = {tag: {k: statistics.median(row[k] for row in data) for k in data[0]}
                             for tag, data in rows.items()}
        gpu = [row for row in rows.get("R_GPU", []) if row.get("samples", 0)]
        if gpu:
            count = sum(row["samples"] for row in gpu)
            result["gpu_sample_weighted_ms"] = {k: sum(row[k] * row["samples"] for row in gpu) / count
                                                for k in gpu[0] if k.endswith("_ms")}
            result["gpu_samples"] = count
        (out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({k: v for k, v in result.items() if k != "windows"}), flush=True)
    finally:
        call("shell", "am", "force-stop", PACKAGE)


if __name__ == "__main__":
    main()
