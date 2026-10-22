# Baseline Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the tick-to-trade and gateway baseline benchmark of roadmap phase 2, step 1: a standalone binary under `benchmarks/` that measures both rounds at the floor, at level 0 (the loop over the bricks) and at level 1 (the runtime) over DPDK's ring driver with a scripted venue peer on a second core, reports per-message distributions, writes a result file, and records the first results in the docs.

**Architecture:** Two `ethdev::Port`s over a pair of cross-connected single-producer single-consumer rings built with `rte_eth_from_rings`; the stack side is wrapped in a `StampedDevice` whose `transmit` takes the stop stamp when the application has armed it; a `VenuePeer` on its own thread scripts the handshake and the rounds with `frames::TcpPeer`, recycling the stack's mbufs so the stack core's pool gets and puts stay balanced; three level functions run the same application work over nothing, over the bricks and over `runtime::Runtime`; raw TSC deltas go into preallocated arrays and are summarised into nearest-rank percentiles after the run. Hugepages are reserved by the process when it has the privilege.

**Tech Stack:** C++26 on libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest for the harness's tests, the existing vcpkg dependencies, DPDK through `Aloe::Dpdk` (the benchmark names `rte_eth_from_rings`, `rte_ring_create` and `rte_ring_free` directly, as `tests/shared` names `rte_dev_probe`), Python 3 standard library for the comparison script. Google Benchmark is not linked. No new dependency.

**Spec:** [Baseline benchmark design](../specs/2026-10-20-baseline-benchmark-design.md). Read the spec, `AGENTS.md` and `codestyle.md` before implementation. This plan is the first of the spec's two pull requests, "The baseline"; the loads and the isolated round are the second pull request and get their own plan once the first results exist. Both documents live in the git-ignored `docs/superpowers/` directory: copy them to the machine that executes this plan beside the repository, and never reference them from a tracked file.

## Global Constraints

