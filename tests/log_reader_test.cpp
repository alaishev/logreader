// Unit tests of logreader::LogReader

#include "filter.h"
#include "log_reader.h"

#ifndef _WIN32
#include <unistd.h>  // geteuid()
#endif

#include <gtest/gtest.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Filter that accepts every line: reading tests check line splitting, not mask matching
class MatchAllFilter final: public IFilter
{
public:
    bool parse(std::string_view) override { return true; }

    bool isValid() const override { return true; }

    bool matchesFilter(std::string_view) override { return true; }
};

// Creates a reader that returns all lines of the file (guaranteed copy elision: LogReader is not movable)
logreader::LogReader makeReader(std::size_t blockSize = logreader::DEFAULT_BLOCK_SIZE)
{
    return logreader::LogReader(std::make_unique<MatchAllFilter>(), blockSize);
}

// Returns true if the current process holds an open descriptor for the given file (Linux, via /proc/self/fd)
bool isOpenedByProcess(const fs::path &path)
{
    std::error_code ec;
    const fs::path target = fs::canonical(path, ec);
    if (ec) {
        return false;
    }
    for (const auto &entry: fs::directory_iterator("/proc/self/fd", ec)) {
        std::error_code linkEc;
        if (fs::read_symlink(entry.path(), linkEc) == target) {
            return true;
        }
    }
    return false;
}

// Fixture: creates a unique temporary directory for test files and removes it afterwards
class LogReaderOpenTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path()
            / (std::string("logreader_test_") + info->name() + "_" + std::to_string(std::random_device{}()));
        fs::remove_all(dir);
        ASSERT_TRUE(fs::create_directories(dir));
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(dir, ec);
    }

    // Creates a file with the given content inside the temporary directory, returns its path
    fs::path makeFile(const std::string &name, const std::string &content = "line1\nline2\n")
    {
        const fs::path path = dir / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    fs::path dir;  // temporary directory of the current test
};

// Any block size is clamped by the constructor, so the reader stays usable; close() without open() is a no-op
TEST_F(LogReaderOpenTest, Construct)
{
    const std::string path = makeFile("log.txt").string();

    for (const std::size_t blockSize: {std::size_t{0}, std::size_t{1}, logreader::DEFAULT_BLOCK_SIZE, SIZE_MAX}) {
        SCOPED_TRACE("blockSize = " + std::to_string(blockSize));
        auto reader = makeReader(blockSize);
        reader.close();
        reader.close();
        EXPECT_TRUE(reader.open(path));
    }
}

// Files that must be opened successfully
TEST_F(LogReaderOpenTest, OpenValidFiles)
{
    const fs::path link = dir / "link.txt";
    fs::create_symlink(makeFile("target.txt"), link);

    const fs::path files[] = {
      makeFile("log.txt"),
      makeFile("empty.txt", ""),
      makeFile("лог файл.txt"),  // spaces and non-ASCII name
      link,                      // symlink to a regular file
    };

    auto reader = makeReader();
    for (const auto &file: files) {
        EXPECT_TRUE(reader.open(file.string())) << file;
    }
}

// Paths that must be rejected; on Linux std::ifstream can "open" a directory, so it is checked explicitly
TEST_F(LogReaderOpenTest, OpenInvalidPathsFails)
{
    const fs::path broken = dir / "broken.txt";
    fs::create_symlink(dir / "missing.txt", broken);

    const fs::path paths[] = {
      dir / "missing.txt",  // non-existent file
      "",                   // empty name
      dir,                  // directory
      "/dev/null",          // character device
      broken,               // broken symlink
    };

    auto reader = makeReader();
    for (const auto &path: paths) {
        EXPECT_FALSE(reader.open(path.string())) << path;
    }
}

TEST_F(LogReaderOpenTest, OpenWithoutPermissionsFails)
{
#ifdef _WIN32
    GTEST_SKIP() << "Windows rights are ACLs: fs::permissions() sets only the read-only attribute";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root ignores file permissions";
    }
#endif
    const fs::path noRead = makeFile("noread.txt");
    fs::permissions(noRead, fs::perms::none);

    const fs::path subdir = dir / "closed";
    fs::create_directory(subdir);
    const fs::path hidden = subdir / "log.txt";
    std::ofstream(hidden) << "line\n";
    fs::permissions(subdir, fs::perms::none);

    auto reader = makeReader();
    EXPECT_FALSE(reader.open(noRead.string()));
    EXPECT_FALSE(reader.open(hidden.string()));

    fs::permissions(subdir, fs::perms::owner_all);  // let TearDown() remove the directory
}

