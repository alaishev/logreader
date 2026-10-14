#include "log_reader.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace logreader {

namespace {

// Wrappers only where libstdc++ has no fast equivalent: string_view::rfind(char) is a byte loop
// (memrchr is SIMD, x16 on a 100-byte line), string_view::find(string_view) is memchr of the first
// char + compare, not memmem. string_view::find(char) already calls memchr, so it is used as is.

// memrchr() for string_view: position of the last byte c in where, npos if there is none
std::size_t rfindByte(std::string_view where, char c)
{
    const void *p = memrchr(where.data(), c, where.size());
    return (p == nullptr) ? std::string_view::npos
                          : static_cast<std::size_t>(static_cast<const char *>(p) - where.data());
}

// memmem() for string_view: position of the first occurrence of the byte sequence what in where,
// npos if there is none
std::size_t findSequence(std::string_view where, std::string_view what)
{
    const void *p = memmem(where.data(), where.size(), what.data(), what.size());
    return (p == nullptr) ? std::string_view::npos
                          : static_cast<std::size_t>(static_cast<const char *>(p) - where.data());
}

}

LogReader::LogReader(std::unique_ptr<IFilter> filter_, std::size_t maxBlockSize) noexcept
    : blockSize(std::clamp(maxBlockSize, MIN_BLOCK_SIZE, MAX_BLOCK_SIZE))
    , filter{std::move(filter_)}
{
}

LogReader::~LogReader() noexcept
{
    close();
}

bool LogReader::open(const std::string &fileName) noexcept
{
    namespace fs = std::filesystem;

    close();

    // fs::path, the block and std::ifstream allocate memory: any failure leaves the reader closed
    try {
        std::error_code ec;
        const fs::path path(fileName);

        if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
            return false;
        }

        blockBuf.resize(blockSize);
        logFile.open(path, std::ios::binary);

        return logFile.is_open();
    }
    catch (...) {
        close();
        return false;
    }
}

void LogReader::close() noexcept
{
    resetPosition();
    logFile.close();
}

std::string_view LogReader::pending() const
{
    return {blockBuf.data() + dataBegin, dataEnd - dataBegin};
}

void LogReader::resetPosition()
{
    dataBegin = 0;
    dataEnd = 0;
    eof = false;
    truncatedLine = false;
}

// Moves the unprocessed tail to the start of the block and appends the next portion of the file after it.
// Returns false if nothing was read (EOF, read error or no free space).
bool LogReader::readBlock()
{
    if (eof || !logFile.is_open()) {
        return false;
    }

    const std::size_t count = dataEnd - dataBegin;
    if (dataBegin > 0) {
        std::memmove(blockBuf.data(), blockBuf.data() + dataBegin, count);
        dataBegin = 0;
        dataEnd = count;
    }

    const std::size_t space = blockBuf.size() - dataEnd;
    if (space == 0) {
        return false;
    }

    logFile.read(blockBuf.data() + dataEnd, static_cast<std::streamsize>(space));
    const auto bytes = static_cast<std::size_t>(logFile.gcount());
    dataEnd += bytes;

    // read() sets failbit together with eofbit on a short read; badbit means an I/O error
    if (logFile.eof() || logFile.bad()) {
        eof = true;
    }

    return bytes > 0;
}

// Drops the rest of the truncated line up to '\n' inclusive, reading blocks as needed.
// Returns false if the file ended before '\n'.
bool LogReader::dropTruncatedRest()
{
    for (;;) {
        if (const std::size_t nl = pending().find('\n'); nl != std::string_view::npos) {
            dataBegin += nl + 1;
            truncatedLine = false;
            return true;
        }
        dataBegin = dataEnd;
        if (!readBlock()) {
            return false;
        }
    }
}

// Returns the next line of the file without '\n', reading blocks as needed; nothing at the end of the file.
// The view points into blockBuf and is valid until the next call. Trailing '\r' is removed.
// A line longer than the block is returned truncated to the block size, the rest of it up to '\n'
// is dropped on the next call.
std::optional<std::string_view> LogReader::nextRawLine()
{
    // If line is truncated - read blocks until first `\n`
    if (truncatedLine && !dropTruncatedRest()) {
        return std::nullopt;
    }

    // Then try to find new line in the buffer
    std::string_view rest = pending();
    std::size_t nl = rest.find('\n');

    // We didn`t find new line - readBlock(): the tail of the line is moved to the start of the block
    // and the file is read after it, so the block is either full or contains the end of the file
    if ((nl == std::string_view::npos) && readBlock()) {
        rest = pending();
        nl = rest.find('\n');
    }

    // Nothing left and nothing read: end of file
    if ((nl == std::string_view::npos) && rest.empty()) {
        return std::nullopt;
    }

    std::string_view line;
    if (nl != std::string_view::npos) {
        // We found new line - return it
        line = rest.substr(0, nl);
        dataBegin += nl + 1;
    }
    else {
        // No '\n' in the rest: the line fills the whole block (truncated) or it is the last line of the file
        line = rest;
        dataBegin = dataEnd;
        truncatedLine = (line.size() == blockBuf.size());
    }

    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }

    return line;
}

// Prefilter: skips the complete lines of the block that do not contain the literal, so they are neither
// split into lines nor passed to matchesFilter(). One memmem() over the block is many times faster than
// matching every line. Stops at the start of the first line with the literal, if there is one.
// The last line of the block without '\n' and the rest of a truncated line are left to nextRawLine().
// A line that loses its trailing '\r' still does not contain the literal, so no matching line is skipped.
void LogReader::skipLinesWithout(std::string_view literal)
{
    if (truncatedLine) {
        return;
    }

    // Window of complete lines: from dataBegin (always the start of a line here) up to the last '\n'
    const std::string_view rest = pending();
    const std::size_t lastNl = rfindByte(rest, '\n');
    if (lastNl == std::string_view::npos) {
        return;
    }

    const std::size_t hit = findSequence(rest.substr(0, lastNl), literal);
    if (hit == std::string_view::npos) {
        dataBegin += lastNl + 1;  // no line of the window can match
        return;
    }

    // Move to the start of the line with the literal: right after the previous '\n'
    if (const std::size_t nl = rfindByte(rest.substr(0, hit), '\n'); nl != std::string_view::npos) {
        dataBegin += nl + 1;
    }
}

// Without a filter implementation (nullptr passed to the constructor) any mask is rejected
bool LogReader::setFilter(const std::string &str) noexcept
{
    if (!filter) {
        return false;
    }

    return filter->parse(str);
}

// A line longer than the block is matched by its first block only, so masks checking the line end
// (abc, *abc, abc?) are applied to the end of that part
bool LogReader::getNextLine(char *buf, std::size_t bufsize) noexcept
{
    if (buf == nullptr || bufsize == 0) {
        return false;
    }

    // Without a filter implementation or a valid mask nothing can be matched: reported as an error.
    // Checked once per call, not per line: the mask does not change inside the loop
    if (!logFile.is_open() || !filter || !filter->isValid()) {
        buf[0] = '\0';
        return false;
    }

    const std::string_view literal = filter->longestLiteral();

    for (;;) {
        if (!literal.empty()) {
            skipLinesWithout(literal);
        }

        const auto line = nextRawLine();
        if (!line) {
            break;
        }
        if (!filter->matchesFilter(*line)) {
            continue;  // no match
        }

        // Copy as much as fits, the result is always a C-string
        const std::size_t n = line->copy(buf, bufsize - 1);
        buf[n] = '\0';
        return true;
    }

    buf[0] = '\0';
    return false;
}

}