- "Nothing in `common/` or `fixtures/` changes for the benchmark except where a defect is found; such a fix is its own commit with its own test."
- "The stamps live in the benchmark's own wrapper device and loop, never in a brick."
- "Mbufs are recycled, never freed across cores": the peer writes every frame it sends into the mbuf the stack's last frame arrived in; the stack core gets from and puts into its own pool in balance.
- "A run fails, with a message, when" a stack `dropped_*` counter is not zero, the peer saw an order out of sequence, with a bad checksum or with the wrong round number, a round took longer than the deadline, or the EAL refused to start. "A failed run writes no result file."
- "Every number is a per-message distribution: p50, p90, p99, p99.9 and max, with the count, never a mean."
- "The benchmark never reserves hugepages ... when it has no privilege, it refuses with the exact command to run"; "It never falls back to `--no-huge` on its own; `--no-huge` reaches the EAL only when passed explicitly after `--`, and the result file records which it was."
- Default topology: the stack on one core, the peer on a second; `--same-core` for the floor and level 0 only; level 1 is always cross-core.
- Variables: tick payloads 64, 256, 1024, 1448 and 1460 bytes; orders 128 bytes; the round number in the first 8 bytes of every tick and order, little-endian; 100 000 measured rounds after 10 000 of warm-up; `--gap` default 0.
- The benchmark's own code may name DPDK where it builds the ring pair and registers threads; everything else goes through `ethdev::Port`. Only `common/execution/execution/execution.hpp` names `stdexec::` or `exec::`; the level 1 code uses `aloe::runtime::task<void>` and `co_await` only.
- Namespace `aloe::benchmarks` for everything under `benchmarks/tick_to_trade/`; another module's names are qualified (`ethdev::Port`, `frames::TcpPeer`); test helpers in `aloe::testing`.
- `codestyle.md` applies: by-value parameters `const` in definitions, never in bodiless declarations; `[[nodiscard]]` on every value-returning function; every struct member has a default member initialiser (clang's `-Wmissing-designated-field-initializers` is an error); no integer literal suffixes beyond `U` on shift counts, `std::uint64_t{n}` for a typed literal; one class in one place with members defined in the class body; `get_` only for atomic or barrier reads.
- Public headers define no macros. The one compile definition this plan adds, `ALOE_BENCHMARK_REVISION`, is set on the executable target and read in `main.cpp` only.
- `std::expected` on hot paths, exceptions on setup paths. Nothing on a measured path allocates, locks, logs or reads a clock other than `core::TscClock::get_ticks()` for a stamp and `core::Clock::now()` where the stack's loop needs its stamp.
- Tests under `tests/` need no root, no hugepages and no network card: every benchmark test starts the EAL through `aloe::testing::EalEnvironment` with `--no-huge`, and pins nothing.
- Completion gates: `debug`, `gcc-debug`, `asan` and `tsan` build and pass (the `tsan` preset turns benchmarks off, so it verifies that the gate still passes without them), `./scripts/check-format.sh` passes, and clang-tidy, which runs inside the clang builds with warnings as errors, is clean.
- Conventional commit subjects with the module as scope: `feat(benchmarks): ...`, `test(benchmarks): ...`, `build(benchmarks): ...`, `docs: ...`.

## Verified Beforehand

Nothing in this plan was built end to end; the author asked for plans without a prototype. These facts were read from the code, the DPDK 26.03 sources under `vcpkg/buildtrees/dpdk/src/v26.03-*/` and the machine on 2026-10-19 and 2026-10-20, some of them by the builder-check reviewer of the spec.

| Fact                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | Where                                                                                                                                                                       |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `net_ring` creates its rings with `RING_F_SP_ENQ \| RING_F_SC_DEQ`, 1024 deep; `eth_ring_rx` and `eth_ring_tx` are `rte_ring_dequeue_burst` and `rte_ring_enqueue_burst` of mbuf pointers with plain counters; `rx_offload_capa` is `RTE_ETH_RX_OFFLOAD_SCATTER` and `tx_offload_capa` is `RTE_ETH_TX_OFFLOAD_MULTI_SEGS`, so no checksum offload. A vdev's queue loops its transmit back onto its own receive (`rxtx` rings for both).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | `drivers/net/ring/rte_eth_ring.c:78-106,190-191,556-563`                                                                                                                    |
| `int rte_eth_from_rings(const char* name, struct rte_ring* const rx_queues[], unsigned nb_rx_queues, struct rte_ring* const tx_queues[], unsigned nb_tx_queues, unsigned numa_node)` returns the port id or -1; the port is attached (`DEV_ATTACH`), so closing it does not free the rings. The header is `<rte_eth_ring.h>`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | `rte_eth_ring.h:32-37`, `rte_eth_ring.c:463-470`                                                                                                                            |
| `ethdev::Port{PortConfig{.name, .queues, .mtu, .descriptors, .pool_size}}` looks the port up by name with `rte_eth_dev_get_port_by_name`, configures `RTE_ETH_MQ_RX_NONE` when the driver has no RSS, tolerates `-ENOTSUP` from `set_mtu`, creates one mempool per queue with `rte_pktmbuf_pool_create(name, pool_size, min(RTE_MEMPOOL_CACHE_MAX_SIZE, pool_size / 2), 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket)`, and exposes `queue_count`, `mac`, `mtu`, `link_up`, `capabilities`, `steering`, `allocate(queue)`, `receive(queue, span)`, `transmit(queue, span)`, `counters(queue)`, `port_id`, `driver_name`; `max_burst` is 64. A port from `rte_eth_from_rings` has an all-zero MAC.                                                                                                                                                                                                                                                                                                                                                                                                                          | `common/ethdev/port/port.hpp`, `port.cpp:105,159-184`                                                                                                                       |
| `ethdev::Packet` is a move-only handle over one `rte_mbuf` with `data()`, `size()`, `empty()`, `headroom()`, `tailroom()`, `prepend(n)`, `append(n)` (both `std::optional<std::span<std::byte>>`), `trim_front(n)`, `trim_back(n)`, `rx()`, `tx()`, `set_tx`, `get()`, `release()`. `trim_back(size())` empties a packet without moving its data offset, so a recycled mbuf keeps its headroom.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | `common/ethdev/packet/ethdev_packet.hpp`                                                                                                                                    |
| `ethdev::Eal{std::span<const std::string>}` starts the EAL once per process; `ethdev::register_thread()` gives the calling thread an lcore id so mempool caches work, throws `EthdevError` when no slot is left, and never unregisters.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | `common/ethdev/eal/eal.hpp`                                                                                                                                                 |
| DPDK's per-thread mempool cache does not amortise a one-way flow: `rte_mempool_do_generic_get` refills with a bulk dequeue of `cache->size + 1` objects when the cache runs dry, and `rte_mempool_do_generic_put` flushes the whole cache when it passes its flush threshold (1.5 × size). With `pool_size` 4096 the cache is 512, so a thread that only gets refills 513 objects every 512 gets and a thread that only puts flushes 768 every 768 puts.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | `rte_mempool.h` (installed copy under `build/<preset>/vcpkg_installed/x64-linux-clang/include/`), `rte_mempool_do_generic_get`, `rte_mempool_do_generic_put`                |
| `tcp::Stack::apply_ack_policy` calls `ack_now` on every second full-sized segment; `ack_now` allocates and transmits a pure ACK inside `process`, before the application runs. At a payload of 1460 (the MSS) every other round carries that ACK inside the window; at 1448 it does not. **Corrected by Task 6:** the count resets whenever the stack sends any ACK, and every order carries one, so the closed-loop tick round never triggers the rule and the 1460 row is clean.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | `common/tcp/stack/tcp_stack.hpp:620`                                                                                                                                        |
| With RSS off, `tcp::Stack::connect` places an active open on queue 0 only (`choose_port` returns `Unplaceable` for any other queue). This plan opens one connection on queue 0 in every configuration.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | `common/tcp/stack/tcp_stack.hpp:157`                                                                                                                                        |
| `tcp::Stack<Ip>{ip, wheel, TcpConfig}`: `listen(port)` → `std::expected<void, ListenError>`; `connect(Endpoint, now)` → `std::expected<ConnectionType*, ConnectError>`; `process(span<net::Datagram<Packet>>, now)`; `poll_event()` → `std::optional<ConnectionEvent<Ip>>` with `connection` and `events`; `flush(now)`; `counters()` → `TcpCounters` with `dropped_bad_header`, `dropped_bad_checksum`, `dropped_no_connection`, `dropped_closed`, `dropped_unexpected`, `dropped_duplicate`, `dropped_out_of_order`, `dropped_out_of_window`, `dropped_no_slot`, `dropped_no_node`, `dropped_table_full`, `send_refused`, `commits_refused`, `allocation_failures`, `pure_acks_sent`, `data_segments_sent`. `TcpConfig` defaults: 1024 connections, 8 listeners, 32 receive segments, 2048 receive-pool nodes.                                                                                                                                                                                                                                                                                                   | `common/tcp/stack/tcp_stack.hpp`, `tcp_config.hpp`, `tcp_counters.hpp`                                                                                                      |
| `tcp::Connection<Ip>`: `unread()` → `View` (a forward range of `std::span<const std::byte>` chunks whose `size()` counts bytes, with `front()` and `empty()`), `consume(n)`, `writable()`, `prepare(n)` → `std::optional<std::span<std::byte>>`, `commit(n)` → `bool`, `committed()`, `acknowledged()` (both `wire::TcpSequence`, `==` defaulted), `state()`, `release()`. `stream::Events` has `connected()`, `accepted()`, `readable()`, `writable()`, `acked()`, `peer_closed()`, `closed()`, `reset()`, `timed_out()`, `unresolved()`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | `common/tcp/stack/tcp_connection.hpp`, `common/tcp/buffer/read_view.hpp`, `common/stream/stream/stream_events.hpp`                                                          |
| `net::Ipv4<Device>{ShardQueue<Device>&, Ipv4Config}` with `Ipv4Config{address, prefix, gateway, ttl, burst_capacity, arp_capacity, arp_reachable (60 s), arp_expire (120 s), arp_request_interval, accept_unsolicited_replies}`; `process(span<Packet>, now)`, `received(wire::Ipv4Protocol)` → `std::span<net::Datagram<Packet>>`, `learn(address, mac, now)`, `counters()` → `Ipv4Counters` whose drop counters are `dropped_ethertype`, `dropped_arp_malformed`, `dropped_arp_conflict`, `dropped_arp_unsolicited`, `dropped_bad_header`, `dropped_bad_checksum`, `dropped_fragment`, `dropped_protocol`, `dropped_icmp`, `dropped_martian`, `dropped_tcp_broadcast`, plus `send_no_route`, `send_unresolved`, `send_oversized`, `send_refused`. The ARP cache uses a stale entry while a refresh is out between `reachable` and `expire`.                                                                                                                                                                                                                                                                      | `common/net/stack/ipv4.hpp:33-45,78-100,144-172,259`, `common/net/arp/arp_cache.hpp:17-18`                                                                                  |
| `loop::ShardQueue<Device>{Device&, index, ring_capacity, ShardCounters&}` with `receive(span)`, `transmit(Packet&&)` (moves only on success), `flush()`, `discard()`, `allocate()`, `mac()`, `index()`, `capabilities()`, `steering()`, `queue_count()`; `ShardCounters` has `frames_received`, `frames_transmitted`, `transmit_refused`. `loop::TimerWheel{core::Duration, core::TimePoint}` with `advance(now)` and `pending()`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | `common/loop/queue/shard_queue.hpp`, `common/loop/counters/counters.hpp`, `common/loop/timer/timer_wheel.hpp:64-80`                                                         |
| `device::IsDevice` requires `Packet`, `queue_count()`, `mac()`, `mtu()`, `link_up()`, `capabilities()`, `steering()`, `allocate(queue)`, `receive(queue, span)`, `transmit(queue, span)`, `counters(queue)`; `tests/shared/tcp/tcp_test_device.hpp` is a forwarding wrapper of that shape.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | `common/device/device/device.hpp`, `tests/shared/tcp/tcp_test_device.hpp`                                                                                                   |
| `runtime::Runtime<Device, Stack>{RuntimeConfig, Device&, stack_args...}` builds one shard per queue and copies `stack_args` into every stack; `RuntimeConfig{ShardConfig shard; std::vector<ShardThread> threads; std::function<void()> thread_hook}`, `ShardThread{std::optional<unsigned> cpu; std::string name}`, `ShardConfig{timer_resolution 1 ms, receive_burst 64, transmit_ring 512, idle (Spin), yield_after}`. `start()`, `stop()`, `join()`, `spawn(index, sender)` from any thread, `scheduler(index)`, `shard(index)` (the shard's thread, or before `start` and after `join`), `counters(index)`. The constructor calls `core::calibrate_clock()`. A shard's step is receive, `on_receive`, `on_tick`, early flush, `run_once`, `on_flush`, final flush.                                                                                                                                                                                                                                                                                                                                            | `common/runtime/runtime/runtime.hpp:40-120,208-240`, `common/runtime/shard/shard.hpp:20-130`                                                                                |
| `runtime::TcpStack<Device>{ShardContext&, ShardQueue<Device>&, net::Ipv4Config, tcp::TcpConfig}` with `ip()`, `tcp()`, `streams()`; `StreamsType::accept(port)` and `connect(tcp::Endpoint)` are senders completing with `std::expected<Stream, stream::Error>`; `Stream` has `unread()`, `consume`, `prepare`, `commit`, `committed()`, `acknowledged()`, `readable(count)` (completes with `std::expected<std::size_t, stream::Error>`), `acked(Sequence)` (completes with `std::expected<void, stream::Error>`), `close()`, `release()`; a task is `aloe::runtime::task<void>`, spawned with `runtime.spawn(index, task)`. `Streams::wake` runs in `on_tick` and pushes completions onto the run queue, so a task resumed by `readable` commits in the same tick and its segment leaves in that tick's final flush.                                                                                                                                                                                                                                                                                             | `common/runtime/stack/tcp_runtime_stack.hpp`, `common/runtime/stream/stream_handle.hpp`, `stream_policies.hpp:35-100`, `examples/tcp_echo_tasks/tcp_echo_tasks.cpp:140-243` |
| `frames::TcpPeer{TcpSpec, wire::TcpSequence initial}` with `syn()`, `syn_ack(seen)`, `ack(seen)`, `ack()`, `data(bytes)`, `fin()`, `segment(flags, payload, seq, ack, with_mss)`, `see(ParsedFrame)` (a SYN without ACK makes the peer adopt the stack's ports), `snd_nxt()`, `rcv_nxt()`, `snd_una()`, `spec()`. `TcpSpec{destination_mac, source_mac, source, destination, source_port, destination_port, sequence, acknowledgement, flags, window (65535), mss (optional), checksums}`. `frames::parse_frame(span)` → `std::optional<ParsedFrame{ethernet, arp, ipv4, icmp, tcp, ipv4_header, l4}>`; `frames::tcp_payload(parsed)`; `frames::l4_checksum_residue(ipv4, l4)` is 0 for a correct TCP checksum; `wire::internet_checksum(ipv4_header)` is 0 for a correct IPv4 header; `frames::fill(packet, bytes)` appends into an empty packet; `frames::tcp_frame(TcpSpec, payload)`; `frames::arp_reply(sender_mac, sender_ip, target_mac, target_ip)` and `frames::arp_frame(arp, destination_mac, source_mac)` build an ARP reply frame; `frames::ethernet_frame(destination, source, ethertype, payload)`. | `fixtures/frames/frames/tcp_peer.hpp`, `tcp_frames.hpp:20-91`, `net_frames.hpp:22-146`, `frames.hpp:32,155-170`                                                             |
| `core::TscClock::get_ticks()` is `lfence; rdtsc`, public and `noexcept`; `core::TscClock::scale()` calibrates once and returns `const core::TscScale&` whose `nanoseconds(ticks)` converts; `core::calibrate_clock()` wraps the first call. The clock refuses a machine without an invariant TSC or with a clocksource other than `tsc`. The headers are `<clock.hpp>`, `<tsc_clock.hpp>`, `<tsc_scale.hpp>` on `Aloe::Common::Core`'s include path.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | `common/core/clock/clock.hpp`, `tsc_clock.hpp:78-108`, `tsc_scale.hpp:45`                                                                                                   |
| The test EAL arguments are `--no-huge --no-pci --in-memory --no-telemetry -m 64 -l 0 --vdev=<vdev>`; `aloe::testing::EalEnvironment{"net_null0"}` starts them from a gtest environment; `aloe::testing::probe_vdev` is not needed by this plan. The shared targets are `${SHARED_TESTING_TARGET}.Ethdev` (EAL environment, links `Aloe::Common::Ethdev`) and `${SHARED_TESTING_TARGET}.Dpdk`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | `tests/shared/dpdk/eal_arguments.hpp`, `eal_environment.hpp`, `tests/shared/CMakeLists.txt`                                                                                 |
| `add_unit_test`, `add_integration_test`, `add_functional_test` take sources positionally and `LINK_LIBS`; `${UNIT_TESTING_TARGET}`, `${INTEGRATION_TESTING_TARGET}`, `${FUNCTIONAL_TESTING_TARGET}`, `${TEST_LIBS}` are set in `tests/CMakeLists.txt`; functional tests may also be plain `add_test` commands with the `functional` label. `tests/unit_tests/CMakeLists.txt` adds `common` and `fixtures`; `tests/integration_tests/CMakeLists.txt` adds `dpdk ethdev runtime net tcp`; `tests/functional_tests/CMakeLists.txt` adds `bootstrap dpdk hooks`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       | `cmake/tests.cmake`, `tests/*/CMakeLists.txt`                                                                                                                               |
| `benchmarks/CMakeLists.txt` sets `BENCHMARKS` to `Aloe.Benchmarks` and adds no target; the root adds `benchmarks/` and `find_package(benchmark)` under `DO_BENCHMARKS`, which is ON by default and OFF in the `ci` and `tsan` presets. The root adds `tests/` before `benchmarks/`, which CMake allows because link targets resolve at generate time. `KEEP_FRAME_POINTERS` and the `release-perf` preset exist. Examples name their targets `${EXAMPLES}.TcpEcho` and link `Aloe::Common::Tcp Net Loop Ethdev`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | `benchmarks/CMakeLists.txt`, `CMakeLists.txt:148-156`, `CMakePresets.json`, `examples/tcp_echo/CMakeLists.txt`                                                              |
| `docs/superpowers/` is git-ignored (`.gitignore:117`); public docs are `docs/architecture/*.md`, `docs/roadmap.md` (step 1 of phase 2 at lines 63-64 still says "on the fabric"), `docs/guides/getting-started.md` (sections "Configure and build" and "Options"), `README.md` ("Building" says tests need no hugepages). `docs/architecture/overview.md` has a status paragraph under "The shard, as a rule" that mentions the two-core shard as deferred.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | `.gitignore`, `docs/`                                                                                                                                                       |
| The author's workstation: AMD Ryzen 5 7600X, 6 cores, 12 threads (SMT sibling pairs 0/6, 1/7, 2/8, 3/9, 4/10, 5/11), one CCX, performance governor, boost on, no `isolcpus` or `nohz_full`, kernel clocksource `tsc`, `constant_tsc nonstop_tsc rdtscp` present, no hugepages reserved (`/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages` is 0, root-writable, mode 0644), perf 7.1 at `perf_event_paranoid=1`, `taskset` and `chrt` present. The hugepage directory holds `nr_hugepages`, `free_hugepages`, `resv_hugepages`, `surplus_hugepages`, `nr_overcommit_hugepages`, `nr_hugepages_mempolicy`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | `lscpu`, `/proc/cmdline`, `/sys`                                                                                                                                            |
| `python3` is present on the workstation; the repository has no Python yet (`scripts/` holds `check-format.sh` and `install-linux.sh`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | `scripts/`                                                                                                                                                                  |

Two claims are not verified and the plan verifies them where they first matter:

- **Probe A (Task 2):** `rte_eth_from_rings` ports wrap in `ethdev::Port` and carry frames both ways under the test EAL arguments. The ring-pair integration test is this probe.
- **Probe B (Task 11):** an unprivileged process uses hugepages someone else reserved with `--in-memory` (DPDK maps them through `memfd_create(MFD_HUGETLB)`). The result decides one sentence of the docs.

## Read This First

Baseline commands, on a checkout of `main` at or after commit 89785ad:

```bash
git switch -c benchmarks/baseline
cmake --preset debug && cmake --build --preset debug && ctest --preset debug      # 42+ tests pass today
./scripts/check-format.sh
```

Code-base traps that bite this plan in particular; the full list is in `AGENTS.md`:

- `loop::ShardQueue::transmit(Packet&&)` moves the packet only on success; clang-tidy's `bugprone-use-after-move` flags correct code that uses it after a refused transmit. Mark such lines `NOLINT(bugprone-use-after-move)` with the reason.
- clang-tidy's `readability-make-member-function-const` fires on a member that only writes through a pointer; call `core::force_non_const(this)` first in a wrapper that owns what the pointer reaches.
- A `std::vector<Packet>` burst must have every slot empty before `receive` writes into it; `ethdev::Port::receive` asserts it.
- `frames::fill` appends into an *empty* packet; recycle a packet with `trim_back(packet.size())` first.
- clang's `-Wmissing-designated-field-initializers` is an error: every struct member gets a default member initialiser, and designated initialisers may then name only what they change.
- GCC's `-Wshadow` warns when a constructor parameter shadows a member function; rename the parameter.
- A type with a `std::atomic` member is immovable; `RunState` is one, so it lives in a `std::unique_ptr` or on the stack of the function that owns the configuration.
- `std::move` on a trivially copyable sender fails clang-tidy; pass senders directly.
- Inside a class template, a call to a member template of a dependent object needs `.template`.
- `std::atomic<bool>::wait(false)` blocks the main thread without spinning, which matters because the EAL's `-l` pins the main thread to the same core the level 1 shard thread spins on.
- The ring port's MAC is all zeros. The peer addresses frames to `stack_port.mac()`, whatever it is; `Ipv4::process` accepts them.

## How to Execute and Review

Use superpowers:subagent-driven-development (preferred) or superpowers:executing-plans. One task at a time, in order; each ends with every listed test passing and a commit.

Per task, a fresh reviewer checks:

1. **Spec conformance.** The task's interfaces match this plan's Interfaces blocks exactly; the behaviour matches the spec sections the task cites; nothing under `common/` or `fixtures/` changed unless the task says so.
2. **Repository rules.** `AGENTS.md` Rules and `codestyle.md`: namespaces, qualified cross-module names, `const` by-value parameters in definitions only, `[[nodiscard]]`, default member initialisers, no literal suffixes, one class in one place, no macros in headers, no DPDK names outside `ring_pair.cpp`, `thread_pinning.hpp` and `benchmark_run.cpp`, no `stdexec::`, no `quill::`, no logging on a measured path.
3. **Measurement hygiene.** No allocation, lock, system call or clock read other than `TscClock::get_ticks()` between a round's start stamp and its stop stamp; the application work is the same at every level; the recycling rule holds (the peer never frees a stack mbuf except the documented surplus, never allocates on the measured path).
4. **Tests.** Each test pins a behaviour that could regress; no test needs root, hugepages, a card or a pinned core; `ctest --preset debug` passes with the new tests, and `-L unit`, `-L integration`, `-L functional` each still pass.
5. **Build.** `cmake --build --preset debug` is warning-free (clang-tidy runs inside it), `./scripts/check-format.sh` passes, and from Task 1 on `cmake --preset tsan && cmake --build --preset tsan` still configures and builds with `DO_BENCHMARKS=OFF`.

At the end, a whole-branch review against the spec's "Done when" for the first pull request, then the completion gates of Task 12, then the pull request titled `[BENCHMARKS][DOCS] Baseline benchmark: tick-to-trade and the gateway round on DPDK's ring driver`.

## Review Focus

Five conditions the spec implies but no task's tests exercise directly, most likely to bite first; each has its pin added to the owning task:

1. **A stop stamp with no start stamp, or a start with no stop.** A pure ACK transmitted while the device is armed, or a round the peer never answered, would leave a hole or a negative delta. Task 10's `summarise_round` refuses a run with any zero or inverted pair and names the round. Pinned in Task 10's integration test by a synthetic `Stamps` with one hole.
2. **TSC not synchronised across the two cores.** A cross-core delta is meaningless then. Task 10 fails the run when any cross-core delta is negative, with a message naming the cores. Pinned by the same synthetic test with an inverted pair.
3. **The 1460 row's second-segment ACK.** The peer must skip pure ACKs when matching orders, and the device must not stamp them. Task 5's `await_order` test feeds a pure ACK before the order; Task 3's `StampedDevice` test transmits unarmed first.
4. **A fragmented machine reserving fewer hugepages than asked.** Task 8's reservation reads the count back and refuses with the shortfall; pinned with a fake sysfs that accepts half.
5. **A stale ARP entry in a long run.** A 100 000-round run at 1 µs is 0.1 s, but `--gap` can stretch it past 60 s. Task 6 and Task 7 configure `arp_reachable` and `arp_expire` to 24 and 48 hours; pinned by a unit assertion on the `Ipv4Config` the levels build (Task 6's `stack_ip_config()` test).

---

## File Structure

Everything new lives under `benchmarks/tick_to_trade/`, on one flat include path like a module, namespace `aloe::benchmarks`. The harness is a static library the executable and the tests link; `main.cpp` is the only file outside it.

| File                                                                                                                       | Responsibility                                                                                                                           |
|----------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------|
| `benchmarks/tick_to_trade/CMakeLists.txt`                                                                                  | `Aloe.Benchmarks.TickToTrade.Harness` (STATIC) and `Aloe.Benchmarks.TickToTrade` (executable).                                           |
| `round_config.hpp`                                                                                                         | `Round`, `Level`, `Topology`, `RoundConfig`, the size constants, `to_string`.                                                            |
| `distribution.hpp`                                                                                                         | `Summary` and `Distribution`: raw tick samples, nearest-rank percentiles in nanoseconds.                                                 |
| `application_work.hpp`                                                                                                     | The work identical at every level: `read_round`, `write_round`, `fold`, `write_order`, `tick_payload`.                                   |
| `stamped_device.hpp`                                                                                                       | `StampedDevice<Device>`: the forwarding wrapper that stamps an armed transmit.                                                           |
| `run_state.hpp`                                                                                                            | `Stamps`, `Phase`, `RunState`: the per-configuration arrays and flags.                                                                   |
| `thread_pinning.hpp`                                                                                                       | `pin_current_thread(cpu)`.                                                                                                               |
| `ring_pair.hpp`, `ring_pair.cpp`                                                                                           | `RingPair`: two ports over cross-connected rings; names DPDK.                                                                            |
| `venue_peer.hpp`, `venue_peer.cpp`                                                                                         | `PeerConfig`, `PeerCounters`, `PeerError`, `VenuePeer`: the scripted venue over its port.                                                |
| `level_floor.hpp`, `level_floor.cpp`                                                                                       | `run_floor`: raw frames, both rounds, both topologies.                                                                                   |
| `level_bricks.hpp`, `level_bricks.cpp`                                                                                     | `stack_ip_config`, `run_bricks`: the loop over the bricks, both rounds, both topologies, breakdown.                                      |
| `level_runtime.hpp`, `level_runtime.cpp`                                                                                   | `run_runtime`: the runtime with one pinned shard and a task, both rounds, cross-core.                                                    |
| `hop.hpp`, `hop.cpp`                                                                                                       | `measure_hop`: the cache-line ping-pong.                                                                                                 |
| `benchmark_options.hpp`, `benchmark_options.cpp`                                                                           | `Options`, `parse_options`, `usage`.                                                                                                     |
| `hugepages.hpp`, `hugepages.cpp`                                                                                           | `HugepageSysfs`, `FileHugepageSysfs`, `HugepageError`, `HugepageReservation`.                                                            |
| `machine_info.hpp`, `machine_info.cpp`                                                                                     | `MachineInfo`, `probe_machine`.                                                                                                          |
| `report.hpp`, `report.cpp`                                                                                                 | `Row`, `RunInfo`, `Report`, `write_json`, `print_table`.                                                                                 |
| `benchmark_run.hpp`, `benchmark_run.cpp`                                                                                   | `PeerWorker`, `summarise_round`, `run_benchmark`: the matrix over the harness.                                                           |
| `main.cpp`                                                                                                                 | Options, hugepages, EAL, run, print, write.                                                                                              |
| `tests/unit_tests/benchmarks/CMakeLists.txt` and `test_*.cpp`                                                              | `Aloe.Tests.Unit.Benchmarks`: distribution, application work, stamped device (over the fabric), options, hugepages (fake sysfs), report. |
| `tests/integration_tests/benchmarks/CMakeLists.txt`, `test_ring_pair.cpp`, `test_venue_peer.cpp`, `test_tick_to_trade.cpp` | `Aloe.Tests.Integration.Benchmarks.RingPair`, `...VenuePeer`, `...TickToTrade`: under the EAL environment, `--no-huge`, unpinned.        |
| `tests/functional_tests/benchmarks/CMakeLists.txt`, `fixtures/*.json`                                                      | `Aloe.Tests.Functional.Benchmarks.Compare.*`: the comparison script on fixture result files.                                             |
| `scripts/compare-benchmark.py`                                                                                             | Two result files side by side, deltas per percentile, a threshold.                                                                       |
| `docs/benchmarks/tick-to-trade.md`, `docs/benchmarks/results/`                                                             | The benchmark page and the committed result files.                                                                                       |
| `docs/guides/getting-started.md`, `docs/roadmap.md`, `docs/architecture/overview.md`, `README.md`, `AGENTS.md`             | The edits of Task 12.                                                                                                                    |

Execution and validation conventions used by every task:

- Build and run one test binary: `cmake --build --preset debug --target <Target> && ./build/debug/<path>/<Target>`; the paths are `tests/unit_tests/benchmarks/`, `tests/integration_tests/benchmarks/`, `benchmarks/tick_to_trade/`.
- "Expected: FAIL" on a first run means a compile error on a missing symbol or a failing assertion, whichever the step says; "Expected: PASS" means the binary exits 0 with every test listed as `[  PASSED  ]`.
- Every code step's signatures are exact; the implementer writes the bodies except where a body is given, which happens where the algorithm is not determined by the signature and the tests, or where the text is fixed by the spec.

---

## Task 1: Harness skeleton, round configuration and the distribution recorder

**Files:**
- Create: `benchmarks/tick_to_trade/CMakeLists.txt`
- Modify: `benchmarks/CMakeLists.txt` (add the subdirectory after the `set(BENCHMARKS ...)` line)
- Create: `benchmarks/tick_to_trade/round_config.hpp`
- Create: `benchmarks/tick_to_trade/distribution.hpp`
- Create: `tests/unit_tests/benchmarks/CMakeLists.txt`
- Create: `tests/unit_tests/benchmarks/test_distribution.cpp`
- Modify: `tests/unit_tests/CMakeLists.txt` (gate a `benchmarks` subdirectory on `DO_BENCHMARKS`)

**Interfaces:**
- Consumes: `core::TscScale::nanoseconds(ticks)` and `core::TscScale::measured(ticks, nanoseconds)` from `<tsc_scale.hpp>`.
- Produces: `aloe::benchmarks::Round`, `Level`, `Topology`, `RoundConfig`, `order_size`, `round_prefix`, `to_string` (round_config.hpp); `aloe::benchmarks::Summary`, `Distribution` (distribution.hpp); the CMake targets `Aloe.Benchmarks.TickToTrade.Harness` with alias `Aloe::Benchmarks::Harness`, and `Aloe.Tests.Unit.Benchmarks`.

- [ ] **Step 1: Write the failing test**

`tests/unit_tests/benchmarks/test_distribution.cpp`:

```cpp
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include <distribution.hpp>
#include <gtest/gtest.h>
#include <tsc_scale.hpp>

namespace {

    using aloe::benchmarks::Distribution;
    using aloe::benchmarks::Summary;

    [[nodiscard]] std::vector<std::uint64_t> one_to(const std::uint64_t count) {
        std::vector<std::uint64_t> samples(count);
        std::iota(samples.begin(), samples.end(), std::uint64_t{1});
        std::ranges::shuffle(samples, std::mt19937{42});  // NOLINT(cert-msc51-cpp): a fixed seed keeps the test reproducible
        return samples;
    }

}  // namespace

TEST(Distribution, NearestRankOverOneToOneHundred) {
    const Summary summary = Distribution::summarise_nanoseconds(one_to(100));
    EXPECT_EQ(summary, (Summary{.count = 100, .min = 1, .p50 = 50, .p90 = 90, .p99 = 99, .p999 = 100, .max = 100}));
}

TEST(Distribution, NearestRankOverOneToOneThousand) {
    const Summary summary = Distribution::summarise_nanoseconds(one_to(1000));
    EXPECT_EQ(summary, (Summary{.count = 1000, .min = 1, .p50 = 500, .p90 = 900, .p99 = 990, .p999 = 999, .max = 1000}));
}

TEST(Distribution, OneSampleIsEveryPercentile) {
    const Summary summary = Distribution::summarise_nanoseconds({7});
    EXPECT_EQ(summary, (Summary{.count = 1, .min = 7, .p50 = 7, .p90 = 7, .p99 = 7, .p999 = 7, .max = 7}));
}

TEST(Distribution, NoSamplesThrows) {
    EXPECT_THROW(std::ignore = Distribution::summarise_nanoseconds({}), std::invalid_argument);
}

TEST(Distribution, SummariseSkipsTheWarmupAndScalesTicks) {
    const aloe::core::TscScale half = aloe::core::TscScale::measured(2, 1);  // one nanosecond per two ticks
    Distribution distribution{3};
    distribution.record(1000);
    distribution.record(2000);
    distribution.record(4000);
    EXPECT_EQ(distribution.size(), 3U);
    const Summary summary = distribution.summarise(half, 1);
    EXPECT_EQ(summary.count, 2U);
    EXPECT_EQ(summary.min, 1000U);
    EXPECT_EQ(summary.p50, 1000U);
    EXPECT_EQ(summary.max, 2000U);
}

TEST(Distribution, SummariseWithEverythingSkippedThrows) {
    Distribution distribution{2};
    distribution.record(1);
    distribution.record(2);
    EXPECT_THROW(std::ignore = distribution.summarise(aloe::core::TscScale::measured(1, 1), 2), std::invalid_argument);
}
```

- [ ] **Step 2: Add the build skeleton**

`benchmarks/CMakeLists.txt` becomes:

```cmake
set(BENCHMARKS ${PROJECT_NAME}.Benchmarks)

# Google Benchmark is resolved by the root CMakeLists.txt, as benchmark::benchmark, for microbenchmarks that may
# come later. The tick-to-trade benchmark records its own distributions and does not link it.
add_subdirectory(tick_to_trade)
```

`benchmarks/tick_to_trade/CMakeLists.txt`:

```cmake
##############################################################################
# The tick-to-trade benchmark: the harness its tests link, and the binary (Task 11)
##############################################################################
set(TICK_TO_TRADE ${BENCHMARKS}.TickToTrade)

add_library(${TICK_TO_TRADE}.Harness INTERFACE
        round_config.hpp
        distribution.hpp
)
target_include_directories(${TICK_TO_TRADE}.Harness INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${TICK_TO_TRADE}.Harness INTERFACE
        Aloe::Common::Core
)
add_library(Aloe::Benchmarks::Harness ALIAS ${TICK_TO_TRADE}.Harness)
##############################################################################
```

Check the core alias name with `grep -n "ALIAS" common/core/CMakeLists.txt` and use what it prints if it is not `Aloe::Common::Core`.

`tests/unit_tests/CMakeLists.txt` gains, after the two existing lines:

```cmake
if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
```

`tests/unit_tests/benchmarks/CMakeLists.txt`:

```cmake
##############################################################################
# The benchmark harness's pure pieces: percentiles, the application work, the stamp wrapper over a fabric port,
# the options, hugepage reservation over a fake sysfs, and the result file
##############################################################################
add_unit_test(${UNIT_TESTING_TARGET}.Benchmarks
        test_distribution.cpp
)
target_link_libraries(${UNIT_TESTING_TARGET}.Benchmarks
        PRIVATE
        Aloe::Benchmarks::Harness
        ${TEST_LIBS}
)
##############################################################################
```

From Task 2 on the harness links `Aloe::Common::Ethdev`, so this unit test binary links DPDK whole, about 200 MB in
debug like the integration tests; none of its tests starts the EAL.

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks`
Expected: FAIL to compile, `distribution.hpp` not found.

- [ ] **Step 4: Write `round_config.hpp`**

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <clock.hpp>

namespace aloe::benchmarks {

    /// The two critical paths the benchmark measures.
    enum class Round : std::uint8_t {
        Tick,     ///< Tick-to-trade: a market-data segment in, an order out.
        Gateway,  ///< The gateway round: an order composed in place, out to the device.
    };

    /// What stands between the device and the application.
    enum class Level : std::uint8_t {
        Floor,    ///< Nothing: raw frames in, a raw frame out.
        Bricks,   ///< Level 0: the hand-written loop over the bricks.
        Runtime,  ///< Level 1: runtime tasks over the same bricks.
    };

    enum class Topology : std::uint8_t {
        CrossCore,  ///< The peer on its own core: the default.
        SameCore,   ///< Peer and stack strictly sequential on one thread: the floor and level 0 only.
    };

    inline constexpr std::size_t order_size   = 128;  ///< Bytes of every order.
    inline constexpr std::size_t round_prefix = 8;    ///< The round number, little-endian, opens every tick and order.

    /// One configuration of the matrix: everything a level function needs to run its rounds.
    struct RoundConfig {
        Round round         = Round::Tick;
        Level level         = Level::Bricks;
        Topology topology   = Topology::CrossCore;
        std::size_t payload = 64;      ///< Tick payload in bytes, at least `round_prefix`; the gateway round ignores it.
        std::size_t rounds  = 100000;  ///< Measured rounds.
        std::size_t warmup  = 10000;   ///< Rounds before them, recorded and discarded.
        core::Duration gap  = core::Duration::zero();  ///< The pause between rounds.
        bool breakdown      = false;   ///< Phase stamps, at level 0 only.

        [[nodiscard]] std::size_t total() const noexcept {
            return warmup + rounds;
        }
    };

    [[nodiscard]] constexpr std::string_view to_string(const Round round) noexcept {
        switch (round) {
            case Round::Tick: return "tick";
            case Round::Gateway: return "gateway";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view to_string(const Level level) noexcept {
        switch (level) {
            case Level::Floor: return "floor";
            case Level::Bricks: return "level0";
            case Level::Runtime: return "level1";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view to_string(const Topology topology) noexcept {
        switch (topology) {
            case Topology::CrossCore: return "cross-core";
            case Topology::SameCore: return "same-core";
        }
        return "?";
    }

}  // namespace aloe::benchmarks
```

- [ ] **Step 5: Write `distribution.hpp`**

```cpp
#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <tsc_scale.hpp>

namespace aloe::benchmarks {

    /// One metric's percentiles in nanoseconds, read from the sorted samples by the nearest-rank rule.
    struct Summary {
        std::uint64_t count = 0;
        std::uint64_t min   = 0;
        std::uint64_t p50   = 0;
        std::uint64_t p90   = 0;
        std::uint64_t p99   = 0;
        std::uint64_t p999  = 0;
        std::uint64_t max   = 0;

        friend constexpr bool operator==(const Summary&, const Summary&) noexcept = default;
    };

    /**
     * @brief The raw TSC deltas of one metric, one per round.
     *
     * `record` is the only member a measured path calls and never allocates after construction; the conversion to
     * nanoseconds and the percentiles come after the run.
     */
    class Distribution {
    public:
        explicit Distribution(const std::size_t capacity) {
            ticks_.reserve(capacity);
        }

        /// Appends one sample. The capacity given at construction is never exceeded; asserted.
        void record(const std::uint64_t ticks) noexcept {
            assert(ticks_.size() < ticks_.capacity() && "more samples than the distribution was sized for");
            ticks_.push_back(ticks);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return ticks_.size();
        }

        [[nodiscard]] std::span<const std::uint64_t> ticks() const noexcept {
            return ticks_;
        }

        /// The samples after the first `skip`, in nanoseconds by `scale`, summarised. Throws std::invalid_argument
        /// when no sample is left.
        [[nodiscard]] Summary summarise(const core::TscScale& scale, const std::size_t skip) const {
            if (ticks_.size() <= skip) {
                throw std::invalid_argument{"no samples left after the warm-up"};
            }
            std::vector<std::uint64_t> nanoseconds;
            nanoseconds.reserve(ticks_.size() - skip);
            for (const std::uint64_t ticks : std::span{ticks_}.subspan(skip)) {
                nanoseconds.push_back(scale.nanoseconds(ticks));
            }
            return summarise_nanoseconds(std::move(nanoseconds));
        }

        /// Sorts `samples` and reads percentile p as the element of 1-based rank ceil(p * count): the nearest-rank
        /// rule, which never interpolates and so never reports a latency nobody saw. Throws std::invalid_argument
        /// for no samples.
        [[nodiscard]] static Summary summarise_nanoseconds(std::vector<std::uint64_t> samples) {
            if (samples.empty()) {
                throw std::invalid_argument{"a summary needs at least one sample"};
            }
            std::ranges::sort(samples);
            const auto at_permille = [&samples](const std::uint64_t permille) {
                const std::uint64_t rank = (samples.size() * permille + 999) / 1000;
                return samples[static_cast<std::size_t>(std::max<std::uint64_t>(rank, 1) - 1)];
            };
            return Summary{.count = samples.size(),
                           .min   = samples.front(),
                           .p50   = at_permille(500),
                           .p90   = at_permille(900),
                           .p99   = at_permille(990),
                           .p999  = at_permille(999),
                           .max   = samples.back()};
        }

    private:
        std::vector<std::uint64_t> ticks_;
    };

}  // namespace aloe::benchmarks
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks`
Expected: PASS, 6 tests.

- [ ] **Step 7: Check the gate without benchmarks still configures**

Run: `cmake --preset tsan && cmake --build --preset tsan --target Aloe.Tests.Unit.Core`
Expected: configures and builds; the `benchmarks` test directory is not added (`DO_BENCHMARKS=OFF`).

- [ ] **Step 8: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/CMakeLists.txt benchmarks/tick_to_trade tests/unit_tests/CMakeLists.txt tests/unit_tests/benchmarks
git commit -m "feat(benchmarks): harness skeleton, round configuration and nearest-rank distributions"
```

---

## Task 2: Two ports over cross-connected rings (Probe A)

**Files:**
- Create: `benchmarks/tick_to_trade/ring_pair.hpp`
- Create: `benchmarks/tick_to_trade/ring_pair.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt` (the harness becomes STATIC and links the stack modules)
- Create: `tests/integration_tests/benchmarks/CMakeLists.txt`
- Create: `tests/integration_tests/benchmarks/test_ring_pair.cpp`
- Modify: `tests/integration_tests/CMakeLists.txt` (gate a `benchmarks` subdirectory on `DO_BENCHMARKS`)

**Interfaces:**
- Consumes: `ethdev::Port`, `ethdev::PortConfig`, `ethdev::Packet` from `<aloe/ethdev>`; `rte_ring_create`, `rte_ring_free`, `rte_eth_from_rings`, `rte_socket_id`, `rte_errno`, `rte_strerror`.
- Produces: `aloe::benchmarks::RingPairConfig{name, queues, ring_size, pool_size, descriptors}`; `class RingPair` with `explicit RingPair(const RingPairConfig&)`, `ethdev::Port& stack() noexcept`, `ethdev::Port& peer() noexcept`, `void drain() noexcept`; the target `Aloe.Tests.Integration.Benchmarks.RingPair`.

Spec: "Topology: two ports over cross-connected rings".

- [ ] **Step 1: Write the failing test**

`tests/integration_tests/benchmarks/test_ring_pair.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <ring_pair.hpp>

// Two ethdev ports over a pair of SPSC rings: what the peer transmits the stack receives and nothing loops back,
// so the benchmark's peer can live on another core. Real mbufs, no root, no hugepages.
namespace {

    using aloe::benchmarks::RingPair;
    using Packet = aloe::ethdev::Packet;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    constexpr aloe::wire::MacAddress alpha{0x02, 0, 0, 0, 0, 0x0a};
    constexpr aloe::wire::MacAddress beta{0x02, 0, 0, 0, 0, 0x0b};

    /// Transmits `frame` on `queue` of `port` from a fresh packet of its pool.
    void send(aloe::ethdev::Port& port, const std::span<const std::byte> frame, const std::uint16_t queue = 0) {
        auto packet = port.allocate(queue);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(port.transmit(queue, burst), 1U);
        EXPECT_TRUE(burst[0].empty()) << "an accepted packet is moved from";
    }

    /// Everything pending on `queue` of `port`, as frames, freed.
    [[nodiscard]] std::vector<std::vector<std::byte>> receive_all(aloe::ethdev::Port& port, const std::uint16_t queue = 0) {
        std::vector<std::vector<std::byte>> frames;
        std::array<Packet, 16> burst{};
        for (std::size_t got = port.receive(queue, burst); got > 0; got = port.receive(queue, burst)) {
            for (Packet& packet : std::span{burst}.first(got)) {
                frames.push_back(aloe::frames::bytes_of(packet));
                packet = Packet{};
            }
        }
        return frames;
    }

}  // namespace

TEST(RingPair, FramesCrossFromPeerToStackAndBackAndNeverLoop) {
    RingPair pair{{.name = "ring-pair-cross", .pool_size = 256}};
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(60));
    send(pair.peer(), frame);
    const auto at_stack = receive_all(pair.stack());
    ASSERT_EQ(at_stack.size(), 1U);
    EXPECT_EQ(at_stack[0], frame);
    EXPECT_TRUE(receive_all(pair.peer()).empty()) << "a vdev would loop it back; the pair must not";
    send(pair.stack(), frame);
    const auto at_peer = receive_all(pair.peer());
    ASSERT_EQ(at_peer.size(), 1U);
    EXPECT_EQ(at_peer[0], frame);
    EXPECT_TRUE(receive_all(pair.stack()).empty());
}

TEST(RingPair, NoChecksumOffloadAndNoSteering) {
    RingPair pair{{.name = "ring-pair-caps", .pool_size = 256}};
    EXPECT_FALSE(pair.stack().capabilities().tx_l4_checksum) << "the stack checksums in software on the ring";
    EXPECT_FALSE(pair.stack().capabilities().rx_l4_checksum);
    EXPECT_FALSE(pair.stack().capabilities().tx_ipv4_checksum);
    EXPECT_FALSE(pair.stack().steering().enabled);
    EXPECT_EQ(pair.stack().queue_count(), 1U);
}

TEST(RingPair, DrainFreesBothDirections) {
    RingPair pair{{.name = "ring-pair-drain", .pool_size = 256}};
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(20));
    send(pair.peer(), frame);
    send(pair.peer(), frame);
    send(pair.stack(), frame);
    pair.drain();
    EXPECT_TRUE(receive_all(pair.stack()).empty());
    EXPECT_TRUE(receive_all(pair.peer()).empty());
}

TEST(RingPair, QueuesStayApart) {
    RingPair pair{{.name = "ring-pair-queues", .queues = 2, .pool_size = 256}};
    const auto frame =
        aloe::frames::ethernet_frame(alpha, beta, aloe::frames::ethertype_experimental, aloe::frames::pattern(20));
    send(pair.peer(), frame, 1);
    EXPECT_TRUE(receive_all(pair.stack(), 0).empty());
    EXPECT_EQ(receive_all(pair.stack(), 1).size(), 1U);
}
```

`tests/integration_tests/CMakeLists.txt` gains at the end:

```cmake
if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
```

`tests/integration_tests/benchmarks/CMakeLists.txt`:

```cmake
##############################################################################
# The benchmark harness over DPDK's ring driver: the ring pair, the scripted venue and the whole run, no root
##############################################################################
add_integration_test(${INTEGRATION_TESTING_TARGET}.Benchmarks.RingPair
        test_ring_pair.cpp
)
target_link_libraries(${INTEGRATION_TESTING_TARGET}.Benchmarks.RingPair
        PRIVATE
        Aloe::Benchmarks::Harness
        Aloe::Fixtures::Frames
        ${SHARED_TESTING_TARGET}.Ethdev
        ${TEST_LIBS}
)
##############################################################################
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --preset debug && cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.RingPair`
Expected: FAIL to compile, `ring_pair.hpp` not found.

- [ ] **Step 3: Make the harness a static library**

In `benchmarks/tick_to_trade/CMakeLists.txt` replace the INTERFACE library with:

```cmake
add_library(${TICK_TO_TRADE}.Harness STATIC
        ring_pair.cpp
)
target_sources(${TICK_TO_TRADE}.Harness PRIVATE
        round_config.hpp
        distribution.hpp
        ring_pair.hpp
)
target_include_directories(${TICK_TO_TRADE}.Harness PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${TICK_TO_TRADE}.Harness PUBLIC
        Aloe::Common::Core
        Aloe::Common::Device
        Aloe::Common::Ethdev
        Aloe::Common::Loop
        Aloe::Common::Net
        Aloe::Common::Stream
        Aloe::Common::Tcp
        Aloe::Common::Runtime
        Aloe::Common::Wire
        Aloe::Fixtures::Frames
        Threads::Threads
)
add_library(Aloe::Benchmarks::Harness ALIAS ${TICK_TO_TRADE}.Harness)
```

Later tasks append their `.cpp` files to the first list and their headers to the second. Check each alias with `grep -rn "ALIAS" common/*/CMakeLists.txt fixtures/*/CMakeLists.txt`.

- [ ] **Step 4: Write `ring_pair.hpp`**

```cpp
#pragma once

#include <aloe/ethdev>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct rte_ring;

namespace aloe::benchmarks {

    struct RingPairConfig {
        std::string name          = "aloe-bench";  ///< Prefix of the ring and port names; unique within the process.
        std::uint16_t queues      = 1;
        unsigned ring_size        = 1024;          ///< Frames one direction holds; a power of two, as the ring driver's.
        std::uint32_t pool_size   = 4096;          ///< Mbufs per queue on each port.
        std::uint16_t descriptors = 1024;
    };

    /**
     * @brief Two Ethernet ports over cross-connected single-producer single-consumer rings.
     *
     * Per queue, `to_stack` carries what the peer transmits and the stack receives, and `to_peer` the reverse, so
     * nothing loops back onto its sender as a `net_ring` vdev's queue would. Both ports are built with
     * `rte_eth_from_rings` and wrapped in `ethdev::Port`, so the stack side is the `Port` a card gets: real mbufs,
     * software checksums, `max_burst` of 64. The rings outlive the ports and are freed last, after a drain.
     */
    class RingPair {
    public:
        explicit RingPair(const RingPairConfig& config);
        RingPair(const RingPair&)            = delete;
        RingPair& operator=(const RingPair&) = delete;
        RingPair(RingPair&&)                 = delete;
        RingPair& operator=(RingPair&&)      = delete;
        ~RingPair();

        [[nodiscard]] ethdev::Port& stack() noexcept {
            return *stack_;
        }

        [[nodiscard]] ethdev::Port& peer() noexcept {
            return *peer_;
        }

        /// Receives and frees every frame still on either direction of every queue, from the calling thread. Between
        /// configurations, with no stack alive, so the next stack never counts a stale frame as a drop.
        void drain() noexcept;

    private:
        void release() noexcept;

        std::vector<rte_ring*> to_stack_;
        std::vector<rte_ring*> to_peer_;
        std::unique_ptr<ethdev::Port> stack_;
        std::unique_ptr<ethdev::Port> peer_;
    };

}  // namespace aloe::benchmarks
```

- [ ] **Step 5: Write `ring_pair.cpp`**

```cpp
#include "ring_pair.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <stdexcept>

#include <rte_errno.h>
#include <rte_eth_ring.h>
#include <rte_lcore.h>
#include <rte_ring.h>

namespace aloe::benchmarks {

    namespace {

        [[nodiscard]] rte_ring* create_ring(const std::string& name, const unsigned size) {
            rte_ring* ring =
                rte_ring_create(name.c_str(), size, static_cast<int>(rte_socket_id()), RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (ring == nullptr) {
                throw std::runtime_error{std::format("rte_ring_create({}): {}", name, rte_strerror(rte_errno))};
            }
            return ring;
        }

        void create_port(const std::string& name,
                         const std::span<rte_ring* const> rx,
                         const std::span<rte_ring* const> tx) {
            const int port = rte_eth_from_rings(name.c_str(),
                                                rx.data(),
                                                static_cast<unsigned>(rx.size()),
                                                tx.data(),
                                                static_cast<unsigned>(tx.size()),
                                                rte_socket_id());
            if (port < 0) {
                throw std::runtime_error{std::format("rte_eth_from_rings({}): {}", name, rte_strerror(rte_errno))};
            }
        }

    }  // namespace

    RingPair::RingPair(const RingPairConfig& config) {
        try {
            for (std::uint16_t queue = 0; queue < config.queues; ++queue) {
                to_stack_.push_back(create_ring(std::format("{}-to-stack-{}", config.name, queue), config.ring_size));
                to_peer_.push_back(create_ring(std::format("{}-to-peer-{}", config.name, queue), config.ring_size));
            }
            create_port(config.name + "-stack", to_stack_, to_peer_);
            create_port(config.name + "-peer", to_peer_, to_stack_);
            stack_ = std::make_unique<ethdev::Port>(ethdev::PortConfig{.name        = config.name + "-stack",
                                                                       .queues      = config.queues,
                                                                       .descriptors = config.descriptors,
                                                                       .pool_size   = config.pool_size});
            peer_  = std::make_unique<ethdev::Port>(ethdev::PortConfig{.name        = config.name + "-peer",
                                                                       .queues      = config.queues,
                                                                       .descriptors = config.descriptors,
                                                                       .pool_size   = config.pool_size});
        } catch (...) {
            release();
            throw;
        }
    }

    RingPair::~RingPair() {
        if (stack_ && peer_) {
            drain();
        }
        release();
    }

    void RingPair::drain() noexcept {
        std::array<ethdev::Packet, ethdev::Port::max_burst> burst{};
        for (ethdev::Port* port : {stack_.get(), peer_.get()}) {
            for (std::uint16_t queue = 0; queue < port->queue_count(); ++queue) {
                for (std::size_t got = port->receive(queue, burst); got > 0; got = port->receive(queue, burst)) {
                    for (ethdev::Packet& packet : std::span{burst}.first(got)) {
                        packet = ethdev::Packet{};
                    }
                }
            }
        }
    }

    void RingPair::release() noexcept {
        stack_.reset();  // closes the port; an attached ring port does not free its rings
        peer_.reset();
        for (rte_ring* ring : to_stack_) {
            rte_ring_free(ring);
        }
        for (rte_ring* ring : to_peer_) {
            rte_ring_free(ring);
        }
        to_stack_.clear();
        to_peer_.clear();
    }

}  // namespace aloe::benchmarks
```

A port that `rte_eth_from_rings` created but `ethdev::Port` failed to wrap stays registered until the process exits; the benchmark exits on that failure, and the test environment never reaches it.

- [ ] **Step 6: Run the test to verify it passes (Probe A)**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.RingPair && ./build/debug/tests/integration_tests/benchmarks/Aloe.Tests.Integration.Benchmarks.RingPair`
Expected: PASS, 4 tests. If `rte_eth_dev_get_port_by_name` fails for the `-stack` name, print `rte_eth_dev_count_avail()` and the names from `rte_eth_dev_get_name_by_port` in a scratch test to see what the driver registered, and adapt the lookup name; record the finding under "Traps" in Task 12.

- [ ] **Step 7: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/integration_tests/CMakeLists.txt tests/integration_tests/benchmarks
git commit -m "feat(benchmarks): two ethdev ports over cross-connected SPSC rings"
```

---

## Task 3: The application work, the stamped device, the run state and thread pinning

**Files:**
- Create: `benchmarks/tick_to_trade/application_work.hpp`
- Create: `benchmarks/tick_to_trade/stamped_device.hpp`
- Create: `benchmarks/tick_to_trade/run_state.hpp`
- Create: `benchmarks/tick_to_trade/thread_pinning.hpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt` (the headers in `target_sources`)
- Create: `tests/unit_tests/benchmarks/test_application_work.cpp`
- Create: `tests/unit_tests/benchmarks/test_stamped_device.cpp`
- Modify: `tests/unit_tests/benchmarks/CMakeLists.txt` (the two sources; link `Aloe::Fixtures::Fabric` and `Aloe::Fixtures::Frames`)

**Interfaces:**
- Consumes: `round_prefix`, `order_size`, `RoundConfig` (Task 1); `stream::IsReadView` from `<aloe/stream>`; `device::IsDevice`, `device::Capabilities`, `device::RssDescription`, `device::QueueCounters` from `<aloe/device>`; `core::TscClock::get_ticks()`, `core::TscClock::scale()`.
- Produces: `read_round(span<const byte>) -> uint64_t`, `write_round(span<byte>, uint64_t)`, `fold(span<const byte>, uint64_t seed = fold_seed) -> uint64_t`, `fold(const View&) -> uint64_t` for an `IsReadView`, `write_order(span<byte>, uint64_t)`, `tick_payload(size_t, uint64_t) -> vector<byte>` (application_work.hpp); `StampedDevice<Device>{Device&, span<uint64_t> stops}` with `arm(uint64_t)`, `armed()`, `stamps_taken()`, `device()` and the IsDevice members (stamped_device.hpp); `Phase`, `phase_count`, `to_string(Phase)`, `Stamps{total, breakdown}` with `start`, `stop`, `phases`, `RunState{RoundConfig}` with `config`, `stamps`, `stop`, `failed`, `failure`, `sink`, `finish()`, `fail(string)`, and `ticks_for(core::Duration) -> uint64_t` (run_state.hpp); `pin_current_thread(unsigned)` (thread_pinning.hpp).

Spec: "The stamp wrapper", "The application at each level", "Recording".

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/benchmarks/test_application_work.cpp`:

```cpp
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <application_work.hpp>
#include <gtest/gtest.h>

namespace {
    using namespace aloe::benchmarks;
}

TEST(ApplicationWork, RoundNumberRoundTripsLittleEndian) {
    std::array<std::byte, 8> bytes{};
    write_round(bytes, std::uint64_t{0x0102030405060708});
    EXPECT_EQ(bytes[0], std::byte{0x08});
    EXPECT_EQ(bytes[7], std::byte{0x01});
    EXPECT_EQ(read_round(bytes), std::uint64_t{0x0102030405060708});
    write_round(bytes, 0);
    EXPECT_EQ(read_round(bytes), 0U);
}

TEST(ApplicationWork, OrderOpensWithTheRoundAndFillsToItsSize) {
    std::array<std::byte, order_size> order{};
    write_order(order, 42);
    EXPECT_EQ(read_round(order), 42U);
    EXPECT_EQ(order[round_prefix], std::byte{round_prefix});
    EXPECT_EQ(order[order_size - 1], std::byte{order_size - 1});
}

TEST(ApplicationWork, TickPayloadMatchesTheOrderLayout) {
    const std::vector<std::byte> tick = tick_payload(64, 7);
    EXPECT_EQ(tick.size(), 64U);
    EXPECT_EQ(read_round(tick), 7U);
    EXPECT_EQ(tick[63], std::byte{63});
}

TEST(ApplicationWork, FoldChainsAcrossChunksAndSeesEveryByte) {
    const std::vector<std::byte> whole = tick_payload(256, 1);
    const std::span<const std::byte> first{whole.data(), 100};
    const std::span<const std::byte> second{whole.data() + 100, 156};
    EXPECT_EQ(fold(whole), fold(second, fold(first)));
    std::vector<std::byte> changed = whole;
    changed[255] = std::byte{0};
    EXPECT_NE(fold(whole), fold(changed));
}
```

`tests/unit_tests/benchmarks/test_stamped_device.cpp`:

```cpp
#include <aloe/device>
#include <aloe/fabric>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include <gtest/gtest.h>
#include <stamped_device.hpp>

// The wrapper every level's device goes through: it stamps the one transmit the application armed and nothing
// else, so a pure ACK leaving in an early flush never closes a round. Over a fabric port: no DPDK.
namespace {

    using Stamped = aloe::benchmarks::StampedDevice<aloe::fabric::Port>;
    using Packet  = aloe::fabric::Packet;

    constexpr aloe::wire::MacAddress mac{0x02, 0, 0, 0, 0, 0x01};
    constexpr std::uint64_t untouched = std::numeric_limits<std::uint64_t>::max();

    static_assert(aloe::device::IsDevice<Stamped>, "the wrapper stands in for the port everywhere");

    void send(Stamped& device, const std::span<const std::byte> frame) {
        auto packet = device.allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(aloe::frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(device.transmit(0, burst), 1U);
    }

    class StampedDeviceTest : public ::testing::Test {
    protected:
        aloe::fabric::Fabric fabric_;
        aloe::fabric::Port& port_ = fabric_.add_port({.mac = mac, .queues = 1, .pool_size = 16});
        std::array<std::uint64_t, 4> stops_{untouched, untouched, untouched, untouched};
        Stamped device_{port_, stops_};
        std::vector<std::byte> frame_ =
            aloe::frames::ethernet_frame(mac, mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(46));
    };

}  // namespace

TEST_F(StampedDeviceTest, StampsOnlyTheArmedTransmit) {
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 0U) << "unarmed: a pure ACK or a control segment";
    device_.arm(2);
    EXPECT_TRUE(device_.armed());
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 1U);
    EXPECT_FALSE(device_.armed());
    EXPECT_NE(stops_[2], untouched);
    EXPECT_EQ(stops_[0], untouched);
    EXPECT_EQ(stops_[1], untouched);
    EXPECT_EQ(stops_[3], untouched);
    send(device_, frame_);
    EXPECT_EQ(device_.stamps_taken(), 1U) << "the arm is spent";
}

TEST_F(StampedDeviceTest, ATransmitTheDeviceTakesNothingOfKeepsTheArm) {
    device_.arm(0);
    std::array<Packet, 0> none{};
    EXPECT_EQ(device_.transmit(0, none), 0U);
    EXPECT_TRUE(device_.armed());
    EXPECT_EQ(device_.stamps_taken(), 0U);
}

TEST_F(StampedDeviceTest, ForwardsTheDevice) {
    EXPECT_EQ(device_.mac(), port_.mac());
    EXPECT_EQ(device_.queue_count(), 1U);
    EXPECT_EQ(device_.mtu(), port_.mtu());
    EXPECT_EQ(&device_.device(), &port_);
}
```

Add both sources to `add_unit_test(${UNIT_TESTING_TARGET}.Benchmarks ...)` and `Aloe::Fixtures::Fabric` and `Aloe::Fixtures::Frames` to its link list.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks`
Expected: FAIL to compile, headers not found.

- [ ] **Step 3: Write `application_work.hpp`**

```cpp
#pragma once

#include <aloe/stream>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <round_config.hpp>

namespace aloe::benchmarks {

    inline constexpr std::uint64_t fold_seed  = std::uint64_t{0xcbf29ce484222325};  ///< FNV-1a's offset basis.
    inline constexpr std::uint64_t fold_prime = std::uint64_t{0x100000001b3};

    /// The round number a tick or an order opens with: `round_prefix` bytes, little-endian.
    [[nodiscard]] inline std::uint64_t read_round(const std::span<const std::byte> payload) noexcept {
        assert(payload.size() >= round_prefix && "a payload opens with the round number");
        std::uint64_t round = 0;
        for (std::size_t index = 0; index < round_prefix; ++index) {
            round |= std::uint64_t{std::to_integer<std::uint8_t>(payload[index])} << (8U * index);
        }
        return round;
    }

    inline void write_round(const std::span<std::byte> payload, const std::uint64_t round) noexcept {
        assert(payload.size() >= round_prefix && "a payload opens with the round number");
        for (std::size_t index = 0; index < round_prefix; ++index) {
            payload[index] = static_cast<std::byte>((round >> (8U * index)) & std::uint64_t{0xff});
        }
    }

    /// A fold over every byte, FNV-1a: the stand-in for decoding the whole message. The caller keeps the result, so
    /// the compiler keeps the work.
    [[nodiscard]] inline std::uint64_t fold(const std::span<const std::byte> bytes,
                                            const std::uint64_t seed = fold_seed) noexcept {
        std::uint64_t accumulator = seed;
        for (const std::byte byte : bytes) {
            accumulator = (accumulator ^ std::to_integer<std::uint64_t>(byte)) * fold_prime;
        }
        return accumulator;
    }

    /// The same fold over the chunks of a read view, in order: what `unread()` gives the application.
    template <stream::IsReadView View>
    [[nodiscard]] std::uint64_t fold(const View& view) noexcept {
        std::uint64_t accumulator = fold_seed;
        for (const std::span<const std::byte> chunk : view) {
            accumulator = fold(chunk, accumulator);
        }
        return accumulator;
    }

    /// An order: the round number, then each byte its own index, to `order_size`.
    inline void write_order(const std::span<std::byte> order, const std::uint64_t round) noexcept {
        assert(order.size() == order_size && "an order is order_size bytes");
        write_round(order, round);
        for (std::size_t index = round_prefix; index < order.size(); ++index) {
            order[index] = static_cast<std::byte>(index);
        }
    }

    /// A tick's payload of `size` bytes with the same layout as an order. The peer builds it, off the clock.
    [[nodiscard]] inline std::vector<std::byte> tick_payload(const std::size_t size, const std::uint64_t round) {
        assert(size >= round_prefix && "a tick carries at least the round number");
        std::vector<std::byte> payload(size);
        write_round(payload, round);
        for (std::size_t index = round_prefix; index < size; ++index) {
            payload[index] = static_cast<std::byte>(index);
        }
        return payload;
    }

}  // namespace aloe::benchmarks
```

- [ ] **Step 4: Write `stamped_device.hpp`**

```cpp
#pragma once

#include <aloe/device>
#include <aloe/wire>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    /**
     * @brief A device seen through a wrapper that stamps the transmit the application armed.
     *
     * Models IsDevice and forwards every call. `arm(round)` says the next transmit the device accepts carries that
     * round's critical frame; once the device has taken it, `transmit` writes `TscClock::get_ticks()` into
     * `stops[round]` and disarms. An unarmed transmit, a pure ACK leaving in an early flush say, stamps nothing. The
     * one stop point of the floor, level 0 and level 1, at the cost of one register read per transmit everywhere.
     */
    template <device::IsDevice Device>
    class StampedDevice {
    public:
        using Packet = typename Device::Packet;

        StampedDevice(Device& owner, const std::span<std::uint64_t> stops) noexcept
            : device_{&owner},
              stops_{stops} {
        }

        void arm(const std::uint64_t round) noexcept {
            assert(round < stops_.size() && "a round beyond the stamps");
            armed_ = true;
            round_ = round;
        }

        [[nodiscard]] bool armed() const noexcept {
            return armed_;
        }

        [[nodiscard]] std::uint64_t stamps_taken() const noexcept {
            return taken_;
        }

        [[nodiscard]] Device& device() noexcept {
            return *device_;
        }

        [[nodiscard]] std::uint16_t queue_count() const noexcept {
            return device_->queue_count();
        }

        [[nodiscard]] wire::MacAddress mac() const noexcept {
            return device_->mac();
        }

        [[nodiscard]] std::uint16_t mtu() const noexcept {
            return device_->mtu();
        }

        [[nodiscard]] bool link_up() const noexcept {
            return device_->link_up();
        }

        [[nodiscard]] const device::Capabilities& capabilities() const noexcept {
            return device_->capabilities();
        }

        [[nodiscard]] const device::RssDescription& steering() const noexcept {
            return device_->steering();
        }

        [[nodiscard]] std::optional<Packet> allocate(const std::uint16_t queue) noexcept {
            return device_->allocate(queue);
        }

        [[nodiscard]] std::size_t receive(const std::uint16_t queue, const std::span<Packet> out) noexcept {
            return device_->receive(queue, out);
        }

        /// Forwards, then stamps when armed and the device took at least one packet.
        [[nodiscard]] std::size_t transmit(const std::uint16_t queue, const std::span<Packet> in) noexcept {
            const std::size_t accepted = device_->transmit(queue, in);
            if (accepted > 0 && armed_) {
                stops_[round_] = core::TscClock::get_ticks();
                armed_         = false;
                ++taken_;
            }
            return accepted;
        }

        [[nodiscard]] device::QueueCounters counters(const std::uint16_t queue) const noexcept {
            return device_->counters(queue);
        }

    private:
        Device* device_;
        std::span<std::uint64_t> stops_;
        std::uint64_t round_ = 0;
        std::uint64_t taken_ = 0;
        bool armed_          = false;
    };

}  // namespace aloe::benchmarks
```

If clang-tidy's `readability-make-member-function-const` fires on `allocate` or `receive`, make them `const`: the wrapper is a non-owning handle, the rule `AGENTS.md` gives for `log::Logger`.

- [ ] **Step 5: Write `run_state.hpp`**

```cpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clock.hpp>
#include <round_config.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    /// The phases of a level 0 round in breakdown mode, each stamped when it ends; the device's stop stamp ends the
    /// last leg, the queue flush.
    enum class Phase : std::uint8_t {
        Received,     ///< `queue.receive` returned the tick.
        Ip,           ///< `ip.process` done.
        Tcp,          ///< `tcp.process` done.
        Application,  ///< The order committed.
        TcpFlush,     ///< `tcp.flush` done; what remains is `queue.flush` and the device.
    };

    inline constexpr std::size_t phase_count = 5;

    [[nodiscard]] constexpr std::string_view to_string(const Phase phase) noexcept {
        switch (phase) {
            case Phase::Received: return "receive";
            case Phase::Ip: return "ip";
            case Phase::Tcp: return "tcp";
            case Phase::Application: return "application";
            case Phase::TcpFlush: return "tcp-flush";
        }
        return "?";
    }

    /// The name of the leg from the last phase stamp to the stop stamp.
    inline constexpr std::string_view last_leg = "queue-flush";

    /// `duration` in TSC ticks, by the calibrated scale: for gaps and deadlines, off the measured path.
    [[nodiscard]] inline std::uint64_t ticks_for(const core::Duration duration) {
        constexpr std::uint64_t probe        = std::uint64_t{1} << 32U;
        const std::uint64_t probe_nanoseconds = core::TscClock::scale().nanoseconds(probe);
        const auto nanoseconds                = static_cast<std::uint64_t>(duration.count());
        return static_cast<std::uint64_t>(static_cast<double>(nanoseconds) * static_cast<double>(probe) /
                                          static_cast<double>(probe_nanoseconds));
    }

    /// One configuration's stamps: raw ticks, one slot per round, each array written by exactly one thread.
    struct Stamps {
        Stamps(const std::size_t total, const bool breakdown)
            : start(total),
              stop(total) {
            if (breakdown) {
                for (std::vector<std::uint64_t>& phase : phases) {
                    phase.assign(total, 0);
                }
            }
        }

        std::vector<std::uint64_t> start;  ///< The peer (tick round) or the application (gateway round).
        std::vector<std::uint64_t> stop;   ///< The stamped device.
        std::array<std::vector<std::uint64_t>, phase_count> phases{};  ///< Level 0 breakdown; empty unless enabled.
    };

    /// Everything one configuration's threads share. Immovable; it lives where the configuration runs.
    struct RunState {
        explicit RunState(const RoundConfig& configuration)
            : config{configuration},
              stamps{configuration.total(), configuration.breakdown} {
        }

        RoundConfig config;
        Stamps stamps;
        std::atomic<bool> stop{false};    ///< Set by whoever finishes or fails first; every loop polls it.
        std::atomic<bool> failed{false};
        std::string failure;              ///< Written by the first to fail, before `failed`; read after the join.
        std::uint64_t sink = 0;           ///< The application's fold, kept so the work is not elided.

        /// Every round is out: stops the loops and wakes a thread sleeping on `stop`.
        void finish() noexcept {
            stop.store(true);
            stop.notify_all();
        }

        /// Records the first failure, then finishes.
        void fail(std::string why) noexcept {
            if (!failed.exchange(true)) {
                failure = std::move(why);
            }
            finish();
        }
    };

}  // namespace aloe::benchmarks
```

- [ ] **Step 6: Write `thread_pinning.hpp`**

```cpp
#pragma once

#include <cerrno>
#include <system_error>

#include <pthread.h>
#include <sched.h>

namespace aloe::benchmarks {

    /// Pins the calling thread to `cpu`, as the runtime pins a shard. Throws std::system_error when the CPU does not
    /// exist or is not allowed.
    inline void pin_current_thread(const unsigned cpu) {
        if (cpu >= CPU_SETSIZE) {
            throw std::system_error{EINVAL, std::generic_category(), "cpu beyond CPU_SETSIZE"};
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); error != 0) {
            throw std::system_error{error, std::generic_category(), "pthread_setaffinity_np"};
        }
    }

}  // namespace aloe::benchmarks
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks`
Expected: PASS, 13 tests.

- [ ] **Step 8: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/unit_tests/benchmarks
git commit -m "feat(benchmarks): the application work, the stamped device and the run state"
```

---

## Task 4: The command line

**Files:**
- Create: `benchmarks/tick_to_trade/benchmark_options.hpp`
- Create: `benchmarks/tick_to_trade/benchmark_options.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/unit_tests/benchmarks/test_benchmark_options.cpp`
- Modify: `tests/unit_tests/benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: `Round`, `Level`, `Topology`, `RoundConfig`, `order_size`, `round_prefix` (Task 1).
- Produces: `TopologyChoice{Cross, Same, Both}`; `Options{rounds, levels, payloads, measured, warmup, gap, topology, stack_core, peer_core, breakdown, hop, arp_on_wire, hugepages, keep_hugepages, no_huge, out, eal}`; `parse_options(std::span<char*>) -> std::expected<Options, std::string>`; `usage() -> std::string`; `default_eal_arguments(std::optional<unsigned>) -> std::vector<std::string>`; `configurations(const Options&) -> std::vector<RoundConfig>`.

Spec: "Build and command line".

- [ ] **Step 1: Write the failing test**

`tests/unit_tests/benchmarks/test_benchmark_options.cpp`:

```cpp
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <benchmark_options.hpp>
#include <gtest/gtest.h>

namespace {

    using namespace aloe::benchmarks;

    /// Parses `words` as argv, with a program name in front.
    [[nodiscard]] std::expected<Options, std::string> parse(std::vector<std::string> words) {
        words.insert(words.begin(), "tick_to_trade");
        std::vector<char*> argv;
        for (std::string& word : words) {
            argv.push_back(word.data());
        }
        return parse_options(argv);
    }

    [[nodiscard]] std::size_t count(const std::vector<RoundConfig>& matrix,
                                    const Round round,
                                    const Level level,
                                    const Topology topology) {
        return static_cast<std::size_t>(std::ranges::count_if(matrix, [&](const RoundConfig& c) {
            return c.round == round && c.level == level && c.topology == topology;
        }));
    }

}  // namespace

TEST(BenchmarkOptions, DefaultsAreTheSpecsNumbers) {
    const auto options = parse({});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->payloads, (std::vector<std::size_t>{64, 256, 1024, 1448, 1460}));
    EXPECT_EQ(options->measured, 100000U);
    EXPECT_EQ(options->warmup, 10000U);
    EXPECT_EQ(options->gap.count(), 0);
    EXPECT_EQ(options->topology, TopologyChoice::Cross);
    EXPECT_EQ(options->stack_core, 2U);
    EXPECT_EQ(options->peer_core, 3U);
    EXPECT_EQ(options->hugepages, 64U);
    EXPECT_FALSE(options->breakdown);
    EXPECT_FALSE(options->hop);
    EXPECT_FALSE(options->no_huge);
    EXPECT_EQ(options->eal,
              (std::vector<std::string>{"tick_to_trade", "--in-memory", "--no-pci", "--no-telemetry", "-l", "2"}));
}

TEST(BenchmarkOptions, EveryOptionParses) {
    const auto options = parse({"--round", "tick", "--level", "0", "--payload", "64,1460", "--rounds", "10",
                                "--warmup", "2", "--gap", "500", "--topology", "both", "--stack-core", "none",
                                "--peer-core", "5", "--breakdown", "--hop", "--arp-on-wire", "--hugepages", "8",
                                "--keep-hugepages", "--out", "r.json", "--", "--no-huge", "--no-pci"});
    ASSERT_TRUE(options.has_value()) << options.error();
    EXPECT_EQ(options->rounds, (std::vector<Round>{Round::Tick}));
    EXPECT_EQ(options->levels, (std::vector<Level>{Level::Bricks}));
    EXPECT_EQ(options->payloads, (std::vector<std::size_t>{64, 1460}));
    EXPECT_EQ(options->measured, 10U);
    EXPECT_EQ(options->warmup, 2U);
    EXPECT_EQ(options->gap, std::chrono::nanoseconds{500});
    EXPECT_EQ(options->topology, TopologyChoice::Both);
    EXPECT_EQ(options->stack_core, std::nullopt);
    EXPECT_EQ(options->peer_core, 5U);
    EXPECT_TRUE(options->breakdown);
    EXPECT_TRUE(options->hop);
    EXPECT_TRUE(options->arp_on_wire);
    EXPECT_EQ(options->hugepages, 8U);
    EXPECT_TRUE(options->keep_hugepages);
    EXPECT_EQ(options->out, "r.json");
    EXPECT_TRUE(options->no_huge) << "detected among the EAL arguments";
    EXPECT_EQ(options->eal, (std::vector<std::string>{"tick_to_trade", "--no-huge", "--no-pci"}));
}

TEST(BenchmarkOptions, RejectsWhatTheSpecRulesOut) {
    EXPECT_FALSE(parse({"--payload", "4"}).has_value()) << "below the round prefix";
    EXPECT_FALSE(parse({"--payload", "1461"}).has_value()) << "above a full segment";
    EXPECT_FALSE(parse({"--rounds", "0"}).has_value());
    EXPECT_FALSE(parse({"--level", "2"}).has_value());
    EXPECT_FALSE(parse({"--topology", "sideways"}).has_value());
    EXPECT_FALSE(parse({"--bogus"}).has_value());
    EXPECT_FALSE(parse({"--rounds"}).has_value()) << "a value is missing";
}

TEST(BenchmarkOptions, DefaultMatrixIsEveryRoundLevelAndPayloadCrossCore) {
    const auto matrix = configurations(*parse({}));
    EXPECT_EQ(matrix.size(), 18U) << "tick: 3 levels x 5 payloads; gateway: 3 levels";
    EXPECT_EQ(count(matrix, Round::Tick, Level::Bricks, Topology::CrossCore), 5U);
    EXPECT_EQ(count(matrix, Round::Gateway, Level::Runtime, Topology::CrossCore), 1U);
    EXPECT_EQ(count(matrix, Round::Tick, Level::Floor, Topology::SameCore), 0U);
    const auto gateway = std::ranges::find_if(matrix, [](const RoundConfig& c) { return c.round == Round::Gateway; });
    ASSERT_NE(gateway, matrix.end());
    EXPECT_EQ(gateway->payload, order_size);
}

TEST(BenchmarkOptions, SameCoreAppliesToTheFloorAndLevelZeroOnly) {
    const auto both = configurations(*parse({"--topology", "both"}));
    EXPECT_EQ(both.size(), 30U) << "tick: (2 levels x 2 topologies + 1) x 5; gateway: 2 x 2 + 1";
    EXPECT_EQ(count(both, Round::Tick, Level::Runtime, Topology::SameCore), 0U);
    EXPECT_EQ(count(both, Round::Tick, Level::Floor, Topology::SameCore), 5U);
    const auto same = configurations(*parse({"--topology", "same"}));
    EXPECT_EQ(same.size(), 12U) << "level 1 cannot share a thread with the peer and is left out";
    EXPECT_EQ(count(same, Round::Gateway, Level::Runtime, Topology::CrossCore), 0U);
}

TEST(BenchmarkOptions, ConfigurationsCarryTheRunNumbers) {
    const auto matrix = configurations(*parse({"--rounds", "7", "--warmup", "3", "--gap", "20", "--breakdown"}));
    for (const RoundConfig& c : matrix) {
        EXPECT_EQ(c.rounds, 7U);
        EXPECT_EQ(c.warmup, 3U);
        EXPECT_EQ(c.gap, std::chrono::nanoseconds{20});
        EXPECT_EQ(c.breakdown, c.level == Level::Bricks) << "phase stamps exist at level 0 only";
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks`
Expected: FAIL to compile, `benchmark_options.hpp` not found.

- [ ] **Step 3: Write `benchmark_options.hpp`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <clock.hpp>
#include <round_config.hpp>

namespace aloe::benchmarks {

    enum class TopologyChoice : std::uint8_t {
        Cross,  ///< The default.
        Same,   ///< The floor and level 0 on one thread; level 1 left out.
        Both,   ///< Cross-core everywhere, plus same-core for the floor and level 0.
    };

    /// What the command line decided. Every member has the spec's default.
    struct Options {
        std::vector<Round> rounds{Round::Tick, Round::Gateway};
        std::vector<Level> levels{Level::Floor, Level::Bricks, Level::Runtime};
        std::vector<std::size_t> payloads{64, 256, 1024, 1448, 1460};
        std::size_t measured    = 100000;
        std::size_t warmup      = 10000;
        core::Duration gap      = core::Duration::zero();
        TopologyChoice topology = TopologyChoice::Cross;
        std::optional<unsigned> stack_core = 2;  ///< Also the EAL's main lcore; `none` leaves it unpinned.
        std::optional<unsigned> peer_core  = 3;
        bool breakdown          = false;
        bool hop                = false;
        bool arp_on_wire        = false;
        std::uint64_t hugepages = 64;     ///< 2 MB pages the run needs free.
        bool keep_hugepages     = false;
        bool no_huge            = false;  ///< `--no-huge` was passed after `--`: no reservation, and the result file says so.
        std::string out;                  ///< The result file; empty means `tick-to-trade-<timestamp>.json` here.
        std::vector<std::string> eal;     ///< argv for the EAL, the program name first.
    };

    /// `argv[0]` is the program; options come first, then `--` and the EAL's arguments verbatim.
    [[nodiscard]] std::expected<Options, std::string> parse_options(std::span<char*> argv);

    [[nodiscard]] std::string usage();

    /// In-memory, no PCI scan, no telemetry, the main lcore on `stack_core` (0 when unpinned); hugepages by default.
    [[nodiscard]] std::vector<std::string> default_eal_arguments(std::optional<unsigned> stack_core);

    /// The matrix the options describe, in the order the table prints: rounds, then levels, then topologies, then
    /// payloads. The gateway round has one configuration per level and topology, its payload `order_size`; level 1
    /// is cross-core only; breakdown applies to level 0 only.
    [[nodiscard]] std::vector<RoundConfig> configurations(const Options& options);

}  // namespace aloe::benchmarks
```

- [ ] **Step 4: Write `benchmark_options.cpp`**

Signatures as declared. The parser walks `argv.subspan(1)`; an argument equal to `--` ends the options and the rest becomes `eal` behind `argv[0]`; when nothing follows `--` or `--` is absent, `eal` is `default_eal_arguments(stack_core)` with `argv[0]` in front. `no_huge` is true when `eal` contains `"--no-huge"`. Values: `--round tick|gateway|both`, `--level floor|0|1|all`, `--payload` a comma list of integers each in `[round_prefix, 1460]`, `--rounds` and `--warmup` integers with `--rounds` at least 1, `--gap` nanoseconds, `--topology cross|same|both`, `--stack-core` and `--peer-core` an integer or `none`, `--hugepages` an integer, `--out` a path, the flags `--breakdown`, `--hop`, `--arp-on-wire`, `--keep-hugepages`, and `--help`, which returns the error `usage()`. Every failure returns `std::unexpected` with a one-line message naming the option. Parse integers with `std::from_chars`, never `std::stoul`.

`configurations`: for each round in `rounds`, for each level in `levels`, for each topology in (`Cross` → `{CrossCore}`, `Same` → `{SameCore}`, `Both` → `{CrossCore, SameCore}`), skip `SameCore` when `level == Level::Runtime`; for `Round::Tick` one `RoundConfig` per payload, for `Round::Gateway` one with `payload = order_size`; copy `measured` into `rounds`, `warmup`, `gap`; `breakdown = options.breakdown && level == Level::Bricks`.

`usage()` returns the text:

```
usage: tick_to_trade [options] [-- <EAL arguments>]
  --round tick|gateway|both     which critical path (both)
  --level floor|0|1|all         the floor, the loop over the bricks, the runtime (all)
  --payload <bytes,...>         tick payloads, 8..1460 (64,256,1024,1448,1460)
  --rounds N                    measured rounds per configuration (100000)
  --warmup N                    rounds discarded before them (10000)
  --gap NS                      pause between rounds, nanoseconds (0)
  --topology cross|same|both    the peer on its own core, or on the stack's thread for the floor and level 0 (cross)
  --stack-core N|none           the stack's core, also the EAL's main lcore (2)
  --peer-core N|none            the peer's core (3)
  --breakdown                   phase stamps at level 0
  --hop                         measure the cache-line hop between the two cores
  --arp-on-wire                 resolve the peer by ARP instead of seeding the cache
  --hugepages N                 2 MB pages the run needs free (64)
  --keep-hugepages              leave pages this process reserved for the next run
  --out FILE                    the result file (tick-to-trade-<timestamp>.json)
  --                            what follows goes to the EAL verbatim; default: --in-memory --no-pci --no-telemetry -l <stack core>
```

Add `benchmark_options.cpp` to the harness's sources and the header to `target_sources`; add the test source to the unit target.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks`
Expected: PASS, 19 tests.

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/unit_tests/benchmarks
git commit -m "feat(benchmarks): the command line and the configuration matrix"
```

---

## Task 5: The scripted venue and the floor

**Files:**
- Create: `benchmarks/tick_to_trade/venue_peer.hpp`
- Create: `benchmarks/tick_to_trade/venue_peer.cpp`
- Create: `benchmarks/tick_to_trade/level_floor.hpp`
- Create: `benchmarks/tick_to_trade/level_floor.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/integration_tests/benchmarks/test_venue_peer.cpp`
- Modify: `tests/integration_tests/benchmarks/CMakeLists.txt` (the target `${INTEGRATION_TESTING_TARGET}.Benchmarks.VenuePeer`, linked like the ring pair's)

**Interfaces:**
- Consumes: `RingPair` (Task 2); `StampedDevice<ethdev::Port>`, `RunState`, `Stamps`, `ticks_for`, `read_round`, `write_order`, `tick_payload`, `fold` (Task 3); `frames::TcpPeer`, `frames::TcpSpec`, `frames::parse_frame`, `frames::tcp_payload`, `frames::tcp_frame`, `frames::fill`, `frames::arp_reply`, `frames::arp_frame`, `frames::l4_checksum_residue`, `frames::ethertype_arp`; `wire::internet_checksum`, `wire::load_be16`, `wire::TcpFlag`, `wire::TcpFlags`.
- Produces: `PeerConfig{stack_mac, peer_mac, stack_ip, peer_ip, stack_port, peer_port, mss, strict, deadline}`, `PeerCounters{orders_seen, pure_acks_seen, arp_replies_sent, surplus_freed, unexpected_frames}`, `PeerError{Timeout, BadChecksum, WrongRound, WrongSequence, Unexpected}`, `to_string(PeerError)`, `class VenuePeer` with `VenuePeer(ethdev::Port&, const PeerConfig&, std::span<std::uint64_t> starts)`, `open_active()`, `finish_active_open()`, `answer_passive_open()`, `finish_passive_open()` (each `std::expected<void, PeerError>`), `send_tick(std::uint64_t round, std::size_t payload)`, `await_order(std::uint64_t round) -> std::expected<void, PeerError>`, `send_bare_ack()`, `counters()`, `script()`; the free function `drive_peer(VenuePeer&, RunState&, bool handshake)` (venue_peer.hpp); `frame_header_size` (54) and `run_floor(RunState&, StampedDevice<ethdev::Port>&, const PeerConfig&, VenuePeer* inline_peer)` (level_floor.hpp).

Spec: "The scripted peer", "Mbufs are recycled, never freed across cores", "The application at each level" (the floor), "Sanity".

- [ ] **Step 1: Write the failing test**

`tests/integration_tests/benchmarks/test_venue_peer.cpp`:

```cpp
#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <level_floor.hpp>
#include <ring_pair.hpp>
#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

// The venue over the peer port and the floor over the stack port: the first closed-loop rounds, with the stack's
// mbufs recycled by the peer. Same-core inline, then cross-core on an unpinned thread. No root, no hugepages.
namespace {

    using namespace aloe::benchmarks;
    using namespace std::chrono_literals;
    using Packet  = ethdev::Packet;
    using Stamped = StampedDevice<ethdev::Port>;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] RoundConfig small(const Round round, const Topology topology, const std::size_t payload) {
        return {.round = round, .level = Level::Floor, .topology = topology, .payload = payload, .rounds = 50, .warmup = 5};
    }

    [[nodiscard]] PeerConfig floor_peer(RingPair& pair) {
        return {.stack_mac = pair.stack().mac(), .strict = false, .deadline = 2s};
    }

    void expect_closed_loop(const RunState& state, const Stamped& device, const VenuePeer& peer) {
        EXPECT_FALSE(state.failed.load()) << state.failure;
        EXPECT_EQ(device.stamps_taken(), state.config.total());
        EXPECT_EQ(peer.counters().orders_seen, state.config.total());
        for (std::size_t round = 0; round < state.config.total(); ++round) {
            EXPECT_NE(state.stamps.start[round], 0U) << round;
            EXPECT_GT(state.stamps.stop[round], state.stamps.start[round]) << round;
        }
    }

}  // namespace

TEST(VenuePeer, FloorTickRoundSameCore) {
    RingPair pair{{.name = "venue-floor-tick-same", .pool_size = 512}};
    RunState state{small(Round::Tick, Topology::SameCore, 64)};
    Stamped device{pair.stack(), state.stamps.stop};
    VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
    run_floor(state, device, floor_peer(pair), &peer);
    expect_closed_loop(state, device, peer);
    EXPECT_EQ(peer.counters().surplus_freed, 0U) << "every order came back as the next tick";
}

TEST(VenuePeer, FloorTickRoundCrossCore) {
    RingPair pair{{.name = "venue-floor-tick-cross", .pool_size = 512}};
    RunState state{small(Round::Tick, Topology::CrossCore, 1460)};
    Stamped device{pair.stack(), state.stamps.stop};
    VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
    std::jthread venue{[&] { drive_peer(peer, state, false); }};
    run_floor(state, device, floor_peer(pair), nullptr);
    venue.join();
    expect_closed_loop(state, device, peer);
}

TEST(VenuePeer, FloorGatewayRoundBothTopologies) {
    {
        RingPair pair{{.name = "venue-floor-gw-same", .pool_size = 512}};
        RunState state{small(Round::Gateway, Topology::SameCore, order_size)};
        Stamped device{pair.stack(), state.stamps.stop};
        VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
        run_floor(state, device, floor_peer(pair), &peer);
        expect_closed_loop(state, device, peer);
    }
    {
        RingPair pair{{.name = "venue-floor-gw-cross", .pool_size = 512}};
        RunState state{small(Round::Gateway, Topology::CrossCore, order_size)};
        Stamped device{pair.stack(), state.stamps.stop};
        VenuePeer peer{pair.peer(), floor_peer(pair), state.stamps.start};
        std::jthread venue{[&] { drive_peer(peer, state, false); }};
        run_floor(state, device, floor_peer(pair), nullptr);
        venue.join();
        expect_closed_loop(state, device, peer);
    }
}

TEST(VenuePeer, AwaitOrderSkipsPureAcksChecksTheRoundAndTheChecksums) {
    RingPair pair{{.name = "venue-await", .pool_size = 256}};
    std::array<std::uint64_t, 4> starts{};
    PeerConfig config = floor_peer(pair);
    config.strict     = true;
    VenuePeer peer{pair.peer(), config, starts};
    // The stack's side, by hand: frames from the stack port's pool, as the brick would send them.
    const auto stack_sends = [&](const std::vector<std::byte>& frame) {
        auto packet = pair.stack().allocate(0);
        ASSERT_TRUE(packet.has_value());
        ASSERT_TRUE(frames::fill(*packet, frame));
        std::array<Packet, 1> burst{std::move(*packet)};
        ASSERT_EQ(pair.stack().transmit(0, burst), 1U);
    };
    const auto stack_receives = [&] {
        std::array<Packet, 4> burst{};
        const std::size_t got = pair.stack().receive(0, burst);
        std::vector<frames::ParsedFrame> parsed;
        for (Packet& packet : std::span{burst}.first(got)) {
            parsed.push_back(*frames::parse_frame(frames::bytes_of(packet)));
            packet = Packet{};
        }
        return parsed;
    };
    frames::TcpSpec from_stack{.destination_mac  = config.peer_mac,
                               .source_mac       = pair.stack().mac(),
                               .source           = config.stack_ip,
                               .destination      = config.peer_ip,
                               .source_port      = config.stack_port,
                               .destination_port = config.peer_port,
                               .sequence         = wire::TcpSequence{1000},
                               .flags            = wire::TcpFlags{wire::TcpFlag::Syn, wire::TcpFlag::Ack},
                               .mss              = 1460};
    // The handshake, the peer opening: it synchronises the script's view of the stack's sequence numbers.
    ASSERT_TRUE(peer.open_active().has_value());
    const auto syn = stack_receives();
    ASSERT_EQ(syn.size(), 1U);
    from_stack.acknowledgement = syn[0].tcp->sequence + 1;
    stack_sends(frames::tcp_frame(from_stack, {}));
    ASSERT_TRUE(peer.finish_active_open().has_value());
    ASSERT_EQ(stack_receives().size(), 1U) << "the handshake's ACK";
    from_stack.sequence        = peer.script().rcv_nxt();
    from_stack.acknowledgement = peer.script().snd_nxt();
    from_stack.flags           = wire::TcpFlags{wire::TcpFlag::Ack};
    from_stack.mss             = std::nullopt;

    stack_sends(frames::tcp_frame(from_stack, {}));  // a pure ACK first, as the 1460 row produces
    frames::TcpSpec order = from_stack;
    order.flags           = wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack};
    std::vector<std::byte> bytes(order_size);
    write_order(bytes, 3);
    stack_sends(frames::tcp_frame(order, bytes));
    EXPECT_TRUE(peer.await_order(3).has_value());
    EXPECT_EQ(peer.counters().pure_acks_seen, 1U);
    EXPECT_EQ(peer.counters().orders_seen, 1U);
    EXPECT_EQ(peer.counters().surplus_freed, 1U) << "the pure ACK's mbuf";

    order.sequence = order.sequence + static_cast<std::uint32_t>(order_size);
    write_order(bytes, 9);
    stack_sends(frames::tcp_frame(order, bytes));
    const auto wrong = peer.await_order(4);
    ASSERT_FALSE(wrong.has_value());
    EXPECT_EQ(wrong.error(), PeerError::WrongRound);

    order.sequence  = order.sequence + static_cast<std::uint32_t>(order_size);
    order.checksums = frames::Checksums::Wrong;
    write_order(bytes, 5);
    stack_sends(frames::tcp_frame(order, bytes));
    const auto bad = peer.await_order(5);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), PeerError::BadChecksum);

    PeerConfig impatient = config;
    impatient.deadline   = 20ms;
    VenuePeer waiting{pair.peer(), impatient, starts};
    const auto timeout = waiting.await_order(0);
    ASSERT_FALSE(timeout.has_value());
    EXPECT_EQ(timeout.error(), PeerError::Timeout);
}

