#pragma once

#include <httplib.h>

#include <string>
#include <unordered_set>
#include <vector>

/*
 * Who a request is actually from.
 *
 * With a reverse proxy in front of this process every connection arrives from
 * the proxy, so `remote_addr` is the same value for everybody, and it is what
 * the rate limiter would otherwise key on. The proxy records the address it saw
 * in X-Forwarded-For, and that header is read only when the connection came
 * from an address configured as a proxy.
 */
namespace clientaddr {

class Resolver {
public:
  /*
   * `trustedPeers` are the addresses whose X-Forwarded-For is read, as they
   * appear in `remote_addr`. Empty is the default and means the header is not
   * read at all: the peer of the connection is the client.
   */
  explicit Resolver(const std::vector<std::string>& trustedPeers);

  /* The address to attribute `req` to. Never empty. */
  std::string of(const httplib::Request& req) const;

  /* Whether any proxy is trusted at all, for the startup line that says so. */
  bool trustsAnyone() const {
    return !trusted_.empty();
  }

private:
  std::unordered_set<std::string> trusted_;
};

} // namespace clientaddr
