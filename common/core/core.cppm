/**
 * The Aloe core module: the version, the clock aliases and small helpers, with no dependencies, so every module can
 * import it. Consumers link Aloe::Common::Core and write `#include <aloe/core>`, which imports it.
 */
export module aloe.core;

export import :clock;
export import :fixed_string;
export import :guards;
export import :tsc_clock;
export import :tsc_scale;
export import :values;
export import :version;
