#!/bin/sh
# Replays the TLC runs listed in README.md. Usage: sh check.sh [quick]
# Needs Java and tla2tools.jar (TLA2TOOLS, default ~/.tlaplus-mcp/lib).
cd "$(dirname "$0")"
run() {
  sh run.sh "$1" > "$1.out"
  printf "%-12s %-40s %s\n" "$1" \
    "$(grep -oE 'No error has been found|Invariant \w+ is violated|Temporal properties were violated|Deadlock reached' "$1.out" | head -1)" \
    "$(grep -oE '[0-9]+ distinct states found' "$1.out" | tail -1)"
}
#            name        N F K  Hold Sync Period  extra invariants
SC=TRUE  sh mk.sh deliver4   3 2 4  100  150  17  DeliveredAfterHead; run deliver4
SC=TRUE  sh mk.sh tail5      3 2 5  100  150  17  DeliveredAfterHead; run tail5      # expected: violated
[ "$1" = quick ] && exit 0
SC=TRUE  sh mk.sh safe6      3 2 6  100  150  17; run safe6
SC=TRUE  GW=2 sh mk.sh burst4 4 1 4  100  150  17; run burst4
SC=FALSE sh mk.sh nofix_head 3 2 6  100  150  17; run nofix_head                # expected: NoSilentCorruption violated
SC=FALSE GW=2 sh mk.sh nofix_burst 4 1 2 100 150 17; run nofix_burst            # expected: NoSilentCorruption violated
# smallest window that holds here is 25 (24 fails: the ZCSY pair is lost
# and the retry, 30 later, comes after the last frame)
SC=TRUE  RW=25 sh mk.sh resync4 6 1 4 10 30 17 Resyncs; run resync4
SC=TRUE  RW=24 sh mk.sh resync4_tight 6 1 4 10 30 17 Resyncs; run resync4_tight  # expected: violated
