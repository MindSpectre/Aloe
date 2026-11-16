# Layout Round Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the DPDK backend to a top-level `devices/` group, name the future layer-7 group `l7/`, put `net`'s IPv4 code under `net/ipv4/` with prefixed type names, make the fabric parse IPv4 with `wire`'s parser, and split the shard counters by owner; all in one pull request.

**Architecture:** Four changes in separate commits on one branch. The first two are moves and renames with no behaviour change. The last two change code: the fabric drops its own IPv4 parser, and `loop::ShardCounters` becomes `loop::ShardQueueCounters` (what `ShardQueue` writes) inside a new `runtime::ShardCounters` (what the runtime writes).

**Tech Stack:** C++26 on libstdc++, clang 22 and GCC 16, CMake and Ninja, GoogleTest, bash for the check scripts and the hook's functional test. No new dependency.

**Spec:** [Layout round: a devices group, net's IPv4 folder, the fabric's parser, the shard counters](../specs/2026-11-16-layout-round-design.md). Read the spec, `AGENTS.md` and `codestyle.md` before Task 1.

## Global Constraints

- Namespaces and umbrellas do not change: `aloe::ethdev` and `<aloe/ethdev>`, `aloe::net` and `<aloe/net>`.
- No renamed `net` module and no `net::v4` namespace. `SendRequest`, `SendError`, `Ipv4Config`, `Ipv4Counters` and `ArpCache` keep their names.
- `devices/` holds production backends only. The fabric stays in `fixtures/`; `device` stays in `common/`.
- `NodePool`, `Work`, `RunQueue` and `Inbox` do not move.
- No change to what the stack sends or receives. The only behaviour changes are the two the spec names: a truncated IPv4 frame on the fabric, and `frames_received` counted by `ShardQueue::receive`.
- No compatibility alias for `Aloe::Common::Ethdev` and no forwarding header for a renamed file: nothing outside this repository uses them.
- Every directory move is a `git mv`, so `git log --follow` keeps working.
- `AGENTS.md` rules apply unchanged: one namespace per module, a flat include path with unique header basenames, umbrella includes across modules, `detail/` per folder, public docs never link `docs/superpowers`.
- Docs change in the same commit as the code they describe.
- Completion gate: `debug`, `gcc-debug`, `asan` and `tsan` build and pass; `./scripts/check-format.sh` and `./scripts/check-includes.sh` pass.
- Conventional commit subjects with the module as scope.

## Review Focus

1. **A TCP or UDP segment shorter than four bytes.** The fabric must steer it without reading ports past the segment. New test in Task 3 (`ASegmentTooShortForPortsIsSteeredWithoutThem`).
2. **A truncated IPv4 frame on a checksum-offloading port.** It must get `Unknown` verdicts and no read past the frame. New test in Task 3 (`LeavesATruncatedFrameUnknown`).
3. **A source under `devices/` or `l7/` that breaks the format or include rules.** The scripts and the hook must catch it. Two new hook scenarios in Task 1.
4. **`frames_received` counted twice under the runtime**, once by the queue and once by the shard. Pinned by the exact counts in `test_shard.cpp` (two tests), `test_steering.cpp` and `test_runtime_ring.cpp`; Task 4 renames their field access and must not change their numbers.
5. **A hand-written loop that reads `frames_received`.** It was always zero for a loop over the bricks. New test in Task 4 (`ReceiveCountsTheFramesItReturns`).

---

## Read This First

**Baseline.** The plan was written against `main` at a2844ae. Before branching:

```bash
git switch main && git pull
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
./scripts/check-format.sh && ./scripts/check-includes.sh
```

All tests pass and both scripts print a file count. `ctest --preset debug` listed 60 tests at a2844ae; each test binary is one entry, so a new gtest case does not change the count and a new hook scenario does.

**Benchmark baseline, before branching.** The spec asks for the 64-byte tick on the branch against `main`. On `main`:

```bash
cmake --preset release && cmake --build --preset release --target Aloe.Benchmarks.TickToTrade
./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --hop --out <scratch>/layout-main.json
```

Run it without `sudo`. If the EAL cannot get hugepages, do not escalate: note it, skip the benchmark steps in Task 5 and say so in the hand-over; the author runs it.

**Branch.** `git switch -c refactoring/layout-round`.

**Facts verified at a2844ae.**

