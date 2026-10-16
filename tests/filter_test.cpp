// Unit tests of Filter: parsing of the glob mask and matching of whole lines

#include "filter.h"

#include <gtest/gtest.h>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Mask, lines that must match it and lines that must not
struct MatchCase {
    std::string mask;              // filter mask
    std::vector<std::string> yes;  // matching lines
    std::vector<std::string> no;   // non-matching lines
};

void checkCases(const std::vector<MatchCase> &cases, bool ignoreCase = false)
{
    Filter filter(ignoreCase);
    for (const auto &c: cases) {
        SCOPED_TRACE("mask = " + ::testing::PrintToString(c.mask));
        ASSERT_TRUE(filter.parse(c.mask));
        for (const auto &line: c.yes) {
            EXPECT_TRUE(filter.matchesFilter(line)) << ::testing::PrintToString(line);
        }
        for (const auto &line: c.no) {
            EXPECT_FALSE(filter.matchesFilter(line)) << ::testing::PrintToString(line);
        }
    }
}

// Invalid masks are rejected: empty, too long
TEST(FilterParse, InvalidMasks)
{
    Filter filter;
    EXPECT_FALSE(filter.parse(std::string_view{}));
    EXPECT_FALSE(filter.parse(""));
    EXPECT_FALSE(filter.parse(std::string(MAX_FILTER_LENGTH, 'a')));
    EXPECT_FALSE(filter.parse(std::string(100000, '*')));
}

// Masks up to the length limit are accepted
TEST(FilterParse, ValidMasks)
{
    Filter filter;
    for (const char *mask: {"a", "*", "?", "\\", "\\*", "*****Some***", "a?b*c", " ", "\t\r"}) {
        EXPECT_TRUE(filter.parse(mask)) << mask;
    }
    EXPECT_TRUE(filter.parse(std::string(MAX_FILTER_LENGTH - 1, 'a')));
    EXPECT_TRUE(filter.parse(std::string(MAX_FILTER_LENGTH - 1, '*')));
}

// isValid() follows the result of the last parse()
TEST(FilterParse, Validity)
{
    Filter filter;
    ASSERT_TRUE(filter.parse("a*"));
    EXPECT_TRUE(filter.isValid());

    EXPECT_FALSE(filter.parse(""));
    EXPECT_FALSE(filter.isValid());

    ASSERT_TRUE(filter.parse("b"));
    EXPECT_TRUE(filter.isValid());

    EXPECT_FALSE(filter.parse(std::string(MAX_FILTER_LENGTH, 'a')));
    EXPECT_FALSE(filter.isValid());
}

// The mask is anchored on both sides: it must cover the whole line
TEST(FilterMatch, Anchoring)
{
    checkCases({
      {"abc", {"abc"}, {"", "ab", "abcd", "xabc", "ABC", "abc "}},
      {"abc*", {"abc", "abcd", "abc abc"}, {"", "ab", "xabc"}},
      {"*abc", {"abc", "xabc", "abcabc"}, {"", "abcd", "ab"}},
      {"*abc*", {"abc", "xabcx", "xxabc", "abcxx", "aabbabc"}, {"", "ab", "acb", "a b c"}},
    });
}

// The atoms after the last '*' are checked against the end of the line; the head must not overlap them
TEST(FilterMatch, TailAfterLastStar)
{
    checkCases({
      {"a*a", {"aa", "aba", "aaa"}, {"a", "", "ab"}},  // one 'a' can not be both the head and the tail
      {"ab*ba", {"abba", "abxba", "ababa"}, {"aba", "ab", "ba", "abab"}},
      {"*x", {"x", "xx", "abcx"}, {"", "xa", "X"}},
      {"*x?", {"xa", "axb", "xxx"}, {"x", "ax", "xab"}},
      {"*[0-9]", {"1", "a1", "12"}, {"", "1a"}},
      {"a*\\*", {"a*", "ab*", "a**"}, {"a", "a*b"}},  // an escaped '*' at the end is an ordinary character
      {"a**b", {"ab", "axb"}, {"a", "b", "abx"}},     // "**" is one '*'
      {"*\r", {"\r", "a\r"}, {"a", "\ra"}},
      {std::string("*\0", 2), {std::string("\0", 1), std::string("a\0", 2)}, {"a", std::string("\0a", 2)}},
    });
}