// Sequences of open()/close() calls in any order keep the reader usable
TEST_F(LogReaderOpenTest, OpenCloseSequences)
{
    const fs::path a = makeFile("a.txt");
    const fs::path b = makeFile("b.txt");
    const std::string missing = (dir / "missing.txt").string();

    auto reader = makeReader();
    EXPECT_TRUE(reader.open(a.string()));  // reopen the same file
    EXPECT_TRUE(reader.open(a.string()));
    EXPECT_TRUE(reader.open(b.string()));  // reopen another file
    reader.close();                        // open after close
    reader.close();
    EXPECT_TRUE(reader.open(a.string()));
    EXPECT_FALSE(reader.open(missing));  // open after failed open
    EXPECT_TRUE(reader.open(a.string()));

    // The opened file may be removed from the file system while the reader holds it (POSIX).
    // Windows does not remove an open file, there it is removed after close()
    std::error_code ec;
    fs::remove(a, ec);
    reader.close();
    fs::remove(a, ec);
    EXPECT_FALSE(reader.open(a.string()));
}

// The file descriptor is released by close(), by the next open() (successful or not) and by the destructor
TEST_F(LogReaderOpenTest, FileIsReleased)
{
#ifndef __linux__
    GTEST_SKIP() << "open descriptors are listed via /proc/self/fd, Linux only";
#endif
    const fs::path a = makeFile("a.txt");
    const fs::path b = makeFile("b.txt");

    {
        auto reader = makeReader();
        ASSERT_TRUE(reader.open(a.string()));
        ASSERT_TRUE(isOpenedByProcess(a));

        ASSERT_TRUE(reader.open(b.string()));
        EXPECT_FALSE(isOpenedByProcess(a)) << "successful open() keeps the previous file";
        EXPECT_TRUE(isOpenedByProcess(b));

        ASSERT_FALSE(reader.open((dir / "missing.txt").string()));
        EXPECT_FALSE(isOpenedByProcess(b)) << "failed open() keeps the previous file";

        ASSERT_TRUE(reader.open(a.string()));
        reader.close();
        EXPECT_FALSE(isOpenedByProcess(a)) << "close() keeps the file";

        ASSERT_TRUE(reader.open(a.string()));
    }
    EXPECT_FALSE(isOpenedByProcess(a)) << "destructor keeps the file";
}

// Many open() calls without close() must not leak file descriptors
TEST_F(LogReaderOpenTest, ManyOpenCalls)
{
    const std::string path = makeFile("log.txt").string();

    auto reader = makeReader();
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(reader.open(path)) << "iteration " << i;
    }
}

// Reading tests use the same temporary directory fixture
using LogReaderReadTest = LogReaderOpenTest;

// Reads all lines with the given output buffer size
std::vector<std::string> readAll(logreader::LogReader &reader, std::size_t bufsize = 64 * 1024)
{
    std::vector<char> buf(bufsize);
    std::vector<std::string> lines;
    while (reader.getNextLine(buf.data(), buf.size())) {
        lines.emplace_back(buf.data());
    }
    return lines;
}

// Line splitting: empty lines, missing trailing '\n', CRLF
TEST_F(LogReaderReadTest, ReadLines)
{
    struct Case {
        std::string content;                // file content
        std::vector<std::string> expected;  // lines returned by the reader
    };

    const Case cases[] = {
      {"", {}},
      {"\n", {""}},
      {"\n\n", {"", ""}},
      {"a\nb\n", {"a", "b"}},
      {"a\nb", {"a", "b"}},        // no '\n' at the end
      {"a\r\nb\r\n", {"a", "b"}},  // CRLF
      {"a\r\nb\r", {"a", "b"}},    // CR at the end of the file
      {"a\rb\n", {"a\rb"}},        // CR inside the line is kept
      {"\r\n", {""}},
      {"  a  \n\tb\n", {"  a  ", "\tb"}},  // spaces are kept
    };

    auto reader = makeReader();
    for (const auto &c: cases) {
        SCOPED_TRACE(::testing::PrintToString(c.content));
        ASSERT_TRUE(reader.open(makeFile("log.txt", c.content).string()));
        EXPECT_EQ(readAll(reader), c.expected);
    }
}