- `ethdev` links `Aloe::Dpdk`, `Aloe::Common::Core`, `Aloe::Common::Device` and `Aloe::Common::Wire`. Nothing under `common/` or `fixtures/` links it.
- `component/` is not on disk. It is named in `AGENTS.md` (one table row), `scripts/check-includes.sh` (two arrays) and `scripts/check-format.sh` (one array). Both scripts skip a listed directory that does not exist.
- `scripts/check-includes.sh` finds modules by their `export/aloe/<name>` umbrella under `module_directories`, at any depth, so `net/ipv4/stack/detail/` needs no script change.
- `fabric::detail::parse_ipv4` has two callers, `Port::inspect` and `Port::fill_checksums` in `fixtures/fabric/fabric/fabric.cpp`. No test calls it.
- `wire::Ipv4Header::parse` returns nothing when the total length exceeds the bytes given; a frame it accepts has `header_length <= total_length <= payload.size()`.
- `wire::EthernetHeader::ethertype` is a `std::uint16_t`; compare it with `std::to_underlying(wire::EtherType::Ipv4)`.
- `ShardQueue` writes `frames_transmitted` and `transmit_refused`. `runtime::Shard::step` writes `frames_received` (`shard.hpp:112`). Every other field of `ShardCounters` is written by `ShardContext`, `Shard` or `TaskScope`.
- `${RUNTIME}.Scope` sits below `${RUNTIME}.Context` in the runtime's CMake and takes the counters by reference, so the new struct needs a component both can link.
- `tcp` classes hold member aliases named `Datagram` (`tcp_stack.hpp:231`). The alias name stays; only its right-hand side changes.

**Traps.**

- After a directory move, run `cmake --preset <name>` again before building. Old object directories under `build/` are left behind and are harmless.
- clang-tidy runs inside the clang builds and its warnings are errors.
- Task 4 changes the layout of a struct the scenario binaries compile in. Build `gcc-debug` with `--clean-first` afterwards (`AGENTS.md`, "Traps already found").
- Executables that link DPDK are about 200 MB in debug; a full rebuild after Task 1 relinks all of them.
- GCC rejects `using X = X<...>;` inside a class of the same namespace; keep the `net::` qualifier on the right-hand side of TCP's `Datagram` aliases.

## How To Execute And Review

- Execute with superpowers:subagent-driven-development or superpowers:executing-plans, one task at a time, one commit per task (Task 1 has two).
- After each task a reviewer checks: the diff does only what the task says; moved files show as renames (`git show --stat -M`); no include breaks the rules; no public doc links `docs/superpowers`; `AGENTS.md`, the README and the architecture pages match the tree.
- For Tasks 1 and 2 the reviewer also checks that no test changed beyond renamed names and paths.
- After Task 5 run superpowers:requesting-code-review on the whole branch against the spec.
- Finish with superpowers:verification-before-completion: paste the commands and their output, do not summarise them.
- Push and open the pull request only when the author asks.

---

### Task 1: The `devices/` group and the `l7/` name

**Files:**
- Modify: `scripts/check-format.sh:14`, `scripts/check-includes.sh:24-25`
- Test: `tests/functional_tests/hooks/pre_commit.sh`, `tests/functional_tests/hooks/CMakeLists.txt:25-37`
- Move: `common/ethdev/` to `devices/ethdev/`
- Create: `devices/CMakeLists.txt`
- Modify: `CMakeLists.txt:134-135`, `common/CMakeLists.txt`, `devices/ethdev/CMakeLists.txt` (first and last target lines)
- Modify, target name only: `examples/{ethernet_echo,ping_responder,tcp_echo,tcp_echo_tasks}/CMakeLists.txt`, `benchmarks/tick_to_trade/CMakeLists.txt`, `tests/shared/CMakeLists.txt`, `tests/integration_tests/{ethdev,net,runtime,tcp}/CMakeLists.txt`, `tests/manual_tests/{ethdev,net,runtime,tcp}/CMakeLists.txt`
- Modify, docs: `AGENTS.md`, `README.md:49`, `docs/architecture/device.md:12`, `codestyle.md:606`
- Modify: `common/tcp/pool/detail/node_pool.hpp` (the TODO, if it is not there yet)

**Interfaces:**
- Produces: the CMake target `Aloe.Devices.Ethdev` with alias `Aloe::Devices::Ethdev`; the group variable `DEVICES` set to `${PROJECT_NAME}.Devices`.

- [ ] **Step 1: Add two failing hook scenarios**