// The character after '*' is searched for with memchr: the line may lack it, or have it many times
TEST(FilterMatch, JumpToLiteral)
{
    checkCases({
      {"*x*", {"x", "abxcd", "xxxx"}, {"", "abc", "X"}},
      {"*ab*", {"ab", "aaab", "abab", "xaxab"}, {"a", "aa", "ba", "axb"}},  // 'a' found, 'b' fails, next 'a'
      {"*a?c*", {"abc", "aabc", "xaxc"}, {"ac", "abd", "aa"}},
      {"a*b*c", {"abc", "abbbc", "axbxc"}, {"ac", "acb", "abcb"}},
    });
}

// '?' takes exactly one character, '*' any sequence including empty
TEST(FilterMatch, Wildcards)
{
    checkCases({
      {"*", {"", "a", "any line at all"}, {}},
      {"?", {"a", " ", "?"}, {"", "ab"}},
      {"???", {"abc", "???"}, {"", "ab", "abcd"}},
      {"a?c", {"abc", "a c", "a?c"}, {"ac", "abbc"}},
      {"?*", {"a", "abc"}, {""}},
      {"*?", {"a", "abc"}, {""}},
      {"a*b*c", {"abc", "aXbYc", "abbbc", "acbc"}, {"ab", "acb", "abcX"}},
      {"order*closed", {"order closed", "order 1 closed", "orderclosed"}, {"order 1 closed!", "order open"}},
    });
}

// Consecutive '*' are equivalent to one, their position and number are not limited
TEST(FilterMatch, ManyAsterisks)
{
    checkCases({
      {"*****Some***", {"Some", "xSome", "Somex", "xxSomexx"}, {"", "Som", "some", "S o m e"}},
      {"**", {"", "a"}, {}},
      {"a**b", {"ab", "axb"}, {"a", "b"}},
      {"*a*a*a*", {"aaa", "xaxaxax"}, {"aa", "", "xaxa"}},
    });
}

// Backtracking to the last '*': the first candidate position is not the right one
TEST(FilterMatch, Backtracking)
{
    checkCases({
      {"*ab", {"aab", "abab", "aaab"}, {"aba", "aa"}},
      {"*aab*", {"aaab", "abaab", "xaxaabx"}, {"abab", "aba"}},
      {"*a?c", {"aabc", "xabc", "aaac"}, {"aabcd", "ac", "abac"}},
    });
}

// '\' makes the next character literal, including '*', '?' and '\'
TEST(FilterMatch, Escaping)
{
    checkCases({
      {"\\*", {"*"}, {"", "a", "**"}},
      {"a\\*", {"a*"}, {"a", "ab", "a*b"}},
      {"\\?", {"?"}, {"a", ""}},
      {"\\\\", {"\\"}, {"", "\\\\"}},
      {"*\\**", {"*", "a*b"}, {"ab", ""}},
      {"\\a", {"a"}, {"\\a"}},
    });
}

// Character classes: sets, ranges, negation; one class takes exactly one character
TEST(FilterMatch, CharClasses)
{
    checkCases({
      {"[abc]", {"a", "b", "c"}, {"", "d", "ab", "A", "[abc]"}},
      {"[a-c]", {"a", "b", "c"}, {"d", "`", "B"}},
      {"[a-cx-z]", {"a", "c", "x", "z"}, {"d", "w"}},
      {"[0-9][0-9]", {"00", "42"}, {"4", "4a", "123"}},
      {"[^0-9]", {"a", " ", std::string(1, '\0')}, {"0", "9", "", "ab"}},
      {"[!0-9]", {"a"}, {"5"}},
      {"[a-a]", {"a"}, {"b"}},
      {"*[0-9]*", {"order 42", "1"}, {"order", ""}},
      {"order*[0-9]*closed", {"order 1 closed", "order #42 closed"}, {"order X closed"}},
      {"*[.]log", {"app.log"}, {"appXlog"}},  // special characters are literal inside a class
      {"[*?[]", {"*", "?", "["}, {"a"}},
    });
}

