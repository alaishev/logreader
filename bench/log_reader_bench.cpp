// Performance benchmarks of logreader::LogReader

#include "filter.h"
#include "log_reader.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// Placeholder: cost of creating and destroying a reader
void BM_ConstructDestruct(benchmark::State &state)
{
    for (auto _: state) {
        logreader::LogReader reader(std::make_unique<Filter>());
        benchmark::DoNotOptimize(&reader);
    }
}

BENCHMARK(BM_ConstructDestruct);

// Typical log lines: matchesFilter() is called for each of them
std::vector<std::string> makeLines()
{
    std::vector<std::string> lines;
    for (int i = 0; i < 1000; ++i) {
        lines.push_back("2026-10-14 12:" + std::to_string(10 + i % 50) + ":07.123 [worker-" + std::to_string(i % 16)
                        + "] INFO  order " + std::to_string(100000 + i) + (i % 10 == 0 ? " ERROR closed" : " opened"));
    }
    return lines;
}

// Throughput of matching: arg 0 - index of the mask, arg 1 - ignore case
void BM_Match(benchmark::State &state)
{
    static const char *const masks[] = {
      "*ERROR*",                      // literal
      "*order 1?????[02468] *",       // '?' and a class
      "*:[0-5][0-9]:*[!0-9] error*",  // several classes and negation
      "*INFO  order*d",               // literal in every line, fixed tail after the last '*'
      "2026-10-14 12:1?:*",           // fixed head, trailing '*'
    };
    const char *mask = masks[state.range(0)];

    Filter filter(state.range(1) != 0);
    if (!filter.parse(mask)) {
        state.SkipWithError("invalid mask");
        return;
    }

    const auto lines = makeLines();
    std::size_t bytes = 0;
    for (auto _: state) {
        for (const auto &line: lines) {
            benchmark::DoNotOptimize(filter.matchesFilter(line));
            bytes += line.size();
        }
    }
    state.SetBytesProcessed(static_cast<int64_t>(bytes));
    state.SetLabel(mask);
}

BENCHMARK(BM_Match)->ArgsProduct({{0, 1, 2, 3, 4}, {0, 1}});

// Size of the generated log file: much larger than the biggest block and than the CPU caches
constexpr std::size_t LOG_FILE_SIZE = 256 * 1024 * 1024;

// Log file for the reading benchmarks: created on first use, removed at program exit.
// It stays in the page cache, so the benchmarks measure the CPU and memory side of reading, not the disk.
class TempLogFile
{
public:
    TempLogFile()
        : path(std::filesystem::temp_directory_path()
               / ("logreader_bench_" + std::to_string(std::random_device{}()) + ".log"))
    {
        std::ofstream out(path, std::ios::binary);
        std::string chunk;
        std::size_t size = 0;
        for (int i = 0; size < LOG_FILE_SIZE; ++i) {
            chunk = "2026-10-14 12:" + std::to_string(10 + i % 50) + ":07.123 [worker-" + std::to_string(i % 16)
                  + "] INFO  order " + std::to_string(100000 + i)
                  + (i % 10 == 0 ? " ERROR closed\n" : " opened " + std::string(i % 81, 'x') + "\n");
            out << chunk;
            size += chunk.size();
        }
    }

    ~TempLogFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempLogFile(const TempLogFile &) = delete;
    TempLogFile &operator=(const TempLogFile &) = delete;

    std::filesystem::path path;
};

const std::string &logFilePath()
{
    static const TempLogFile file;
    static const std::string path = file.path.string();
    return path;
}

// Reading the whole file with different block sizes: arg 0 - block size in bytes, arg 1 - index of the mask.
// "?" has no literal for the prefilter and rejects every line at once (the lines are longer than one byte),
// so it measures reading and splitting into lines only.
void BM_BlockSize(benchmark::State &state)
{
    static const char *const masks[] = {
      "?",        // reading and splitting only
      "*ERROR*",  // typical search: 10% of the lines match
    };
    const char *mask = masks[state.range(1)];
    const std::string &path = logFilePath();

    char buf[8192];
    std::size_t matched = 0;
    for (auto _: state) {
        logreader::LogReader reader(std::make_unique<Filter>(), static_cast<std::size_t>(state.range(0)));
        if (!reader.open(path) || !reader.setFilter(mask)) {
            state.SkipWithError("cannot open the log file");
            return;
        }
        matched = 0;
        while (reader.getNextLine(buf, sizeof buf)) {
            ++matched;
        }
        benchmark::DoNotOptimize(matched);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * LOG_FILE_SIZE));
    state.counters["matched"] = static_cast<double>(matched);
    state.SetLabel(mask);
}

BENCHMARK(BM_BlockSize)
  ->ArgsProduct({{64 << 10, 256 << 10, 1 << 20, 4 << 20, 16 << 20, 64 << 20}, {0, 1}})
  ->Unit(benchmark::kMillisecond)
  ->UseRealTime();

}  // namespace