In `pre_commit.sh`, before the `*)` case, and both names in the `foreach` list of `hooks/CMakeLists.txt`:

```bash
    checks_sources_under_devices_and_l7)
        write_file devices/ethdev/bad.cpp "$unformatted"
        write_file l7/http11/bad.hpp "$unformatted"
        git add devices/ethdev/bad.cpp l7/http11/bad.hpp
        try_commit
        expect_rejected
        expect_output_contains "devices/ethdev/bad.cpp"
        expect_output_contains "l7/http11/bad.hpp"
        ;;
    rejects_an_include_of_a_device_modules_header)
        write_file devices/ethdev/export/aloe/ethdev '#include <port.hpp>'
        write_file devices/ethdev/port/port.hpp '#pragma once'
        write_file common/tcp/export/aloe/tcp '#pragma once'
        git add devices/ethdev common/tcp/export
        write_file common/tcp/stack/tcp_stack.hpp '#include <port.hpp>'
        git add common/tcp/stack/tcp_stack.hpp
        try_commit
        expect_rejected
        expect_output_contains "common/tcp/stack/tcp_stack.hpp:1"
        expect_output_contains "<aloe/ethdev>"
        ;;
```

- [ ] **Step 2: Run them and see both fail**

Run: `cmake --preset debug && ctest --preset debug -R "PreCommit.(checks_sources_under|rejects_an_include_of_a_device)" --output-on-failure`
Expected: both FAIL with "the commit went through".

- [ ] **Step 3: Name the groups in the scripts**

`check-format.sh`: `source_directories=(common devices l7 fixtures tests examples benchmarks)`.
`check-includes.sh`: the same list for `source_directories`, and `module_directories=(common devices l7 fixtures)`.

- [ ] **Step 4: Run the hook scenarios**

Run: `ctest --preset debug -R PreCommit --output-on-failure`
Expected: every `PreCommit` scenario passes.

- [ ] **Step 5: Commit**

```bash
git add scripts tests/functional_tests/hooks
git commit -m "build(scripts): check sources under devices/ and l7/"
```

- [ ] **Step 6: Move ethdev**

```bash
mkdir devices && git mv common/ethdev devices/ethdev
```

- `devices/CMakeLists.txt`: `set(DEVICES ${PROJECT_NAME}.Devices)` and `add_subdirectory(ethdev)`, in the shape of `fixtures/CMakeLists.txt`.
- Root `CMakeLists.txt`: `add_subdirectory(devices)` between `common` and `fixtures`.
- `common/CMakeLists.txt`: drop the `ethdev` line.
- `devices/ethdev/CMakeLists.txt`: `set(ETHDEV ${DEVICES}.Ethdev)` and `add_library(Aloe::Devices::Ethdev ALIAS ${ETHDEV})`.
- The fourteen consumer CMake files listed above: `Aloe::Common::Ethdev` becomes `Aloe::Devices::Ethdev`.

- [ ] **Step 7: Update the docs**

- `AGENTS.md`, layout table. Remove the `ethdev (...)` clause from the `common/<module>/` row. Add a row after it and replace the `component/<module>/` row:

```markdown
| `devices/<module>/` | Production device backends, each a concrete implementation of `device`'s concepts: `ethdev` (the DPDK backend, the only module that links DPDK). Built on `common`; nothing in `common` or `fixtures` depends on them. |
| `l7/<module>/`      | Layer-7 protocol modules built on a stream (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, ...), each an independent unit built on `common`. None exist yet; the directory appears with the first. |
```

- `AGENTS.md`, targets paragraph: the groups are `Common`, `Devices`, `Fixtures` and `Benchmarks`.
- `README.md:49`: the directory cell becomes `devices/ethdev/`.
- `docs/architecture/device.md:12`: the target cell becomes `Aloe::Devices::Ethdev`.
- `codestyle.md:606`: "Each module under `common/`, `devices/`, `fixtures/` or `l7/` ships an umbrella header".

- [ ] **Step 8: Add the pool's TODO if it is missing**

Above the doc comment of `class NodePool` in `common/tcp/pool/detail/node_pool.hpp`:

```cpp
    // TODO: move to core when a second module needs a fixed pool, and decide then whether the reservation, which
    // exists for TCP's FIN node, moves with it
```

- [ ] **Step 9: Build, test, check**