// Literal ']' and '-' in a class, escaping inside a class
TEST(FilterMatch, CharClassSpecialChars)
{
    checkCases({
      {"[]]", {"]"}, {"a"}},
      {"[]a]", {"]", "a"}, {"b"}},
      {"[^]]", {"a"}, {"]"}},
      {"[-a]", {"-", "a"}, {"b"}},
      {"[a-]", {"-", "a"}, {"b"}},
      {"[a-c-e]", {"a", "b", "-", "e"}, {"d"}},  // '-' after a range is literal
      {"[\\]]", {"]"}, {"\\"}},
      {"[\\\\]", {"\\"}, {"]"}},
      {"[\\^a]", {"^", "a"}, {"b"}},
      {"[a\\-c]", {"a", "-", "c"}, {"b"}},
      {"[\\a-\\c]", {"b"}, {"d"}},
      {"\\[a]", {"[a]"}, {"a"}},
    });
}

// Classes of bytes above 127: the range is compared by unsigned byte values
TEST(FilterMatch, CharClassHighBytes)
{
    checkCases({
      {"[\x80-\xFF]", {"\x80", "\xC0", "\xFF"}, {"\x7F", "a"}},
      {"[^\x80-\xFF]", {"a", "\x7F"}, {"\x80", "\xFF"}},
      {"[\x01-\xFF]", {"\x01", "a", "\xFF"}, {std::string(1, '\0')}},
    });
}

// Invalid classes are rejected: not closed, reversed range
TEST(FilterParse, InvalidCharClasses)
{
    Filter filter;
    for (const char *mask: {"[", "[a", "[]", "[^]", "[^", "[a-", "[a\\]", "[a\\", "[z-a]", "*[abc", "[9-0]*"}) {
        EXPECT_FALSE(filter.parse(mask)) << mask;
        EXPECT_FALSE(filter.isValid()) << mask;
    }
    EXPECT_TRUE(filter.parse("]"));  // ']' outside a class is literal
}

// Case is ignored only for Latin letters: literals, escaped letters and classes
TEST(FilterMatch, IgnoreCase)
{
    checkCases(
      {
        {"abc", {"abc", "ABC", "aBc"}, {"abd", "ab"}},
        {"*error*", {"ERROR: x", "an Error"}, {"err"}},
        {"\\a", {"a", "A"}, {"b"}},
        {"[a-c]", {"a", "B", "C"}, {"d", "D"}},
        {"[A-C]", {"a", "b"}, {"d"}},
        {"[^a]", {"b", "B"}, {"a", "A"}},  // negation excludes both cases
        {"1-2_?", {"1-2_x"}, {"1-2_"}},    // non-letters are unchanged
        {"[@-Z]", {"@", "z"}, {"`"}},
        {"\xC0", {"\xC0"}, {"\xE0"}},  // bytes above 127 are compared as is
      },
      true);

    // Without the flag the case is significant
    checkCases({
      {"abc", {"abc"}, {"ABC", "aBc"}},
      {"[a-c]", {"b"}, {"B"}},
    });
}

// Lines are single-byte data: '\0', '\r' and bytes above 127 are ordinary characters
TEST(FilterMatch, BinaryCharacters)
{
    const std::string nul("a\0b", 3);
    const std::string high("\xC0\xFF", 2);  // bytes of a non-ASCII ANSI string

    checkCases({
      {"a?b", {nul, "a\rb"}, {"ab"}},
      {"a*", {nul}, {}},
      {"*b", {nul}, {}},
      {"a", {}, {nul}},  // the line is not cut at '\0'
      {high, {high}, {"\xC0", "\xC0\xFE"}},
      {"??", {high}, {}},
    });
}

