#include "harmonycast/HiLogParser.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace harmonycast {

HiLogParser::HiLogParser() {
    // Matches standard OpenHarmony HiLog output:
    // e.g. "10-23 11:24:45.913  1162  1162 D A06666/MVP_RealClientProcessor: [MVP_RealClientProcessor] msg..."
    m_line_regex = std::regex(
        R"(^\s*(?:(?:\d{4}-)?\d{2}-\d{2}\s+)?(\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?)\s+(\d+)\s+(\d+)\s+([DIWEF])\s+(?:([ACIKP0-9a-fA-F]{3,10})/)?\s*([^:\s][^:]*?):\s*(.*)$)",
        std::regex::optimize
    );

    m_header_regex = std::regex(
        R"(^\s*(?:(?:\d{4}-)?\d{2}-\d{2}\s+)?\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?\s+(\d+)\s+\d+\s+[DIWEF]\s+([ACIKP])[0-9a-fA-F]{5,8}/)",
        std::regex::optimize
    );
}

std::string HiLogParser::stripAnsi(std::string_view text) {
    if (text.find('\x1b') == std::string_view::npos) {
        return std::string(text);
    }

    std::string clean;
    clean.reserve(text.size());
    bool inside_esc = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!inside_esc) {
            if (text[i] == '\x1b') {
                inside_esc = true;
            } else {
                clean.push_back(text[i]);
            }
        } else {
            if (text[i] == 'm') {
                inside_esc = false;
            }
        }
    }
    return clean;
}

HiLogEntry HiLogParser::parseLine(std::string_view raw_line, const std::string& bundle) const {
    HiLogEntry entry;
    entry.raw = std::string(raw_line);
    entry.bundle = bundle;

    std::string clean = stripAnsi(raw_line);
    std::smatch m;
    if (std::regex_match(clean, m, m_line_regex)) {
        entry.timestamp = m[1].str().substr(0, 12);
        entry.pid = std::stoi(m[2].str());
        entry.tid = std::stoi(m[3].str());

        char lvl_ch = m[4].str()[0];
        switch (lvl_ch) {
            case 'D': entry.level = LogLevel::Debug; break;
            case 'I': entry.level = LogLevel::Info;  break;
            case 'W': entry.level = LogLevel::Warn;  break;
            case 'E': entry.level = LogLevel::Error; break;
            case 'F': entry.level = LogLevel::Fatal; break;
            default:  entry.level = LogLevel::Info;  break;
        }

        entry.domain = m[5].str();
        entry.tag = m[6].str();
        entry.message = m[7].str();

        if (!entry.domain.empty()) {
            entry.category = (entry.domain[0] == 'A') ? "app" : "system";
        } else {
            entry.category = "system";
        }
    } else {
        // Fallback: generate local timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto timer = std::chrono::system_clock::to_time_t(now);
        std::tm bt{};
#ifdef _WIN32
        localtime_s(&bt, &timer);
#else
        localtime_r(&timer, &bt);
#endif
        std::ostringstream ss;
        ss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        entry.timestamp = ss.str();
        entry.level = clean.starts_with("[Fail]") ? LogLevel::Error : LogLevel::Info;
        entry.message = clean;
        entry.category = "system";
    }

    return entry;
}

static bool stringContainsCasefold(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return it != haystack.end();
}

bool HiLogParser::packageMatches(std::string_view actual_bundle, std::string_view pattern) {
    if (pattern.empty() || pattern == "全部应用") return true;
    if (pattern.ends_with('*')) {
        std::string_view prefix = pattern.substr(0, pattern.size() - 1);
        return actual_bundle.starts_with(prefix);
    }
    return actual_bundle == pattern;
}

bool HiLogParser::matches(const HiLogEntry& entry,
                          std::string_view keyword,
                          LogLevel min_level,
                          std::string_view category,
                          std::string_view bundle_filter) {
    if (static_cast<int>(entry.level) < static_cast<int>(min_level)) {
        return false;
    }

    if (!category.empty() && entry.category != category) {
        return false;
    }

    if (!bundle_filter.empty() && !packageMatches(entry.bundle, bundle_filter)) {
        return false;
    }

    if (!keyword.empty()) {
        if (!stringContainsCasefold(entry.message, keyword) &&
            !stringContainsCasefold(entry.tag, keyword) &&
            !stringContainsCasefold(entry.bundle, keyword) &&
            !stringContainsCasefold(std::to_string(entry.pid), keyword)) {
            return false;
        }
    }

    return true;
}

} // namespace harmonycast
