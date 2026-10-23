#pragma once

#include "Types.h"
#include <string>
#include <string_view>
#include <optional>
#include <regex>

namespace harmonycast {

class HiLogParser {
public:
    HiLogParser();
    ~HiLogParser() = default;

    // Fast zero-allocation ANSI color stripper
    static std::string stripAnsi(std::string_view text);

    // Parses a single raw HiLog line into a structured HiLogEntry
    HiLogEntry parseLine(std::string_view raw_line, const std::string& bundle = "") const;

    // Checks if entry matches current active filters
    static bool matches(const HiLogEntry& entry,
                        std::string_view keyword = "",
                        LogLevel min_level = LogLevel::Debug,
                        std::string_view category = "",
                        std::string_view bundle_filter = "");

    // Package wildcard / prefix match (e.g. "com.yarward.*")
    static bool packageMatches(std::string_view actual_bundle, std::string_view pattern);

private:
    std::regex m_line_regex;
    std::regex m_header_regex;
};

} // namespace harmonycast
