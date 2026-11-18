"""Prints a TLC counterexample one line per step: time, action, what changed.

    sh run.sh A2 | python trace.py
"""
import re
import sys

KEEP = ("now", "st", "have", "losses", "lastC", "needSync", "kfLeft", "probes",
        "sdone", "valid", "net")

text = sys.stdin.read()
for line in text.splitlines():
    if line.startswith("Error:") and "behavior" not in line:
        print(line)
states = re.split(r"\nState \d+: ", text)[1:]
prev = {}
for s in states:
    head, _, body = s.partition("\n")
    act = re.match(r"<(\w+)", head)
    act = act.group(1) if act else head.strip()
    cur = {}
    for m in re.finditer(r"^/\\ (\w+) = (.*?)(?=^/\\ |\Z)", body, re.S | re.M):
        cur[m.group(1)] = " ".join(m.group(2).split())
    diff = []
    for k in KEEP:
        if k in cur and cur.get(k) != prev.get(k):
            v = cur[k]
            if k == "net":
                v = re.sub(r"\[f \|-> (\d+), i \|-> (\d+), k \|-> \"(\w)\", s \|-> ([^,\]]*\}), t \|-> (\d+)\]",
                           r"\3(f\1 i\2 \4 @\5)", v)
            diff.append(f"{k}={v}")
    if act == "Tick": prev = cur; continue
    print(f"t={cur.get('now', '?'):>4} {act:<12} " + "  ".join(d for d in diff if not d.startswith("now=")))
    prev = cur
if "Temporal properties were violated" in text or "Deadlock" in text:
    print("(liveness / deadlock trace)")
