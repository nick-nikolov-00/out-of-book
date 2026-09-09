#include "FloatBits.h"
#include "common.h"
#include "types.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

constexpr size_t N_BUCKETS = Stockfish::NUMBER_OF_BUCKETS;
constexpr uint32_t TARGET_MIN = 200;
constexpr uint32_t N_DRIFT = 500;
constexpr double S_CAP = 2000;
constexpr double W_GRID[] = {0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.40, 0.50, 0.60, 0.75, 0.90};
constexpr size_t G = std::size(W_GRID);

struct NbrReport {
  double sse[N_BUCKETS][G]; // sum err^2 predicting bucket b at W_GRID[g];
  uint64_t cells[N_BUCKETS];
  double bestW[N_BUCKETS];

  double sseD[N_BUCKETS][G];
  double noiseD[N_BUCKETS];
  uint64_t cellsD[N_BUCKETS];
  double sigma2[N_BUCKETS];
  double drift2[N_BUCKETS];
  double S[N_BUCKETS];
};

NbrReport calibratePriors(std::ifstream& otbFile, size_t indexOffset) {
  double PW[G][N_BUCKETS];
  for (size_t g = 0; g < G; ++g) {
    for (int d = 0; d < N_BUCKETS; ++d) {
      PW[g][d] = std::pow(W_GRID[g], d);
    }
  }

  NbrReport report{};
  double s2num[N_BUCKETS]{};
  double s2den[N_BUCKETS]{};
  uint64_t nodesRead = 0;
  while (std::streamoff(otbFile.tellg()) < std::streamoff(indexOffset)) {
    alignas(NodeBlobHeader) char buff[maxBlobSize];
    auto& blobHeader = *reinterpret_cast<NodeBlobHeader*>(buff);
    otbFile.read(reinterpret_cast<char*>(&blobHeader), sizeof(NodeBlobHeader));
    if (!otbFile)
      throw std::runtime_error("truncated OTB: short read on a node blob header");

    if (blobHeader.nEntries > NodeBlobHeader::MAX_CHILDREN)
      throw std::runtime_error("OTB node blob claims " + std::to_string(blobHeader.nEntries) +
                               " entries, over the " +
                               std::to_string(NodeBlobHeader::MAX_CHILDREN) + " cap");

    otbFile.read(reinterpret_cast<char*>(&blobHeader) + sizeof(NodeBlobHeader),
                 sizeof(NodeBlobEntry) * blobHeader.nEntries);
    if (!otbFile)
      throw std::runtime_error("truncated OTB: short read on a node blob body");

    if (++nodesRead % 2'000'000 == 0)
      std::cerr << "  scanned " << nodesRead << " nodes ("
                << 100.0 * double(std::streamoff(otbFile.tellg())) / double(indexOffset) << "%)\n";

    double cnt[N_BUCKETS]{};
    double val[N_BUCKETS]{};
    double var[N_BUCKETS]{};

    for (int i = 0; i < blobHeader.nEntries; ++i) {
      auto& entry = blobHeader.entries[i];

      assert(entry.bucket < N_BUCKETS);
      assert(entry.white_wins + entry.draws <= entry.count);

      double v = (entry.white_wins + 0.5 * entry.draws) / double(entry.count);
      double pW = double(entry.white_wins) / entry.count;
      double pD = double(entry.draws) / entry.count;

      cnt[entry.bucket] += entry.count;
      val[entry.bucket] += entry.count * v;
      var[entry.bucket] += entry.count * (pW + 0.25 * pD - v * v);
    }

    for (int b = 0; b < N_BUCKETS; ++b) {
      s2num[b] += var[b];
      s2den[b] += cnt[b];
    }

    for (int b = 0; b < N_BUCKETS; ++b) {
      if (cnt[b] < TARGET_MIN) {
        continue;
      }

      double target = val[b] / cnt[b];
      bool driftCell = cnt[b] >= N_DRIFT;
      double err2[G];
      bool ok = true;

      for (size_t g = 0; g < G && ok; ++g) {
        double num = 0, den = 0;
        for (int a = 0; a < N_BUCKETS; ++a) {
          if (a == b)
            continue;
          num += PW[g][std::abs(a - b)] * val[a];
          den += PW[g][std::abs(a - b)] * cnt[a];
        }
        ok = den > 0;
        if (ok) {
          double e = num / den - target;
          err2[g] = e * e;
        }
      }
      if (!ok)
        continue;
      for (size_t g = 0; g < G; ++g)
        report.sse[b][g] += err2[g];
      report.cells[b]++;
      if (driftCell) {
        for (size_t g = 0; g < G; ++g)
          report.sseD[b][g] += err2[g];

        report.noiseD[b] += (var[b] / cnt[b]) / cnt[b];
        report.cellsD[b]++;
      }
    }
  }
  assert(otbFile.tellg() == indexOffset);

  for (int b = 0; b < N_BUCKETS; ++b) {
    size_t best = 0;
    for (size_t g = 1; g < G; ++g) {
      if (report.sse[b][g] < report.sse[b][best])
        best = g;
    }
    report.bestW[b] = W_GRID[best];
    report.sigma2[b] = s2den[b] > 0 ? s2num[b] / s2den[b] : 0.25;
    if (report.cellsD[b] == 0) {
      report.S[b] = NAN;
      continue;
    }
    report.drift2[b] =
        std::max(0.0, (report.sseD[b][best] - report.noiseD[b]) / double(report.cellsD[b]));
    report.S[b] =
        report.drift2[b] > 0 ? std::min(S_CAP, report.sigma2[b] / report.drift2[b]) : S_CAP;
  }

  return report;
}


