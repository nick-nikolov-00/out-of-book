#pragma once

#include <string>
#include <string_view>

/*
 * What counts as one client for throttling.
 *
 * An address is not the same thing as a customer. IPv4 hands a machine a single
 * address, so the address is a fair stand-in for whoever is behind it. IPv6
 * hands a site a /64 and lets the machine pick any of the addresses inside it:
 * SLAAC privacy extensions rotate the low half on a timer, so an ordinary phone
 * drifts between addresses over a day without anybody intending it.
 *
 * So a v6 address is grouped to its /64 and a v4 address is left whole, which
 * is the same line the common proxies and CDNs draw.
 */
namespace ratelimitkey {

/*
 * The bucket identity for `address`. Returns the /64 with a `/64` suffix for a
 * genuine IPv6 address, and the address unchanged for everything else --
 * IPv4, an IPv4-mapped v6 address, and anything that does not parse.
 *
 * The suffix is what keeps a group from ever colliding with a whole address,
 * so the two kinds of key cannot be confused for one another in the table.
 *
 * Anything unparseable is passed through rather than rejected. The caller has
 * already decided this string identifies somebody; narrowing it is this
 * function's job, and vetting it is not.
 */
std::string group(std::string_view address);

} // namespace ratelimitkey