TEST(VenuePeer, AnswersArpRequestsWhileWaiting) {
    RingPair pair{{.name = "venue-arp", .pool_size = 256}};
    std::array<std::uint64_t, 1> starts{};
    PeerConfig config = floor_peer(pair);
    config.deadline   = 50ms;
    VenuePeer peer{pair.peer(), config, starts};
    const auto request = frames::arp_frame(
        frames::arp_request(pair.stack().mac(), config.stack_ip, config.peer_ip), wire::MacAddress::broadcast());
    auto packet = pair.stack().allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(frames::fill(*packet, request));
    std::array<Packet, 1> burst{std::move(*packet)};
    ASSERT_EQ(pair.stack().transmit(0, burst), 1U);
    EXPECT_EQ(peer.await_order(0).error(), PeerError::Timeout) << "no order, but the request was answered meanwhile";
    EXPECT_EQ(peer.counters().arp_replies_sent, 1U);
    std::array<Packet, 4> at_stack{};
    ASSERT_EQ(pair.stack().receive(0, at_stack), 1U);
    const auto reply = frames::parse_frame(frames::bytes_of(at_stack[0]));
    ASSERT_TRUE(reply.has_value() && reply->arp.has_value());
    EXPECT_EQ(reply->arp->operation, wire::ArpOperation::Reply);
    EXPECT_EQ(reply->arp->sender_mac, config.peer_mac);
    EXPECT_EQ(reply->arp->sender_ip, config.peer_ip);
}
```

If `wire::MacAddress::broadcast()` does not exist under that name, use what `grep -n broadcast common/wire/ethernet/mac_address.hpp` prints.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.VenuePeer`
Expected: FAIL to compile, headers not found.

