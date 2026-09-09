# Initium web

Analysis board for the `.otb` opening tree and its `.ote` expectimax
evaluations.

- `server/` — C++ HTTP server that answers position lookups.
- `ui/` — React + TypeScript + Vite front end.

## Running it

The server is a normal target of the top-level CMake build. It takes a tree, an
evaluation file, and the list of moves the book will not recommend:

```sh
initium_server <tree.otb> <evals.ote> --dubious <dubious.txt>
```

It listens on `127.0.0.1:8080` by default (`--host`, `--port`). Only the tree's
index is held in memory; node blobs and eval entries are read from disk per
request.

An `.ote` does not record the ranking constants that built it, and those change
between runs, so the server re-derives a sample of recorded picks at startup and
refuses to serve a file that disagrees with the rule it was built with. Rebuild
the server against the header that produced the file, or regenerate the file
with the current one.

Then, in `ui/`:

```sh
npm install
npm run dev
```

Vite proxies `/api` to the server; set `INITIUM_SERVER` to point it elsewhere.
The server answers `/api` and nothing else — in production a reverse proxy
serves the built bundle and forwards `/api`. There is no single-command mode
that does both.

## API

`GET /api/position?fen=<fen>` returns everything the board needs for one
position in a single response: per-bucket totals, the expectimax evaluation for
each side, the pick per bucket, and a row per child move carrying that move's
per-bucket statistics, its child evaluation, its rank and score in the search's
own order, and a note if the book will not recommend it.

Every array is nine long, one entry per rating bucket. Two of the rows are not
playable moves: one stands for games that ended in this position, one for
children pooled together because they were too rarely played to trust
individually. Both count toward the bucket totals.

Unknown positions return `"found": false`. Malformed FENs return 400.

`GET /api/health` reports what the open tree is — node count, bucket count,
whether evaluations were loaded — rather than whether the process is up.

Anything else 404s as JSON rather than HTML, so a bad URL surfaces as a bad URL
and not as a parse error somewhere downstream.

## Evaluations

Both evaluation arrays are **white scores** in `[0, 1]`, computed per rating
bucket: 0 means Black wins, 1 means White wins.

- `evalW` — the value when **White** plays the expectimax-optimal move and
  Black follows the empirical move distribution.
- `evalB` — the mirror.

Neither is "the eval" on its own. Which applies is fixed by the side being
analysed, not by whose turn it is: the opponent is the field in both arrays. The
UI shows `1 - evalB` for Black so that higher is better on either side of the
board; only the presentation is inverted, never the ordering.

Buckets are evaluated independently, so exactly one is active at a time.
Averaging them would not correspond to anything expectimax produced.

## Move ranking

A child's evaluation is **not** the number the search ranked it by. Every
candidate is shrunk toward a prior — what neighbouring rating buckets say about
that same child — in proportion to how much evidence stands behind it. The
prior's weight is capped by the effective sample size of that neighbour average,
so a prior assembled from a handful of weighted games cannot be spent as if it
were a thousand observations.

The consequence is not a detail: a move played a few hundred times can carry a
better raw evaluation than one played tens of thousands of times and still lose
the argmax, so **the best move is routinely not the best-scoring child**. On top
of the shrink, a thin candidate that out-scores the best well-supported one only
takes the position if it beats it by a margin measured against its own noise.

Applying that rule repeatedly — best move, then the best of what is left, and so
on — gives a total order over the candidates, and it is the one order that
agrees with the search at every seat in the list. Sorting on the scores does not
reproduce it, and no post-hoc tweak of a score-sorted list will: removing the
leading move changes what a thin move is measured against. The scores are still
the right numbers to show beside the moves; they just are not the order. Rows
whose support is too thin to rank on are dimmed, so a dimmed higher score
sitting above an undimmed lower one is the tree saying it does not believe the
higher one.

The rule lives in a single header shared by the evaluator, the server and the
verifier, so they cannot drift.

### Moves the book will not recommend

The ranking is a statement about the data, and a few moves win games for a
reason that is not about the move — a piece hung against a field that has
already premoved its reply, say. The evaluator is right to like those and the
book is wrong to suggest them, because whoever reads the book is about to play
an opponent who is looking at the board.

The `--dubious` file lists them; its own header gives the format. A flagged move
is pushed behind every unflagged move in the order, in every bucket, so it can
never be the recommendation. Nothing else about it changes: its share, its win
rates and its score are untouched, and the opponent's half of the table is
untouched, because the field really does play it and a repertoire that hid it
would leave you unready for it.

It is deliberately not part of the ranking rule — an editorial exception kept
out of the rule lets each be read on its own — and it is not seen by the
evaluator, so a flagged move is still inside the value backed up for its parent.
The demotion is a serving-time correction.

## UI

Views sit behind a tab bar, one mounted at a time. A view owns its own state,
controls and layout and takes no props, so adding one is a file plus an entry in
the registry; the shell renders whichever the URL names and knows nothing else
about it. Views are mounted rather than hidden, so a background view does not
keep fetching.

Routing is flat, with the registry as the route table — few enough routes that a
library would be more configuration than code. `/`, an unknown path and a
trailing slash all rewrite to the canonical route of whatever is being shown, so
there is one URL per screen and a mistyped path cannot leave the app blank. The
tabs are anchors rather than buttons, so modified clicks stay with the browser.

The analysis board keeps its position, rating band and side in the query, so a
link is a position. Defaults are omitted. The move list is deliberately not in
the URL — a shared link opens its position as a fresh root — and the URL is
replaced rather than pushed as the board moves, so Back leaves the view instead
of walking a move at a time through everything that was tried.

Fetched positions are cached in the browser, bounded by bytes rather than
entries: payloads span three orders of magnitude, and one large position should
not evict many small ones. The cache assumes positions are immutable for a given
tree, which they are — restarting the server on different evaluations without
reloading the page would serve stale numbers.

Training plays a line out against the field: the tree replies as the crowd does,
weighted by how often each move was played in that bucket, and each of your
moves is graded against the tree's pick using the same ranking score the
analysis table sorts by. A move that falls short is held back rather than
played, and offered to you to keep as a deliberate deviation of your own; only
committing names the tree's pick, because printing the answer to a question you
are still looking at answers it for you. A reply the field hardly ever plays
raises a warning, since being asked to handle something rare is worth knowing.

### Debug surfaces

Maintenance UI is rendered only when the debug opt-in is on: load the app once
with `?debug=1`, or use the console handle. The flag is remembered locally and
the query parameter is stripped from the address bar as soon as it is read, so a
link copied out of the app does not carry the panels with it.

It is deliberately a runtime switch rather than a build-time one. What is
deployed is a production build, so a build-time flag would expose the debug UI
only in development — the one place it is least needed.
