#!/bin/sh
J="${JAVA:-java}"
"$J" -Xmx16g -XX:+UseParallelGC -cp "${TLA2TOOLS:-$HOME/.tlaplus-mcp/lib/tla2tools.jar}" tlc2.TLC -workers auto -config $1.cfg -metadir "$TEMP/tlc_$1_$$" ZcTransport.tla 2>&1
