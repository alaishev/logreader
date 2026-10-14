// filter.h
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <string_view>

#include "ifilter.h"

// Filter by a glob mask ("*", "?", "[a-z]"). Works in two stages:
//   parse()         - once: the mask is turned into an array of atoms, one atom per element of the mask;
//   matchesFilter() - for every line: the line is checked against the atoms.
// All the work with the mask syntax (escaping, classes, case) is done in parse(),
// so the matching loop stays small and fast.

// Maximum length of the mask in bytes, exclusive: masks of 1..MAX_FILTER_LENGTH-1 bytes are accepted
inline constexpr std::size_t MAX_FILTER_LENGTH = 32;

enum class AtomType {
    LITERAL,       // Any symbol
    ANY_CHAR,      // '?'
    ANY_SEQUENCE,  // '*'
    CHAR_CLASS,    // '[abc]', '[a-z]', '[^0-9]'; also a letter when the case is ignored
    // Add more types
};

// Set of bytes: 256 bits, one per byte value. Bit c is set if byte c belongs to the set.
// Example: the set of [0-9] has bits 48..57 set ('0' is 48, '9' is 57).
using CharSet = std::bitset<256>;

// One element of the mask. Every atom except '*' takes exactly one character of the line,
// and the set says which characters it accepts:
//   'a'     -> {a}           '?'      -> all 256 bytes
//   'a', -i -> {a, A}        '[0-9]'  -> {0..9}
// '*' has an empty set: it takes any number of characters and is handled by Filter::matchesFilter().
struct FilterAtom {
    AtomType type = AtomType::LITERAL;
    CharSet set;  // bytes the atom accepts

    // Add more fields

    // Does the atom accept the character: one bit test, the same for all atom types
    bool matchesChar(char ch) const { return set[static_cast<unsigned char>(ch)]; }
};

class Filter final: public IFilter
{
public:
    // ignoreCase - Latin letters are compared ignoring case (ASCII only: the code page of the data is unknown)
    explicit Filter(bool ignoreCase = false) noexcept;

    bool parse(std::string_view filter) override;

    bool isValid() const override;

    bool matchesFilter(std::string_view line) override;

    std::string_view longestLiteral() const override;

private:
    FilterAtom makeLiteral(char ch) const;  // atom of an ordinary character; with ignoreCase a letter gets both cases

    std::array<FilterAtom, MAX_FILTER_LENGTH> atoms{};
    std::size_t length = 0;  // number of atoms of the current mask, atoms[length..] are unused
    std::array<char, MAX_FILTER_LENGTH> literal{};  // the longest run of ordinary characters of the mask
    std::size_t literalLength = 0;
    bool valid = false;
    const bool ignoreCase;
};