// Only the bytes of the view are used: the line may be a part of a bigger buffer without '\0'
TEST(FilterMatch, LineIsNotNulTerminated)
{
    Filter filter;
    ASSERT_TRUE(filter.parse("abc"));
    const char data[] = {'a', 'b', 'c', 'd', 'e'};
    EXPECT_TRUE(filter.matchesFilter({data, 3}));
    EXPECT_FALSE(filter.matchesFilter({data, 2}));
    EXPECT_FALSE(filter.matchesFilter({data, 5}));
}

// An empty line (also a default view without data) matches only masks that accept an empty string
TEST(FilterMatch, EmptyLine)
{
    Filter filter;
    for (const char *mask: {"*", "**"}) {
        ASSERT_TRUE(filter.parse(mask));
        EXPECT_TRUE(filter.matchesFilter(std::string_view{})) << mask;
        EXPECT_TRUE(filter.matchesFilter("")) << mask;
    }
    for (const char *mask: {"a", "?", "*?", "[a]", "*[a]*"}) {
        ASSERT_TRUE(filter.parse(mask));
        EXPECT_FALSE(filter.matchesFilter(std::string_view{})) << mask;
    }
}

// The mask is a view, not a C-string: '\0' inside it is an ordinary literal, not the end of the mask
TEST(FilterMatch, NulInsideMask)
{
    using namespace std::string_literals;

    Filter filter;
    ASSERT_TRUE(filter.parse("a\0b"s));
    EXPECT_TRUE(filter.matchesFilter("a\0b"s));
    EXPECT_FALSE(filter.matchesFilter("a"));
    EXPECT_FALSE(filter.matchesFilter("a b"));

    ASSERT_TRUE(filter.parse("*\0*"s));
    EXPECT_TRUE(filter.matchesFilter("xx\0yy"s));
    EXPECT_FALSE(filter.matchesFilter("xxyy"));
}

// A new mask fully replaces the previous one
TEST(FilterMatch, Reparse)
{
    Filter filter;
    ASSERT_TRUE(filter.parse("a*"));
    EXPECT_TRUE(filter.matchesFilter("abc"));

    ASSERT_TRUE(filter.parse("b"));
    EXPECT_FALSE(filter.matchesFilter("abc"));
    EXPECT_TRUE(filter.matchesFilter("b"));

    ASSERT_TRUE(filter.parse("*"));
    EXPECT_TRUE(filter.matchesFilter("abc"));
}

// Long lines with the mask of maximum length: the result is correct and comes without a hang
TEST(FilterMatch, LongLines)
{
    const std::string longLine(1024 * 1024, 'a');

    Filter filter;
    ASSERT_TRUE(filter.parse("*b"));
    EXPECT_FALSE(filter.matchesFilter(longLine));
    EXPECT_TRUE(filter.matchesFilter(longLine + "b"));

    // Worst case of backtracking: a long literal between two '*', so the tail check does not cut it off;
    // memchr finds 'a' at every position of the line, and almost the whole mask is compared there
    // (O(line length * mask length))
    const std::string mask = "*" + std::string(MAX_FILTER_LENGTH - 4, 'a') + "b*";
    ASSERT_TRUE(filter.parse(mask));
    const std::string line(64 * 1024, 'a');
    EXPECT_FALSE(filter.matchesFilter(line));
    EXPECT_TRUE(filter.matchesFilter(line + "b"));
}