- [ ] **Step 3: Write `venue_peer.hpp`**

```cpp
#pragma once

#include <aloe/ethdev>
#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include <clock.hpp>
#include <run_state.hpp>

namespace aloe::benchmarks {

    struct PeerConfig {
        wire::MacAddress stack_mac{};  ///< The stack port's MAC: all zeros on a ring port.
        wire::MacAddress peer_mac{0x02, 0, 0, 0, 0xfe, 0xed};
        wire::Ipv4Address stack_ip{10, 0, 0, 2};
        wire::Ipv4Address peer_ip{10, 0, 0, 1};
        std::uint16_t stack_port = 7;      ///< Where the stack listens in the tick round.
        std::uint16_t peer_port  = 40000;  ///< The peer's port; the stack connects to it in the gateway round.
        std::uint16_t mss        = 1460;
        bool strict              = true;   ///< Check checksums and sequence numbers of every order; off at the floor.
        core::Duration deadline  = std::chrono::seconds{1};  ///< The longest wait for a frame before the run fails.
    };

    struct PeerCounters {
        std::uint64_t orders_seen       = 0;
        std::uint64_t pure_acks_seen    = 0;  ///< The 1460 row's second-segment ACKs, skipped.
        std::uint64_t arp_replies_sent  = 0;
        std::uint64_t surplus_freed     = 0;  ///< Stack mbufs the peer had no use for, freed on its own core.
        std::uint64_t unexpected_frames = 0;  ///< Neither TCP nor ARP.
    };

    enum class PeerError : std::uint8_t {
        Timeout,        ///< No frame within the deadline.
        BadChecksum,    ///< An order's IPv4 or TCP checksum was wrong.
        WrongRound,     ///< An order whose first bytes are not the round awaited.
        WrongSequence,  ///< An order out of sequence.
        Unexpected,     ///< Another frame where a handshake segment was due.
    };

    [[nodiscard]] constexpr std::string_view to_string(const PeerError error) noexcept {
        switch (error) {
            case PeerError::Timeout: return "no frame within the deadline";
            case PeerError::BadChecksum: return "an order with a bad checksum";
            case PeerError::WrongRound: return "an order for another round";
            case PeerError::WrongSequence: return "an order out of sequence";
            case PeerError::Unexpected: return "an unexpected frame during the handshake";
        }
        return "?";
    }

    /**
     * @brief The venue, scripted: the other end of the stack's one connection, over the peer port of a RingPair.
     *
     * Every member runs on the peer's thread, or inline on the stack's thread in the same-core topology. Setup
     * frames come from the peer port's pool; every frame after the handshake is written into the carrier, the mbuf
     * the stack's last frame arrived in, so the stack core's pool stays balanced. The peer answers an ARP request
     * whenever it sees one. `starts[round]` receives the tick round's start stamp.
     */
    class VenuePeer {
    public:
        using Packet = ethdev::Packet;

        VenuePeer(ethdev::Port& port, const PeerConfig& config, std::span<std::uint64_t> starts);

        /// Tick round, the peer opens: SYN from the pool.
        [[nodiscard]] std::expected<void, PeerError> open_active();
        /// Tick round: the SYN-ACK, kept as the carrier, then the ACK from the pool.
        [[nodiscard]] std::expected<void, PeerError> finish_active_open();
        /// Gateway round, the stack opens: the SYN, kept as the carrier, then the SYN-ACK from the pool.
        [[nodiscard]] std::expected<void, PeerError> answer_passive_open();
        /// Gateway round: the handshake's ACK, freed.
        [[nodiscard]] std::expected<void, PeerError> finish_passive_open();

        /// Builds tick `round` of `payload` bytes into the carrier, stamps `starts[round]`, transmits.
        void send_tick(std::uint64_t round, std::size_t payload);
        /// Polls for the order of `round`: skips and frees pure ACKs, answers ARP, checks the order, keeps its mbuf
        /// as the carrier. Fails on the deadline or on a wrong order.
        [[nodiscard]] std::expected<void, PeerError> await_order(std::uint64_t round);
        /// Gateway round: a bare ACK of everything seen, in the carrier.
        void send_bare_ack();

        [[nodiscard]] const PeerCounters& counters() const noexcept {
            return counters_;
        }

        [[nodiscard]] const frames::TcpPeer& script() const noexcept {
            return script_;
        }

    private:
        /// The next frame within the deadline, ARP requests answered and dropped on the way; nothing on the deadline.
        [[nodiscard]] std::optional<Packet> collect();
        /// A fresh packet from the pool carrying `frame`; a refused transmit is counted by the round that times out.
        void inject(std::span<const std::byte> frame);
        /// `frame` into the carrier, which must be present, and out.
        void transmit_carrier(std::span<const std::byte> frame);
        void answer_arp(const frames::ParsedFrame& request);
        [[nodiscard]] static bool is_pure_ack(const frames::ParsedFrame& parsed) noexcept;

        ethdev::Port* port_;
        PeerConfig config_;
        frames::TcpPeer script_;
        std::span<std::uint64_t> starts_;
        Packet carrier_;
        std::array<Packet, 16> burst_{};
        std::size_t pending_ = 0;  ///< Frames in `burst_` not yet taken: `[next_, pending_)`.
        std::size_t next_    = 0;
        PeerCounters counters_;
    };

    /// The peer's side of every round of `state`, on the calling thread: the handshake when `handshake`, then the
    /// tick or gateway rounds, honouring `state.config.gap` and failing `state` on the first error.
    void drive_peer(VenuePeer& peer, RunState& state, bool handshake);

}  // namespace aloe::benchmarks
```

