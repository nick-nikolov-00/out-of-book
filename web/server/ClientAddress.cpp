#include "ClientAddress.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace clientaddr {

namespace {

/*
 * The longest an address can be written out: an IPv4-mapped IPv6 address, which
 * is the widest of the forms that appear here. Anything longer is not an
 * address. The value becomes a key in the rate limiter's table, so its length
 * is settled here rather than left to the header.
 */
constexpr size_t MAX_ADDRESS = 45;

/*
 * Whether `text` could be an address at all. Deliberately a character-class
 * check rather than a parse: the value is used as an opaque identity, never
 * connected to, so what matters is that it is short, printable and drawn from
 * the alphabet the two address families share.
 */
bool plausibleAddress(std::string_view text) {
  if (text.empty() || text.size() > MAX_ADDRESS)
    return false;

  return std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return std::isxdigit(c) || c == '.' || c == ':' || c == '%';
  });
}

/*
 * The last comma-separated entry of an X-Forwarded-For header.
 *
 * The last rather than the first. The header is a trail, `client, proxy,
 * proxy`, and each hop appends what it saw, so with exactly one proxy in front
 * the last entry is the one that proxy wrote.
 */
std::string_view lastForwardedFor(std::string_view header) {
  const auto comma = header.rfind(',');
  std::string_view last = comma == std::string_view::npos ? header : header.substr(comma + 1);

  const auto begin = last.find_first_not_of(" \t");

  if (begin == std::string_view::npos)
    return {};

  const auto end = last.find_last_not_of(" \t");
  last = last.substr(begin, end - begin + 1);

  /* A proxy may bracket an IPv6 literal, as a URL would. The brackets are
   * punctuation around the address rather than part of it. */
  if (last.size() >= 2 && last.front() == '[' && last.back() == ']')
    last = last.substr(1, last.size() - 2);

  return last;
}

} // namespace

Resolver::Resolver(const std::vector<std::string>& trustedPeers)
    : trusted_(trustedPeers.begin(), trustedPeers.end()) {}

std::string Resolver::of(const httplib::Request& req) const {
  if (trusted_.find(req.remote_addr) == trusted_.end())
    return req.remote_addr;

  /* Held by name rather than viewed straight out of the call: get_header_value
   * returns by value, and a view of that temporary would dangle before it could
   * be read. */
  const std::string header = req.get_header_value("X-Forwarded-For");
  const std::string_view forwarded = lastForwardedFor(header);

  /*
   * A peer that sent nothing usable is answered with its own address rather
   * than with an empty key.
   */
  if (!plausibleAddress(forwarded))
    return req.remote_addr;

  return std::string(forwarded);
}

} // namespace clientaddr
