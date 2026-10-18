# Block log compression

`block_log` is the append-only file in which steemd keeps every block, and it is by far the biggest thing on a
node's disk (about **414 GB** in October 2026, 110.5 million blocks). This branch lets steemd store the blocks
**zstd compressed** (with built-in dictionaries): the same chain takes about **221 GB**.

It changes how blocks are stored on one machine, nothing else: there is no consensus change, no hardfork, no
change to the network protocol and none to the database state. A node can switch to compressed storage or back
at any time without replaying.

## Measured on the real chain (block 1 - 110,524,687)

| | |
|---|---|
| original `block_log` | 413,741,814,455 bytes |
| compressed (zstd level 15 + dictionaries) | 220,630,498,517 bytes (**53.3 %**) |
| blocks stored compressed | 110,431,500 of 110,524,687 (the rest are tiny early blocks that do not shrink) |
| conversion (6 threads, idle IO priority, while the node kept running) | about 4 h 50 min |
| verification: every block linked to the previous one and byte-identical to the original | 110,524,687 blocks, 0 problems, about 1 h |
| replay of the first 3 M blocks, plain vs compressed | 3,689 s vs 3,693 s (no difference) |

Compression ratio by height (level 15 with dictionaries): 49-58 %, the big early/spam era blocks compress best.
Blocks written by a running node use a lower level (3, about 52-63 %) to stay cheap.

## How it works

Every entry of the block log is `[block bytes][8 byte trailer]`, and the index file holds the same 8 byte value for
every block. Without compression that value is the position where the block starts. With compression the unused
top bits of the same value say how the block is stored:

```
bit 63       1 = zstd compressed, 0 = plain
bit 56       1 = a dictionary was used
bits 48-55   dictionary number
bits 0-47    position of the block in the file
```

* A log may **mix** plain and compressed blocks. Plain blocks are bit-for-bit what the old format wrote.
* The length of a compressed block is the distance to the next entry, so blocks are read by number through the
  index (this is how steemd always read them, apart from replay, which now does the same).
* A compressed block is a zstd frame without magic number, size, dictionary id or checksum.
* The index can be rebuilt from the main file at any time (steemd does it when the index is missing or short):
  the file is walked from the end using the trailers.
* Eight dictionaries (220 KB each, `libraries/chain/block_log_dictionaries/`) were trained on blocks of eight height
  ranges. The dictionary number is stored per block. **A dictionary that has been used is never changed or
  renumbered** - a new one only ever gets a new number (see `dictionaries.txt`).
* zstd 1.5.5 is included in `libraries/vendor/zstd`, so the result does not depend on the zstd of the build OS.

## Compatibility - read this

* A steemd **without** this branch cannot read a block log that contains compressed blocks. Do not run an
  unpatched steemd on it, and do not give such a log to anyone who does.
* Tools that parse `block_log` themselves will not understand it either. `compress_block_log convert --level 0`
  turns it back into a plain log.
* Block log **snapshots** shared between nodes: share the plain log (or tell the receiver to use this build).
* Nothing else on disk (shared memory file, rocksdb state) depends on the block log format.

## Using it

Build (the Dockerfile of steem-docker already takes the source as build arguments, so nothing else changes):

```
docker build --build-arg STEEM_SOURCE=https://github.com/kimyountak-ux/steem.git \\
             --build-arg steemd_version=0.23.1-blocklog-zstd.1 -t steem-zstd dkr/
# or: docker build -f contrib/blocklog/steem-docker.Dockerfile -t steem-zstd .
```

The image has `steemd` and `compress_block_log` in `/usr/local/bin`.

**New blocks**: in `config.ini`

```
block-log-compression = 3
```

compresses the blocks the node writes from now on (1-22, 3 is a good choice; 0 = off, the default). Reading never
depends on this option.

**Existing blocks**: `contrib/blocklog/convert_block_log.sh SRC/block_log NEWDIR` converts them into a separate file.
The source is only read (with `pread`, the file is never opened by steemd's code), so it is safe on a running node
and it needs the free disk space of the result (about half of the source). Then, with steemd stopped for a
few minutes, `contrib/blocklog/finish_block_log.sh SRC/block_log NEWDIR` adds the blocks written in the meantime,
verifies the whole result against the original and prints the `mv` commands for the switch - it does not replace
anything itself. Start steemd (the new image) on the compressed file; no replay is needed. Keep the original
until you have watched the node for a while.

`compress_block_log` commands:

```
convert  --input SRC/block_log --output DST/block_log [--level 15] [--threads N] [--from N --to N] [--include-last]
verify   --input DST/block_log [--reference SRC/block_log] [--threads N]    # chain linkage, optional byte comparison
stats    --input DST/block_log
train    --input SRC/block_log --from A --to B --out FILE.dict            # new dictionary
estimate --input SRC/block_log --from A --to B [--level 15] [--no-dictionary]
```

## What was tested

* Round trip of the whole chain: convert, then verify against the original (0 problems, see above).
* steemd replay of a compressed log, same duration as plain.
* steemd syncing new blocks from the network into a compressed log (blocks are appended compressed), a clean stop,
  a `kill -9`, and restarts: the log is consistent each time (verified after every step).
* The old format is read as before (the new reader is used for plain logs too; plain logs were part of every test).

## Not included

* Splitting the log into parts and pruning old blocks (Hive has both). This branch only shrinks the file.
* Dictionaries retrained on later data: add a new dictionary number with a higher first block, never replace one.
