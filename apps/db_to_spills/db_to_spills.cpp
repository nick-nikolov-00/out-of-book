#include "SPMCQueue.h"
#include "movegen.h"
#include "position.h"
#include "uci.h"

#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "ZstStreamer.h"
#include "AppMetrics.h"
#include "RecordCache.h"
#include "extractor.h"
#include "metrics.h"

constexpr size_t MIN_DEPTH_HALF_MOVES = 4;
constexpr size_t MAX_DEPTH_HALF_MOVES = 40;
const size_t WORKER_THREADS = 5;
constexpr size_t QUEUE_MAX = WORKER_THREADS * 2 + 10;
constexpr int LIMIT_BLOBS = 0;

static thread_local int myId = 0;

struct PgnBlob {
    std::shared_ptr<std::array<char, CHUNK_SIZE>> blob;
    std::shared_ptr<std::array<char, CHUNK_SIZE>> nextBlob;
};

void processMove(std::string_view rawMove, Stockfish::RatingBucket bucket, Stockfish::GameResult result,
                 std::array<Stockfish::StateInfo, MAX_DEPTH_HALF_MOVES> &states, Stockfish::Position &position,
                 int currentHalfMove, RecordCache &cache) {
    assert(currentHalfMove < states.size());
    Stockfish::Move move = Stockfish::getMove(rawMove, position);
    if (!move.is_ok()) {
        std::cerr << "Invalid move: " << rawMove << std::endl;
        assert(false);
    }

    cache.add(position.key(), move.raw(), bucket, result);
    position.do_move(move, states[currentHalfMove]);
}

void processSingleGame(AppMetrics& appMetrics, std::string_view game, RecordCache &cache) {
    GameMetadata metadata;

    // std::cout<<"starting metadata extraction:\n";
    bool filter = extractMetadata(game, metadata);

    // std::cout<<"for game: "<<game<<" "<<filter<<" "<<int(metadata.result)<<"\n";
    // std::cout<<"END OF THIS GAME\n\n\n";

    if (!filter) {
        ++appMetrics.get<GAMES_DISCARDED>();
        return;
    }

    auto bucket = Stockfish::ratingBucket((metadata.whiteElo + metadata.blackElo) / 2);
    auto result = metadata.result;

    Stockfish::Position pos;
    Stockfish::StateInfo st;
    pos.set(Stockfish::StartFEN, false, &st);
    std::array<Stockfish::StateInfo, MAX_DEPTH_HALF_MOVES> states;

    int halfmove = 0;
    std::string_view rawMoveBuffer[MIN_DEPTH_HALF_MOVES];
    forEachMove(metadata.moves, [&](std::string_view move) {
        if (halfmove < MIN_DEPTH_HALF_MOVES) {
            rawMoveBuffer[halfmove++] = move;
            return true;
        }

        if (halfmove == MIN_DEPTH_HALF_MOVES) {
            for (int i = 0; i < MIN_DEPTH_HALF_MOVES; ++i) {
                processMove(rawMoveBuffer[i], bucket, result, states, pos, i, cache);
            }
        }

        if (halfmove == MAX_DEPTH_HALF_MOVES)
            return false;

        processMove(move, bucket, result, states, pos, halfmove, cache);

        halfmove++;

        return true;
    });

    if (halfmove < MIN_DEPTH_HALF_MOVES) {
        ++appMetrics.get<GAMES_DISCARDED>();
    } else {
        cache.add(pos.key(), Stockfish::Move::termination().raw(), bucket, result);
    }

    // std::cout << std::endl << "GAME END\n";
}

void processBlob(AppMetrics& appMetrics, PgnBlob &&blobs, RecordCache &cache) {
    const std::string_view current(blobs.blob->data(), CHUNK_SIZE);

    constexpr std::string_view EVENT = "[Event";

    size_t gameStart = current.find(EVENT);
    if (gameStart == std::string_view::npos)
        return;

    while (true) {
        size_t nextEvent = current.find(EVENT, gameStart + 1);

        // Normal case: another game begins inside this blob.
        if (nextEvent != std::string_view::npos) {
            ++appMetrics.get<GAMES_SEEN>();
            processSingleGame(appMetrics, current.substr(gameStart, nextEvent - gameStart), cache);

            gameStart = nextEvent;
            continue;
        }

        // Last game in this blob.

        // No next blob -> this is the final game in the file.
        if (!blobs.nextBlob) {
            ++appMetrics.get<GAMES_SEEN>();
            processSingleGame(appMetrics, current.substr(gameStart), cache);
            break;
        }

        // Look for the first event in the next blob.
        std::string_view next(blobs.nextBlob->data(), CHUNK_SIZE);

        size_t firstEventInNext = next.find(EVENT);

        // Blob ended exactly between games.
        if (firstEventInNext == 0) {
            processSingleGame(appMetrics, current.substr(gameStart), cache);
        } else {
            // Game overflowed into next blob.
            std::string game(current.substr(gameStart));

            if (firstEventInNext != std::string_view::npos)
                game.append(next.data(), firstEventInNext);
            else
                game.append(next.data(), next.size());

            processSingleGame(appMetrics, game, cache);
        }
        ++appMetrics.get<GAMES_SEEN>();

        break;
    }
}