- [ ] **Step 4: Write `venue_peer.cpp`**

```cpp
#include "venue_peer.hpp"

#include <cassert>
#include <format>
#include <tuple>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        [[nodiscard]] frames::TcpSpec peer_spec(const PeerConfig& config) {
            return {.destination_mac  = config.stack_mac,
                    .source_mac       = config.peer_mac,
                    .source           = config.peer_ip,
                    .destination      = config.stack_ip,
                    .source_port      = config.peer_port,
                    .destination_port = config.stack_port,
                    .mss              = config.mss};
        }

        constexpr wire::TcpSequence peer_initial_sequence{1000};

    }  // namespace

    VenuePeer::VenuePeer(ethdev::Port& port, const PeerConfig& config, const std::span<std::uint64_t> starts)
        : port_{&port},
          config_{config},
          script_{peer_spec(config), peer_initial_sequence},
          starts_{starts} {
    }

    std::expected<void, PeerError> VenuePeer::open_active() {
        inject(script_.syn());
        return {};
    }

    std::expected<void, PeerError> VenuePeer::finish_active_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !parsed->tcp->flags.has(wire::TcpFlag::Syn) ||
            !parsed->tcp->flags.has(wire::TcpFlag::Ack)) {
            return std::unexpected{PeerError::Unexpected};
        }
        inject(script_.ack(*parsed));
        carrier_ = std::move(*packet);
        return {};
    }

    std::expected<void, PeerError> VenuePeer::answer_passive_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !parsed->tcp->flags.has(wire::TcpFlag::Syn) ||
            parsed->tcp->flags.has(wire::TcpFlag::Ack)) {
            return std::unexpected{PeerError::Unexpected};
        }
        inject(script_.syn_ack(*parsed));
        carrier_ = std::move(*packet);
        return {};
    }

    std::expected<void, PeerError> VenuePeer::finish_passive_open() {
        std::optional<Packet> packet = collect();
        if (!packet) {
            return std::unexpected{PeerError::Timeout};
        }
        const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
        if (!parsed || !parsed->tcp || !is_pure_ack(*parsed)) {
            return std::unexpected{PeerError::Unexpected};
        }
        script_.see(*parsed);
        ++counters_.surplus_freed;
        return {};
    }

    void VenuePeer::send_tick(const std::uint64_t round, const std::size_t payload) {
        if (carrier_.empty()) {  // the floor has no handshake to seed the carrier: one mbuf of the peer's pool, once
            std::optional<Packet> fresh = port_->allocate(0);
            if (!fresh) {
                return;
            }
            carrier_ = std::move(*fresh);
        }
        const std::vector<std::byte> frame = script_.data(tick_payload(payload, round));
        starts_[round]                     = core::TscClock::get_ticks();
        transmit_carrier(frame);
    }

    std::expected<void, PeerError> VenuePeer::await_order(const std::uint64_t round) {
        for (;;) {
            std::optional<Packet> packet = collect();
            if (!packet) {
                return std::unexpected{PeerError::Timeout};
            }
            const auto parsed = frames::parse_frame(std::span<const std::byte>{packet->data()});
            if (!parsed || !parsed->tcp) {
                ++counters_.unexpected_frames;
                ++counters_.surplus_freed;
                continue;  // the packet frees as it goes out of scope
            }
            if (is_pure_ack(*parsed)) {
                ++counters_.pure_acks_seen;
                if (config_.strict) {
                    script_.see(*parsed);
                }
                ++counters_.surplus_freed;
                continue;
            }
            if (config_.strict) {
                if (frames::l4_checksum_residue(*parsed->ipv4, parsed->l4) != 0 ||
                    wire::internet_checksum(parsed->ipv4_header) != 0) {
                    return std::unexpected{PeerError::BadChecksum};
                }
                if (parsed->tcp->sequence != script_.rcv_nxt()) {
                    return std::unexpected{PeerError::WrongSequence};
                }
                script_.see(*parsed);
            }
            const std::span<const std::byte> payload = frames::tcp_payload(*parsed);
            if (payload.size() < round_prefix || read_round(payload) != round) {
                return std::unexpected{PeerError::WrongRound};
            }
            ++counters_.orders_seen;
            carrier_ = std::move(*packet);
            return {};
        }
    }

    void VenuePeer::send_bare_ack() {
        transmit_carrier(script_.ack());
    }

    std::optional<VenuePeer::Packet> VenuePeer::collect() {
        const core::TimePoint deadline = core::Clock::now() + config_.deadline;
        for (;;) {
            while (next_ < pending_) {
                Packet taken = std::move(burst_[next_++]);
                if (next_ == pending_) {
                    next_ = pending_ = 0;
                }
                const std::span<const std::byte> data = taken.data();
                if (data.size() >= wire::EthernetHeader::size &&
                    wire::load_be16(data.subspan(12, 2)) == frames::ethertype_arp) {
                    if (const auto parsed = frames::parse_frame(data); parsed && parsed->arp) {
                        answer_arp(*parsed);
                    }
                    ++counters_.surplus_freed;
                    continue;
                }
                return taken;
            }
            pending_ = port_->receive(0, burst_);
            next_    = 0;
            if (pending_ == 0 && core::Clock::now() >= deadline) {
                return std::nullopt;
            }
        }
    }

    void VenuePeer::inject(const std::span<const std::byte> frame) {
        std::optional<Packet> packet = port_->allocate(0);
        if (!packet || !frames::fill(*packet, frame)) {
            return;  // the round that waits for the answer times out and names the peer
        }
        std::array<Packet, 1> out{std::move(*packet)};
        std::ignore = port_->transmit(0, out);
    }

    void VenuePeer::transmit_carrier(const std::span<const std::byte> frame) {
        assert(!carrier_.empty() && "the carrier is the stack's last frame; the handshake seeds it");
        carrier_.trim_back(carrier_.size());
        if (!frames::fill(carrier_, frame)) {
            return;
        }
        std::array<Packet, 1> out{std::move(carrier_)};
        if (port_->transmit(0, out) != 1) {
            carrier_ = std::move(out[0]);  // NOLINT(bugprone-use-after-move): a refused transmit leaves the packet
        }
    }

    void VenuePeer::answer_arp(const frames::ParsedFrame& request) {
        if (request.arp->operation != wire::ArpOperation::Request || request.arp->target_ip != config_.peer_ip) {
            return;
        }
        inject(frames::arp_frame(
            frames::arp_reply(config_.peer_mac, config_.peer_ip, request.arp->sender_mac, request.arp->sender_ip),
            request.arp->sender_mac));
        ++counters_.arp_replies_sent;
    }

    bool VenuePeer::is_pure_ack(const frames::ParsedFrame& parsed) noexcept {
        const wire::TcpFlags flags = parsed.tcp->flags;
        return frames::tcp_payload(parsed).empty() && !flags.has(wire::TcpFlag::Syn) &&
               !flags.has(wire::TcpFlag::Fin) && !flags.has(wire::TcpFlag::Rst);
    }

    void drive_peer(VenuePeer& peer, RunState& state, const bool handshake) {
        const auto fail = [&](const PeerError error, const std::uint64_t round) {
            state.fail(std::format("peer, round {}: {}", round, to_string(error)));
        };
        if (handshake) {
            const auto opened = state.config.round == Round::Tick
                                    ? peer.open_active().and_then([&] { return peer.finish_active_open(); })
                                    : peer.answer_passive_open().and_then([&] { return peer.finish_passive_open(); });
            if (!opened) {
                fail(opened.error(), 0);
                return;
            }
        }
        const std::uint64_t gap_ticks = ticks_for(state.config.gap);
        std::uint64_t resume_at       = 0;
        for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
            if (state.config.round == Round::Tick) {
                while (core::TscClock::get_ticks() < resume_at) {
                }
                peer.send_tick(round, state.config.payload);
                if (const auto seen = peer.await_order(round); !seen) {
                    fail(seen.error(), round);
                    return;
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            } else {
                if (const auto seen = peer.await_order(round); !seen) {
                    fail(seen.error(), round);
                    return;
                }
                peer.send_bare_ack();
            }
        }
    }

}  // namespace aloe::benchmarks
```

`std::expected::and_then` with a lambda returning `std::expected<void, PeerError>` is C++23; both compilers have it. `frames::tcp_payload` on a `const ParsedFrame&`: check its parameter is `const ParsedFrame&`; if it takes a non-const reference, take a copy first.

- [ ] **Step 5: Write `level_floor.hpp`**

```cpp
#pragma once

#include <aloe/ethdev>
#include <cstddef>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /// Ethernet, IPv4 and TCP headers without options: where a tick's or an order's payload starts in the frame.
    inline constexpr std::size_t frame_header_size = 14 + 20 + 20;

    /**
     * @brief The floor: nothing between the device and the application.
     *
     * Tick round: receive the raw frame, fold its payload, write the order into a fresh packet from a pre-built frame
     * template, arm, transmit. Gateway round: stamp, write the order, arm, transmit, then receive and drop the peer's
     * bare ACK before the next round. Runs on the calling thread until every round is stamped or `state` stops.
     * With `inline_peer` the peer's side of each round runs inline before and after the stack's (same-core);
     * without it a `drive_peer` on another thread does it.
     */
    void run_floor(RunState& state,
                   StampedDevice<ethdev::Port>& device,
                   const PeerConfig& peer_config,
                   VenuePeer* inline_peer);

}  // namespace aloe::benchmarks
```

- [ ] **Step 6: Write `level_floor.cpp`**

```cpp
#include "level_floor.hpp"

#include <aloe/frames>
#include <aloe/wire>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <utility>
#include <vector>

#include <application_work.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Packet = ethdev::Packet;

        /// The order frame the floor transmits: stack to peer, PSH+ACK, fixed numbers; the peer is not strict here.
        [[nodiscard]] std::vector<std::byte> order_template(const PeerConfig& config) {
            const frames::TcpSpec spec{.destination_mac  = config.peer_mac,
                                       .source_mac       = config.stack_mac,
                                       .source           = config.stack_ip,
                                       .destination      = config.peer_ip,
                                       .source_port      = config.stack_port,
                                       .destination_port = config.peer_port,
                                       .sequence         = wire::TcpSequence{1},
                                       .acknowledgement  = wire::TcpSequence{1},
                                       .flags            = wire::TcpFlags{wire::TcpFlag::Psh, wire::TcpFlag::Ack}};
            return frames::tcp_frame(spec, std::vector<std::byte>(order_size));
        }

        /// Allocates, fills the template, writes the order, arms, transmits. False when the device had no packet.
        [[nodiscard]] bool send_order(StampedDevice<ethdev::Port>& device,
                                      const std::span<const std::byte> frame,
                                      const std::uint64_t round) {
            std::optional<Packet> packet = device.allocate(0);
            if (!packet || !frames::fill(*packet, frame)) {
                return false;
            }
            write_order(packet->data().subspan(frame_header_size), round);
            device.arm(round);
            std::array<Packet, 1> out{std::move(*packet)};
            return device.transmit(0, out) == 1;
        }

        void run_floor_tick(RunState& state, StampedDevice<ethdev::Port>& device, const PeerConfig& peer_config, VenuePeer* inline_peer) {
            const std::vector<std::byte> frame = order_template(peer_config);
            std::vector<Packet> burst(ethdev::Port::max_burst);
            const std::size_t total = state.config.total();
            const auto step = [&] {
                const std::size_t got = device.receive(0, burst);
                for (Packet& packet : std::span{burst}.first(got)) {
                    const std::span<const std::byte> payload = std::span<const std::byte>{packet.data()}.subspan(frame_header_size);
                    const std::uint64_t round                = read_round(payload);
                    state.sink ^= fold(payload);
                    packet = Packet{};  // the tick's mbuf, back to this core's pool
                    if (!send_order(device, frame, round)) {
                        state.fail("floor: no packet for the order");
                        return;
                    }
                    if (round + 1 == total) {
                        state.finish();
                    }
                }
            };
            if (inline_peer == nullptr) {
                while (!state.stop.load()) {
                    step();
                }
                return;
            }
            for (std::uint64_t round = 0; round < total && !state.stop.load(); ++round) {
                inline_peer->send_tick(round, state.config.payload);
                while (device.stamps_taken() <= round && !state.stop.load()) {
                    step();
                }
                if (const auto seen = inline_peer->await_order(round); !seen) {
                    state.fail(std::format("peer, round {}: {}", round, to_string(seen.error())));
                }
            }
        }

        void run_floor_gateway(RunState& state, StampedDevice<ethdev::Port>& device, const PeerConfig& peer_config, VenuePeer* inline_peer) {
            const std::vector<std::byte> frame = order_template(peer_config);
            std::vector<Packet> burst(ethdev::Port::max_burst);
            const std::uint64_t gap_ticks   = ticks_for(state.config.gap);
            const std::uint64_t deadline_ticks = ticks_for(peer_config.deadline);
            std::uint64_t resume_at = 0;
            for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                while (core::TscClock::get_ticks() < resume_at) {
                }
                state.stamps.start[round] = core::TscClock::get_ticks();
                if (!send_order(device, frame, round)) {
                    state.fail("floor: no packet for the order");
                    return;
                }
                if (inline_peer != nullptr) {
                    if (const auto seen = inline_peer->await_order(round); !seen) {
                        state.fail(std::format("peer, round {}: {}", round, to_string(seen.error())));
                        return;
                    }
                    inline_peer->send_bare_ack();
                }
                const std::uint64_t give_up = core::TscClock::get_ticks() + deadline_ticks;
                std::size_t got             = 0;
                while (got == 0 && !state.stop.load()) {
                    got = device.receive(0, burst);
                    if (got == 0 && core::TscClock::get_ticks() > give_up) {
                        state.fail(std::format("floor, round {}: no acknowledgement within the deadline", round));
                        return;
                    }
                }
                for (Packet& packet : std::span{burst}.first(got)) {
                    packet = Packet{};  // the bare ACK rides in the order's own mbuf: back to this core's pool
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            }
        }

    }  // namespace

    void run_floor(RunState& state,
                   StampedDevice<ethdev::Port>& device,
                   const PeerConfig& peer_config,
                   VenuePeer* inline_peer) {
        if (state.config.round == Round::Tick) {
            run_floor_tick(state, device, peer_config, inline_peer);
        } else {
            run_floor_gateway(state, device, peer_config, inline_peer);
        }
    }

}  // namespace aloe::benchmarks
```

In the cross-core gateway round the peer's `drive_peer` awaits each order and answers; the stack side only needs the ACK to arrive. Note that the floor's gateway round counts the device's `receive` of the ACK in the next round's start only through `resume_at`, never inside a window.

Add `venue_peer.cpp` and `level_floor.cpp` to the harness sources and the headers to `target_sources`.

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.VenuePeer && ./build/debug/tests/integration_tests/benchmarks/Aloe.Tests.Integration.Benchmarks.VenuePeer`
Expected: PASS, 5 tests. The floor has no handshake to seed the carrier, which is why `send_tick` allocates one mbuf
from the peer's pool when the carrier is empty: the stack frees that one on its core once, a put without a get,
which the docs mention for the floor rows.

- [ ] **Step 8: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/integration_tests/benchmarks
git commit -m "feat(benchmarks): the scripted venue peer and the floor round"
```

---

## Task 6: Level 0, the loop over the bricks

**Files:**
- Create: `benchmarks/tick_to_trade/stack_sanity.hpp`
- Create: `benchmarks/tick_to_trade/level_bricks.hpp`
- Create: `benchmarks/tick_to_trade/level_bricks.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/unit_tests/benchmarks/test_stack_config.cpp`
- Modify: `tests/unit_tests/benchmarks/CMakeLists.txt`
- Create: `tests/integration_tests/benchmarks/test_levels.cpp`
- Modify: `tests/integration_tests/benchmarks/CMakeLists.txt` (the target `${INTEGRATION_TESTING_TARGET}.Benchmarks.Levels`, linked like the venue's plus `${SHARED_TESTING_TARGET}.Log`, which the runtime's tests link for quill)

**Interfaces:**
- Consumes: `VenuePeer`, `PeerConfig`, `drive_peer` (Task 5); `StampedDevice`, `RunState`, `Phase`, `ticks_for`, the application work (Task 3); `loop::ShardCounters`, `loop::ShardQueue`, `loop::TimerWheel`, `net::Ipv4`, `net::Ipv4Config`, `net::Ipv4Counters`, `tcp::Stack`, `tcp::TcpCounters`, `tcp::Endpoint`, `stream::Events`.
- Produces: `check_stack_counters(RunState&, const tcp::TcpCounters&, const net::Ipv4Counters&, const loop::ShardCounters&)` (stack_sanity.hpp); `stack_ip_config(const PeerConfig&) -> net::Ipv4Config` and `run_bricks(RunState&, StampedDevice<ethdev::Port>&, const PeerConfig&, bool arp_on_wire, VenuePeer* inline_peer)` (level_bricks.hpp).

Spec: "The application at each level" (level 0 is the `tcp_echo` loop), "Breakdown mode, level 0 only", "Sanity", "The scripted peer" (setup: `learn` unless `--arp-on-wire`, a long `reachable` and `expire`).

- [ ] **Step 1: Write the failing tests**

`tests/unit_tests/benchmarks/test_stack_config.cpp` (Review Focus 5):

```cpp
#include <chrono>

#include <gtest/gtest.h>
#include <level_bricks.hpp>

TEST(StackConfig, ArpNeverGoesStaleDuringARun) {
    const aloe::benchmarks::PeerConfig peer{};
    const aloe::net::Ipv4Config config = aloe::benchmarks::stack_ip_config(peer);
    EXPECT_EQ(config.address, peer.stack_ip);
    EXPECT_EQ(config.prefix, 24U);
    EXPECT_GE(config.arp_reachable, std::chrono::hours{24}) << "a gapped run can last minutes; no refresh on the hot path";
    EXPECT_GT(config.arp_expire, config.arp_reachable);
}
```

`tests/integration_tests/benchmarks/test_levels.cpp`:

```cpp
#include <aloe/ethdev>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <level_bricks.hpp>
#include <ring_pair.hpp>
#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

// Level 0 and, from Task 7, level 1 against the scripted venue over the ring pair: the handshake both ways, closed-loop
// rounds, the recycled mbufs, the counters at zero. Same-core inline, cross-core on an unpinned thread. No root.
namespace {

    using namespace aloe::benchmarks;
    using namespace std::chrono_literals;
    using Stamped = StampedDevice<aloe::ethdev::Port>;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] RoundConfig small(const Round round, const Level level, const Topology topology,
                                    const std::size_t payload, const bool breakdown = false) {
        return {.round = round, .level = level, .topology = topology, .payload = payload,
                .rounds = 40, .warmup = 4, .breakdown = breakdown};
    }

    [[nodiscard]] PeerConfig strict_peer(RingPair& pair) {
        return {.stack_mac = pair.stack().mac(), .deadline = 2s};
    }

    void expect_closed_loop(const RunState& state, const Stamped& device, const VenuePeer& peer) {
        EXPECT_FALSE(state.failed.load()) << state.failure;
        EXPECT_EQ(device.stamps_taken(), state.config.total());
        EXPECT_EQ(peer.counters().orders_seen, state.config.total());
        for (std::size_t round = 0; round < state.config.total(); ++round) {
            EXPECT_NE(state.stamps.start[round], 0U) << round;
            EXPECT_GT(state.stamps.stop[round], state.stamps.start[round]) << round;
        }
    }

    /// Runs level 0 over a fresh pair: inline peer for same-core, a thread otherwise.
    void run_level0(const RoundConfig& config, const char* name, const bool arp_on_wire = false) {
        RingPair pair{{.name = name, .pool_size = 512}};
        RunState state{config};
        Stamped device{pair.stack(), state.stamps.stop};
        const PeerConfig peer_config = strict_peer(pair);
        VenuePeer peer{pair.peer(), peer_config, state.stamps.start};
        if (config.topology == Topology::SameCore) {
            run_bricks(state, device, peer_config, arp_on_wire, &peer);
        } else {
            std::jthread venue{[&] { drive_peer(peer, state, true); }};
            run_bricks(state, device, peer_config, arp_on_wire, nullptr);
            venue.join();
        }
        expect_closed_loop(state, device, peer);
        if (config.payload == 1460) {
            EXPECT_GE(peer.counters().pure_acks_seen, state.config.total() / 2 - 1) << "the second-segment ACK";
        } else {
            EXPECT_EQ(peer.counters().pure_acks_seen, 0U);
        }
        if (config.breakdown) {
            for (std::size_t round = 0; round < state.config.total(); ++round) {
                std::uint64_t previous = state.stamps.start[round];
                for (const auto& phase : state.stamps.phases) {
                    EXPECT_GE(phase[round], previous) << round;
                    previous = phase[round];
                }
                EXPECT_GE(state.stamps.stop[round], previous) << round;
            }
        }
    }

}  // namespace

TEST(Levels, BricksTickSameCore) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::SameCore, 64, true), "lv0-tick-same");
}

TEST(Levels, BricksTickSameCoreFullSegmentCarriesTheSecondSegmentAck) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::SameCore, 1460), "lv0-tick-same-1460");
}

TEST(Levels, BricksTickCrossCore) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::CrossCore, 256), "lv0-tick-cross");
}

TEST(Levels, BricksTickWithArpOnTheWire) {
    run_level0(small(Round::Tick, Level::Bricks, Topology::CrossCore, 64), "lv0-tick-arp", true);
}

TEST(Levels, BricksGatewaySameCore) {
    run_level0(small(Round::Gateway, Level::Bricks, Topology::SameCore, order_size), "lv0-gw-same");
}

TEST(Levels, BricksGatewayCrossCore) {
    run_level0(small(Round::Gateway, Level::Bricks, Topology::CrossCore, order_size), "lv0-gw-cross");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks Aloe.Tests.Integration.Benchmarks.Levels`
Expected: FAIL to compile, headers not found.

- [ ] **Step 3: Write `stack_sanity.hpp`**

```cpp
#pragma once

#include <aloe/loop>
#include <aloe/net>
#include <aloe/tcp>
#include <format>
#include <string>

#include <run_state.hpp>

namespace aloe::benchmarks {

    /// Fails `state` when the stack dropped, refused or starved anything during the run: a benchmark that measured
    /// garbage must say so instead of reporting it.
    inline void check_stack_counters(RunState& state,
                                     const tcp::TcpCounters& tcp,
                                     const net::Ipv4Counters& ip,
                                     const loop::ShardCounters& shard) {
        std::string problems;
        const auto expect_zero = [&problems](const std::uint64_t value, const char* name) {
            if (value != 0) {
                problems += std::format(" {}={}", name, value);
            }
        };
        expect_zero(tcp.dropped_bad_header, "tcp.dropped_bad_header");
        expect_zero(tcp.dropped_bad_checksum, "tcp.dropped_bad_checksum");
        expect_zero(tcp.dropped_no_connection, "tcp.dropped_no_connection");
        expect_zero(tcp.dropped_closed, "tcp.dropped_closed");
        expect_zero(tcp.dropped_unexpected, "tcp.dropped_unexpected");
        expect_zero(tcp.dropped_duplicate, "tcp.dropped_duplicate");
        expect_zero(tcp.dropped_out_of_order, "tcp.dropped_out_of_order");
        expect_zero(tcp.dropped_out_of_window, "tcp.dropped_out_of_window");
        expect_zero(tcp.dropped_no_slot, "tcp.dropped_no_slot");
        expect_zero(tcp.dropped_no_node, "tcp.dropped_no_node");
        expect_zero(tcp.dropped_table_full, "tcp.dropped_table_full");
        expect_zero(tcp.send_refused, "tcp.send_refused");
        expect_zero(tcp.commits_refused, "tcp.commits_refused");
        expect_zero(tcp.allocation_failures, "tcp.allocation_failures");
        expect_zero(tcp.retransmits, "tcp.retransmits");
        expect_zero(ip.dropped_ethertype, "ip.dropped_ethertype");
        expect_zero(ip.dropped_arp_malformed, "ip.dropped_arp_malformed");
        expect_zero(ip.dropped_arp_conflict, "ip.dropped_arp_conflict");
        expect_zero(ip.dropped_bad_header, "ip.dropped_bad_header");
        expect_zero(ip.dropped_bad_checksum, "ip.dropped_bad_checksum");
        expect_zero(ip.dropped_fragment, "ip.dropped_fragment");
        expect_zero(ip.dropped_protocol, "ip.dropped_protocol");
        expect_zero(ip.dropped_martian, "ip.dropped_martian");
        expect_zero(ip.dropped_tcp_broadcast, "ip.dropped_tcp_broadcast");
        expect_zero(ip.send_no_route, "ip.send_no_route");
        expect_zero(ip.send_oversized, "ip.send_oversized");
        expect_zero(ip.send_refused, "ip.send_refused");
        expect_zero(shard.transmit_refused, "shard.transmit_refused");
        if (!problems.empty()) {
            state.fail("the stack's counters are not clean:" + problems);
        }
    }

}  // namespace aloe::benchmarks
```

`ip.send_unresolved` and `tcp.send_unresolved` are allowed: with `--arp-on-wire` the first SYN or SYN-ACK waits for the reply once. `ip.dropped_arp_unsolicited` is allowed too: the peer's reply to a request the stack never sent cannot happen here, but the rule is the brick's, not the benchmark's.

- [ ] **Step 4: Write `level_bricks.hpp`**

