#!/usr/bin/env python3
"""Parse an XNU run log against tools/autotest-tests.json -> PASS/FAIL/UNKNOWN.
Usage: autotest-parse.py <logfile> <profile> <outdir>. Report-only: always exits 0."""
import json, re, sys, os, datetime

def main():
    log, profile, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    spec = json.load(open(os.path.join(root, "tools", "autotest-tests.json")))
    prof = spec["profiles"][profile]
    text = open(log, encoding="utf-8", errors="replace").read()
    results = []
    for t in prof["tests"]:
        fails = [p for p in t.get("fail", []) if re.search(p, text)]
        missing = [p for p in t.get("pass", []) if not re.search(p, text)]
        passes = [p for p in t.get("pass", []) if re.search(p, text)]
        if fails:
            status = "FAIL"
            ev = next((re.search(p, text).group(0) for p in fails), "")
        elif not missing:
            status = "PASS"
            ev = next((re.search(p, text).group(0) for p in passes), "")
        else:
            status = "UNKNOWN"
            ev = "not observed: " + "; ".join(missing)
        results.append({"id": t["id"], "desc": t["desc"], "status": status, "evidence": ev[:200]})
    summary = {s: sum(1 for r in results if r["status"] == s) for s in ("PASS", "FAIL", "UNKNOWN")}
    report = {"profile": profile, "log": log,
              "ts": datetime.datetime.now().isoformat(timespec="seconds"),
              "args": prof.get("args", ""), "summary": summary, "results": results}
    os.makedirs(outdir, exist_ok=True)
    json.dump(report, open(os.path.join(outdir, "report.json"), "w"), indent=2, ensure_ascii=False)
    lines = ["# Autotest: " + profile, "", "Log: " + log, "Args: " + prof.get("args", ""), "",
             "| Test | Status | Detail |", "| --- | --- | --- |"]
    for r in results:
        lines.append("| %s | %s | %s |" % (r["desc"], r["status"], r["evidence"].replace("|", "/")))
    lines += ["", "Summary: %d pass / %d fail / %d unknown" % (summary["PASS"], summary["FAIL"], summary["UNKNOWN"])]
    open(os.path.join(outdir, "report.md"), "w").write("\n".join(lines) + "\n")
    print("AUTOTEST %s: %d pass / %d fail / %d unknown" % (profile, summary["PASS"], summary["FAIL"], summary["UNKNOWN"]))
    for r in results:
        print("  [%s] %s" % (r["status"], r["id"]))
    print("report: " + os.path.join(outdir, "report.md"))

if __name__ == "__main__":
    main()
