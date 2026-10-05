# Log Module

The log module (`common/log/`) is the one header that names quill: named loggers and the process's logging
backend. It depends on quill and on [`core`](core.md) for `FixedString`. Everything is reached through the
umbrella `#include <aloe/log>` (`export/aloe/log`), and targets link `Aloe::Common::Log`. Only the runtime,
the examples and the tests link it: a brick never logs.

## Key types

- **`aloe::log::Logging`, `aloe::log::LoggingConfig`** -- the process's logging backend: construct one, once,
  with the level, an optional file instead of the console, and an optional CPU to pin the backend thread to.
- **`aloe::log::Logger`, `aloe::log::logger(name)`** -- a named logger. `log.info<"shard {} starting">(index)`:
  the format string is a template parameter, so there are no macros and each call site owns its metadata.
  Formatting happens on the backend thread.
- **`aloe::log::LogLevel`** -- `Trace` to `Critical`, and `None`.

## Usage

```cpp
#include <aloe/log>

int main() {
    aloe::log::Logging logging{{.level = aloe::log::LogLevel::Info}};
    const aloe::log::Logger log = aloe::log::logger("main");
    log.info<"listening on queue {}">(0);
    log.flush();
}
```

## Design notes

Logging is quill behind the same kind of alias as stdexec in [`execution`](execution.md):
`common/log/log/log.hpp` is the only file that names `quill::`. The wrapper is macro-free, which the "public
headers define no macros" rule demands, and keeps quill's shape: a lock-free per-thread queue on the calling
side, one backend thread that formats and writes. That backend thread must not land on a shard's core, which
is what `LoggingConfig::backend_cpu` is for. Nothing in the stack logs on a hot path; the convention is
enforced by review, not by the type system.

The module is separate from [`core`](core.md) so that linking core, which every module does, never brings
quill with it.