```cpp
#pragma once

#include <aloe/ethdev>
#include <aloe/net>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /// The IP brick's configuration at every level: the stack's address, and an ARP cache whose entries outlive any
    /// run, so no refresh request lands on the hot path.
    [[nodiscard]] inline net::Ipv4Config stack_ip_config(const PeerConfig& peer) noexcept {
        return {.address       = peer.stack_ip,
                .prefix        = 24,
                .arp_reachable = std::chrono::hours{24},
                .arp_expire    = std::chrono::hours{48}};
    }

    /**
     * @brief Level 0: the hand-written loop over the bricks, `examples/tcp_echo`'s loop with the application in the
     * event drain.
     *
     * Tick round: the stack listens, the peer opens; on Readable with the whole tick unread, fold it, consume it,
     * prepare, write the order, arm, commit; `tcp.flush` and `queue.flush` carry it out. Gateway round: the stack
     * connects; once connected, and after each order is acknowledged and the gap has passed, stamp the start, prepare,
     * write, arm, commit. `--breakdown` stamps the phases of the tick round (`Phase`); the gateway round records the
     * Application and TcpFlush phases only. Runs on the calling thread until every round is stamped or `state` stops,
     * then checks the counters. With `inline_peer` the peer's side runs inline between steps (same-core).
     */
    void run_bricks(RunState& state,
                    StampedDevice<ethdev::Port>& device,
                    const PeerConfig& peer_config,
                    bool arp_on_wire,
                    VenuePeer* inline_peer);

}  // namespace aloe::benchmarks
```

- [ ] **Step 5: Write `level_bricks.cpp`**

```cpp
#include "level_bricks.hpp"

#include <aloe/loop>
#include <aloe/stream>
#include <aloe/tcp>
#include <aloe/wire>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <application_work.hpp>
#include <clock.hpp>
#include <stack_sanity.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Device = StampedDevice<ethdev::Port>;
        using Ipv4   = net::Ipv4<Device>;
        using Tcp    = tcp::Stack<Ipv4>;
        using Packet = ethdev::Packet;

        /// The bricks of one shard and the loop over them, with the two applications.
        class BricksLoop {
        public:
            BricksLoop(RunState& state, Device& device, const PeerConfig& peer_config, const bool arp_on_wire)
                : state_{&state},
                  device_{&device},
                  queue_{device, 0, 512, counters_},
                  ip_{queue_, stack_ip_config(peer_config)},
                  wheel_{std::chrono::milliseconds{1}, core::Clock::now()},
                  tcp_{ip_, wheel_, {}},
                  gap_ticks_{ticks_for(state.config.gap)},
                  deadline_ticks_{ticks_for(peer_config.deadline)},
                  progress_{core::TscClock::get_ticks()} {
                if (!arp_on_wire) {
                    ip_.learn(peer_config.peer_ip, peer_config.peer_mac, core::Clock::now());
                }
                if (state.config.round == Round::Tick) {
                    if (!tcp_.listen(peer_config.stack_port)) {
                        state.fail("level 0: listen refused");
                    }
                } else {
                    const auto opened = tcp_.connect({.address = peer_config.peer_ip, .port = peer_config.peer_port},
                                                     core::Clock::now());
                    if (!opened) {
                        state.fail(std::format("level 0: connect refused ({})", static_cast<int>(opened.error())));
                    }
                }
            }

            /// One iteration of the loop. Returns once every round is out, or on a failure.
            void step() {
                const core::TimePoint now  = core::Clock::now();
                const bool breakdown       = state_->config.breakdown;
                const std::size_t received = queue_.receive(burst_);
                const std::uint64_t t_received = breakdown && received > 0 ? core::TscClock::get_ticks() : 0;
                ip_.process(std::span<Packet>{burst_}.first(received), now);
                const std::uint64_t t_ip = breakdown && received > 0 ? core::TscClock::get_ticks() : 0;
                tcp_.process(ip_.received(wire::Ipv4Protocol::Tcp), now);
                const std::uint64_t t_tcp = breakdown && received > 0 ? core::TscClock::get_ticks() : 0;
                std::ignore = wheel_.advance(now);
                std::optional<std::uint64_t> stamped;
                while (const auto event = tcp_.poll_event()) {
                    progress_ = core::TscClock::get_ticks();
                    Tcp::ConnectionType& c            = *event->connection;
                    const stream::Events events       = event->events;
                    if (events.accepted() || events.connected()) {
                        connection_ = &c;
                    }
                    if (events.readable() && state_->config.round == Round::Tick) {
                        stamped = answer_tick(c);
                    }
                    if (events.closed() || events.reset() || events.timed_out() || events.unresolved()) {
                        state_->fail(std::format("level 0: the connection ended, events {:#x}", events.raw()));
                        return;
                    }
                }
                if (state_->config.round == Round::Gateway && connection_ != nullptr) {
                    stamped = issue_order();
                }
                const std::uint64_t t_application = breakdown && stamped ? core::TscClock::get_ticks() : 0;
                tcp_.flush(now);
                const std::uint64_t t_flush = breakdown && stamped ? core::TscClock::get_ticks() : 0;
                std::ignore = queue_.flush();
                if (breakdown && stamped) {
                    auto& phases = state_->stamps.phases;
                    phases[static_cast<std::size_t>(Phase::Received)][*stamped]    = t_received;
                    phases[static_cast<std::size_t>(Phase::Ip)][*stamped]          = t_ip;
                    phases[static_cast<std::size_t>(Phase::Tcp)][*stamped]         = t_tcp;
                    phases[static_cast<std::size_t>(Phase::Application)][*stamped] = t_application;
                    phases[static_cast<std::size_t>(Phase::TcpFlush)][*stamped]    = t_flush;
                }
                if (core::TscClock::get_ticks() - progress_ > deadline_ticks_) {
                    state_->fail("level 0: nothing happened within the deadline");
                }
            }

            [[nodiscard]] bool acknowledged() const noexcept {
                return connection_ != nullptr && !order_in_flight_;
            }

            [[nodiscard]] std::uint64_t frames_transmitted() const noexcept {
                return counters_.frames_transmitted;
            }

            [[nodiscard]] bool connected() const noexcept {
                return connection_ != nullptr;
            }

            void check() {
                check_stack_counters(*state_, tcp_.counters(), ip_.counters(), counters_);
            }

        private:
            /// The tick-to-trade application: the whole tick must be unread; returns the round it answered.
            [[nodiscard]] std::optional<std::uint64_t> answer_tick(Tcp::ConnectionType& c) {
                const auto view = c.unread();
                if (view.size() < state_->config.payload) {
                    return std::nullopt;  // a partial tick: not in this benchmark, but never a wrong answer
                }
                const std::uint64_t round = read_round(view.front());
                state_->sink ^= fold(view);
                c.consume(view.size());
                const auto out = c.prepare(order_size);
                if (!out) {
                    state_->fail(std::format("level 0, round {}: prepare refused, writable {}", round, c.writable()));
                    return std::nullopt;
                }
                write_order(*out, round);
                device_->arm(round);
                if (!c.commit(order_size)) {
                    state_->fail(std::format("level 0, round {}: commit refused", round));
                    return std::nullopt;
                }
                if (round + 1 == state_->config.total()) {
                    state_->finish();  // after this step's flush carries the order out
                }
                return round;
            }

            /// The gateway application: one order in flight; the next after its ACK and the gap.
            [[nodiscard]] std::optional<std::uint64_t> issue_order() {
                if (order_in_flight_ && connection_->acknowledged() == connection_->committed()) {
                    order_in_flight_ = false;
                    progress_        = core::TscClock::get_ticks();
                    resume_at_       = progress_ + gap_ticks_;
                    if (next_round_ == state_->config.total()) {
                        state_->finish();
                    }
                }
                if (order_in_flight_ || next_round_ == state_->config.total() ||
                    core::TscClock::get_ticks() < resume_at_) {
                    return std::nullopt;
                }
                const std::uint64_t round     = next_round_++;
                state_->stamps.start[round]   = core::TscClock::get_ticks();
                const auto out                = connection_->prepare(order_size);
                if (!out) {
                    state_->fail(std::format("level 0, round {}: prepare refused", round));
                    return std::nullopt;
                }
                write_order(*out, round);
                device_->arm(round);
                if (!connection_->commit(order_size)) {
                    state_->fail(std::format("level 0, round {}: commit refused", round));
                    return std::nullopt;
                }
                order_in_flight_ = true;
                return round;
            }

            RunState* state_;
            Device* device_;
            loop::ShardCounters counters_;
            loop::ShardQueue<Device> queue_;
            Ipv4 ip_;
            loop::TimerWheel wheel_;
            Tcp tcp_;
            std::vector<Packet> burst_ = std::vector<Packet>(ethdev::Port::max_burst);
            Tcp::ConnectionType* connection_ = nullptr;
            bool order_in_flight_            = false;
            std::uint64_t next_round_        = 0;
            std::uint64_t gap_ticks_;
            std::uint64_t deadline_ticks_;
            std::uint64_t resume_at_ = 0;
            std::uint64_t progress_;
        };

        /// Same-core: the peer's side between steps, with the handshake split so the stack can answer each segment.
        void run_inline(RunState& state, Device& device, BricksLoop& loop, VenuePeer& peer) {
            const auto until = [&](auto&& done) {
                while (!done() && !state.stop.load()) {
                    loop.step();
                }
            };
            const auto fail = [&](const PeerError error, const std::uint64_t round) {
                state.fail(std::format("peer, round {}: {}", round, to_string(error)));
            };
            if (state.config.round == Round::Tick) {
                if (const auto r = peer.open_active(); !r) { fail(r.error(), 0); return; }
                until([&] { return loop.frames_transmitted() >= 1; });  // the SYN-ACK is out
                if (const auto r = peer.finish_active_open(); !r) { fail(r.error(), 0); return; }
                until([&] { return loop.connected(); });
                for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                    peer.send_tick(round, state.config.payload);
                    until([&] { return device.stamps_taken() > round; });
                    if (const auto r = peer.await_order(round); !r) { fail(r.error(), round); return; }
                }
                return;
            }
            until([&] { return loop.frames_transmitted() >= 1; });  // the SYN is out
            if (const auto r = peer.answer_passive_open(); !r) { fail(r.error(), 0); return; }
            until([&] { return loop.connected(); });  // Connected raised, the handshake ACK out in that step's flush
            if (const auto r = peer.finish_passive_open(); !r) { fail(r.error(), 0); return; }
            for (std::uint64_t round = 0; round < state.config.total() && !state.stop.load(); ++round) {
                until([&] { return device.stamps_taken() > round; });
                if (const auto r = peer.await_order(round); !r) { fail(r.error(), round); return; }
                peer.send_bare_ack();
                until([&] { return loop.acknowledged(); });
            }
        }

    }  // namespace

    void run_bricks(RunState& state,
                    Device& device,
                    const PeerConfig& peer_config,
                    const bool arp_on_wire,
                    VenuePeer* inline_peer) {
        BricksLoop loop{state, device, peer_config, arp_on_wire};
        if (inline_peer != nullptr) {
            run_inline(state, device, loop, *inline_peer);
        } else {
            while (!state.stop.load()) {
                loop.step();
            }
        }
        loop.check();
    }

}  // namespace aloe::benchmarks
```

Two details the signatures do not decide: `finish()` on the last round happens before that step's flush, and the cross-core loop still runs the flush of the current step because `step()` returns only after it; a same-core `until` loop sees `stop` set and does not call `step()` again, which is right because the order left in the step that set it. And `step()` resets `progress_` on every event, so the deadline covers only a silent peer.

Add `level_bricks.cpp` to the harness sources and the two headers to `target_sources`; add `test_stack_config.cpp` to the unit target; add the integration target.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks Aloe.Tests.Integration.Benchmarks.Levels && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/integration_tests/benchmarks/Aloe.Tests.Integration.Benchmarks.Levels`
Expected: PASS, 20 unit tests and 6 level tests. If `BricksTickWithArpOnTheWire` fails with `dropped_arp_unsolicited`, the stack's request went out before the listen and the peer answered a different one; the counter is allowed by `check_stack_counters`, so look for the real cause in `tcp.send_unresolved` and the peer's `arp_replies_sent`.

- [ ] **Step 7: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/unit_tests/benchmarks tests/integration_tests/benchmarks
git commit -m "feat(benchmarks): level 0, the loop over the bricks, with breakdown stamps"
```

---

## Task 7: Level 1, the runtime

**Files:**
- Create: `benchmarks/tick_to_trade/level_runtime.hpp`
- Create: `benchmarks/tick_to_trade/level_runtime.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Modify: `tests/integration_tests/benchmarks/test_levels.cpp` (three tests)

**Interfaces:**
- Consumes: `stack_ip_config`, `check_stack_counters` (Task 6); `runtime::Runtime`, `runtime::RuntimeConfig`, `runtime::ShardThread`, `runtime::IdlePolicy`, `runtime::TcpStack`, `runtime::task`, `ethdev::register_thread`.
- Produces: `run_runtime(RunState&, StampedDevice<ethdev::Port>&, const PeerConfig&, bool arp_on_wire, std::optional<unsigned> stack_core)` (level_runtime.hpp).

Spec: "The application at each level" (level 1), "The order, gateway round" (level 1), "`--same-core` applies to the floor and level 0 only".

- [ ] **Step 1: Write the failing tests**

Append to `tests/integration_tests/benchmarks/test_levels.cpp`, with `#include <level_runtime.hpp>` and `#include <optional>`:

```cpp
namespace {

    /// Runs level 1 over a fresh pair with the peer on an unpinned thread; the shard thread is unpinned too.
    void run_level1(const RoundConfig& config, const char* name) {
        RingPair pair{{.name = name, .pool_size = 512}};
        RunState state{config};
        Stamped device{pair.stack(), state.stamps.stop};
        const PeerConfig peer_config = strict_peer(pair);
        VenuePeer peer{pair.peer(), peer_config, state.stamps.start};
        std::jthread venue{[&] { drive_peer(peer, state, true); }};
        run_runtime(state, device, peer_config, false, std::nullopt);
        venue.join();
        expect_closed_loop(state, device, peer);
    }

}  // namespace

TEST(Levels, RuntimeTickCrossCore) {
    run_level1(small(Round::Tick, Level::Runtime, Topology::CrossCore, 64), "lv1-tick");
}

TEST(Levels, RuntimeTickFullSegment) {
    run_level1(small(Round::Tick, Level::Runtime, Topology::CrossCore, 1460), "lv1-tick-1460");
}

TEST(Levels, RuntimeGatewayCrossCore) {
    run_level1(small(Round::Gateway, Level::Runtime, Topology::CrossCore, order_size), "lv1-gw");
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.Levels`
Expected: FAIL to compile, `level_runtime.hpp` not found.

- [ ] **Step 3: Write `level_runtime.hpp`**

```cpp
#pragma once

#include <aloe/ethdev>
#include <optional>

#include <run_state.hpp>
#include <stamped_device.hpp>
#include <venue_peer.hpp>

namespace aloe::benchmarks {

    /**
     * @brief Level 1: the runtime with one shard, pinned to `stack_core` when given, and one task.
     *
     * Tick round: the task accepts, then loops `readable(payload)`, fold, consume, prepare, write, arm, commit; the
     * shard's flush carries the order out. Gateway round: the task connects, then per round stamps the start,
     * prepares, writes, arms, commits and awaits `acked(committed())`, with the gap before the next. The calling
     * thread sleeps on `state.stop` meanwhile, stops and joins the runtime, then checks the counters. Cross-core only.
     */
    void run_runtime(RunState& state,
                     StampedDevice<ethdev::Port>& device,
                     const PeerConfig& peer_config,
                     bool arp_on_wire,
                     std::optional<unsigned> stack_core);

}  // namespace aloe::benchmarks
```

- [ ] **Step 4: Write `level_runtime.cpp`**

```cpp
#include "level_runtime.hpp"

#include <aloe/runtime>
#include <aloe/stream>
#include <aloe/tcp>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <application_work.hpp>
#include <clock.hpp>
#include <level_bricks.hpp>
#include <stack_sanity.hpp>
#include <tsc_clock.hpp>

namespace aloe::benchmarks {

    namespace {

        using Device  = StampedDevice<ethdev::Port>;
        using Stack   = runtime::TcpStack<Device>;
        using Runtime = runtime::Runtime<Device, Stack>;

        /// The tick-to-trade application as a task.
        runtime::task<void> trade(Stack::StreamsType* streams, Device* device, RunState* state, const std::uint16_t port) {
            auto stream = co_await streams->accept(port);
            if (!stream) {
                state->fail(std::format("level 1: accept failed, stream error {}", static_cast<int>(stream.error())));
                co_return;
            }
            const std::size_t total = state->config.total();
            for (;;) {
                const auto readable = co_await stream->readable(state->config.payload);
                if (!readable) {
                    state->fail(std::format("level 1: readable failed, stream error {}", static_cast<int>(readable.error())));
                    break;
                }
                const auto view           = stream->unread();
                const std::uint64_t round = read_round(view.front());
                state->sink ^= fold(view);
                stream->consume(view.size());
                const auto out = stream->prepare(order_size);
                if (!out) {
                    state->fail(std::format("level 1, round {}: prepare refused", round));
                    break;
                }
                write_order(*out, round);
                device->arm(round);
                if (!stream->commit(order_size)) {
                    state->fail(std::format("level 1, round {}: commit refused", round));
                    break;
                }
                if (round + 1 == total) {
                    break;
                }
            }
            state->finish();
        }

        /// The gateway application as a task.
        runtime::task<void> orders(Stack::StreamsType* streams, Device* device, RunState* state, const tcp::Endpoint venue) {
            auto stream = co_await streams->connect(venue);
            if (!stream) {
                state->fail(std::format("level 1: connect failed, stream error {}", static_cast<int>(stream.error())));
                co_return;
            }
            const std::uint64_t gap_ticks = ticks_for(state->config.gap);
            std::uint64_t resume_at       = 0;
            for (std::uint64_t round = 0; round < state->config.total(); ++round) {
                while (core::TscClock::get_ticks() < resume_at) {
                }
                state->stamps.start[round] = core::TscClock::get_ticks();
                const auto out             = stream->prepare(order_size);
                if (!out) {
                    state->fail(std::format("level 1, round {}: prepare refused", round));
                    break;
                }
                write_order(*out, round);
                device->arm(round);
                if (!stream->commit(order_size)) {
                    state->fail(std::format("level 1, round {}: commit refused", round));
                    break;
                }
                const auto acked = co_await stream->acked(stream->committed());
                if (!acked) {
                    state->fail(std::format("level 1, round {}: acked failed, stream error {}", round, static_cast<int>(acked.error())));
                    break;
                }
                resume_at = core::TscClock::get_ticks() + gap_ticks;
            }
            state->finish();
        }

    }  // namespace

    void run_runtime(RunState& state,
                     Device& device,
                     const PeerConfig& peer_config,
                     const bool arp_on_wire,
                     const std::optional<unsigned> stack_core) {
        Runtime runtime{
            {.shard       = {.idle = runtime::IdlePolicy::Spin},
             .threads     = {{.cpu = stack_core, .name = "aloe-bench-shard"}},
             .thread_hook = ethdev::register_thread},
            device,
            stack_ip_config(peer_config),
            tcp::TcpConfig{}
        };
        if (!arp_on_wire) {
            runtime.shard(0).stack().ip().learn(peer_config.peer_ip, peer_config.peer_mac, core::Clock::now());
        }
        runtime.start();
        Stack::StreamsType* streams = &runtime.shard(0).stack().streams();
        if (state.config.round == Round::Tick) {
            runtime.spawn(0, trade(streams, &device, &state, peer_config.stack_port));
        } else {
            runtime.spawn(0, orders(streams, &device, &state, {.address = peer_config.peer_ip, .port = peer_config.peer_port}));
        }
        state.stop.wait(false);
        runtime.stop();
        runtime.join();
        check_stack_counters(state, runtime.shard(0).stack().tcp().counters(), runtime.shard(0).stack().ip().counters(), runtime.counters(0));
    }

}  // namespace aloe::benchmarks
```

When the task ends its `Stream` is destroyed and `release()` aborts the connection with a RST after the last order; the peer has finished by then and the frame is drained between configurations (Task 10). The task spins through the gap on the shard thread, which is idle at that point: the previous order's ACK completed `acked` before the spin starts.

Add `level_runtime.cpp` and the header to the harness.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.Levels && ./build/debug/tests/integration_tests/benchmarks/Aloe.Tests.Integration.Benchmarks.Levels`
Expected: PASS, 9 tests. A hang in `state.stop.wait(false)` means `finish()` or `fail()` was reached without a `notify_all`; both call it (Task 3).

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/integration_tests/benchmarks
git commit -m "feat(benchmarks): level 1, the runtime with one pinned shard and a task"
```

---

## Task 8: Hugepages, reserved by the process when it can

**Files:**
- Create: `benchmarks/tick_to_trade/hugepages.hpp`
- Create: `benchmarks/tick_to_trade/hugepages.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/unit_tests/benchmarks/test_hugepages.cpp`
- Modify: `tests/unit_tests/benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing of the harness.
- Produces: `hugepage_root` (`/sys/kernel/mm/hugepages/hugepages-2048kB`); `class HugepageError : std::runtime_error`; `class HugepageSysfs` with `virtual std::uint64_t read(std::string_view file)` and `virtual void write_reserved(std::uint64_t pages)`; `class FileHugepageSysfs final : HugepageSysfs` with `explicit FileHugepageSysfs(std::filesystem::path root = hugepage_root)`; `class HugepageReservation` with `HugepageReservation(HugepageSysfs&, std::uint64_t needed, bool keep)`, `reserved_by_us()`, `free_before()`; `manual_reservation_command(std::uint64_t pages) -> std::string`.

Spec: "Hugepages are turned on by the benchmark itself, for its own run, whenever it can" (the paragraph under "Build and command line").

- [ ] **Step 1: Write the failing test**

`tests/unit_tests/benchmarks/test_hugepages.cpp`:

```cpp
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <hugepages.hpp>

namespace {

    using namespace aloe::benchmarks;

    /// A kernel in miniature: `nr_hugepages` and `free_hugepages`, a write that may be refused or only partly honoured.
    class FakeSysfs final : public HugepageSysfs {
    public:
        std::uint64_t reserved = 0;
        std::uint64_t free     = 0;
        bool refuse            = false;  ///< Not root.
        std::uint64_t grant_at_most = UINT64_MAX;  ///< A fragmented machine reserves fewer than asked.
        std::vector<std::uint64_t> writes;

        [[nodiscard]] std::uint64_t read(const std::string_view file) override {
            return file == "nr_hugepages" ? reserved : free;
        }

        void write_reserved(const std::uint64_t pages) override {
            if (refuse) {
                throw std::system_error{std::make_error_code(std::errc::permission_denied), "nr_hugepages"};
            }
            writes.push_back(pages);
            const std::uint64_t granted = pages > reserved ? std::min(pages - reserved, grant_at_most) : 0;
            if (pages >= reserved) {
                reserved += granted;
                free += granted;
            } else {
                const std::uint64_t released = std::min(reserved - pages, free);
                reserved -= released;
                free -= released;
            }
        }
    };

}  // namespace

TEST(Hugepages, EnoughFreePagesWritesNothing) {
    FakeSysfs sysfs;
    sysfs.reserved = 128;
    sysfs.free     = 100;
    {
        const HugepageReservation reservation{sysfs, 64, false};
        EXPECT_EQ(reservation.reserved_by_us(), 0U);
        EXPECT_EQ(reservation.free_before(), 100U);
    }
    EXPECT_TRUE(sysfs.writes.empty());
}

TEST(Hugepages, TheShortfallIsReservedAndReleasedAtExit) {
    FakeSysfs sysfs;
    sysfs.reserved = 10;
    sysfs.free     = 4;
    {
        const HugepageReservation reservation{sysfs, 64, false};
        EXPECT_EQ(reservation.reserved_by_us(), 60U);
        EXPECT_EQ(sysfs.reserved, 70U);
        EXPECT_EQ(sysfs.free, 64U);
    }
    EXPECT_EQ(sysfs.reserved, 10U) << "put back to what was found";
    EXPECT_EQ(sysfs.writes, (std::vector<std::uint64_t>{70, 10}));
}

TEST(Hugepages, KeepLeavesTheReservation) {
    FakeSysfs sysfs;
    {
        const HugepageReservation reservation{sysfs, 8, true};
        EXPECT_EQ(reservation.reserved_by_us(), 8U);
    }
    EXPECT_EQ(sysfs.reserved, 8U);
}

TEST(Hugepages, AFragmentedMachineFailsWithTheShortfallAndRestores) {
    FakeSysfs sysfs;
    sysfs.grant_at_most = 32;
    try {
        const HugepageReservation reservation{sysfs, 64, false};
        FAIL() << "half the pages is a failure";
    } catch (const HugepageError& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("32 of the 64"), std::string::npos) << what;
        EXPECT_NE(what.find("compact_memory"), std::string::npos) << what;
    }
    EXPECT_EQ(sysfs.reserved, 0U) << "the partial reservation was put back";
}

TEST(Hugepages, WithoutPrivilegeTheErrorNamesBothRemedies) {
    FakeSysfs sysfs;
    sysfs.refuse = true;
    try {
        const HugepageReservation reservation{sysfs, 64, false};
        FAIL() << "no privilege, no pages";
    } catch (const HugepageError& error) {
        const std::string what = error.what();
        EXPECT_NE(what.find("sudo"), std::string::npos) << what;
        EXPECT_NE(what.find(manual_reservation_command(64)), std::string::npos) << what;
    }
}

