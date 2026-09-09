#pragma once

#include <optional>
#include <string>
#include <vector>

struct Options {
  std::string otb;
  std::string ote;
  std::string host = "127.0.0.1";
  int port = 8080;

  /*
   * Addresses whose X-Forwarded-For is read, as they appear as the peer of the
   * connection.
   *
   * Empty by default, which means the peer of the connection is the client.
   */
  std::vector<std::string> trustForwardedFrom;

  /*
   * /metrics is served by a second listener rather than added to the one above,
   * so the two can be bound to different addresses. This one defaults to the
   * loopback interface.
   */
  std::string metricsHost = "127.0.0.1";
  int metricsPort = 9101;

  /*
   * The list of moves the book will not recommend, whatever their record says.
   *
   * Empty means nothing is flagged, which is a correct way to run: every move
   * is then judged on its record alone. It is not the *deployed* way to run,
   * so the count that was loaded is said out loud at startup — a deploy that
   * dropped the flag would otherwise show up only as the book resuming a
   * recommendation somebody had already ruled out.
   */
  std::string dubious;

  /*
   * Load the files, run the pairing check, say whether it passed and exit
   * without listening.
   *
   * This exists for the deploy scripts. The check that a .ote was built with
   * this binary's ranking rule only runs at startup, so without a way to ask
   * the question separately the only way to discover a mismatched pair is to
   * point the live server at it and watch it refuse to boot — an outage, found
   * at the one moment it is most expensive.
   */
  bool checkOnly = false;
};

/* Nullopt for --help and for anything malformed; throws on a flag whose value
 * is missing. */
std::optional<Options> parseArgs(int argc, char** argv);

/* The usage line, without a trailing newline. */
const char* usage();