/*
 * The two numbers this exists to produce are expectimax's NEIGHBOUR_WEIGHT and
 * S_PRIOR. W is read straight off the grid: for each bucket, how steeply to
 * discount a neighbouring bucket's games when predicting this one. S is the
 * empirical-Bayes pseudo-count sigma2 / drift2 -- the sampling variance of a
 * single game over the variance the prior itself still carries -- which is the
 * n at which a cell's own evidence is worth as much as the prior.
 *
 * expectimax applies one W and one S to every bucket, so the pooled lines at
 * the bottom are what actually gets transplanted. The per bucket rows are
 * there to show whether pooling is throwing much away.
 */
void printReport(const NbrReport& report) {
  std::cout << "\nneighbour prior fit, RMSE of predicting a bucket from the others\n\n";
  std::cout << "bucket  cells    ";
  for (size_t g = 0; g < G; ++g)
    std::cout << std::setw(7) << W_GRID[g];
  std::cout << "   bestW\n";

  for (size_t b = 0; b < N_BUCKETS; ++b) {
    std::cout << std::setw(6) << b << std::setw(9) << report.cells[b] << "    ";
    for (size_t g = 0; g < G; ++g) {
      double rmse =
          report.cells[b] > 0 ? std::sqrt(report.sse[b][g] / double(report.cells[b])) : NAN;
      std::cout << std::setw(7) << std::fixed << std::setprecision(4) << rmse;
    }
    std::cout << std::setw(8) << std::setprecision(2) << report.bestW[b] << "\n";
  }

  std::cout << "\nshrinkage strength\n\n";
  std::cout << "bucket  driftCells     sigma2     drift2          S\n";
  for (size_t b = 0; b < N_BUCKETS; ++b) {
    std::cout << std::setw(6) << b << std::setw(12) << report.cellsD[b] << std::setw(11)
              << std::setprecision(5) << report.sigma2[b] << std::setw(11) << report.drift2[b]
              << std::setw(11) << std::setprecision(1) << report.S[b] << "\n";
  }

  /* Equal weight per bucket rather than per cell, so the densely populated
   * middle buckets do not decide W on their own. */
  size_t pooledBest = 0;
  double pooledMse[G]{};
  for (size_t g = 0; g < G; ++g) {
    size_t used = 0;
    for (size_t b = 0; b < N_BUCKETS; ++b) {
      if (report.cells[b] == 0)
        continue;
      pooledMse[g] += report.sse[b][g] / double(report.cells[b]);
      ++used;
    }
    if (used > 0)
      pooledMse[g] /= double(used);
    if (pooledMse[g] < pooledMse[pooledBest])
      pooledBest = g;
  }

  std::vector<double> finiteS;
  for (size_t b = 0; b < N_BUCKETS; ++b)
    if (floatbits::isFinite(report.S[b]))
      finiteS.push_back(report.S[b]);
  std::sort(finiteS.begin(), finiteS.end());

  std::cout << "\npooled\n\n";
  std::cout << "  NEIGHBOUR_WEIGHT = " << std::setprecision(2) << W_GRID[pooledBest]
            << "   (mean per bucket RMSE " << std::setprecision(4)
            << std::sqrt(pooledMse[pooledBest]) << ")\n";
  if (finiteS.empty()) {
    std::cout << "  S_PRIOR          = no bucket had enough drift cells to estimate it\n";
  } else {
    std::cout << "  S_PRIOR          = " << std::setprecision(0)
              << finiteS[finiteS.size() / 2] << "   (median over " << finiteS.size()
              << " buckets, range " << finiteS.front() << " to " << finiteS.back() << ")\n";
  }
  std::cout << std::endl;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: calibration <data.otb>\n";
    return 1;
  }

  const auto otbFileName = std::string(argv[1]);

  std::ifstream otbFile(otbFileName, std::ios::binary);

  if (!otbFile)
    throw std::runtime_error("cannot open OTB file: " + otbFileName);

  OtbHeader otbHeader;
  otbFile.read(reinterpret_cast<char*>(&otbHeader), sizeof(otbHeader));

  if (!otbFile)
    throw std::runtime_error("cannot read OTB header");

  const auto report = calibratePriors(otbFile, otbHeader.indexOffset);

  printReport(report);

  return 0;
}