void processDB(AppMetrics& appMetrics, ZstStreamer &reader, const char *spillsFolder) {
    static std::array<std::array<char, CHUNK_SIZE>, QUEUE_MAX + 1> buffers;

    SPMCQueue<std::array<char, CHUNK_SIZE> *> freeChunkQueue(buffers.size());
    SPMCQueue<PgnBlob> blobs(buffers.size());

    for (auto &buff: buffers) {
        freeChunkQueue.push(&buff);
    }

    // Producer thread
    std::thread producer([&]() {
        std::shared_ptr<std::array<char, CHUNK_SIZE>> lastChunk;
        std::array<char, CHUNK_SIZE> *curr;
        size_t size = 0;

        int blobsPushed = 0;
        while (true) {
            freeChunkQueue.pop(curr);
            if (!reader.readChunk(*curr, size))
                break;

            appMetrics.maybePrint();

            std::shared_ptr<std::array<char, CHUNK_SIZE>> newChunk(
                    curr, [&](std::array<char, CHUNK_SIZE> *chunk) { freeChunkQueue.push(chunk); });

            if (!lastChunk) {
                lastChunk = newChunk;
                continue;
            }

            PgnBlob blob{lastChunk, newChunk};

            blobs.push(std::move(blob));

            if (LIMIT_BLOBS > 0 && blobsPushed++ >= LIMIT_BLOBS) {
                break;
            }

            lastChunk = newChunk;
        }

        blobs.setFinished();
    });

    std::vector<std::thread> workers;
    for (size_t i = 0; i < WORKER_THREADS; ++i) {
        workers.emplace_back([&, i]() {
            myId = i + 10;

            RecordCache recordCache(appMetrics, myId, spillsFolder);
            PgnBlob blob;
            while (blobs.pop(blob)) {
                processBlob(appMetrics, std::move(blob), recordCache);
            }
        });
    }

    producer.join();
    for (auto &t: workers)
        t.join();
}


int main(int argc, char **argv) {
    AppMetrics appMetrics(
            std::chrono::seconds(1),
            [](AppMetrics::Snapshot snapshot) {
                std::cout << "total read: " << snapshot.get<BYTES_READ>() / 1'000'000'000.0 << " gb "
                          << "read: " << snapshot.ratePerSecond<BYTES_READ>() / 1'000'000.0 << " MB/s "
                          << "decompression: " << snapshot.ratePerSecond<BYTES_DECOMPRESSED>() / 1'000'000 << " MB/s "
                          << "wrote: " << snapshot.ratePerSecond<BYTES_WRITTEN>() / 1'000'000 << " MB/s "
                          << "games seen: " << snapshot.ratePerSecond<GAMES_SEEN>() << " ps" << std::endl;
            },
            [](AppMetrics::Snapshot snapshot) {
                std::cout << "\n===== Final Metrics =====\n";
                std::cout << "Finished in: "
                          << std::chrono::duration_cast<std::chrono::seconds>(snapshot.getRuntime()).count()
                          << " seconds\n";
                std::cout << "read: " << snapshot.get<BYTES_READ>() / 1'000'000'000.0 << " gb\n"
                          << "decompressed: " << snapshot.get<BYTES_DECOMPRESSED>() / 1'000'000'000.0 << " gb\n"
                          << "wrote: " << snapshot.get<BYTES_WRITTEN>() / 1'000'000'000.0 << " gb\n"
                          << "games seen: " << snapshot.get<GAMES_SEEN>() << std::endl
                          << "game discarded: " << snapshot.get<GAMES_DISCARDED>() << std::endl;
            });

    if (argc != 3) {
        std::cerr << "Usage: db_to_spills <input.pgn> <spills_dir>\n";
        return 1;
    }

    Stockfish::Bitboards::init();
    Stockfish::Position::init();

    ZstStreamer streamer(appMetrics, argv[1]);

    processDB(appMetrics, streamer, argv[2]);

    return 0;
}
