// filter.cpp
#include "filter.h"

#include <algorithm>
#include <optional>

namespace {

// Returns the same Latin letter in the other case: 'a' -> 'A', 'Q' -> 'q'.
// For any other byte (digit, punctuation, byte above 127) returns nothing: it has no case.
std::optional<unsigned char> makeChar(unsigned char c)
{
    if (c >= 'a' && c <= 'z') {
        return static_cast<unsigned char>(c - ('a' - 'A'));
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned char>(c + ('a' - 'A'));
    }
    return std::nullopt;
}

// Helper of parseCharClass(): takes one character of a class from the mask.
// pos is the index of the character in the mask; on return it points to the next one.
// "\x" is read as the character x itself, so "\]" gives ']' that does not close the class.
// Returns nothing if the mask is over: the class is not closed.
std::optional<unsigned char> readClassChar(std::string_view filter, std::size_t &pos)
{
    if ((pos < filter.size()) && (filter[pos] == '\\')) {
        ++pos;
    }
    if (pos >= filter.size()) {
        return std::nullopt;
    }
    return static_cast<unsigned char>(filter[pos++]);
}

// Turns a character class of the mask like "[a-z0-9_]" into the set of bytes it accepts.
// On input pos is the index right after '['; on success it is the index of the closing ']'.
//
// Supported syntax:
//   [abc]          - one of the listed characters
//   [a-z]          - any character from 'a' to 'z' (by byte values)
//   [^abc], [!abc] - any character except the listed ones
//   []a], [a-]     - ']' right after '[' and '-' before ']' are ordinary characters
//   [\]], [\-]     - '\' makes the next character ordinary
//
// Returns nothing for an invalid class: no closing ']' or a reversed range like [z-a].
std::optional<CharSet> parseCharClass(std::string_view filter, std::size_t &pos, bool ignoreCase)
{
    CharSet set;

    // '^' or '!' right after '[' inverts the class; the inversion is applied at the end
    bool negate = false;
    if ((pos < filter.size()) && ((filter[pos] == '^') || (filter[pos] == '!'))) {
        negate = true;
        ++pos;
    }

    // One iteration reads one item: a single character "a" or a range "a-z".
    // first: ']' as the very first item is an ordinary character, later it closes the class
    for (bool first = true;; first = false) {
        if (pos >= filter.size()) {
            return std::nullopt;  // the mask ended before ']'
        }
        if ((filter[pos] == ']') && !first) {
            break;
        }

        const auto lo = readClassChar(filter, pos);
        if (!lo) {
            return std::nullopt;
        }

        // A range "lo-hi" if '-' follows and it is not the last character before ']':
        // "[a-]" is 'a' and '-', not a range
        auto hi = lo;
        if ((pos + 1 < filter.size()) && (filter[pos] == '-') && (filter[pos + 1] != ']')) {
            ++pos;
            hi = readClassChar(filter, pos);
            if (!hi || (*hi < *lo)) {
                return std::nullopt;
            }
        }

        // A single character is the range lo..lo
        for (unsigned c = *lo; c <= *hi; ++c) {
            set.set(c);
        }
    }

    // Ignoring case: if a letter is in the set in any case, both cases are added.
    // It is done before the inversion, so [^a] excludes both 'a' and 'A'.
    // In the other order [^a] would become "everything but 'a'" and would still accept 'A'.
    if (ignoreCase) {
        for (unsigned char lower = 'a'; lower <= 'z'; ++lower) {
            const auto upper = static_cast<unsigned char>(lower - ('a' - 'A'));
            if (set[lower] || set[upper]) {
                set.set(lower);
                set.set(upper);
            }
        }
    }
    if (negate) {
        set.flip();
    }

    return set;
}

}

Filter::Filter(bool ignoreCase_) noexcept
    : ignoreCase(ignoreCase_)
{
}

