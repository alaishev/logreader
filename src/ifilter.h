// ifilter.h
#pragma once

#include <string_view>

// Interface of filter implementation for matching strings.
// LogReader owns the implementation and calls it from one thread only.
// Masks and lines are single-byte (ANSI) data: any byte, including '\0' and '\r', is an ordinary character.
class IFilter
{
public:
    // Parses the mask and replaces the previous one.
    // Returns false if the mask is invalid (empty, too long, bad syntax); the filter then becomes invalid.
    virtual bool parse(std::string_view filter) = 0;

    // Returns true if the filter holds a successfully parsed mask, so matchesFilter() can be used
    virtual bool isValid() const = 0;

    // Returns true if the whole line (not a substring) matches the mask.
    // The view points into the reader's block and is valid only during the call.
    // Called for every line of the file, so it must be fast and must not allocate memory.
    virtual bool matchesFilter(std::string_view line) = 0;

    // Bytes that every matching line contains as they are (case-sensitive); empty if the mask has no such part.
    // The reader searches the whole block for them and skips the lines without them, never calling
    // matchesFilter() for those lines. The view must stay valid until the next parse().
    virtual std::string_view longestLiteral() const { return {}; }

    virtual ~IFilter() = default;
};