TEST(Hugepages, FileSysfsReadsAndWritesUnderARoot) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aloe-hugepages-test";
    std::filesystem::create_directories(root);
    std::ofstream{root / "nr_hugepages"} << "12\n";
    std::ofstream{root / "free_hugepages"} << "5\n";
    FileHugepageSysfs sysfs{root};
    EXPECT_EQ(sysfs.read("nr_hugepages"), 12U);
    EXPECT_EQ(sysfs.read("free_hugepages"), 5U);
    sysfs.write_reserved(40);
    EXPECT_EQ(sysfs.read("nr_hugepages"), 40U);
    std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks`
Expected: FAIL to compile, `hugepages.hpp` not found.

- [ ] **Step 3: Write `hugepages.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aloe::benchmarks {

    /// Where Linux keeps the 2 MB hugepage counts.
    inline const std::filesystem::path hugepage_root{"/sys/kernel/mm/hugepages/hugepages-2048kB"};

    /// The run cannot have the hugepages it needs; `what()` is the message for the user, remedy included.
    class HugepageError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// The two files a reservation reads and the one it writes, behind an interface so a test needs no root.
    class HugepageSysfs {
    public:
        HugepageSysfs()                                = default;
        HugepageSysfs(const HugepageSysfs&)            = delete;
        HugepageSysfs& operator=(const HugepageSysfs&) = delete;
        HugepageSysfs(HugepageSysfs&&)                 = delete;
        HugepageSysfs& operator=(HugepageSysfs&&)      = delete;
        virtual ~HugepageSysfs()                       = default;

        /// `nr_hugepages` or `free_hugepages`. Throws std::system_error when the file cannot be read.
        [[nodiscard]] virtual std::uint64_t read(std::string_view file) = 0;
        /// Writes `nr_hugepages`. Throws std::system_error, `permission_denied` when the process is not root.
        virtual void write_reserved(std::uint64_t pages) = 0;
    };

    /// The files under `root`.
    class FileHugepageSysfs final : public HugepageSysfs {
    public:
        explicit FileHugepageSysfs(std::filesystem::path root = hugepage_root);
        [[nodiscard]] std::uint64_t read(std::string_view file) override;
        void write_reserved(std::uint64_t pages) override;

    private:
        std::filesystem::path root_;
    };

    /// `echo <pages> | sudo tee <root>/nr_hugepages`: the manual remedy, for the error message and the docs.
    [[nodiscard]] std::string manual_reservation_command(std::uint64_t pages);

    /**
     * @brief The run's hugepages, reserved by this process when it can and released when it is done.
     *
     * Construction reads the free pages. Enough: nothing happens. Not enough and the write succeeds: the reservation
     * rises by the shortfall and is read back, and a kernel that granted less, fragmented, gets its reservation put
     * back and the caller a HugepageError with the shortfall and the remedy. Not enough and the write is refused: a
     * HugepageError with both remedies, the benchmark under sudo or the reservation by hand. Destruction writes the
     * original count back unless `keep`, which frees only pages nobody uses.
     */
    class HugepageReservation {
    public:
        HugepageReservation(HugepageSysfs& sysfs, std::uint64_t needed, bool keep);
        HugepageReservation(const HugepageReservation&)            = delete;
        HugepageReservation& operator=(const HugepageReservation&) = delete;
        HugepageReservation(HugepageReservation&&)                 = delete;
        HugepageReservation& operator=(HugepageReservation&&)      = delete;
        ~HugepageReservation();

        [[nodiscard]] std::uint64_t reserved_by_us() const noexcept {
            return raised_to_ - original_;
        }

        [[nodiscard]] std::uint64_t free_before() const noexcept {
            return free_before_;
        }

    private:
        HugepageSysfs* sysfs_;
        std::uint64_t original_    = 0;
        std::uint64_t raised_to_   = 0;
        std::uint64_t free_before_ = 0;
        bool keep_;
    };

}  // namespace aloe::benchmarks
```

- [ ] **Step 4: Write `hugepages.cpp`**

Signatures as declared. `FileHugepageSysfs::read` opens `root_ / file` with `std::ifstream` and parses the first integer, throwing `std::system_error{errno, std::generic_category(), path}` when the stream fails to open. `write_reserved` opens `root_ / "nr_hugepages"` with POSIX `open(O_WRONLY)` and writes the decimal text, throwing `std::system_error{errno, std::generic_category(), path}` on a failed open or short write, and closes the descriptor. `manual_reservation_command(pages)` returns `std::format("echo {} | sudo tee {}", pages, (hugepage_root / "nr_hugepages").string())`.

The constructor:

```cpp
    HugepageReservation::HugepageReservation(HugepageSysfs& sysfs, const std::uint64_t needed, const bool keep)
        : sysfs_{&sysfs},
          keep_{keep} {
        free_before_ = sysfs.read("free_hugepages");
        original_    = sysfs.read("nr_hugepages");
        raised_to_   = original_;
        if (free_before_ >= needed) {
            return;
        }
        const std::uint64_t shortfall = needed - free_before_;
        try {
            sysfs.write_reserved(original_ + shortfall);
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::permission_denied) {
                throw HugepageError{std::format(
                    "{} free 2 MB hugepages, {} needed, and only root may reserve them. Either run the benchmark "
                    "under sudo, so it reserves {} more for its own run and releases them after, or reserve them by "
                    "hand first: {}",
                    free_before_, needed, shortfall, manual_reservation_command(original_ + shortfall))};
            }
            throw;
        }
        raised_to_                 = sysfs.read("nr_hugepages");
        const std::uint64_t gained = raised_to_ > original_ ? raised_to_ - original_ : 0;
        if (free_before_ + gained < needed) {
            try {
                sysfs.write_reserved(original_);
            } catch (const std::system_error&) {  // NOLINT(bugprone-empty-catch): best effort on the way out
            }
            raised_to_ = original_;
            throw HugepageError{std::format(
                "the kernel reserved {} of the {} hugepages asked for: memory is fragmented. Try "
                "`echo 1 | sudo tee /proc/sys/vm/compact_memory` and run again, or run with --hugepages {}",
                gained, shortfall, free_before_ + gained)};
        }
    }

    HugepageReservation::~HugepageReservation() {
        if (keep_ || raised_to_ == original_) {
            return;
        }
        try {
            sysfs_->write_reserved(original_);
        } catch (const std::system_error&) {  // NOLINT(bugprone-empty-catch): a destructor reports nothing
        }
    }
```

Add the two files to the harness and the test to the unit target.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks`
Expected: PASS, 26 tests.

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/unit_tests/benchmarks
git commit -m "feat(benchmarks): hugepages reserved by the process when it can, released at exit"
```

---

## Task 9: The machine, the result file and the table

**Files:**
- Create: `benchmarks/tick_to_trade/machine_info.hpp`
- Create: `benchmarks/tick_to_trade/machine_info.cpp`
- Create: `benchmarks/tick_to_trade/report.hpp`
- Create: `benchmarks/tick_to_trade/report.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/unit_tests/benchmarks/test_report.cpp`
- Modify: `tests/unit_tests/benchmarks/CMakeLists.txt`

**Interfaces:**
- Consumes: `Summary` (Task 1).
- Produces: `MachineInfo{cpu, cpus, smt, governor, clocksource, kernel, hugepages_free}`, `probe_machine() -> MachineInfo`, `cpu_model_from_cpuinfo(std::string_view) -> std::string` (machine_info.hpp); `RunInfo{command, revision, build, timestamp, tsc_ghz, rounds, warmup, gap_ns, stack_core, peer_core, no_huge}`, `Row{round, level, topology, payload, metric, summary}`, `Report{machine, run, rows}`, `write_json(std::ostream&, const Report&)`, `print_table(std::ostream&, const Report&)`, `json_escape(std::string_view) -> std::string` (report.hpp).

Spec: "Output".

- [ ] **Step 1: Write the failing test**

`tests/unit_tests/benchmarks/test_report.cpp`:

```cpp
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <machine_info.hpp>
#include <report.hpp>

namespace {

    using namespace aloe::benchmarks;

    [[nodiscard]] Report one_row_report() {
        return {.machine = {.cpu = "Example CPU", .cpus = 12, .smt = "on", .governor = "performance",
                            .clocksource = "tsc", .kernel = "7.1.4", .hugepages_free = 64},
                .run     = {.command = "tick_to_trade --rounds 3", .revision = "89785ad", .build = "release",
                            .timestamp = "2026-10-21T10:00:00Z", .tsc_ghz = 4.7, .rounds = 3, .warmup = 1,
                            .gap_ns = 0, .stack_core = "2", .peer_core = "3", .no_huge = false},
                .rows    = {{.round = "tick", .level = "level0", .topology = "cross-core", .payload = 64,
                             .metric = "round",
                             .summary = {.count = 3, .min = 900, .p50 = 1000, .p90 = 1100, .p99 = 1200,
                                         .p999 = 1200, .max = 1300}}}};
    }

}  // namespace

TEST(Report, JsonIsFlatAndExact) {
    std::ostringstream out;
    write_json(out, one_row_report());
    const std::string expected = R"({
  "schema": 1,
  "machine": {"cpu": "Example CPU", "cpus": 12, "smt": "on", "governor": "performance", "clocksource": "tsc", "kernel": "7.1.4", "hugepages_free": 64},
  "run": {"command": "tick_to_trade --rounds 3", "revision": "89785ad", "build": "release", "timestamp": "2026-10-21T10:00:00Z", "tsc_ghz": 4.700, "rounds": 3, "warmup": 1, "gap_ns": 0, "stack_core": "2", "peer_core": "3", "no_huge": false},
  "rows": [
    {"round": "tick", "level": "level0", "topology": "cross-core", "payload": 64, "metric": "round", "count": 3, "min": 900, "p50": 1000, "p90": 1100, "p99": 1200, "p999": 1200, "max": 1300}
  ]
}
)";
    EXPECT_EQ(out.str(), expected);
}

TEST(Report, JsonEscapesQuotesAndBackslashes) {
    EXPECT_EQ(json_escape(R"(a "quoted" \ path)"), R"(a \"quoted\" \\ path)");
    EXPECT_EQ(json_escape("line\nbreak"), R"(line\nbreak)");
}

TEST(Report, TableNamesTheBlockAndTheColumns) {
    std::ostringstream out;
    print_table(out, one_row_report());
    const std::string text = out.str();
    EXPECT_NE(text.find("tick level0 cross-core"), std::string::npos) << text;
    EXPECT_NE(text.find("p99.9"), std::string::npos) << text;
    EXPECT_NE(text.find("1300"), std::string::npos) << text;
    EXPECT_NE(text.find("Example CPU"), std::string::npos) << text;
}

TEST(MachineInfo, ProbeFillsWhatEveryLinuxHas) {
    const MachineInfo machine = probe_machine();
    EXPECT_GT(machine.cpus, 0U);
    EXPECT_NE(machine.kernel, "unknown");
    EXPECT_FALSE(machine.clocksource.empty());
}

TEST(MachineInfo, CpuModelComesFromTheFirstModelNameLine) {
    const std::string cpuinfo = "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Ryzen 5 7600X 6-Core Processor\nprocessor\t: 1\nmodel name\t: AMD Ryzen 5 7600X 6-Core Processor\n";
    EXPECT_EQ(cpu_model_from_cpuinfo(cpuinfo), "AMD Ryzen 5 7600X 6-Core Processor");
    EXPECT_EQ(cpu_model_from_cpuinfo("nothing here"), "unknown");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks`
Expected: FAIL to compile, headers not found.

- [ ] **Step 3: Write `machine_info.hpp` and `machine_info.cpp`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aloe::benchmarks {

    /// What a reader needs to know before trusting a number: every field says "unknown" when the machine does not
    /// expose it.
    struct MachineInfo {
        std::string cpu         = "unknown";  ///< `/proc/cpuinfo`'s first `model name`.
        unsigned cpus           = 0;          ///< `std::thread::hardware_concurrency()`.
        std::string smt         = "unknown";  ///< `/sys/devices/system/cpu/smt/control`.
        std::string governor    = "unknown";  ///< `/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`.
        std::string clocksource = "unknown";  ///< `/sys/devices/system/clocksource/clocksource0/current_clocksource`.
        std::string kernel      = "unknown";  ///< `uname -r`.
        std::uint64_t hugepages_free = 0;     ///< Free 2 MB pages when probed; 0 under `--no-huge` or when unreadable.
    };

    [[nodiscard]] MachineInfo probe_machine();

    /// The value of the first `model name` line, or "unknown".
    [[nodiscard]] std::string cpu_model_from_cpuinfo(std::string_view cpuinfo);

}  // namespace aloe::benchmarks
```

`machine_info.cpp`: `probe_machine` reads each file with a `read_first_line(path)` helper in the anonymous namespace that returns an empty string when the file cannot be read, trimming the newline, and keeps the default on empty; `cpus` is `std::thread::hardware_concurrency()`; `kernel` is `utsname::release` from `uname`; `hugepages_free` is the first integer of `hugepage_root / "free_hugepages"` when readable (include `<hugepages.hpp>` for the path). `cpu_model_from_cpuinfo` finds the first line starting with `model name`, takes the text after the first `:` and trims spaces and tabs.

- [ ] **Step 4: Write `report.hpp` and `report.cpp`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <distribution.hpp>
#include <machine_info.hpp>

namespace aloe::benchmarks {

    /// How the run was made: enough to repeat it and to refuse a comparison across machines or builds.
    struct RunInfo {
        std::string command;              ///< argv, space-joined.
        std::string revision = "unknown"; ///< The git revision the binary was configured from.
        std::string build    = "unknown"; ///< The CMake build type.
        std::string timestamp;            ///< UTC, ISO 8601.
        double tsc_ghz       = 0;
        std::size_t rounds   = 0;
        std::size_t warmup   = 0;
        std::uint64_t gap_ns = 0;
        std::string stack_core = "none";
        std::string peer_core  = "none";
        bool no_huge           = false;
    };

    /// One distribution of the result: a round, a breakdown leg or the hop.
    struct Row {
        std::string round;     ///< "tick", "gateway" or "hop".
        std::string level;     ///< "floor", "level0", "level1" or "-".
        std::string topology;  ///< "cross-core" or "same-core".
        std::size_t payload = 0;
        std::string metric;    ///< "round", a `Phase` name, `last_leg` or "hop".
        Summary summary;
    };

    struct Report {
        MachineInfo machine;
        RunInfo run;
        std::vector<Row> rows;
    };

    [[nodiscard]] std::string json_escape(std::string_view text);

    /// The result file: schema 1, flat objects, one row per line, as the test fixes it.
    void write_json(std::ostream& out, const Report& report);

    /// The printed table: the machine and the run, then one block per (round, level, topology) with a line per
    /// payload and metric and the columns count, min, p50, p90, p99, p99.9, max in nanoseconds.
    void print_table(std::ostream& out, const Report& report);

}  // namespace aloe::benchmarks
```

`report.cpp`: `json_escape` escapes `"`, `\`, and control characters below 0x20 as `\n`, `\t`, `\r` or `\u00XX`. `write_json` writes exactly the layout of the test with `std::format` and `{:.3f}` for `tsc_ghz`, rows separated by `,\n`, a trailing newline after the closing brace. `print_table` writes a header of the machine (`cpu`, `cpus`, `smt`, `governor`, `clocksource`, `kernel`, `hugepages_free` or `no hugepages`) and the run (`rounds`, `warmup`, `gap_ns`, cores, `tsc_ghz`, `revision`, `build`), then for each distinct `(round, level, topology)` in order of first appearance a line `== <round> <level> <topology> ==`, a column header `payload  metric  count  min  p50  p90  p99  p99.9  max`, and one line per row, numbers right-aligned in 8-character columns with `std::format("{:>8}")`.

Add the four files to the harness and the test to the unit target.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Unit.Benchmarks && ./build/debug/tests/unit_tests/benchmarks/Aloe.Tests.Unit.Benchmarks`
Expected: PASS, 31 tests.

- [ ] **Step 6: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/unit_tests/benchmarks
git commit -m "feat(benchmarks): the machine probe, the JSON result file and the table"
```

---

## Task 10: The run over the matrix, the hop, and the whole-run test

**Files:**
- Create: `benchmarks/tick_to_trade/hop.hpp`
- Create: `benchmarks/tick_to_trade/hop.cpp`
- Create: `benchmarks/tick_to_trade/benchmark_run.hpp`
- Create: `benchmarks/tick_to_trade/benchmark_run.cpp`
- Modify: `benchmarks/tick_to_trade/benchmark_options.hpp` (one field)
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt`
- Create: `tests/integration_tests/benchmarks/test_tick_to_trade.cpp`
- Modify: `tests/integration_tests/benchmarks/CMakeLists.txt` (the target `${INTEGRATION_TESTING_TARGET}.Benchmarks.TickToTrade`, linked like the levels')

**Interfaces:**
- Consumes: everything so far; `core::TscClock::scale()`, `core::calibrate_clock()`.
- Produces: `measure_hop(std::optional<unsigned> other_core, std::size_t rounds) -> Distribution` (hop.hpp); `summarise_round(const RunState&, const core::TscScale&, const RoundConfig&) -> std::vector<Row>` and `run_benchmark(const Options&, const MachineInfo&, std::string command) -> Report` (benchmark_run.hpp); `Options::ring_name` (default `"tick-to-trade"`, not a command-line option).

Spec: "The hop", "Recording", "Sanity", "Delivery" (one ring pair for the run, the rings drained between configurations), the "Checked and not open" notes on port reuse.

- [ ] **Step 1: Write the failing test**

`tests/integration_tests/benchmarks/test_tick_to_trade.cpp`:

```cpp
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <benchmark_options.hpp>
#include <benchmark_run.hpp>
#include <eal_environment.hpp>
#include <gtest/gtest.h>
#include <machine_info.hpp>
#include <tsc_scale.hpp>

// The whole matrix, small, under the test EAL: every configuration runs to a clean end over one ring pair, the rows
// come out complete, and the stamp checks refuse a hole and an inverted pair. No root, no hugepages, no pinning.
namespace {

    using namespace aloe::benchmarks;

    const auto* const environment = ::testing::AddGlobalTestEnvironment(new aloe::testing::EalEnvironment{"net_null0"});

    [[nodiscard]] Options parse(std::vector<std::string> words) {
        words.insert(words.begin(), "tick_to_trade");
        std::vector<char*> argv;
        for (std::string& word : words) {
            argv.push_back(word.data());
        }
        auto options = parse_options(argv);
        if (!options) {
            throw std::runtime_error{options.error()};
        }
        return *options;
    }

    [[nodiscard]] std::size_t count_metric(const Report& report, const std::string& metric) {
        return static_cast<std::size_t>(
            std::ranges::count_if(report.rows, [&](const Row& row) { return row.metric == metric; }));
    }

}  // namespace

TEST(TickToTrade, TheWholeMatrixRunsSmall) {
    Options options = parse({"--rounds", "60", "--warmup", "6", "--payload", "64,1460", "--topology", "both",
                             "--stack-core", "none", "--peer-core", "none", "--breakdown", "--hop"});
    options.ring_name = "whole-matrix";
    const Report report = run_benchmark(options, probe_machine(), "test");
    EXPECT_EQ(count_metric(report, "round"), 15U) << "tick: (2 + 2) x 2 + 2; gateway: 2 + 2 + 1";
    for (const Row& row : report.rows) {
        EXPECT_EQ(row.summary.count, 60U) << row.round << ' ' << row.level << ' ' << row.metric;
        EXPECT_GT(row.summary.min, 0U) << row.metric;
        EXPECT_LE(row.summary.min, row.summary.p50);
        EXPECT_LE(row.summary.p50, row.summary.p99);
        EXPECT_LE(row.summary.p99, row.summary.max);
    }
    for (const char* leg : {"receive", "ip", "tcp"}) {
        EXPECT_EQ(count_metric(report, leg), 4U) << leg << ": level 0 tick rows, 2 payloads x 2 topologies";
    }
    for (const char* leg : {"application", "tcp-flush", "queue-flush"}) {
        EXPECT_EQ(count_metric(report, leg), 6U) << leg << ": those four and the gateway's two topologies";
    }
    EXPECT_EQ(count_metric(report, "hop"), 1U);
    EXPECT_EQ(report.run.rounds, 60U);
    EXPECT_FALSE(report.machine.kernel.empty());
}

TEST(TickToTrade, SummariseRoundRefusesAHoleAndAnInvertedPair) {
    const RoundConfig config{.rounds = 3, .warmup = 0};
    RunState state{config};
    for (std::size_t round = 0; round < 3; ++round) {
        state.stamps.start[round] = 1000 * (round + 1);
        state.stamps.stop[round]  = 1000 * (round + 1) + 100;
    }
    const aloe::core::TscScale& scale = aloe::core::TscClock::scale();
    const auto rows                   = summarise_round(state, scale, config);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].summary.count, 3U);
    state.stamps.stop[1] = 0;
    EXPECT_THROW(std::ignore = summarise_round(state, scale, config), std::runtime_error);
    state.stamps.stop[1] = state.stamps.start[1] - 1;
    try {
        std::ignore = summarise_round(state, scale, config);
        FAIL() << "an inverted pair is a failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string{error.what()}.find("round 1"), std::string::npos) << error.what();
    }
}
```

Add `#include <tsc_clock.hpp>` for `TscClock::scale()`. The integration CMake target links like the levels'.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.TickToTrade`
Expected: FAIL to compile, `benchmark_run.hpp` not found.

- [ ] **Step 3: Write `hop.hpp` and `hop.cpp`**

```cpp
#pragma once

#include <cstddef>
#include <optional>

#include <distribution.hpp>

namespace aloe::benchmarks {

    /// The one-way cache-line transfer between the calling thread's core and `other_core` (pinned there when given),
    /// as a distribution over `rounds` ping-pongs: half of each round trip, in ticks.
    [[nodiscard]] Distribution measure_hop(std::optional<unsigned> other_core, std::size_t rounds);

}  // namespace aloe::benchmarks
```

`hop.cpp`: an `alignas(64) std::atomic<std::uint64_t> flag{0}`; a `std::jthread` that pins itself with `pin_current_thread` when `other_core` is given and then, for each round `r`, spins until `flag.load(std::memory_order_acquire) == 2 * r + 1` and stores `2 * r + 2` with release; the calling thread, per round, takes `t0 = TscClock::get_ticks()`, stores `2 * r + 1`, spins until it reads `2 * r + 2`, takes `t1` and records `(t1 - t0) / 2`. Join before returning.

- [ ] **Step 4: Write `benchmark_run.hpp`**

```cpp
#pragma once

#include <string>
#include <vector>

#include <benchmark_options.hpp>
#include <machine_info.hpp>
#include <report.hpp>
#include <round_config.hpp>
#include <run_state.hpp>
#include <tsc_scale.hpp>

namespace aloe::benchmarks {

    /**
     * The rows of one finished configuration: the "round" distribution of stop minus start over every round, the
     * warm-up skipped, and with breakdown the legs between the phase stamps that are present. Throws
     * std::runtime_error naming the round when a start or stop stamp is missing, or when a stop precedes its start,
     * which across cores means the TSC is not synchronised between them.
     */
    [[nodiscard]] std::vector<Row> summarise_round(const RunState& state,
                                                   const core::TscScale& scale,
                                                   const RoundConfig& config);

    /**
     * The matrix of `options` over one ring pair and one peer thread, the rings drained between configurations, then
     * the hop when asked. The EAL is running. Throws std::runtime_error with the configuration and the cause on the
     * first configuration that fails its sanity checks; nothing is written then.
     */
    [[nodiscard]] Report run_benchmark(const Options& options, const MachineInfo& machine, std::string command);

}  // namespace aloe::benchmarks
```

Add to `Options` (Task 4): `std::string ring_name = "tick-to-trade";  ///< The ring pair's name prefix; tests pick their own.`

- [ ] **Step 5: Write `benchmark_run.cpp`**

The anonymous namespace holds `PeerWorker`, a thread started by its constructor that pins itself to `peer_core` when given, calls `ethdev::register_thread()`, then waits on a `std::mutex` and `std::condition_variable` for jobs: `void run(std::function<void()> job)` hands one over and returns; `void wait()` blocks until it has finished; the destructor posts a stop and joins. One worker serves the whole run, so DPDK registers one peer thread.

`summarise_round`:

```cpp
    std::vector<Row> summarise_round(const RunState& state, const core::TscScale& scale, const RoundConfig& config) {
        const std::size_t total = config.total();
        Distribution round_trip{total};
        for (std::size_t round = 0; round < total; ++round) {
            const std::uint64_t start = state.stamps.start[round];
            const std::uint64_t stop  = state.stamps.stop[round];
            if (start == 0 || stop == 0) {
                throw std::runtime_error{std::format("round {} has no {} stamp", round, start == 0 ? "start" : "stop")};
            }
            if (stop <= start) {
                throw std::runtime_error{std::format(
                    "round {}: the stop stamp precedes the start{}", round,
                    config.topology == Topology::CrossCore
                        ? "; across cores this means the TSC is not synchronised between the stack's and the peer's core"
                        : "")};
            }
            round_trip.record(stop - start);
        }
        const auto label = [&](std::string metric, Summary summary) {
            return Row{.round    = std::string{to_string(config.round)},
                       .level    = std::string{to_string(config.level)},
                       .topology = std::string{to_string(config.topology)},
                       .payload  = config.payload,
                       .metric   = std::move(metric),
                       .summary  = summary};
        };
        std::vector<Row> rows;
        rows.push_back(label("round", round_trip.summarise(scale, config.warmup)));
        if (!config.breakdown) {
            return rows;
        }
        // The legs between the stamps that are present: start, each phase with any nonzero entry, stop.
        std::vector<std::pair<std::string, const std::vector<std::uint64_t>*>> present;
        for (std::size_t index = 0; index < phase_count; ++index) {
            const auto& phase = state.stamps.phases[index];
            if (std::ranges::any_of(phase, [](const std::uint64_t t) { return t != 0; })) {
                present.emplace_back(std::string{to_string(static_cast<Phase>(index))}, &phase);
            }
        }
        const std::vector<std::uint64_t>* previous = &state.stamps.start;
        for (const auto& [name, stamps] : present) {
            Distribution leg{total};
            for (std::size_t round = 0; round < total; ++round) {
                leg.record((*stamps)[round] - (*previous)[round]);
            }
            rows.push_back(label(name, leg.summarise(scale, config.warmup)));
            previous = stamps;
        }
        Distribution last{total};
        for (std::size_t round = 0; round < total; ++round) {
            last.record(state.stamps.stop[round] - (*previous)[round]);
        }
        rows.push_back(label(std::string{last_leg}, last.summarise(scale, config.warmup)));
        return rows;
    }
```

`run_benchmark`:

