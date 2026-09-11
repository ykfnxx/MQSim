# DWPDSim MQSim golden tests

`test_golden.py` compares a canonical subset of MQSim output with the committed
`golden_expected.json`. The subset intentionally includes deterministic fields
that define the DWPDSim integration contract and omits path names, wall-clock
runtime, progress output, and floating-point throughput formatting.

The golden groups cover:

- exact dependency request/flow/operation mapping plus predecessor-release and
  submit-at-release relations (concurrent root completion timestamps are excluded);
- READ, WRITE, partial/full TRIM, and LBA reuse counters;
- isolated exact SLC/TLC response time plus pool, media-profile, capacity,
  PE-limit, and NAND statistics;
- an empty flow sharing the SLC pool;
- two active flows using the full pool-local namespace of one shared pool;
- the half-open host/NAND measurement window;
- exact GC relocation and static wear-leveling counts under fixed-seed replay;
- both out-of-order transaction schedulers.

Run it with:

```bash
python3 tests/dwpdsim_vnext/test_golden.py --binary ./MQSim
```

To inspect a proposed result without modifying the golden file:

```bash
python3 tests/dwpdsim_vnext/test_golden.py --binary ./MQSim --print-actual
```

Never refresh the golden merely because the test failed. First determine whether
the difference is a regression or an intentional model/ABI change. For an
intentional change, review the canonical diff and update the expected JSON in the
same commit as the implementation and design rationale.

These are compatibility goldens, not an independent proof that MQSim's NAND timing
model is physically accurate. The logical TRIM bitmap oracle in `stress_replay.py`
and sanitizer runs remain separate validation layers.

`test_mapping_scale.py` is included in `test_vnext.sh`. Its six deterministic
cases span 8,192 logical NAND pages per flow and 100,000 I/Os per case, covering
mixed reads/writes, burst TRIM/reuse, and four flows sharing a CMT across two
pools, under both schedulers. It checks exact independently generated host and
TRIM totals, complete drain, and nonzero mapping reads/programs and GC. These
are functional oracle tests, not snapshots of unverified simulator output.

For a larger synthetic replay (90,000 NAND pages, 1.3 million I/Os per scheduler):

```bash
python3 tests/dwpdsim_vnext/stress_replay.py --binary ./MQSim \
  --count 1300000 --working-set-pages 90000 --filter mixed \
  --timeout 180 --output build/large-mapping-mixed
```

Use `--filter burst-trim` for burst writes/reads/TRIM, or `--filter four-flows`
with `--working-set-pages 45000` for two flows per pool and 90,000 pages per pool.
The working-set option scales logical and physical SSD capacities together;
it counts NAND pages **per flow**, not DWPDSim KV blocks. Synthetic I/O counts
are not a claim to reproduce a private DWPDSim event sequence.

If drain fails, stderr now includes `AMU pending: stream=... <container>=<count>`
or the channel/chip/die/plane and overfull-write count. These diagnostics contain
no logical addresses or trace contents. They retain the fatal drain check and
do not discard pending work.

`test_frontier_drain.py` covers the free-block-reserve boundary under both
schedulers. A 16-block SLC plane with a small CMT receives 210 or 224 sequential
page writes. The old unconditional reserve check stalls the 210th write with
`Write_transactions_for_overfull_planes=1` even though the existing data
frontier has 15 writable pages. The oracle requires all writes and exact bytes
to complete, mapping programs to occur, and TSU queues to drain. The fix permits
use of the requesting stream's existing frontier; allocating a new frontier
still obeys the GC reserve.

`test_overfull_gc.py` adds eight tight-capacity cases (four workloads under
both schedulers), also run by `test_vnext.sh`:

- 208 live pages in a 16-block x 16-page plane, followed by overwrites: the
  original 5,000-write burst leaves four overfull AMU waiters at `4b2a98b`;
- the 512-write prefix, which requests a mapping block from an empty pool;
- 192 live pages with 5,000 writes spaced 1 ms apart;
- two flows sharing a 64-byte CMT across six channels and two media pools.

These use independently derived request IDs, dependency ordering, completed
write counts and bytes, plus NAND/TSU/pool/channel conservation checks. They
require actual GC and mapping programs; they do not bless new output by
updating a snapshot. Legacy mapping counters can include writebacks folded
into a GC mapping relocation, so separate physical mapping programs are
counted from TSU submissions for the NAND conservation assertion.

The repair retires an entirely invalid GC frontier only under space pressure
and after its plane's relocation operations and the block's user I/O drain.
The partial block then goes through normal GC erase, not an in-place reset.
Mapping writeback waits before invalidating its old flash page if allocating
a new mapping block would consume the blocks reserved for ongoing GC. Erase
completion retries those writebacks before host space waiters. The pending
mapping-writeback container is included in the fatal AMU drain check.

Run the focused suite or compare an older binary with:

```bash
python3 tests/dwpdsim_vnext/test_overfull_gc.py --binary ./MQSim
python3 tests/dwpdsim_vnext/test_overfull_gc.py --binary /path/to/old/MQSim --case gc-frontier-burst
```

This is a finite-headroom liveness regression, not a guarantee that every
advertised logical-capacity configuration has enough physical space for all
per-stream data, mapping and GC frontiers at full occupancy. In particular,
the 224-live-page / 256-physical-page overwrite probe still stalls: after
initial fill there is no user frontier, the mapping frontier occupies a
block, and the final free block is reserved. Unlike the 208-page reproducer,
there is no invalid GC frontier to reclaim. That capacity/admission boundary
is not repaired by this change (the existing 224-write first-fill test still
passes).

`test_waiting_cmt.py` exercises six/eight flows sharing a 64-byte CMT with
8,192 pages per flow and 100,000 burst I/Os under both schedulers. Before the
fix, the six-flow seed-321 case ended with three unmapped writes on stream 3
and one on stream 5: an evicted WAITING reservation made the mapping-completion
callback skip its waiters and delete the arriving record. The regression
requires exact host accounting, real mapping I/O and full drain. Completion
now recreates missing reservations for actual waiters, preserving an already
valid mapping if present; merge-only reads do not populate the cache.
