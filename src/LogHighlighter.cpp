#include "harmonycast/LogHighlighter.h"

#include <QColor>
#include <QFont>

namespace harmonycast {

LogHighlighter::LogHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    initFormats();
}

void LogHighlighter::initFormats() {
    // Regular expressions
    m_header_regex = QRegularExpression(
        R"(^(\d{2}:\d{2}:\d{2}\.\d{3}|\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d{3})\s+([DIWEF]|DEBUG|INFO|WARN(?:ING)?|ERROR|FATAL|LOG)\b)"
    );
    m_wv_header_regex = QRegularExpression(
        R"(^\[(LOG|DEBUG|INFO|WARN(?:ING)?|ERROR|FATAL)\])"
    );
    m_pid_regex = QRegularExpression(R"(\b\d{1,6}\/\d{1,6}\b)");
    m_tag_regex = QRegularExpression(R"(\[([^\]]+)\])");
    m_hex_regex = QRegularExpression(R"(\b0x[0-9a-fA-F]+\b)");
    m_url_regex = QRegularExpression(R"(https?://[^\s"'<>]+)");
    m_string_regex = QRegularExpression(R"("[^"\\\r\n]*(?:\\.[^"\\\r\n]*)*")");
    m_success_regex = QRegularExpression(
        R"(\b(?i)(success(?:ful|fully)?|succeeded|ok|passed|connected)\b)"
    );
    m_fail_regex = QRegularExpression(
        R"(\b(?i)(fail(?:ed|ure)?|exception|crash|error|fatal|denied|timeout|refused|nullpointer)\b)"
    );

    // Formats: Header fields
    m_fmt_timestamp.setForeground(QColor("#627588"));

    m_fmt_pid.setForeground(QColor("#4d6073"));

    m_fmt_tag.setForeground(QColor("#79c0ff"));

    // Levels
    m_fmt_debug.setForeground(QColor("#7f93a7"));

    m_fmt_info.setForeground(QColor("#5ce0c1"));

    m_fmt_warn.setForeground(QColor("#f3c969"));
    m_fmt_warn.setFontWeight(QFont::DemiBold);

    m_fmt_error.setForeground(QColor("#ff7b72"));
    m_fmt_error.setFontWeight(QFont::Bold);

    m_fmt_fatal.setForeground(QColor("#d2a8ff"));
    m_fmt_fatal.setFontWeight(QFont::Bold);

    // Message body — foreground only; row background handled by m_row_* applied first
    m_msg_debug.setForeground(QColor("#8a9cb0"));
    m_msg_info.setForeground(QColor("#dbe5ee"));
    m_msg_warn.setForeground(QColor("#ffe08a"));
    m_msg_error.setForeground(QColor("#ffa198"));
    m_msg_fatal.setForeground(QColor("#f0b3ff"));

    // Tokens
    m_fmt_hex.setForeground(QColor("#f0883e"));

    m_fmt_string.setForeground(QColor("#a5d6ff"));

    m_fmt_url.setForeground(QColor("#58a6ff"));
    m_fmt_url.setFontUnderline(true);

    m_fmt_success.setForeground(QColor("#7ee787"));

    m_fmt_fail.setForeground(QColor("#ff7b72"));
    m_fmt_fail.setFontWeight(QFont::Bold);

    // Full-row background tints — subtle, semi-transparent
    m_row_warn.setBackground(QColor(55, 44, 8, 100));    // dark amber
    m_row_error.setBackground(QColor(70, 16, 14, 115));  // dark crimson
    m_row_fatal.setBackground(QColor(55, 10, 65, 120));  // dark purple
}

