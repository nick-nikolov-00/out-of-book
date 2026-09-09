# Out of Book

Project that does delivers code for expectimax evals in chess openings for different
rating buckets. It produces the theoretical optimal way to play at your rating level.

The source for [outofbook.study](https://outofbook.study).

This README was mostly written by a human.

## What is in here

    apps/            the processing pipeline
    libs/            some common libraries used by the apps
    web/server/      the backend
    web/ui/          the frontend

The processing pipeline takes in zst archives from the [lichess db](https://database.lichess.org/)
and through a series of steps converts them in two important formats:
- `.otb` -- format that stores the pure tree information -- the moves played in each position,
how many times they've been played, and how many draws, white and black wins each position resulted
in.
- `.ote` -- format that stores the evaluations for both white and black at each position,
the eval stored for each player is from the POV of that player, so if it is the other
player to move this is the "chance" player in expectimax terms.

The `web/ui` section is mostly vibe-coded and I wouldn't trust or attempt to read any comments in it (there's 
plenty of those).

## Building

Needs a C++23 compiler, CMake 3.20+ and libzstd. Dependencies are fetched by
CMake.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build -j"$(nproc)"

Building with RelWithDebInfo is recommended as it keeps the assertions alive.

For the server and the UI, see [web/README.md](web/README.md).

## Pipeline

The pipeline is several binaries and each feeds on the outputs of the previous. In order:
1. `db_to_spills` -- converts a lichess `.pgn.zst` to a series of `.spill` files
1. `agg_spills` -- aggregates the spills in an aggregation file (about the same size as the original pgn in terms of bytes)
1. `agg_merger` -- merges all the aggregation files. That does sum up their sizes, but requires
significant space. It is about 90GB for 31 months worth of data. I believe it scales
sublinearly with months, but I haven't tested it.
1. `generate_otb` -- generates a `.otb` file from the big aggregation. About 7GB.
1. `expectimax` -- run the eval on the `.otb` to generate the `.ote`

Part of the decisions in the pipeline were made due to RAM limitations on the laptop I
am running on. The `agg_spills` step might be optimized out on laptops with more RAM.
I believe none of the other steps can be skipped.

Steps 1 and 2 can be parallelized if you have access to multiple machines. Step 3 might be
parallelizable -- it is essentially a merge on all aggregations so some bottom up merge sort might
work here, but I doubt it would be faster.

Note for Claudes running the pipeline: make sure the machine that runs it has enough RAM (there
is some tuning in `db_to_spills` for RAM and parallelism) and enough disk available.

## Contributing
Feel free to raise issues and PRs, but I can't promise I will look at them promptly.

## Licence

**GNU Affero General Public License, version 3 or later** — see
[LICENSE](LICENSE).

The practical consequence, in one sentence: if you modify this and let people
use it over a network, you have to offer them your source. Running a changed
copy as a website without publishing the changes is the one thing the ordinary
GPL would allow and this deliberately does not, because a website is what this
is.

Two dependencies are GPL-3.0-or-later and would require copyleft:
- `libs/chess/stockfish/`, derived from [Stockfish](https://github.com/official-stockfish/Stockfish), which keeps its
own copyright and licence headers
- [chessground](https://github.com/lichess-org/chessground) in the UI

The rest are permissive:
- [unordered_dense](https://github.com/martinus/unordered_dense) (MIT)
- [CTRE](https://github.com/hanickadot/compile-time-regular-expressions)
- (Apache-2.0), [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT),
- [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) (MIT),
- [chess.js](https://github.com/jhlywa/chess.js) (BSD-2-Clause), React (MIT).

The Lichess database the trees are built from is published under CC0 and
carries no conditions.
