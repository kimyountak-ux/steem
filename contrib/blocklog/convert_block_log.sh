#!/bin/bash
# Converts a block_log to the compressed format in a separate directory. The source is only READ, so this is
# safe while steemd is running (it only converts the blocks that are already in the index; the last blocks are
# added later by finish_block_log.sh while the node is stopped).
#
#   convert_block_log.sh /path/to/blockchain/block_log /path/to/new-dir [threads] [level]
#
# Needs the compress_block_log binary (from the steem-zstd image: docker run --rm -v ... steem-zstd compress_block_log).
set -euo pipefail
SRC=${1:?source block_log}
DST=${2:?directory for the compressed block_log}
THREADS=${3:-4}
LEVEL=${4:-15}
mkdir -p "$DST"
echo "free space in $DST: $(df -h "$DST" | tail -1 | awk '{print $4}') (the result is roughly half the size of the source)"
nice -n 15 ionice -c3 compress_block_log convert --input "$SRC" --output "$DST/block_log" --level "$LEVEL" --threads "$THREADS"
compress_block_log stats --input "$DST/block_log"
