# LogReader

A fast reader of huge text log files (hundreds of MB to tens of GB) that returns only the lines matching a mask.

(modern C++ adoptation of test task for MetaQuotes - https://www.metaquotes.net/ru/company/vacancies/tests/c-plus-plus)

- The file is read in fixed-size blocks (4 MB by default), never loaded into memory as a whole.
- Lines are returned one by one into the caller's buffer, nothing is cached.
- Data is single-byte (ANSI): any byte, including `\0` and `\r`, is an ordinary character. `\r\n` line ends
  are supported.
- No method throws; every failure is reported by the return value.

## Build

Requirements: C++23 compiler, CMake 3.25+. The library and the CLI have no external dependencies:

```bash
cmake -S . -B build && cmake --build build     # or ./build-project.sh; Release by default
```

Tests and benchmarks need GoogleTest and Google Benchmark, which the scripts install with
[Conan 2](https://conan.io) into a separate directory `build-dev/`:

```bash
./run-tests.sh         # unit tests; arguments go to ctest, e.g. ./run-tests.sh -R FilterMatch
./run-bench.sh         # benchmarks; arguments go to Google Benchmark, e.g. --benchmark_filter=Match
```

With GoogleTest installed in the system, tests also build without Conan: `cmake -S . -B build -DBUILD_TESTING=ON`.

## Command line

```bash
logreader-cli [-i] <path_to_file> <mask>
```

Matched lines go to stdout, the summary `Done. Matched lines: N` goes to stderr. `-i` ignores the case of
Latin letters.

```bash
logreader-cli app.log "*ERROR*"
logreader-cli -i app.log "*order*[0-9]*closed"
```

## Library

```cpp
#include "filter.h"
#include "log_reader.h"

logreader::LogReader reader(std::make_unique<Filter>());

if (reader.open("app.log") && reader.setFilter("*ERROR*")) {
    char buf[8192];
    while (reader.getNextLine(buf, sizeof buf)) {
        // buf holds the next matching line as a C-string, without '\n'
    }
}
reader.close();
```

`getNextLine()` returns `false` at the end of the file or on an error (no file opened, invalid mask).
A line longer than the buffer is truncated to fit it.

### Custom filter

Reading and matching are independent. `LogReader` gets its filter through the `IFilter` interface
(`src/ifilter.h`), so any matching logic can be plugged in without touching the reader:

```cpp
#include "ifilter.h"

// Accepts lines that contain the mask as a substring
class SubstringFilter final : public IFilter
{
public:
    bool parse(std::string_view mask) override { text = mask; return !text.empty(); }
    bool isValid() const override { return !text.empty(); }
    bool matchesFilter(std::string_view line) override { return line.find(text) != std::string_view::npos; }

private:
    std::string text;
};

logreader::LogReader reader(std::make_unique<SubstringFilter>());
```

`matchesFilter()` is called for every line of the file, so it should be fast and should not allocate memory.

A filter may also override `longestLiteral()` and return bytes that every matching line contains as they are
(the built-in `Filter` returns the longest run of ordinary characters of the mask, `ERROR` for `*ERROR*`).
The reader then searches whole blocks for these bytes with `memmem()` and skips the lines without them, never
calling `matchesFilter()` for those lines. This pays off when the bytes are rare: if they are in almost every
line, the search adds work and makes the reading 10-40% slower.

## Mask syntax

The built-in `Filter` uses glob masks. The mask is matched against the **whole** line:

| Mask | Matches lines that |
|---|---|
| `abc` | are exactly `abc` |
| `abc*` | start with `abc` |
| `*abc` | end with `abc` |
| `*abc*` | contain `abc` |
| `order*closed` | start with `order` and end with `closed` |
| `*****Some***` | contain `Some` (consecutive `*` are the same as one) |

Elements of the mask:

| Element | Meaning |
|---|---|
| any character | exactly this character (case-sensitive unless `-i`) |
| `?` | exactly one character, any |
| `*` | any sequence of characters, including empty |
| `[abc]`, `[a-z]` | one character from the set or range |
| `[^abc]`, `[!abc]` | one character not in the set |
| `\x` | the character `x` itself: `\*`, `\?`, `\[`, `\\` |

Inside `[...]`, `]` right after `[` (or `[^`), `-` at the start or before `]`, and any character after `\` are
ordinary characters; `*`, `?` and `[` have no special meaning there. A `]` outside of a class is an ordinary
character, so `*worker-3]*` works without escaping.

Examples:

```text
*ERROR*                    lines containing ERROR
2026-10-14 *               lines starting with the date
*user[0-9][0-9][0-9] *     lines containing "user" followed by exactly three digits and a space
*[!0-9]                    lines whose last character is not a digit
*timeout*      (with -i)   lines containing "timeout", "Timeout", "TIMEOUT", ...
```

Rules and limits:

- The mask length is 1 to 31 bytes. An empty or longer mask, an unclosed class `[abc` and a reversed range
  `[z-a]` are rejected: `setFilter()` returns `false` and no lines are returned until a valid mask is set.
- Ignoring case works for Latin letters only; bytes above 127 are compared as is, because the code page of the
  data is unknown.
- A `\` at the very end of the mask is ignored.
- A line longer than the reader's block (4 MB by default) is matched and returned by its first block only.

Matching takes at most O(line length × mask length): there is no recursion and no exponential backtracking,
whatever the mask. Regular-expression features such as `+`, `{n,m}` or `(a|b)` are not supported.


