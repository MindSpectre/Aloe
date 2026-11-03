/**
 * The Aloe stream module: the zero-copy stream contract every protocol layer offers, as concepts, the event and error
 * vocabulary, and the convenience `send`. Names no execution model. Consumers link Aloe::Common::Stream and write
 * `#include <aloe/stream>`, which imports it.
 */
export module aloe.stream;

export import :stream;
export import :stream_events;
