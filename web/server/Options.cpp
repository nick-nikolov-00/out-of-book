#include "Options.h"

#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

/* Split a comma-separated list, dropping empty entries so a trailing comma or
 * a stray space is not turned into an address nothing can match. */
std::vector<std::string> splitOnCommas(const std::string& text) {
  std::vector<std::string> parts;
  std::istringstream stream(text);
  std::string part;

  while (std::getline(stream, part, ',')) {
    const auto begin = part.find_first_not_of(" \t");

    if (begin == std::string::npos)
      continue;

    parts.push_back(part.substr(begin, part.find_last_not_of(" \t") - begin + 1));
  }

  return parts;
}

} // namespace

std::optional<Options> parseArgs(int argc, char** argv) {
  Options options;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + what);

      return argv[++i];
    };

    if (arg == "--port")
      options.port = std::stoi(next("--port"));
    else if (arg == "--host")
      options.host = next("--host");
    else if (arg == "--trust-forwarded-from")
      options.trustForwardedFrom = splitOnCommas(next("--trust-forwarded-from"));
    else if (arg == "--metrics-port")
      options.metricsPort = std::stoi(next("--metrics-port"));
    else if (arg == "--metrics-host")
      options.metricsHost = next("--metrics-host");
    else if (arg == "--dubious")
      options.dubious = next("--dubious");
    else if (arg == "--check")
      options.checkOnly = true;
    else if (arg == "-h" || arg == "--help")
      return std::nullopt;
    else
      positional.push_back(arg);
  }

  if (positional.empty() || positional.size() > 2)
    return std::nullopt;

  options.otb = positional[0];

  if (positional.size() == 2)
    options.ote = positional[1];

  return options;
}

const char* usage() {
  return "usage: initium_server <data.otb> [evals.ote] [--host H] [--port P] "
         "[--trust-forwarded-from A,B] [--metrics-host H] [--metrics-port P] "
         "[--dubious FILE] [--check]";
}