// Makes the atom of an ordinary character of the mask: its set contains just this byte.
// Ignoring case, a letter gets both cases: 'e' accepts 'e' and 'E'. So the case is handled once here,
// and matchesFilter() never converts the case of the line.
FilterAtom Filter::makeLiteral(char ch) const
{
    const auto c = static_cast<unsigned char>(ch);

    FilterAtom atom;
    atom.type = AtomType::LITERAL;
    atom.set.set(c);
    atom.ch = ch;

    if (const auto symbol = makeChar(c); ignoreCase && symbol) {
        atom.type = AtomType::CHAR_CLASS;
        atom.set.set(*symbol);
    }
    return atom;
}

// Turns the mask into the array of atoms, one atom per element of the mask:
//   "Err*[0-9]?"  ->  'E' 'r' 'r' '*' [0-9] '?'  (6 atoms)
// On error the filter becomes invalid (isValid() is false) and the previous mask is lost.
bool Filter::parse(std::string_view filter)
{
    valid = false;
    length = 0;
    lastStar = NO_STAR;
    literalLength = 0;

    if (filter.empty() || (filter.size() >= MAX_FILTER_LENGTH)) {
        return false;
    }

    // The longest run of ordinary characters is collected on the way: curr is the current run, literal the
    // longest one so far. '*', '?', a class and a letter ignoring case (it becomes a class) break the run.
    // The bytes are copied, not referenced in the mask: "a\*b" gives "a*b" without the backslash.
    std::array<char, MAX_FILTER_LENGTH> curr{};
    std::size_t currLength = 0;
    const auto endRun = [&]() {
        if (currLength > literalLength) {
            std::copy_n(curr.begin(), currLength, literal.begin());
            literalLength = currLength;
        }
        currLength = 0;
    };

    // src - index in the mask, dst - index of the next atom.
    // An atom takes at least one byte of the mask, so dst <= src and the atoms always fit the array.
    std::size_t dst = 0;

    // Adds the atom of an ordinary character and continues the run, or breaks it if the atom is a class
    const auto addLiteral = [&](char ch) {
        atoms[dst] = makeLiteral(ch);
        if (atoms[dst++].type == AtomType::LITERAL) {
            curr[currLength++] = ch;
        }
        else {
            endRun();
        }
    };

    for (std::size_t src = 0; src < filter.size(); ++src) {
        const char ch = filter[src];

        // "\x" is the character x itself: "\*" is a star, not "any sequence". A lone '\' at the end is ignored
        if (ch == '\\') {
            if (++src < filter.size()) {
                addLiteral(filter[src]);
            }
            continue;
        }

        if (ch == '*') {
            endRun();
            // "**" means the same as "*", so one atom is enough
            if ((dst > 0) && (atoms[dst - 1].type == AtomType::ANY_SEQUENCE)) {
                continue;
            }
            atoms[dst++] = FilterAtom{.type = AtomType::ANY_SEQUENCE, .set = CharSet{}};
            continue;
        }

        if (ch == '[') {
            endRun();
            std::size_t pos = src + 1;
            const auto set = parseCharClass(filter, pos, ignoreCase);
            if (!set) {
                return false;
            }
            atoms[dst++] = FilterAtom{.type = AtomType::CHAR_CLASS, .set = *set};
            src = pos;  // pos is at ']', ++src of the loop moves past it
            continue;
        }

        // '?' accepts any byte: all 256 bits are set
        if (ch == '?') {
            endRun();
            atoms[dst++] = FilterAtom{.type = AtomType::ANY_CHAR, .set = CharSet{}.set()};
            continue;
        }

        addLiteral(ch);
    }
    endRun();

    length = dst;
    valid = true;

    for (std::size_t i = 0; i < length; ++i) {
        if (atoms[i].type == AtomType::ANY_SEQUENCE) {
            lastStar = i;
        }
    }

    return true;
}

// After a failed parse() there is no literal: the run collected before the error is not returned
std::string_view Filter::longestLiteral() const
{
    return valid ? std::string_view(literal.data(), literalLength) : std::string_view{};
}

