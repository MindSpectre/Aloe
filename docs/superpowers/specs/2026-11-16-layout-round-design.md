# Layout round: a devices group, net's IPv4 folder, the fabric's parser, the shard counters

Status: design, approved by the author on 2026-11-16 with two rulings added the same day: the group is `l7/`,
and the round is one pull request, not two. From the discussion of that day, checked against `main` at a2844ae
(receive side merged as #40). Private, like every file under `docs/superpowers/`.

## Amended on 2026-11-16, after the branch review

The branch differs from the text below in three places; this section wins where they disagree.

- **The test count grows.** "The test count is unchanged" under Verification meant that no test is removed. The
  branch adds three hook scenarios (`ctest` lists 63 tests in `debug`, from 60) and three gtest cases, and
  replaces one gtest case.
- **The DPDK rule in `AGENTS.md` is unchanged.** Decision 1 lists its wording among the docs to edit. The rule says
  to link `Aloe::Dpdk` and nothing else, which is still true, and "only `devices/` links it" would not be:
  `examples/hello` links it too. The layout table's `devices/` row carries the statement instead.
- **A trap is recorded.** `AGENTS.md` gained a line under "Traps already found" for decision 4's behaviour change:
  a malformed IPv4 frame on a multi-queue fabric port lands on queue 0.

## Why

The layout should say what each group holds, and production code should sit apart from testing code. Today
`common/` holds a DPDK backend nothing in `common/` uses, `net` has IPv4-only types under generic names, the
fabric carries an IPv4 parser older than `wire`, and `loop` owns a counters struct most of whose fields only the
runtime writes.

## Goals, non-goals, anti-goals

- **Goals.** A top-level `devices/` group for production backends. `net`'s IPv4 code under `net/ipv4/`, with
  every IPv4-only type saying so in its name. One IPv4 parser, `wire`'s. Counters owned by whoever writes them.
- **Non-goals.** IPv6 itself, or making `tcp::IsIp` neutral over the address type. Moving `NodePool` to core
  (a TODO in `node_pool.hpp` records it). Moving `Work`, `RunQueue` or `Inbox`. Moving the fabric.
- **Anti-goals.** A renamed `net` module or a `net::v4` namespace. Any change to what the stack sends or
  receives. A change to the tick path that shows above run-to-run noise.

## Constraints

- The rules in `AGENTS.md` hold: one namespace per module, a flat include path with unique header basenames,
  umbrella includes across modules, `detail/` per folder.
- Namespaces and umbrellas do not change: `aloe::ethdev` and `<aloe/ethdev>`, `aloe::net` and `<aloe/net>`.
- A change is done when `debug`, `gcc-debug`, `asan` and `tsan` build and pass and both check scripts pass.

## Approaches

How the round is cut into pull requests:

- **A. One PR.** Least ceremony. **Chosen by the author**: "there are few changes, you could basically merge
  them in one PR instead of two". The moves and the two behaviour-visible changes stay in separate commits, so a
  reviewer can still read the moves as `git mv` plus names.
- **B. Two PRs: layout, then code.** My proposal; overruled.
- **C. One PR per decision.** Five rounds of four presets for changes this small.

## Decisions

### 1. `devices/`: the production backends (author's ruling)

- `common/ethdev/` moves to `devices/ethdev/`. A new `devices/CMakeLists.txt` sets the group, and the root adds
  the directory after `common`.
- The target becomes `Aloe.Devices.Ethdev` with the alias `Aloe::Devices::Ethdev`. Fourteen CMake files name
  the old alias: four examples, the benchmark, and nine under `tests/` (`tests/shared` among them).
- `devices/` holds production backends only. The fabric stays in `fixtures/` (author's ruling: production and
  testing apart). The concept module `device` stays in `common/`.
- `tests/integration_tests/ethdev` and `tests/manual_tests/ethdev` are not under a group folder and stay.
- `scripts/check-includes.sh` (both arrays) and `scripts/check-format.sh` gain `devices`.
- Docs: `AGENTS.md` (layout table, the targets paragraph, the DPDK rule's wording), the README module table,
  `docs/architecture/device.md` and `overview.md`, `codestyle.md`'s "Module Layout".

### 2. The group for HTTP and WebSocket: `l7/` (author's ruling: "binding to OSI makes sense")

- `component/` is not on disk. It is named in one row of `AGENTS.md` and three script arrays, the same lines
  decision 1 edits.
- `l7/<module>/`, targets `Aloe::L7::<Module>`: layer-7 protocol modules built on a stream. `protocols/` was
  set aside because ARP, IP and TCP are protocols too. The code already speaks in layer numbers (`l3_offset`,
  `l4_length`, `L4Checksum`).
- Where TLS goes is decided in its own phase.

### 3. `net/ipv4/` (author's ruling)

- `common/net/stack/` moves to `common/net/ipv4/stack/`, `common/net/arp/` to `common/net/ipv4/arp/`.
- `net::Datagram<Packet>` becomes `net::Ipv4Datagram<Packet>` in `ipv4_datagram.hpp`; `net::Prebuilt` becomes
  `net::Ipv4Prebuilt` in `ipv4_prebuilt.hpp`. Users are in `net`, five `tcp` headers (`IsIp` among them), two
  test files and the net and TCP pages.
- `ArpResolution`, defined in `datagram.hpp` today, gets a header of its own beside the cache (my call).
- `SendRequest`, `SendError`, `Ipv4Config`, `Ipv4Counters` and `ArpCache` keep their names. `SendRequest` and
  `SendError` are the contract a transport is written against, which is what stays neutral when IPv6 arrives.

### 4. The fabric parses with `wire` (author's ruling: "replace")

- `fabric::detail::Ipv4Frame`, `parse_ipv4` and the constants in `fabric/detail/frame.hpp` that `wire` already
  has go. `Port::inspect` and `Port::fill_checksums` parse with `wire::EthernetHeader::parse` and
  `wire::Ipv4Header::parse`.
- What stays in the fabric is what `wire` has no reason to hold: the L4 offset and length inside the frame, the
  two ports, UDP's two offsets (there is no UDP format in `wire`) and `flow_of`.
- **One behaviour changes** (reported to the author on 2026-11-16; no objection). The fabric's parser accepts a
  frame whose total length claims more bytes than the frame carries and steers it by its flow; `wire`'s parser
  rejects it. After the change such a frame is not IPv4 to the fabric: queue 0, no hash, no checksum verdict.
  The test `FabricSteering.ATruncatedFrameIsStillSteeredAndDoesNotCrash` pins today's behaviour and is
  rewritten to expect queue 0 and no crash. The stack drops the frame either way, since `Ipv4` uses the same
  parser; only the shard that counts the drop differs.

### 5. Counters by owner (author's ruling: split; the shape and names are my calls, shown and not objected to)

- `loop::ShardQueueCounters`: `frames_received`, `frames_transmitted`, `transmit_refused`. `ShardQueue` takes
  it and writes all three. Not `loop::QueueCounters`: `device::QueueCounters` exists.
- `runtime::ShardCounters`: a `loop::ShardQueueCounters queue` member, then `ticks`, `idle_ticks`,
  `inbox_received`, `work_run`, `timers_fired` and the four `tasks_*`. `ShardContext`, `TaskScope` and
  `Runtime::counters(index)` use it.
- `frames_received` is counted by `ShardQueue::receive` instead of `runtime::Shard::step`, so a hand-written
  loop gets it too. For the runtime the count moves, the cost does not.
- A program over the bricks builds the three-field struct (the two examples, the benchmark's bricks level, the
  net and TCP fixtures, the scenario harness). A reader of runtime counters writes
  `counters.queue.frames_transmitted`.

## Not in this round

- `NodePool` to core: when a second module needs a fixed pool (TODO in `common/tcp/pool/detail/node_pool.hpp`,
  which rides with this PR).
- A two-shard example over the bricks that forwards ARP through an `Inbox`. Offered; not asked for.
- A script check that nothing under `common/` or `fixtures/` names `Aloe::Dpdk`. Offered; not asked for.

## Left to the builder

- The branch name, the PR title under the tag rule, the order of commits.
- Whether the fabric's remaining L4 helpers live in `fabric.cpp` or a slim `detail` header.
- File names for the two counters structs, within the unique-basename rule.

## Verification

- The four presets and both check scripts on the finished branch.
- The commits that move and rename edit no test beyond renamed names and paths; the test count is unchanged
  across the whole PR.
- `gcc-debug` built with `--clean-first`, since a struct the scenario binaries compile in changes layout (the
  stale-objects trap in `AGENTS.md`).
- The 64-byte tick of `tick_to_trade` on the branch against `main`, as far as it runs without root.
