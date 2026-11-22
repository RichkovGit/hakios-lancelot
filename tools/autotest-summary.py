#!/usr/bin/env python3
"""autotest-summary.py: one dashboard over the latest run of every autotest profile.

Scans logs/autotest/*-<profile>/report.json, keeps the newest run per profile, and writes
logs/autotest/dashboard.md (+ dashboard.json): every feature's PASS/FAIL/UNKNOWN at a glance.
"""
import glob, json, os

NL = chr(10)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUTOTEST = os.path.join(ROOT, "logs", "autotest")


def latest_per_profile():
    best = {}
    for path in glob.glob(os.path.join(AUTOTEST, "*", "report.json")):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                rep = json.load(f)
        except Exception:
            continue
        prof = rep.get("profile") or os.path.basename(os.path.dirname(path)).split("-", 1)[-1]
        ts = rep.get("ts", "")
        if prof not in best or ts > best[prof].get("ts", ""):
            rep["_path"] = path
            best[prof] = rep
    return best


def main():
    best = latest_per_profile()
    order = ["hwprobe", "audio3", "audio4", "audio4g", "audio5", "sinkfed", "mic", "conn",
             "demo", "btapp", "btsettings", "perf", "native", "nativefps", "fps"]
    profiles = sorted(best, key=lambda p: (order.index(p) if p in order else 99, p))
    tot = {"PASS": 0, "FAIL": 0, "UNKNOWN": 0}
    lines = ["# HakiOS lancelot autotest dashboard", "",
             "Latest run per profile (tools/autotest-summary.py).", "",
             "| Profile | Run | PASS | FAIL | UNKNOWN |", "| --- | --- | --- | --- | --- |"]
    for p in profiles:
        rep = best[p]
        s = rep.get("summary", {})
        for k in tot:
            tot[k] += s.get(k, 0)
        run = os.path.basename(os.path.dirname(rep["_path"]))
        lines.append("| " + p + " | " + run + " | " + str(s.get("PASS", 0)) + " | " + str(s.get("FAIL", 0)) + " | " + str(s.get("UNKNOWN", 0)) + " |")
    lines += ["", "**Overall: " + str(tot["PASS"]) + " pass / " + str(tot["FAIL"]) + " fail / " + str(tot["UNKNOWN"]) + " unknown across " + str(len(profiles)) + " profiles.**", ""]
    marks = {"PASS": "[PASS]", "FAIL": "[FAIL]", "UNKNOWN": "[??]"}
    for p in profiles:
        rep = best[p]
        lines.append("## " + p + "  (" + rep.get("ts", "") + ")")
        for r in rep.get("results", []):
            lines.append("- " + marks.get(r.get("status"), "?") + " " + r.get("id", "") + " - " + r.get("desc", ""))
        lines.append("")
    md = NL.join(lines)
    with open(os.path.join(AUTOTEST, "dashboard.md"), "w", encoding="utf-8") as f:
        f.write(md)
    with open(os.path.join(AUTOTEST, "dashboard.json"), "w", encoding="utf-8") as f:
        json.dump({"totals": tot, "profiles": {p: best[p].get("summary", {}) for p in profiles}}, f, indent=2)
    print(md[:1700])
    print(NL + "wrote dashboard.md and dashboard.json")


if __name__ == "__main__":
    main()
