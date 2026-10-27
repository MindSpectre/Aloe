# Scenario scripts

These are Aloe's own scenario scripts, written in packetdrill's syntax and asserting what today's TCP brick sends
and accepts. `Aloe.Tests.Unit.Scenarios` finds every `.pkt` under this directory at run time and runs each one
twice, once in each checksum mode of the fabric (`NoOffloads` and `Checksums`), as the test
`Scenario.<path without extension>/<mode>`. The grammar the runner accepts, the system calls it binds to the
brick's verbs, and how to run one script are in [docs/architecture/scenario.md](../../docs/architecture/scenario.md).