// Lines that do not fit the block: the first block is returned, the rest up to '\n' is dropped
TEST_F(LogReaderReadTest, ReadLongLines)
{
    constexpr std::size_t block = logreader::MIN_BLOCK_SIZE;
    const std::string fits(block - 1, 'f');  // with '\n' fills the block exactly
    const std::string full(block, 'x');      // the block without '\n'
    const std::string longer(block + 100, 'y');
    const std::string huge(3 * block + 7, 'z');  // spans several blocks

    struct Case {
        std::string name;                   // case description
        std::string content;                // file content
        std::vector<std::string> expected;  // lines returned by the reader
    };

    const Case cases[] = {
      {"fits", fits + "\n" + fits + "\n", {fits, fits}},
      {"exactly block", full + "\nb\n", {full, "b"}},
      {"longer", longer + "\nb\n", {longer.substr(0, block), "b"}},
      {"huge", huge + "\nb\n", {huge.substr(0, block), "b"}},
      {"after short line", "a\n" + longer + "\nb\n", {"a", longer.substr(0, block), "b"}},
      {"last without '\\n'", "a\n" + huge, {"a", huge.substr(0, block)}},
      {"two in a row", longer + "\n" + huge + "\n", {longer.substr(0, block), huge.substr(0, block)}},
    };

    auto reader = makeReader(block);
    for (const auto &c: cases) {
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(reader.open(makeFile("log.txt", c.content).string()));
        EXPECT_EQ(readAll(reader), c.expected);
    }
}

// Many lines crossing block boundaries at different offsets come in order and without losses
TEST_F(LogReaderReadTest, ReadManyLines)
{
    std::string content;
    std::vector<std::string> expected;
    for (int i = 0; i < 100000; ++i) {
        expected.push_back("line " + std::to_string(i) + std::string(static_cast<std::size_t>(i % 37), '.'));
        content += expected.back() + "\n";
    }

    auto reader = makeReader(logreader::MIN_BLOCK_SIZE);
    ASSERT_TRUE(reader.open(makeFile("log.txt", content).string()));
    EXPECT_EQ(readAll(reader), expected);
}

// A line longer than the output buffer is truncated, the next call returns the next line
TEST_F(LogReaderReadTest, OutputBufferLimits)
{
    auto reader = makeReader();
    ASSERT_TRUE(reader.open(makeFile("log.txt", "abcdef\nxy\n").string()));
    EXPECT_EQ(readAll(reader, 4), (std::vector<std::string>{"abc", "xy"}));

    ASSERT_TRUE(reader.open(makeFile("log.txt", "abcdef\nxy\n").string()));
    EXPECT_EQ(readAll(reader, 1), (std::vector<std::string>{"", ""}));

    // Invalid buffer does not consume lines
    char buf[16];
    ASSERT_TRUE(reader.open(makeFile("log.txt", "abcdef\n").string()));
    EXPECT_FALSE(reader.getNextLine(nullptr, sizeof buf));
    EXPECT_FALSE(reader.getNextLine(buf, 0));
    ASSERT_TRUE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "abcdef");
}

// '\0' inside a line does not break line splitting, the bytes are copied as is
TEST_F(LogReaderReadTest, NulInsideLine)
{
    auto reader = makeReader();
    ASSERT_TRUE(reader.open(makeFile("log.txt", std::string("a\0b\nc\n", 6)).string()));

    char buf[16];
    ASSERT_TRUE(reader.getNextLine(buf, sizeof buf));
    EXPECT_EQ(std::string(buf, 3), std::string("a\0b", 3));
    ASSERT_TRUE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "c");
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
}

// getNextLine() without an open file, after EOF and after close() returns false and an empty string
TEST_F(LogReaderReadTest, ReadStates)
{
    char buf[16] = "garbage";
    auto reader = makeReader();
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "");

    const std::string path = makeFile("log.txt", "a\nb\n").string();
    ASSERT_TRUE(reader.open(path));
    ASSERT_TRUE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "a");

    // Reopen starts from the beginning
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(readAll(reader), (std::vector<std::string>{"a", "b"}));
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));

    ASSERT_TRUE(reader.open(path));
    reader.close();
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));

    // Failed open() after a successful one leaves nothing to read
    ASSERT_TRUE(reader.open(path));
    ASSERT_FALSE(reader.open((dir / "missing.txt").string()));
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
}