// Required literal: the longest run of ordinary characters, used by the reader to skip lines
TEST(FilterLiteral, LongestLiteral)
{
    struct Case {
        const char *mask;      // filter mask
        bool ignoreCase;       // Filter(ignoreCase)
        std::string expected;  // longestLiteral()
    };

    const Case cases[] = {
      {"*ERROR*", false, "ERROR"},
      {"ERROR", false, "ERROR"},
      {"*order 1?????[02468] *", false, "order 1"},
      {"ab?cde*f", false, "cde"},  // the longest run wins
      {"ab*cd", false, "ab"},      // equal runs: the first one
      {"a\\*b*", false, "a*b"},    // an escaped '*' is an ordinary byte
      {"[x]yz", false, "yz"},      // a class is not a literal
      {"*", false, ""},
      {"?[0-9]*", false, ""},
      {"*ERROR*", true, ""},          // ignoring case, letters are classes
      {"*12-34 x*", true, "12-34 "},  // but digits and punctuation are not
    };

    for (const auto &c: cases) {
        Filter filter(c.ignoreCase);
        ASSERT_TRUE(filter.parse(c.mask)) << c.mask;
        EXPECT_EQ(filter.longestLiteral(), c.expected) << c.mask;
    }

    // A failed parse leaves no literal
    Filter filter;
    ASSERT_TRUE(filter.parse("*ERROR*"));
    EXPECT_FALSE(filter.parse("[abc"));
    EXPECT_TRUE(filter.longestLiteral().empty());
}

// Element of a mask for the reference matcher: '*', or one character accepted by the set
struct RefAtom {
    bool star = false;     // '*': any sequence
    std::string accepted;  // characters a single-character element accepts
};

// Reference glob matching by dynamic programming: dp[i][j] - the first i atoms match the first j characters.
// Slow and obviously correct; Filter::matchesFilter() must give the same answer.
bool refMatch(const std::vector<RefAtom> &atoms, std::string_view line)
{
    std::vector<std::vector<char>> dp(atoms.size() + 1, std::vector<char>(line.size() + 1, 0));
    dp[0][0] = 1;
    for (std::size_t i = 1; i <= atoms.size(); ++i) {
        const RefAtom &a = atoms[i - 1];
        for (std::size_t j = 0; j <= line.size(); ++j) {
            if (a.star) {
                dp[i][j] = dp[i - 1][j] || (j > 0 && dp[i][j - 1]);
            }
            else {
                dp[i][j] = j > 0 && dp[i - 1][j - 1] && a.accepted.find(line[j - 1]) != std::string::npos;
            }
        }
    }
    return dp[atoms.size()][line.size()] != 0;
}

// Random masks and lines over a tiny alphabet, so that matches, overlaps of the head and the tail and
// repeated '*' are frequent. Each mask element is given as its text and its reference meaning.
TEST(FilterMatch, SameAsReference)
{
    struct Element {
        const char *text;  // element in the mask syntax
        RefAtom atom;      // its meaning for the reference matcher
    };

    const Element elements[] = {
      {"a", {false, "a"}},
      {"b", {false, "b"}},
      {"?", {false, "ab*"}},
      {"*", {true, ""}},
      {"\\*", {false, "*"}},
      {"[ab]", {false, "ab"}},
      {"[!a]", {false, "b*"}},
    };
    const char lineChars[] = {'a', 'b', '*'};

    std::mt19937 rng(7);
    Filter filter;
    for (int m = 0; m < 3000; ++m) {
        std::string mask;
        std::vector<RefAtom> atoms;
        const std::size_t count = 1 + rng() % 7;
        for (std::size_t k = 0; k < count; ++k) {
            const Element &e = elements[rng() % std::size(elements)];
            mask += e.text;
            atoms.push_back(e.atom);
        }
        ASSERT_TRUE(filter.parse(mask)) << mask;

        for (int l = 0; l < 30; ++l) {
            std::string line(rng() % 11, 'a');
            for (char &c: line) {
                c = lineChars[rng() % std::size(lineChars)];
            }
            ASSERT_EQ(filter.matchesFilter(line), refMatch(atoms, line)) << "mask " << mask << ", line " << line;
        }
    }
}

}  // namespace