bool Filter::isValid() const
{
    return valid;
}

// Checks if the whole line matches the mask. The line and the atoms are walked from left to right together.
//
// The only difficulty is '*': it is unknown in advance how many characters it must take. So '*' first takes
// nothing, and on a later mismatch it takes one more character and the atoms after it are checked again.
// Only the last '*' is moved this way: whatever an earlier '*' could take, the last one can take too,
// because '*' accepts any characters. So there is no search over all '*', and the time is at most
// O(line length * mask length).
//
// Example: mask "a*x", line "abcx". '*' takes "", "b", "bc" in turn; with "bc" the 'x' matches.
bool Filter::matchesFilter(std::string_view line)
{
    // No '*': one character per atom, so the length is known
    if (lastStar == NO_STAR) {
        return (line.size() == length) && matchesFixed(line, 0);
    }

    // The atoms after the last '*' take the last characters of the line: they are checked at once,
    // and the rest of the mask (it ends with that '*') is matched against the rest of the line.
    // Example: mask "*INFO*x", line "...INFO...xxx": 'x' is checked against the last character only,
    // instead of trying every 'x' of the line as the end of the match.
    const std::size_t tail = length - lastStar - 1;
    if (line.size() < tail) {
        return false;
    }
    if (!matchesFixed(line.substr(line.size() - tail), lastStar + 1)) {
        return false;
    }
    line.remove_suffix(tail);
    const std::size_t end = lastStar + 1;  // atoms of the rest of the mask: [0, end), the last one is '*'

    std::size_t p = 0;            // index of the current atom
    std::size_t t = 0;            // index of the current character of the line
    std::size_t starP = NO_STAR;  // index of the atom of the last '*' met, NO_STAR if there was none yet
    std::size_t starT = 0;        // first character of the line after the part taken by the last '*'
    bool jump = false;            // the atom after the last '*' is an ordinary character: find it with memchr

    while (t < line.size()) {
        // The atom accepts the character: go to the next atom and the next character
        if ((p < end) && (atoms[p].type != AtomType::ANY_SEQUENCE) && atoms[p].matchesChar(line[t])) {
            ++p;
            ++t;
        }
        // The atom is '*': remember where it is, first it takes no characters
        else if ((p < end) && (atoms[p].type == AtomType::ANY_SEQUENCE)) {
            starP = p;
            starT = t;
            ++p;
            // The last atom is '*': it takes the rest of the line, whatever it is
            if (p == end) {
                return true;
            }
            // The next atom is an ordinary character: '*' takes everything up to its next occurrence,
            // so jump there with memchr (string_view::find) instead of moving by one character
            jump = (atoms[p].type == AtomType::LITERAL);
            if (jump) {
                t = line.find(atoms[p].ch, t);
                if (t == std::string_view::npos) {
                    return false;  // an earlier '*' can not help: it would only move the atoms further right
                }
                starT = t;
            }
        }
        // Mismatch, but there was a '*': it takes one more character, the atoms after it are checked again
        else if (starP != NO_STAR) {
            p = starP + 1;
            if (jump) {
                t = line.find(atoms[p].ch, starT + 1);
                if (t == std::string_view::npos) {
                    return false;
                }
                starT = t;
            }
            else {
                t = ++starT;
            }
        }
        // Mismatch and no '*' before: the line does not match
        else {
            return false;
        }
    }

    // The line is over. The rest of the mask matches only if it is just '*' (they may take nothing):
    // e.g. mask "abc*" and line "abc".
    while ((p < end) && (atoms[p].type == AtomType::ANY_SEQUENCE)) {
        ++p;
    }

    return p == end;
}

// Checks text against the atoms from first on, one character per atom: no '*' among them
bool Filter::matchesFixed(std::string_view text, std::size_t first) const
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!atoms[first + i].matchesChar(text[i])) {
            return false;
        }
    }
    return true;
}