// Without a filter implementation the reader does not crash: any mask is rejected and nothing is read
TEST_F(LogReaderReadTest, NullFilter)
{
    logreader::LogReader reader(nullptr);
    EXPECT_FALSE(reader.setFilter("*"));
    ASSERT_TRUE(reader.open(makeFile("log.txt").string()));

    char buf[16] = "garbage";
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "");
    reader.close();
}

// The reader with the real Filter returns only matching lines; CRLF is removed before matching
TEST_F(LogReaderReadTest, ReadWithFilter)
{
    logreader::LogReader reader(std::make_unique<Filter>());
    const std::string path = makeFile("log.txt", "order 1 closed\norder 2 open\r\nfoo\norder 3 closed\r\n").string();

    EXPECT_FALSE(reader.setFilter(""));

    ASSERT_TRUE(reader.setFilter("order*closed"));
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(readAll(reader), (std::vector<std::string>{"order 1 closed", "order 3 closed"}));

    ASSERT_TRUE(reader.setFilter("*open"));
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(readAll(reader), (std::vector<std::string>{"order 2 open"}));

    ASSERT_TRUE(reader.setFilter("bar"));
    ASSERT_TRUE(reader.open(path));
    EXPECT_TRUE(readAll(reader).empty());
}

// After a failed setFilter() the previous mask is not used and nothing is read, even empty lines
TEST_F(LogReaderReadTest, ReadWithInvalidFilter)
{
    logreader::LogReader reader(std::make_unique<Filter>());
    const std::string path = makeFile("log.txt", "a\n\nb\n").string();

    ASSERT_TRUE(reader.setFilter("*"));
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(readAll(reader), (std::vector<std::string>{"a", "", "b"}));

    char buf[16] = "garbage";
    EXPECT_FALSE(reader.setFilter(""));
    ASSERT_TRUE(reader.open(path));
    EXPECT_FALSE(reader.getNextLine(buf, sizeof buf));
    EXPECT_STREQ(buf, "");

    // A valid mask makes the reader usable again
    ASSERT_TRUE(reader.setFilter("?"));
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(readAll(reader), (std::vector<std::string>{"a", "b"}));
}

// Same matching as Filter, but without the required literal, so the reader does not use the prefilter
class NoPrefilter final: public IFilter
{
public:
    bool parse(std::string_view mask) override { return filter.parse(mask); }

    bool isValid() const override { return filter.isValid(); }

    bool matchesFilter(std::string_view line) override { return filter.matchesFilter(line); }

private:
    Filter filter;
};

// The prefilter only skips lines that cannot match: with it and without it the reader returns the same lines.
// The file has lines longer than the block with the literal before and after the block end, '\r', '\0',
// the literal split by the end of a block and no '\n' at the end of the file.
TEST_F(LogReaderReadTest, PrefilterKeepsResult)
{
    std::mt19937 rng(42);
    const std::string pieces[] = {"ERROR", "ERR", "OR", "x", " ", "\r", std::string(1, '\0'), "E"};
    std::string content;
    for (int i = 0; i < 3000; ++i) {
        const std::size_t len = (i % 97 == 0) ? 5000 + rng() % 4000 : rng() % 60;  // some lines exceed 4 KB
        std::string line;
        while (line.size() < len) {
            line += pieces[rng() % std::size(pieces)];
        }
        content += line + ((i % 5 == 0) ? "\r\n" : "\n");
    }
    content += "last ERROR";  // no '\n' at the end

    const std::string path = makeFile("log.txt", content).string();
    for (const std::size_t blockSize: {logreader::MIN_BLOCK_SIZE, logreader::DEFAULT_BLOCK_SIZE}) {
        for (const char *mask: {"*ERROR*", "ERROR*", "*ERROR", "*RO?ER*", "*E*R*O*", "x*ERROR*x", "*R\r", "*O\nE*"}) {
            SCOPED_TRACE(::testing::PrintToString(mask) + " block " + std::to_string(blockSize));

            logreader::LogReader fast(std::make_unique<Filter>(), blockSize);
            logreader::LogReader plain(std::make_unique<NoPrefilter>(), blockSize);
            ASSERT_TRUE(fast.open(path) && fast.setFilter(mask));
            ASSERT_TRUE(plain.open(path) && plain.setFilter(mask));
            EXPECT_EQ(readAll(fast), readAll(plain));
        }
    }
}

}  // namespace