Run:
```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
./scripts/check-format.sh && ./scripts/check-includes.sh
git grep -nE "Common::Ethdev|Common\.Ethdev|common/ethdev|component/|common component" -- . ':!docs/superpowers'
```
Expected: the build is clean, all 62 tests pass (the baseline's 60 and the two hook scenarios), both scripts pass, and the `git grep` prints nothing. `docs/architecture/overview.md` named no moved path at a2844ae; the `git grep` is what proves it still does not.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "refactor(ethdev): move the DPDK backend to devices/, name the l7 group"
```

---

### Task 2: `net/ipv4/` and the prefixed types

**Files:**
- Move: `common/net/stack/` to `common/net/ipv4/stack/`, `common/net/arp/` to `common/net/ipv4/arp/`
- Rename: `ipv4/stack/datagram.hpp` to `ipv4/stack/ipv4_datagram.hpp`, `ipv4/stack/prebuilt.hpp` to `ipv4/stack/ipv4_prebuilt.hpp`
- Create: `common/net/ipv4/arp/arp_resolution.hpp`
- Modify: `common/net/CMakeLists.txt`, `common/net/export/aloe/net`
- Modify, names: `common/net/ipv4/stack/ipv4.hpp`, `ipv4/stack/detail/{ipv4_arp,ipv4_input,ipv4_output}.hpp`, `ipv4/arp/arp_cache.cpp:115` (comment), `common/tcp/stack/{tcp_fwd,tcp_stack}.hpp`, `common/tcp/stack/detail/{receiver,segment_writer}.hpp`, `common/tcp/connection/detail/frame_template.hpp`, `common/runtime/stack/tcp_runtime_stack.hpp` (only if it names a moved header)
- Modify, tests: `tests/unit_tests/common/net/test_net_prebuild.cpp`, `tests/unit_tests/common/net/test_net_arp_cache.cpp:214` (message text), `tests/functional_tests/hooks/pre_commit.sh:48`
- Modify, docs: `docs/architecture/net.md` (lines 24, 37, 44, 129), `AGENTS.md:46` (net's `stack/detail/` becomes `ipv4/stack/detail/`), `codestyle.md:545-550`

**Interfaces:**
- Produces: `net::Ipv4Datagram<Packet>` in `<ipv4_datagram.hpp>`; `net::Ipv4Prebuilt` in `<ipv4_prebuilt.hpp>`; `net::ArpResolution` in `<arp_resolution.hpp>`. Members and layout are unchanged.

This task changes no behaviour, so it has no new test: the old names stop existing, and the compiler and the existing suite are the check.

- [ ] **Step 1: Move the folders and rename the two files**

```bash
mkdir common/net/ipv4
git mv common/net/stack common/net/ipv4/stack
git mv common/net/arp common/net/ipv4/arp
git mv common/net/ipv4/stack/datagram.hpp common/net/ipv4/stack/ipv4_datagram.hpp
git mv common/net/ipv4/stack/prebuilt.hpp common/net/ipv4/stack/ipv4_prebuilt.hpp
```

- [ ] **Step 2: Give `ArpResolution` its own header**

Move the struct, with its comment, from `ipv4_datagram.hpp` to the new `ipv4/arp/arp_resolution.hpp` in `namespace aloe::net`. Add the header to `${NET}.Arp`'s sources. Whoever used `ArpResolution` through `datagram.hpp` includes `<arp_resolution.hpp>`.

- [ ] **Step 3: Rename the types and fix the paths**

- `Datagram` becomes `Ipv4Datagram`, `Prebuilt` becomes `Ipv4Prebuilt`, in `net`, in `tcp` and in the tests.
- TCP's member aliases keep their name: `using Datagram = net::Ipv4Datagram<Packet>;`.
- `common/net/CMakeLists.txt`: every `arp/` and `stack/` path gains the `ipv4/` prefix, the two renamed files are listed by their new names, and the banner above `${NET}.Arp`, which still says "Wire formats", says "The ARP cache".
- `common/net/export/aloe/net`: the include list, kept alphabetical, names `arp_resolution.hpp`, `ipv4_datagram.hpp` and `ipv4_prebuilt.hpp`.
- `pre_commit.sh:48`: the `two_modules` helper writes `common/net/ipv4/stack/ipv4.hpp`.

- [ ] **Step 4: Update the docs**

`docs/architecture/net.md`: the three type names and the path of the parts. `AGENTS.md:46`: net's parts are in `ipv4/stack/detail/`. `codestyle.md:545-550`: the example's `Datagram` becomes `Ipv4Datagram`.

- [ ] **Step 5: Build with both compilers, test, check**

Run:
```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug
./scripts/check-format.sh && ./scripts/check-includes.sh
git grep -nE "net::(Datagram|Prebuilt)\b|<(datagram|prebuilt)\.hpp>|net/(stack|arp)/" -- . ':!docs/superpowers'
```
Expected: both builds are clean, all 62 tests pass, both scripts pass, and the `git grep` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(net): move the IPv4 brick under net/ipv4 and prefix its types"
```

---

### Task 3: The fabric parses with `wire`

**Files:**
- Modify: `fixtures/fabric/fabric/fabric.cpp` (`Port::inspect`, `Port::fill_checksums`)
- Delete: `fixtures/fabric/fabric/detail/frame.hpp`
- Modify: `fixtures/fabric/CMakeLists.txt:25`
- Test: `tests/unit_tests/fixtures/fabric/test_fabric_steering.cpp`, `tests/unit_tests/fixtures/fabric/test_fabric_offloads.cpp`

**Interfaces:**
- Consumes: `wire::EthernetHeader::parse`, `wire::Ipv4Header::parse`, `wire::Ipv4Header::is_fragment`, `wire::TcpHeader::size` and `wire::TcpHeader::checksum_offset`, `wire::Ipv4Header::size`.
- Produces, in the anonymous namespace of `fabric.cpp`, nothing public:

```cpp
inline constexpr std::size_t udp_header_size     = 8;  // wire has no UDP format
inline constexpr std::size_t udp_checksum_offset = 6;
inline constexpr std::size_t ports_size          = 4;  // source and destination port lead both TCP and UDP

/// The IPv4 header of an Ethernet frame, as wire parses it. Nothing for any other frame, a truncated one included.
[[nodiscard]] std::optional<wire::Ipv4Header> ipv4_of(std::span<const std::byte> frame) noexcept;

/// Where the segment starts in the frame, and how long the total length says it is. Inside the frame for any
/// header `ipv4_of` returned.
[[nodiscard]] std::size_t l4_offset_of(const wire::Ipv4Header& ipv4) noexcept;
[[nodiscard]] std::size_t l4_length_of(const wire::Ipv4Header& ipv4) noexcept;

/// The flow a card steers the frame by. A fragment keeps only its addresses. The ports take part only for an
/// unfragmented TCP or UDP segment of at least `ports_size` bytes.
[[nodiscard]] device::FlowTuple flow_of(const wire::Ipv4Header& ipv4, std::span<const std::byte> l4) noexcept;

/// The offset of the checksum field inside the segment: unfragmented TCP or UDP with a whole header.
[[nodiscard]] std::optional<std::size_t> l4_checksum_offset(const wire::Ipv4Header& ipv4,
                                                            std::size_t l4_length) noexcept;
```

- [ ] **Step 1: Pin the behaviour that must not change**

Add to `test_fabric_steering.cpp`:

```cpp
TEST_F(FabricSteering, ASegmentTooShortForPortsIsSteeredWithoutThem) {
    auto frame = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Tcp, 40000), aloe::frames::pattern(0));
    frame.resize(14 + 20 + 2);                                        // two bytes of segment: no room for the ports
    aloe::wire::store_be16(std::span{frame}.subspan(14 + 2, 2), 22);  // total length: the header and those two bytes
    const aloe::device::FlowTuple flow{
        .source = client_ip, .destination = server_ip, .protocol = aloe::wire::Ipv4Protocol::Tcp};
    const std::uint16_t expected = aloe::device::queue_for(server_port_.steering(), flow);
    send(frame);
    aloe::fabric::Packet packet = receive_only_on(expected);
    ASSERT_FALSE(packet.empty());
    EXPECT_EQ(packet.rx().rss_hash, aloe::device::flow_hash(server_port_.steering(), flow));
}
```

Run: `cmake --build --preset debug && ./build/debug/tests/unit_tests/fixtures/fabric/Aloe.Tests.Unit.Fabric --gtest_filter='FabricSteering.ASegment*'`
Expected: PASS. It passes before the change and must still pass after it.

- [ ] **Step 2: Write the two tests of the new behaviour**

Replace `ATruncatedFrameIsStillSteeredAndDoesNotCrash` in `test_fabric_steering.cpp`:

```cpp
TEST_F(FabricSteering, ATruncatedFrameGoesToQueueZeroWithoutAHash) {
    std::uint16_t source_port = 40000;
    while (aloe::device::queue_for(server_port_.steering(),
                                   aloe::frames::flow_of(spec(aloe::wire::Ipv4Protocol::Tcp, source_port))) == 0) {
        ++source_port;  // a flow the hash would send elsewhere, so queue 0 below is not the hash's choice
    }
    auto frame = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Tcp, source_port), aloe::frames::pattern(64));
    frame.resize(frame.size() - 40);  // the total length now claims more than the frame carries
    send(frame);
    aloe::fabric::Packet packet = receive_only_on(0);
    ASSERT_FALSE(packet.empty());
    EXPECT_FALSE(packet.rx().rss_hash.has_value());
}
```

Add to `test_fabric_offloads.cpp`:

```cpp
TEST_F(FabricOffloads, LeavesATruncatedFrameUnknown) {
    auto frame = aloe::frames::ipv4_frame(spec(aloe::wire::Ipv4Protocol::Udp, aloe::frames::Checksums::Correct),
                                          aloe::frames::pattern(64));
    frame.resize(frame.size() - 40);  // the total length now claims more than the frame carries
    aloe::fabric::Packet received = exchange(frame, aloe::device::TxMetadata{});
    EXPECT_EQ(received.rx().l3, aloe::device::ChecksumVerdict::Unknown);
    EXPECT_EQ(received.rx().l4, aloe::device::ChecksumVerdict::Unknown);
}
```

- [ ] **Step 3: Run them and see both fail**

Run: `cmake --build --preset debug && ./build/debug/tests/unit_tests/fixtures/fabric/Aloe.Tests.Unit.Fabric --gtest_filter='FabricSteering.ATruncated*:FabricOffloads.LeavesATruncated*'`
Expected: `ATruncatedFrameGoesToQueueZeroWithoutAHash` FAILS (the frame is on the hashed queue and carries a hash); `LeavesATruncatedFrameUnknown` FAILS (`l3` is `Good`, `l4` is `Bad`).

- [ ] **Step 4: Replace the parser**

Write the five helpers of the Interfaces block in `fabric.cpp`'s anonymous namespace. `Port::inspect` and `Port::fill_checksums` use them in place of `detail::parse_ipv4` and `Ipv4Frame`; every assertion in `fill_checksums` stays, reading `ipv4->header_length` from wire's header. Delete `fabric/detail/frame.hpp`, its line in `fixtures/fabric/CMakeLists.txt` and its include.

- [ ] **Step 5: Run the fabric tests, then everything, under asan too**

Run:
```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan -R "Unit.Fabric"
./scripts/check-format.sh && ./scripts/check-includes.sh
```
Expected: every test passes; asan reports nothing. If a test outside the fabric's own fails because it relied on a truncated frame being steered, stop and report it: the spec did not foresee one.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(fabric): parse IPv4 with wire's parser"
```

---

### Task 4: Counters by owner

**Files:**
- Rename: `common/loop/counters/counters.hpp` to `common/loop/counters/shard_queue_counters.hpp`
- Create: `common/runtime/counters/shard_counters.hpp`
- Modify: `common/loop/CMakeLists.txt:35`, `common/loop/export/aloe/loop`, `common/loop/queue/shard_queue.hpp:14,32,51-53,147`
- Modify: `common/runtime/CMakeLists.txt` (new component, `Scope` links it, the combined library lists it), `common/runtime/export/aloe/runtime`
- Modify: `common/runtime/scope/task_scope.hpp:85,132`, `scope/task_scope.cpp:14`, `context/shard_context.hpp:155-166`, `shard/shard.hpp:88,112`, `runtime/runtime.hpp:138-149,234`
- Modify, type name only: `examples/ping_responder/ping_responder.cpp:108`, `examples/tcp_echo/tcp_echo.cpp:193`, `benchmarks/tick_to_trade/level_bricks.cpp:195`, `benchmarks/tick_to_trade/stack_sanity.hpp:21` and its caller in `level_runtime.cpp`, `fixtures/scenario/run/harness.hpp:113`, `tests/shared/net/net_fixture.hpp:119`, `tests/shared/tcp/tcp_fixture.hpp:201`, and every test that names `loop::ShardCounters` (`git grep -l ShardCounters tests`)
- Modify, field access: `examples/ethernet_echo/ethernet_echo.cpp:155-157`, `tests/unit_tests/common/runtime/test_shard.cpp:277,278,299,302,321,328`, `test_steering.cpp:110-111`, `tests/integration_tests/runtime/test_runtime_ring.cpp:110,112`, `tests/manual_tests/runtime/test_runtime_tap.cpp:78`
- Test: `tests/unit_tests/common/loop/test_shard_queue.cpp`
- Modify, docs: `docs/architecture/loop.md:34,57`, `docs/architecture/runtime.md:25,73`, `docs/architecture/net.md:68`, `docs/architecture/tcp.md:88`, `README.md:90`, `codestyle.md:329,334`, `docs/roadmap.md:161`

**Interfaces:**
- Produces, in `<shard_queue_counters.hpp>`:

```cpp
namespace aloe::loop {
    /// What one shard's device queue counted, all monotonic. Read on the shard's thread, or after it has stopped.
    struct ShardQueueCounters {
        std::uint64_t frames_received    = 0;
        std::uint64_t frames_transmitted = 0;  ///< Accepted by the device.
        std::uint64_t transmit_refused   = 0;  ///< `transmit` returned false, or a frame was dropped at drain.

        friend constexpr bool operator==(const ShardQueueCounters&, const ShardQueueCounters&) noexcept = default;
    };
}
```

- Produces, in `<shard_counters.hpp>`, component `${RUNTIME}.Counters` (INTERFACE, include directory `counters/`, links `Aloe::Common::Loop`):

```cpp
namespace aloe::runtime {
    /// Per-shard counters of the runtime, all monotonic. Read on the shard's thread, or after the shard has stopped.
    struct ShardCounters {
        loop::ShardQueueCounters queue{};   ///< What the shard's device queue counted.
        std::uint64_t ticks           = 0;
        std::uint64_t idle_ticks      = 0;  ///< Steps in which nothing was received, run, fired or sent.
        std::uint64_t inbox_received  = 0;
        std::uint64_t work_run        = 0;
        std::uint64_t timers_fired    = 0;
        std::uint64_t tasks_spawned   = 0;
        std::uint64_t tasks_completed = 0;
        std::uint64_t tasks_stopped   = 0;
        std::uint64_t tasks_failed    = 0;

        friend constexpr bool operator==(const ShardCounters&, const ShardCounters&) noexcept = default;
    };
}
```

- Changes: `loop::ShardQueue(Device&, std::uint16_t, std::size_t, ShardQueueCounters&)`; `ShardQueue::receive` adds what it returns to `frames_received`; `runtime::TaskScope(log::Logger, std::uint16_t, ShardCounters&)`; `ShardContext::counters()` and `Runtime::counters(index)` return `runtime::ShardCounters`.

- [ ] **Step 1: Write the failing test**

Add to `test_shard_queue.cpp` (it needs `<array>`):

```cpp
TEST_F(QueueFixture, ReceiveCountsTheFramesItReturns) {
    constexpr aloe::wire::MacAddress peer_mac{0x02, 0, 0, 0, 0, 0x02};
    aloe::fabric::Port& peer = fabric.add_port({.mac = peer_mac, .pool_size = 4});
    auto packet              = peer.allocate(0);
    ASSERT_TRUE(packet.has_value());
    ASSERT_TRUE(aloe::frames::fill(
        *packet,
        aloe::frames::ethernet_frame(mac, peer_mac, aloe::frames::ethertype_experimental, aloe::frames::pattern(20))));
    std::array<aloe::fabric::Packet, 1> sent{std::move(*packet)};
    ASSERT_EQ(peer.transmit(0, sent), 1);

    std::array<aloe::fabric::Packet, 4> burst;
    EXPECT_EQ(queue.receive(burst), 1);
    EXPECT_EQ(counters.frames_received, 1);
    EXPECT_EQ(queue.receive(burst), 0);
    EXPECT_EQ(counters.frames_received, 1) << "an empty receive counts nothing";
}
```

- [ ] **Step 2: Run it and see it fail**

Run: `cmake --build --preset debug && ctest --preset debug -R "Unit.Loop.Queue" --output-on-failure`
Expected: FAIL at the first `frames_received` check, which reads 0.

- [ ] **Step 3: Split the struct**

- `git mv` the loop header and reduce the struct to `ShardQueueCounters` as in the Interfaces block. `ShardQueue` takes and holds it, and `receive` counts.
- Create the runtime's header and component. `${RUNTIME}.Scope` links `${RUNTIME}.Counters`; the combined runtime library lists it; the runtime umbrella includes `<shard_counters.hpp>`.
- `ShardContext` owns a `runtime::ShardCounters`. `Shard` constructs its queue with `context_.counters().queue` and no longer adds to `frames_received` in `step`.
- `TaskScope`, `Runtime::counters` and the runtime's summary line use the new type; the summary reads the two frame counts through `.queue`.
- Every program over the bricks declares `loop::ShardQueueCounters`. `stack_sanity.hpp` takes `const loop::ShardQueueCounters&`, and the runtime level passes `.queue`.
- Readers of the three queue fields under the runtime write `counters.queue.<field>`. No expected number in any test changes.

- [ ] **Step 4: Update the docs**

- `loop.md:34`: the bullet describes `ShardQueueCounters`, three counts written by the queue, and says the runtime's `ShardCounters` holds one beside its own. `loop.md:57` and the examples in `net.md:68`, `tcp.md:88`, `README.md:90` and `codestyle.md:329,334` use the new type name.
- `runtime.md`: name `runtime::ShardCounters` where the context's counters and `counters(i)` are described, with `queue` as the member that holds the frame counts.
- `docs/roadmap.md:161`: delete the open question about `loop::ShardCounters`; it is answered.

- [ ] **Step 5: Build with both compilers, test, check**

Run:
```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug --clean-first && ctest --preset gcc-debug
./scripts/check-format.sh && ./scripts/check-includes.sh
git grep -nE "loop::ShardCounters|<counters\.hpp>" -- . ':!docs/superpowers'
```
Expected: both builds are clean, every test passes, both scripts pass, and the `git grep` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(loop): counters by owner, the queue's in loop and the shard's in runtime"
```

---

### Task 5: The whole branch

**Files:**
- Add, forced: `docs/superpowers/specs/2026-11-16-layout-round-design.md`, `docs/superpowers/plans/2026-11-16-layout-round.md`

- [ ] **Step 1: The four presets and both scripts**

Run, for each of `debug`, `gcc-debug`, `asan`, `tsan`:
```bash
cmake --preset <name> && cmake --build --preset <name> && ctest --preset <name>
```
then `./scripts/check-format.sh && ./scripts/check-includes.sh`.
Expected: four clean builds; every test passes in each; `debug` lists 62 tests, the baseline's 60 and the two hook scenarios. The three new gtest cases and the replaced one live inside existing binaries: confirm them with `--gtest_list_tests` on `Aloe.Tests.Unit.Fabric` and `Aloe.Tests.Unit.Loop.Queue`.

- [ ] **Step 2: The tree says what the docs say**

Run:
```bash
git grep -nE "Common::Ethdev|common/ethdev|component/|net::(Datagram|Prebuilt)\b|loop::ShardCounters|detail/frame\.hpp|parse_ipv4" -- . ':!docs/superpowers'
git grep -n "docs/superpowers" -- AGENTS.md README.md codestyle.md docs/architecture docs/guides docs/roadmap.md
git diff --stat -M main... | tail -1
```
Expected: the first two print nothing. Read `AGENTS.md`'s layout table against `ls common devices fixtures` once.

- [ ] **Step 3: The benchmark, if the baseline was recorded**

Run:
```bash
cmake --preset release && cmake --build --preset release --target Aloe.Benchmarks.TickToTrade
./build/release/benchmarks/tick_to_trade/tick_to_trade --topology both --hop --out <scratch>/layout-branch.json
./scripts/compare-benchmark.py <scratch>/layout-main.json <scratch>/layout-branch.json
```
Expected: no gated change on the 64-byte rows beyond what `docs/benchmarks/tick-to-trade.md` describes as run-to-run noise. Report the table either way. Do not commit a result file: this round records no result.

- [ ] **Step 4: Commit the spec and the plan**

```bash
git add -f docs/superpowers/specs/2026-11-16-layout-round-design.md docs/superpowers/plans/2026-11-16-layout-round.md
git commit -m "docs: add the layout round's spec and plan"
```

- [ ] **Step 5: Whole-branch review**

Run superpowers:requesting-code-review against the spec, then superpowers:verification-before-completion. Hand over with: the five commits, the four presets' test counts, the benchmark table or the reason it was skipped, and the pull request title `[REFACTORING][INFRASTRUCTURE] A devices group, net's IPv4 folder, one IPv4 parser, counters by owner`. Push and open the pull request when the author asks.
