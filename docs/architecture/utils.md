# Utils Module

The utils module (`common/utils/`) holds small header-only helpers with no dependencies: discarding values
on purpose, choosing a fallback, a string literal as a template parameter, and two guards that pin what a
member function is. Everything is reached through the umbrella `#include <aloe/utils>`
(`export/aloe/utils`), and targets link `Aloe::Common::Utils`.
It is separate from [`core`](core.md) so that a header which needs a guard does not pull stdexec in with it.

## Key types

- **`aloe::utils::unused_value(values...)`** -- discards any number of values inside a body, where a
  template decides not to use something it was handed. `[[maybe_unused]]` marks a declaration and
  `std::ignore = f()` discards one result at a call; this is the visible form for everything else. Every
  argument is evaluated exactly once.
- **`aloe::utils::value_or(value, fallback)`** -- GCC's `value ?: fallback` as a function: `value` when it
  converts to true, else `fallback`, with `value` evaluated once. Two lvalues of one type yield that lvalue,
  so nothing is copied; any other mix yields a value of the common type.
- **`aloe::utils::FixedString<N>`** -- a string literal as a template parameter: `template <FixedString Text>`.
  Structural, so equal texts are one specialisation. Core's logger takes its format strings this way.
- **`aloe::utils::force_non_const(this)`** -- does not compile inside a const member function. A const
  member may still write through a pointer member, so the compiler never objects when such a function is
  declared const by mistake; this does.
- **`aloe::utils::force_non_static(this)`** -- does not compile inside a static member function, where there
  is no `this`.

## Usage

```cpp
#include <aloe/utils>

class Handle {
public:
    void set(const int value) noexcept {
        aloe::utils::force_non_const(this);  // writes through target_, which a const member could still do
        *target_ = value;
    }

private:
    int* target_ = nullptr;
};

template <typename... Args>
void ignore_all(Args&&... args) {
    aloe::utils::unused_value(args...);
}

const char* name_or_default(const char* name) {
    return aloe::utils::value_or(name, "anonymous");
}
```

## Design notes

The guards cost nothing: empty `constexpr` functions whose only content is a `static_assert` on the deduced
type. They exist for handle types such as the ethdev packet, whose state lives behind a pointer. There,
`const` on a member function is a promise the compiler cannot check, and the guard turns it into one it can.

The module has no dependencies and keeps none: it is included from hot-path headers, and the roadmap keeps
stdexec out of those.
