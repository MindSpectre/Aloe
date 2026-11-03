namespace stdexec {
  struct forwarding_query_t;
  extern forwarding_query_t const forwarding_query;
}
namespace stdexec {
  struct forwarding_query_t {};
  inline constexpr forwarding_query_t forwarding_query{};
}