void LogHighlighter::highlightBlock(const QString& text) {
    if (text.isEmpty()) {
        setCurrentBlockState(StateNormal);
        return;
    }

    auto match = m_header_regex.match(text);
    auto wv_match = m_wv_header_regex.match(text);

    int state = StateNormal;
    int bodyStart = 0;

    if (match.hasMatch()) {
        // Determine level first so we can paint full-row background
        QString lvl = match.captured(2).toUpper();
        QTextCharFormat lvlFmt;
        QTextCharFormat msgFmt;
        QTextCharFormat rowFmt; // whole-line background tint

        if (lvl.startsWith('E')) {
            state = StateError;
            lvlFmt = m_fmt_error;
            msgFmt = m_msg_error;
            rowFmt = m_row_error;
        } else if (lvl.startsWith('F')) {
            state = StateFatal;
            lvlFmt = m_fmt_fatal;
            msgFmt = m_msg_fatal;
            rowFmt = m_row_fatal;
        } else if (lvl.startsWith('W')) {
            state = StateWarn;
            lvlFmt = m_fmt_warn;
            msgFmt = m_msg_warn;
            rowFmt = m_row_warn;
        } else if (lvl.startsWith('D')) {
            state = StateDebug;
            lvlFmt = m_fmt_debug;
            msgFmt = m_msg_debug;
            // no row tint for debug
        } else {
            state = StateInfo;
            lvlFmt = m_fmt_info;
            msgFmt = m_msg_info;
            // no row tint for info
        }

        // 1. Paint full-row background tint (only for warn/error/fatal)
        if (rowFmt.background().color().isValid()) {
            setFormat(0, text.length(), rowFmt);
        }

        // 2. Timestamp
        setFormat(match.capturedStart(1), match.capturedLength(1), m_fmt_timestamp);

        // 3. Level badge
        setFormat(match.capturedStart(2), match.capturedLength(2), lvlFmt);

        setCurrentBlockState(state);

        // 4. Body text
        bodyStart = match.capturedEnd();
        if (bodyStart < text.length()) {
            setFormat(bodyStart, text.length() - bodyStart, msgFmt);
        }
    } else if (wv_match.hasMatch()) {
        QString lvl = wv_match.captured(1).toUpper();
        QTextCharFormat lvlFmt;
        QTextCharFormat msgFmt;
        QTextCharFormat rowFmt;

        if (lvl == "ERROR") {
            state = StateError;
            lvlFmt = m_fmt_error;
            msgFmt = m_msg_error;
            rowFmt = m_row_error;
        } else if (lvl.startsWith("WARN")) {
            state = StateWarn;
            lvlFmt = m_fmt_warn;
            msgFmt = m_msg_warn;
            rowFmt = m_row_warn;
        } else if (lvl == "FATAL") {
            state = StateFatal;
            lvlFmt = m_fmt_fatal;
            msgFmt = m_msg_fatal;
            rowFmt = m_row_fatal;
        } else if (lvl == "DEBUG") {
            state = StateDebug;
            lvlFmt = m_fmt_debug;
            msgFmt = m_msg_debug;
        } else {
            state = StateInfo;
            lvlFmt = m_fmt_info;
            msgFmt = m_msg_info;
        }

        // Full-row background tint
        if (rowFmt.background().color().isValid()) {
            setFormat(0, text.length(), rowFmt);
        }

        setFormat(wv_match.capturedStart(0), wv_match.capturedLength(0), lvlFmt);
        setCurrentBlockState(state);

        bodyStart = wv_match.capturedEnd();
        if (bodyStart < text.length()) {
            setFormat(bodyStart, text.length() - bodyStart, msgFmt);
        }
    } else {
        // Multiline stack trace or continued line without timestamp
        int prev = previousBlockState();
        if (prev < StateNormal || prev > StateFatal) {
            prev = StateNormal;
        }
        state = prev;
        setCurrentBlockState(state);

        QTextCharFormat contFmt = m_msg_info;
        QTextCharFormat rowFmt;
        if (prev == StateError) { contFmt = m_msg_error; rowFmt = m_row_error; }
        else if (prev == StateFatal) { contFmt = m_msg_fatal; rowFmt = m_row_fatal; }
        else if (prev == StateWarn)  { contFmt = m_msg_warn;  rowFmt = m_row_warn; }
        else if (prev == StateDebug) { contFmt = m_msg_debug; }

        if (rowFmt.background().color().isValid()) {
            setFormat(0, text.length(), rowFmt);
        }
        setFormat(0, text.length(), contFmt);
    }

    // Determine current-line row background for token inheritance
    QColor rowBg;
    if (state == StateWarn)  rowBg = m_row_warn.background().color();
    else if (state == StateError) rowBg = m_row_error.background().color();
    else if (state == StateFatal) rowBg = m_row_fatal.background().color();

    auto withBg = [&](QTextCharFormat fmt) -> QTextCharFormat {
        if (rowBg.isValid()) fmt.setBackground(rowBg);
        return fmt;
    };

    // PID/TID formatting
    auto pidIt = m_pid_regex.globalMatch(text);
    while (pidIt.hasNext()) {
        auto m = pidIt.next();
        setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_pid));
    }

    // Tag / Package / Bracketed identifiers
    auto tagIt = m_tag_regex.globalMatch(text);
    while (tagIt.hasNext()) {
        auto m = tagIt.next();
        // Skip webview leading [INFO] etc. which was already handled
        if (m.capturedStart() == 0 && wv_match.hasMatch()) continue;
        setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_tag));
    }

    // URLs
    auto urlIt = m_url_regex.globalMatch(text);
    while (urlIt.hasNext()) {
        auto m = urlIt.next();
        setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_url));
    }

    // Hex addresses
    auto hexIt = m_hex_regex.globalMatch(text);
    while (hexIt.hasNext()) {
        auto m = hexIt.next();
        setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_hex));
    }

    // Quoted strings
    auto strIt = m_string_regex.globalMatch(text);
    while (strIt.hasNext()) {
        auto m = strIt.next();
        setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_string));
    }

    // Success / Failure keywords in normal/info/warn lines
    if (state != StateError && state != StateFatal) {
        auto failIt = m_fail_regex.globalMatch(text);
        while (failIt.hasNext()) {
            auto m = failIt.next();
            if (m.capturedStart() >= bodyStart) {
                setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_fail));
            }
        }

        auto succIt = m_success_regex.globalMatch(text);
        while (succIt.hasNext()) {
            auto m = succIt.next();
            if (m.capturedStart() >= bodyStart) {
                setFormat(m.capturedStart(), m.capturedLength(), withBg(m_fmt_success));
            }
        }
    }
}

} // namespace harmonycast