```cpp
    Report run_benchmark(const Options& options, const MachineInfo& machine, std::string command) {
        core::calibrate_clock();
        const core::TscScale& scale = core::TscClock::scale();
        RingPair pair{{.name = options.ring_name}};
        PeerConfig peer_config{.stack_mac = pair.stack().mac()};
        PeerWorker worker{options.peer_core};
        Report report{.machine = machine, .run = run_info(options, scale, std::move(command)), .rows = {}};
        for (const RoundConfig& config : configurations(options)) {
            RunState state{config};
            StampedDevice<ethdev::Port> device{pair.stack(), state.stamps.stop};
            PeerConfig this_peer = peer_config;
            this_peer.strict     = config.level != Level::Floor;
            VenuePeer peer{pair.peer(), this_peer, state.stamps.start};
            const bool handshake = config.level != Level::Floor;
            const auto run_level = [&](VenuePeer* inline_peer) {
                switch (config.level) {
                    case Level::Floor: run_floor(state, device, this_peer, inline_peer); break;
                    case Level::Bricks: run_bricks(state, device, this_peer, options.arp_on_wire, inline_peer); break;
                    case Level::Runtime: run_runtime(state, device, this_peer, options.arp_on_wire, options.stack_core); break;
                }
            };
            if (config.topology == Topology::SameCore) {
                run_level(&peer);
            } else {
                worker.run([&] { drive_peer(peer, state, handshake); });
                run_level(nullptr);
                worker.wait();
            }
            if (state.failed.load()) {
                throw std::runtime_error{std::format("{} {} {} payload {}: {}", to_string(config.round),
                                                     to_string(config.level), to_string(config.topology),
                                                     config.payload, state.failure)};
            }
            for (Row& row : summarise_round(state, scale, config)) {
                report.rows.push_back(std::move(row));
            }
            pair.drain();
        }
        if (options.hop) {
            Distribution hop = measure_hop(options.peer_core, options.warmup + options.measured);
            report.rows.push_back(Row{.round = "hop", .level = "-", .topology = "cross-core", .payload = 0,
                                      .metric = "hop", .summary = hop.summarise(scale, options.warmup)});
        }
        return report;
    }
```

`run_info` fills `RunInfo` from the options: `tsc_ghz` as `(1 << 32) / scale.nanoseconds(1 << 32)`, the cores as decimal text or `"none"`, `timestamp` as UTC ISO 8601 from `std::chrono::system_clock::now()` with `std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(now))`; `revision` and `build` stay `"unknown"` here and `main.cpp` fills them (Task 11).

Two orderings matter and the signatures do not say them: in the cross-core branch the peer's job starts before the level runs, because `drive_peer` spins in `collect()` until the stack's first frame arrives; and `pair.drain()` runs after the level function returned and so after the stack's bricks were destroyed, which is when the RST a released `Stream` sends at level 1, and the last ACK of a gateway round, are still on the rings.

Add `hop.cpp`, `benchmark_run.cpp` and the headers to the harness.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build --preset debug --target Aloe.Tests.Integration.Benchmarks.TickToTrade && ./build/debug/tests/integration_tests/benchmarks/Aloe.Tests.Integration.Benchmarks.TickToTrade`
Expected: PASS, 2 tests, in well under a minute in debug. Then the whole suite: `ctest --preset debug` passes.

- [ ] **Step 7: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade tests/integration_tests/benchmarks
git commit -m "feat(benchmarks): the run over the matrix, the stamp checks and the cache-line hop"
```

---

## Task 11: The binary, the hugepage probe and the comparison script

**Files:**
- Create: `benchmarks/tick_to_trade/main.cpp`
- Modify: `benchmarks/tick_to_trade/CMakeLists.txt` (the executable, with `ALOE_BENCHMARK_REVISION` and `ALOE_BENCHMARK_BUILD_TYPE`)
- Create: `scripts/compare-benchmark.py`
- Create: `tests/functional_tests/benchmarks/CMakeLists.txt`, `tests/functional_tests/benchmarks/fixtures/baseline.json`, `tests/functional_tests/benchmarks/fixtures/regressed.json`
- Modify: `tests/functional_tests/CMakeLists.txt` (gate a `benchmarks` subdirectory on `DO_BENCHMARKS`)

**Interfaces:**
- Consumes: `parse_options`, `usage`, `HugepageReservation`, `FileHugepageSysfs`, `HugepageError`, `ethdev::Eal`, `probe_machine`, `run_benchmark`, `print_table`, `write_json`.
- Produces: the executable `Aloe.Benchmarks.TickToTrade`; the script `scripts/compare-benchmark.py <a.json> <b.json> [--threshold PERCENT]` exiting 1 when any compared percentile moved by more than the threshold; the targets `Aloe.Tests.Functional.Benchmarks.Compare.flags_a_regression` and `...no_change_passes`.

Spec: "Build and command line", "Output" (comparison), "Hugepages" (Probe B).

- [ ] **Step 1: Write `main.cpp`**

```cpp
// The tick-to-trade benchmark: both rounds, three levels, a scripted venue on a second core, over DPDK's ring
// driver. See docs/benchmarks/tick-to-trade.md for how to run it and how to read the table.

#include <aloe/ethdev>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <span>
#include <string>

#include <benchmark_options.hpp>
#include <benchmark_run.hpp>
#include <clock.hpp>
#include <hugepages.hpp>
#include <machine_info.hpp>
#include <report.hpp>

#ifndef ALOE_BENCHMARK_REVISION
#define ALOE_BENCHMARK_REVISION "unknown"
#endif
#ifndef ALOE_BENCHMARK_BUILD_TYPE
#define ALOE_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

    [[nodiscard]] std::string joined(const std::span<char*> argv) {
        std::string command;
        for (const char* argument : argv) {
            command += command.empty() ? "" : " ";
            command += argument;
        }
        return command;
    }

    [[nodiscard]] std::string default_result_name() {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return std::format("tick-to-trade-{:%Y%m%d-%H%M%S}.json", now);
    }

}  // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments{argv, static_cast<std::size_t>(argc)};
    const auto options = aloe::benchmarks::parse_options(arguments);
    if (!options) {
        std::println(stderr, "{}", options.error());
        return EXIT_FAILURE;
    }
    try {
        aloe::core::calibrate_clock();
        std::optional<aloe::benchmarks::FileHugepageSysfs> sysfs;
        std::optional<aloe::benchmarks::HugepageReservation> hugepages;
        if (!options->no_huge) {
            sysfs.emplace();
            hugepages.emplace(*sysfs, options->hugepages, options->keep_hugepages);
            if (hugepages->reserved_by_us() > 0) {
                std::println("reserved {} hugepages for this run{}", hugepages->reserved_by_us(),
                             options->keep_hugepages ? ", kept after it" : "");
            }
        }
        aloe::ethdev::Eal eal{options->eal};
        aloe::benchmarks::MachineInfo machine = aloe::benchmarks::probe_machine();
        if (options->no_huge) {
            machine.hugepages_free = 0;
        }
        aloe::benchmarks::Report report = aloe::benchmarks::run_benchmark(*options, machine, joined(arguments));
        report.run.revision             = ALOE_BENCHMARK_REVISION;
        report.run.build                = ALOE_BENCHMARK_BUILD_TYPE;
        aloe::benchmarks::print_table(std::cout, report);
        const std::string path = options->out.empty() ? default_result_name() : options->out;
        std::ofstream file{path};
        if (!file) {
            std::println(stderr, "cannot write {}", path);
            return EXIT_FAILURE;
        }
        aloe::benchmarks::write_json(file, report);
        std::println("results written to {}", path);
        return EXIT_SUCCESS;
    } catch (const aloe::benchmarks::HugepageError& error) {
        std::println(stderr, "hugepages: {}", error.what());
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::println(stderr, "tick_to_trade: {}", error.what());
        return EXIT_FAILURE;
    }
}
```

The two macros are read here only, with defaults so the file compiles without the definitions; they are not in a header.

- [ ] **Step 2: Add the executable to `benchmarks/tick_to_trade/CMakeLists.txt`**

```cmake
##############################################################################
# The binary: tick_to_trade [options] [-- <EAL arguments>]
##############################################################################
execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE TICK_TO_TRADE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (NOT TICK_TO_TRADE_REVISION)
    set(TICK_TO_TRADE_REVISION "unknown")
endif ()
add_executable(${TICK_TO_TRADE}
        main.cpp
)
target_link_libraries(${TICK_TO_TRADE}
        PRIVATE
        ${TICK_TO_TRADE}.Harness
)
target_compile_definitions(${TICK_TO_TRADE}
        PRIVATE
        ALOE_BENCHMARK_REVISION="${TICK_TO_TRADE_REVISION}"
        ALOE_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
set_target_properties(${TICK_TO_TRADE} PROPERTIES OUTPUT_NAME tick_to_trade)
##############################################################################
```

The revision is read at configure time; the docs say a result file's `revision` is as of the last configure and the committed result files name the commit in their file name.

- [ ] **Step 3: Build and run the binary without hugepages, as a smoke test**

Run: `cmake --preset release && cmake --build --preset release --target Aloe.Benchmarks.TickToTrade && ./build/release/benchmarks/tick_to_trade/tick_to_trade --rounds 2000 --warmup 200 --payload 64 --stack-core none --peer-core none --out /tmp/smoke.json -- --in-memory --no-pci --no-telemetry --no-huge -l 0`
Expected: a table for every configuration and `results written to /tmp/smoke.json`; `grep '"no_huge": true' /tmp/smoke.json` matches.

- [ ] **Step 4: Probe B, hugepages unprivileged and by the process**

Run, in this order, and record each outcome in the docs page of Task 12:

```bash
./build/release/benchmarks/tick_to_trade/tick_to_trade --rounds 1000 --warmup 100 --payload 64 --stack-core none --peer-core none
# expected: "hugepages: 0 free 2 MB hugepages, 64 needed, and only root may reserve them. Either run ... under sudo ... or reserve them by hand first: echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --rounds 1000 --warmup 100 --payload 64 --stack-core none --peer-core none --out /tmp/root.json
# expected: "reserved 64 hugepages for this run", a table, and afterwards: cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages prints 0
echo 64 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
./build/release/benchmarks/tick_to_trade/tick_to_trade --rounds 1000 --warmup 100 --payload 64 --stack-core none --peer-core none --out /tmp/user.json
# Probe B: either a table (unprivileged use of reserved pages works with --in-memory) or an EAL error naming hugepages
echo 0 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

If the third command fails inside the EAL, the docs say `sudo` is the way and why; if it works, the docs give both ways. A result file written by the root run is owned by root; `sudo chown $USER` it or write it under `--out` to a path the user owns afterwards.

- [ ] **Step 5: Write `scripts/compare-benchmark.py`**

```python
#!/usr/bin/env python3
"""Two tick-to-trade result files side by side, with the change of every percentile.

    scripts/compare-benchmark.py baseline.json candidate.json [--threshold PERCENT]

Rows are matched by round, level, topology, payload and metric. The exit status is 1 when any p50, p90, p99,
p99.9 or max moved by more than the threshold (default 5 percent) in either direction, so a rerun after a phase 2
step can gate on it; the table is printed either way. Machines and builds are printed, never compared: a baseline
from one machine is not read against another.
"""

import argparse
import json
import sys

PERCENTILES = ("p50", "p90", "p99", "p999", "max")


def key(row):
    return (row["round"], row["level"], row["topology"], row["payload"], row["metric"])


def load(path):
    with open(path, encoding="utf-8") as handle:
        report = json.load(handle)
    if report.get("schema") != 1:
        sys.exit(f"{path}: unknown schema {report.get('schema')!r}")
    return report


def describe(report):
    machine, run = report["machine"], report["run"]
    return (f"{machine['cpu']}, {machine['cpus']} cpus, smt {machine['smt']}, {machine['governor']}, "
            f"clocksource {machine['clocksource']}, kernel {machine['kernel']}; revision {run['revision']}, "
            f"{run['build']}, {run['rounds']} rounds, gap {run['gap_ns']} ns, cores {run['stack_core']}/{run['peer_core']}, "
            f"{'no hugepages' if run['no_huge'] else 'hugepages'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--threshold", type=float, default=5.0, help="percent of change that flags a row (5)")
    arguments = parser.parse_args()
    baseline, candidate = load(arguments.baseline), load(arguments.candidate)
    print(f"baseline:  {describe(baseline)}")
    print(f"candidate: {describe(candidate)}")
    before = {key(row): row for row in baseline["rows"]}
    flagged = 0
    header = f"{'round':8} {'level':7} {'topology':10} {'payload':>7} {'metric':12}" + "".join(
        f" {p + ' base':>10} {p + ' new':>10} {'%':>7}" for p in PERCENTILES)
    print(header)
    for row in candidate["rows"]:
        base = before.get(key(row))
        line = f"{row['round']:8} {row['level']:7} {row['topology']:10} {row['payload']:>7} {row['metric']:12}"
        if base is None:
            print(line + "  (new row)")
            continue
        for p in PERCENTILES:
            old, new = base[p], row[p]
            change = (new - old) * 100.0 / old if old else 0.0
            mark = "!" if abs(change) > arguments.threshold else " "
            flagged += mark == "!"
            line += f" {old:>10} {new:>10} {change:>+6.1f}{mark}"
        print(line)
    if flagged:
        print(f"{flagged} percentile(s) moved by more than {arguments.threshold}%")
        return 1
    print(f"no percentile moved by more than {arguments.threshold}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`chmod +x scripts/compare-benchmark.py`.

- [ ] **Step 6: Write the functional test and its fixtures**

`tests/functional_tests/benchmarks/fixtures/baseline.json` is the one-row report of Task 9's test (copy the expected text). `regressed.json` is the same with `"p99": 1200` replaced by `"p99": 1500` and `"max": 1300` by `"max": 1600`.

`tests/functional_tests/CMakeLists.txt` gains:

```cmake
if (DO_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()
```

`tests/functional_tests/benchmarks/CMakeLists.txt`:

```cmake
##############################################################################
# scripts/compare-benchmark.py against fixture result files; skipped without python3
##############################################################################
find_program(PYTHON3_PROGRAM python3)
if (PYTHON3_PROGRAM)
    set(COMPARE ${CMAKE_SOURCE_DIR}/scripts/compare-benchmark.py)
    set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression
            COMMAND ${PYTHON3_PROGRAM} -I ${COMPARE} ${FIXTURES}/baseline.json ${FIXTURES}/regressed.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.flags_a_regression PROPERTIES
            LABELS "functional"
            PASS_REGULAR_EXPRESSION "2 percentile\\(s\\) moved by more than 5.0%"
    )

    add_test(
            NAME ${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes
            COMMAND ${PYTHON3_PROGRAM} -I ${COMPARE} ${FIXTURES}/baseline.json ${FIXTURES}/baseline.json
    )
    set_tests_properties(${FUNCTIONAL_TESTING_TARGET}.Benchmarks.Compare.no_change_passes PROPERTIES
            LABELS "functional"
            PASS_REGULAR_EXPRESSION "no percentile moved"
    )
else ()
    message(STATUS "python3 not found: the benchmark comparison tests are not registered")
endif ()
##############################################################################
```

- [ ] **Step 7: Run the functional tests**

Run: `cmake --preset debug && ctest --preset debug -L functional -R Benchmarks`
Expected: 2 tests pass.

- [ ] **Step 8: Format and commit**

```bash
./scripts/check-format.sh
git add benchmarks/tick_to_trade scripts/compare-benchmark.py tests/functional_tests
git commit -m "feat(benchmarks): the tick_to_trade binary and the result comparison script"
```

---

## Task 12: Documentation, the recorded baseline and the completion gates

**Files:**
- Create: `docs/benchmarks/tick-to-trade.md`
- Create: `docs/benchmarks/results/<date>-<revision>-<hostname>.json` (the first recorded run)
- Modify: `docs/guides/getting-started.md` (a "Benchmarks and hugepages" section after "Configure and build"; the `release` row of the preset table mentions the benchmark)
- Modify: `docs/roadmap.md` (step 1 of phase 2)
- Modify: `docs/architecture/overview.md` (the status paragraph under "The shard, as a rule"; a line under "Build decisions")
- Modify: `README.md` ("Building": tests need no hugepages, the benchmark does, with the link)
- Modify: `AGENTS.md` (Layout row for `benchmarks/`; Documentation pattern; Traps)

Spec: "Documentation", "Delivery", "Done when".

- [ ] **Step 1: Run the baseline on the author's machine**

In `release`, on a quiet machine, with the cores whose SMT siblings are idle (2 and 3 by default on the 7600X; close browsers and IDE indexers first):

```bash
cmake --preset release && cmake --build --preset release --target Aloe.Benchmarks.TickToTrade
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --breakdown --hop --out /tmp/baseline.json
sudo chown "$USER" /tmp/baseline.json
cp /tmp/baseline.json "docs/benchmarks/results/$(date -u +%Y-%m-%d)-$(git rev-parse --short HEAD)-$(hostname).json"
```

Then, as a check of the regression tooling, run it a second time and compare:

```bash
sudo ./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --breakdown --hop --out /tmp/again.json
./scripts/compare-benchmark.py docs/benchmarks/results/*.json /tmp/again.json
```

A same-machine rerun that flags rows at p50 means the machine was not quiet; note which rows in the docs rather than tuning the threshold.

- [ ] **Step 2: Write `docs/benchmarks/tick-to-trade.md`**

Sections, in this order, each a few paragraphs in the voice of `docs/architecture/tcp.md`:

1. **What it measures.** The two rounds and the three levels, in the spec's words; the per-percentile side-by-side reading; the regression purpose.
2. **How to run it.** The `release` build; hugepages (the process reserves them under `sudo`, or the manual command, and what Probe B found); the cores (`--stack-core`, `--peer-core`, choose cores whose SMT siblings are idle, `lscpu -e` shows the pairs); the full command used for the baseline; `--topology both`, `--breakdown`, `--hop`, `--gap`, `--arp-on-wire`, `--payload`; where the result file lands.
3. **Reading the table.** The columns; nearest-rank percentiles; the floor against level 0 is the stack's cost and level 0 against level 1 the runtime's, per percentile, never subtracted; the breakdown legs by name; the hop row; the two floors' difference under `--topology both` as the hop seen through the device.
4. **Known artefacts.** Software checksums on the ring; no card and no DMA; the ring port's zero MAC; why the 1448 row exists (phase 2's timestamps option shrinks a full segment to 1448) and that the 1460 row carries no second-segment ACK in a closed loop, contrary to the brainstorm's expectation, because the stack resets its full-segment count whenever it sends an ACK; the same-core topology's exclusion of level 1; a cross-core run needs the TSC synchronised across cores, which the `tsc` clocksource guarantees; SMT siblings and boost on a workstation; the `revision` field is as of the last configure.
5. **Results.** The machine, the command, the hugepages, the date and the revision, then the table of the recorded run pasted from the binary's output (the floor, level 0 and level 1 blocks for both rounds, the breakdown and the hop), and two or three sentences on what the first numbers say: the stack's cost at p50 and at p99.9, the runtime's overhead, the hop.
6. **Tracking regressions.** Every phase 2 step reruns the same command on the same machine, commits its result file beside this one, runs `scripts/compare-benchmark.py`, and adds a row to a small history table here: date, revision, step, p50 and p99.9 of level 0 tick at 64 bytes and of the gateway round. A delta the step's added work does not explain gets a `perf record -g` session on the `release-perf` preset.
7. **What comes next.** The loads and the isolated round of the second pull request, and the two-core shard question they inform, in two sentences.

- [ ] **Step 3: Edit the other docs**

- `docs/guides/getting-started.md`: after "Configure and build", a section "Benchmarks and hugepages": the binary, that it needs hugepages where the tests do not, the two ways to have them with the exact commands, `--keep-hugepages`, that a reservation does not survive a reboot unless made persistent through `sysctl vm.nr_hugepages`, and the link to `docs/benchmarks/tick-to-trade.md`. In the preset table, `release` reads "Optimized build with the full feature set; the benchmark runs from here".
- `docs/roadmap.md`: step 1 becomes "**Baseline benchmark.** Tick-to-trade and the gateway round through the bricks and through the runtime, against a floor of raw frames, on DPDK's ring driver with a scripted venue on a second core; per-message distributions, recorded in [docs/benchmarks/tick-to-trade.md](benchmarks/tick-to-trade.md) before any step adds work to the hot path. Every later step reruns it. The loads and the isolated round that inform the two-core shard follow in a second part."
- `docs/architecture/overview.md`: the status paragraph gains "The baseline that measures the single-core shard, and that the two-core question will be decided with, is [the tick-to-trade benchmark](../benchmarks/tick-to-trade.md)." Under "Build decisions" add "**The benchmark runs on hugepages and reserves them itself under root;** the tests still need none."
- `README.md`: in "Building", after "The tests need neither root nor hugepages nor a network card." add "The benchmark under `benchmarks/` does need hugepages; see [docs/benchmarks/tick-to-trade.md](docs/benchmarks/tick-to-trade.md)."
- `AGENTS.md`: the Layout row `examples/`, `benchmarks/` becomes two rows, the second "`benchmarks/tick_to_trade/`: the baseline benchmark, a harness library and a binary in `aloe::benchmarks`, run by hand in `release` on hugepages; its tests live under `tests/*/benchmarks/` and need neither." The Documentation section gains "`docs/benchmarks/<name>.md` describes one benchmark and carries its recorded results; a result file is committed beside it under `docs/benchmarks/results/`." Traps gains what this plan found, each one line: the ring driver's SPSC rings and loopback, `rte_eth_from_rings` ports and their zero MAC, the mempool cache's one-way flow at p99.9, the ACK policy's full-segment count resetting on every ACK sent (so a closed loop at 1460 produces no pure ACK), the 15-character ring-pair name limit, `--same-core` and level 1, same-core with `--arp-on-wire` deadlocking, plus whatever the executor hit.

- [ ] **Step 4: Comment on issue #16**

Post a comment (`gh issue comment 16`) in three lines: hugepages instead of `--no-huge`, since the deployment uses them and the benchmark reserves them itself; two ports over cross-connected rings instead of the vdev, whose queue loops its transmit back; and the link to the docs page once the pull request is open.

- [ ] **Step 5: Completion gates**

```bash
for preset in debug gcc-debug asan tsan; do
    cmake --preset "$preset" && cmake --build --preset "$preset" && ctest --preset "$preset" || break
done
./scripts/check-format.sh
```

Expected: every preset builds and every test passes; `tsan` builds without the benchmark and its tests (`DO_BENCHMARKS=OFF`) and still passes; the format check prints nothing to fix. Then `git status` shows only the committed tree.

- [ ] **Step 6: Commit the docs and open the pull request**

```bash
git add docs AGENTS.md README.md
git commit -m "docs: the tick-to-trade baseline, how to run it and the first recorded results"
git push -u origin benchmarks/baseline
gh pr create --title "[BENCHMARKS][DOCS] Baseline benchmark: tick-to-trade and the gateway round on DPDK's ring driver" --body-file - <<'BODY'
Step 1 of phase 2 (#16), first part: the baseline every later step is measured against.

A standalone benchmark under `benchmarks/tick_to_trade/`: two `ethdev::Port`s over cross-connected SPSC rings
(`rte_eth_from_rings`), a scripted venue on a second core recycling the stack's mbufs, both rounds (tick-to-trade,
the gateway round) at the floor, at level 0 (the loop over the bricks) and at level 1 (the runtime), per-message
distributions with nearest-rank percentiles, a JSON result file and a comparison script. Hugepages are reserved by
the process under root. The first results are recorded in docs/benchmarks/tick-to-trade.md.

Verified: debug, gcc-debug, asan and tsan build and pass; check-format passes; the release run on <machine> as
documented. The loads and the isolated round follow in a second pull request.
BODY
```

---

## Coverage map

| Spec section                                                                                              | Task                                                                                                              |
|-----------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| Goal: two rounds, three levels, distributions                                                             | 5 (floor), 6 (level 0), 7 (level 1), 1 and 10 (distributions, rows)                                               |
| Topology: two ports over cross-connected rings; `--same-core`; the hop                                    | 2, 4 (`--topology`), 10 (`measure_hop`, the two floors)                                                           |
| Mbufs recycled, never freed across cores                                                                  | 5 (`VenuePeer`'s carrier), 6 and 7 (the stack frees on its core)                                                  |
| The stamp wrapper                                                                                         | 3                                                                                                                 |
| The scripted peer: handshake both ways, per-round protocol, ARP answered, every frame built off the clock | 5                                                                                                                 |
| The order, gateway round: composed in place, start before `prepare`, closed loop on the bare ACK          | 5 (floor), 6, 7                                                                                                   |
| The application at each level, identical work                                                             | 3 (`application_work.hpp`), 5, 6, 7                                                                               |
| Recording: preallocated arrays, one writer each, warm-up discarded, nearest rank                          | 1, 3 (`Stamps`), 10                                                                                               |
| Breakdown mode, level 0 only                                                                              | 6, 10 (legs), 4 (`--breakdown` applies to level 0)                                                                |
| Sanity: counters, orders, deadline, EAL; no result file on failure                                        | 6 (`check_stack_counters`), 5 (`PeerError`), 10 (stamp checks, throw), 11 (`main` writes nothing on an exception) |
| Variables: payloads 64..1460 with 1448, order 128, response 64 (second PR)                                | 4 (defaults), 1 (`order_size`)                                                                                    |
| Output: table, JSON, comparison script                                                                    | 9, 11                                                                                                             |
| Build and command line; EAL defaults; hugepages by the process                                            | 1, 4, 8, 11                                                                                                       |
| Documentation: page, results, roadmap, overview, getting-started, AGENTS, issue comment                   | 12                                                                                                                |
| Done when: builds in release and the four presets, results committed, docs current, format                | 11 (release), 12                                                                                                  |

## Plan handoff

Execute with superpowers:subagent-driven-development, one task per subagent in order, a fresh reviewer after each
with the checklist under "How to Execute and Review", and a whole-branch review before Task 12's gates. The
first pull request is the baseline; the loads and the isolated round get their own plan from the spec's second
part once the recorded numbers exist, because the loads' defaults (how many idle connections, how large a burst)
should be chosen with the clean numbers in hand.
