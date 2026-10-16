// log_reader.h
#pragma once

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ifilter.h>

namespace logreader {

// Default block size — 1 MB
// Big enough to reduce syscalls and small enough to fit L2/L3 cache
inline constexpr std::size_t DEFAULT_BLOCK_SIZE = 1 * 1024 * 1024;

// Limits of the block size: requested values outside of them are clamped by the constructor
inline constexpr std::size_t MIN_BLOCK_SIZE = 4 * 1024;           // 4 KB — one memory page
inline constexpr std::size_t MAX_BLOCK_SIZE = 256 * 1024 * 1024;  // 256 MB — sane upper bound of memory use

// Reads a text log file block by block and returns lines matching the filter.
// Not intended for inheritance or extension. Not thread-safe: an instance is used by one thread at a time.
// No method throws: all failures are reported by return values.
class LogReader final
{
public:
    explicit LogReader(std::unique_ptr<IFilter> filter, std::size_t maxBlockSize = DEFAULT_BLOCK_SIZE) noexcept;
    ~LogReader() noexcept;

    // Owns an open file and a position in it, so it is neither copied nor moved
    LogReader(const LogReader &) = delete;
    LogReader &operator=(const LogReader &) = delete;

    bool open(const std::string &fileName) noexcept;  // open file, false - error
    void close() noexcept;                            // close file

    bool setFilter(const std::string &filter) noexcept;  // set filter of strings, false - error (invalid filter)
    bool getNextLine(char *buf,
                     std::size_t bufsize) noexcept;  // request of every matched line, false - end of file or error

private:
    bool readBlock();                                 // append next portion of the file to the block
    std::optional<std::string_view> nextLine();       // next line of the file, without filtering
    bool dropTruncatedRest();                         // skip the rest of a long line up to '\n'
    void skipLinesWithout(std::string_view literal);  // prefilter: skip complete lines without the literal
    void resetPosition();                             // drop block content and EOF state
    std::string_view pending() const;                 // unprocessed data of the block: [dataBegin, dataEnd)

    const std::size_t blockSize;
    std::ifstream logFile;
    std::vector<char> blockBuf;

    // Position of unprocessed data in blockBuf
    std::size_t dataBegin = 0;
    std::size_t dataEnd = 0;
    std::size_t lastNl = std::string_view::npos;  // last '\n' of the read data, from the start of blockBuf

    bool eof = false;
    bool truncatedLine = false;  // current line is longer than the block, its rest will be dropped

    // Implementation of filter which used for matching
    std::unique_ptr<IFilter> filter;
};

}