#include "filter.h"
#include "log_reader.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <print>
#include <string>
#include <string_view>

int main(int argc, char *argv[])
{
    // Optional first argument: ignore case of Latin letters
    const bool ignoreCase = (argc > 1) && (std::string_view{argv[1]} == "-i");
    const int argBase = ignoreCase ? 2 : 1;

    if (argc < argBase + 2) {
        std::println("Usage: {} [-i] <path_to_file> <filter>", argv[0]);
        std::println("Example: {} -i logfile.log \"order*[0-9]*closed\"", argv[0]);
        return EXIT_SUCCESS;
    }

    const std::string filePath{argv[argBase]};
    const std::string filterStr{argv[argBase + 1]};

    auto filter = std::make_unique<Filter>(ignoreCase);
    logreader::LogReader reader{std::move(filter)};

    if (!reader.open(filePath)) {
        std::println(stderr, "Error: cannot open file \"{}\"", filePath);
        return EXIT_FAILURE;
    }

    if (!reader.setFilter(filterStr)) {
        std::println(stderr, "Error: invalid filter \"{}\"", filterStr);
        reader.close();
        return EXIT_FAILURE;
    }

    const int BUF_SIZE = 8192;
    char buf[BUF_SIZE];

    std::size_t matchCount = 0;
    while (reader.getNextLine(buf, BUF_SIZE)) {
        // Hot path: plain C-string output, no format parsing
        std::fputs(buf, stdout);
        std::fputc('\n', stdout);
        ++matchCount;
    }

    reader.close();

    // Summary goes to stderr so stdout contains only matched lines
    std::println(stderr, "Done. Matched lines: {}", matchCount);
    return EXIT_SUCCESS;
}
