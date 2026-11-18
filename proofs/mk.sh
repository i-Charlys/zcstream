#!/bin/sh
# usage: mk.sh name N F MaxLoss Hold SyncEvery Period [inv...]
name=$1; n=$2; f=$3; k=$4; hold=$5; sync=$6; per=$7; shift 7
{
echo "SPECIFICATION Spec"
echo "CONSTANTS"
echo "  N = $n"; echo "  F = $f"; echo "  Period = $per"
echo "  NackDelay = 3"; echo "  NackInt = 10"; echo "  Hold = $hold"
echo "  Linger = 200"; echo "  ProbeGap = 20"; echo "  Probes = 3"
echo "  SyncEvery = $sync"; echo "  KfBurst = 4"; echo "  Lat = 1"
echo "  MaxLoss = $k"; echo "  GapWin = ${GW:-8}"; echo "  SeqCheck = ${SC:-FALSE}"; echo "  ResyncWindow = ${RW:-1000}"
echo "INVARIANTS TypeOK InOrder Consistent NoSilentCorruption"
for i in "$@"; do echo "INVARIANT $i"; done
echo "PROPERTY Ends"
} > $name.cfg
