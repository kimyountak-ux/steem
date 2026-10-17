#!/bin/bash
# The second half of the switch. steemd MUST BE STOPPED (it appends to the source log).
#   1. adds the blocks that were written since convert_block_log.sh to the compressed log
#   2. verifies the whole compressed log: every block links to the one before it, and its bytes are identical
#      to the original's
#   3. does NOT replace anything - it prints the two mv commands to run, so the original is kept until you
#      have started the node on the compressed log and watched it work.
#
#   finish_block_log.sh /path/to/blockchain/block_log /path/to/new-dir [threads]
set -euo pipefail
SRC=${1:?source block_log}
DST=${2:?directory with the compressed block_log}
THREADS=${3:-4}

if pgrep -x steemd >/dev/null; then echo "steemd is still running - stop it first"; exit 1; fi

compress_block_log convert --input "$SRC" --output "$DST/block_log" --level 15 --threads "$THREADS" --include-last
compress_block_log stats --input "$DST/block_log"
compress_block_log verify --input "$DST/block_log" --reference "$SRC" --threads "$THREADS"

cat <<EOF

The compressed log in $DST is complete and verified. To switch (keeping the original):

  mv "$SRC" "$SRC.plain" && mv "$SRC.index" "$SRC.index.plain"
  cp "$DST/block_log" "$SRC" && cp "$DST/block_log.index" "$SRC.index"      # or mv, if it is on the same disk

then start steemd from the steem-zstd image. No replay is needed: the state does not depend on how the
block log is stored. Delete the *.plain files once you are happy.
EOF
