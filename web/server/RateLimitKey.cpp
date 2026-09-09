#include "RateLimitKey.h"

#include <arpa/inet.h>

#include <cstring>

namespace ratelimitkey {

namespace {

/* Longest address this will attempt, matching the cap the address is admitted
 * under: an IPv4-mapped IPv6 address is the widest of the forms that appear,
 * at 45 characters. The buffer is one longer for the terminator inet_pton
 * requires, which a string_view does not carry. */
constexpr size_t MAX_ADDRESS = 45;

/*
 * Whether this is an IPv4 address wearing a v6 costume: ::ffff:a.b.c.d, the
 * form a dual-stack socket reports an IPv4 peer as.
 *
 * This case has to be found before any truncation happens. The whole IPv4
 * internet lives inside ::ffff:0:0/96, so masking one of these to its /64
 * would file every IPv4 visitor under ::/64 -- one bucket for the lot of them.
 */
bool isV4Mapped(const in6_addr& addr) {
  for (int i = 0; i < 10; ++i) {
    if (addr.s6_addr[i] != 0)
      return false;
  }

  return addr.s6_addr[10] == 0xff && addr.s6_addr[11] == 0xff;
}

} // namespace

std::string group(std::string_view address) {
  if (address.empty() || address.size() > MAX_ADDRESS)
    return std::string(address);

  char text[MAX_ADDRESS + 1];
  std::memcpy(text, address.data(), address.size());
  text[address.size()] = '\0';

  in6_addr addr{};

  /* A plain IPv4 address fails this and is returned whole, which is the
   * intended answer for it rather than a fallback. inet_pton also rejects a
   * zone id, so a link-local address keeps its interface suffix and stays
   * distinct -- one cannot reach this server from another link in any case. */
  if (inet_pton(AF_INET6, text, &addr) != 1)
    return std::string(address);

  if (isV4Mapped(addr))
    return std::string(address);

  /* The interface identifier is the half the client chooses, so it goes. */
  std::memset(addr.s6_addr + 8, 0, 8);

  char prefix[INET6_ADDRSTRLEN];

  if (inet_ntop(AF_INET6, &addr, prefix, sizeof(prefix)) == nullptr)
    return std::string(address);

  return std::string(prefix) + "/64";
}

} // namespace ratelimitkey
