#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <vector>

namespace harmonycast {

class LogHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit LogHighlighter(QTextDocument* parent = nullptr);
    ~LogHighlighter() override = default;

protected:
    void highlightBlock(const QString& text) override;

private:
    enum BlockState {
        StateNormal = 0,
        StateDebug = 1,
        StateInfo = 2,
        StateWarn = 3,
        StateError = 4,
        StateFatal = 5
    };

    void initFormats();

    QRegularExpression m_header_regex;
    QRegularExpression m_wv_header_regex;
    QRegularExpression m_tag_regex;
    QRegularExpression m_pid_regex;
    QRegularExpression m_hex_regex;
    QRegularExpression m_url_regex;
    QRegularExpression m_string_regex;
    QRegularExpression m_success_regex;
    QRegularExpression m_fail_regex;

    QTextCharFormat m_fmt_timestamp;
    QTextCharFormat m_fmt_pid;
    QTextCharFormat m_fmt_tag;
    QTextCharFormat m_fmt_debug;
    QTextCharFormat m_fmt_info;
    QTextCharFormat m_fmt_warn;
    QTextCharFormat m_fmt_error;
    QTextCharFormat m_fmt_fatal;

    QTextCharFormat m_msg_debug;
    QTextCharFormat m_msg_info;
    QTextCharFormat m_msg_warn;
    QTextCharFormat m_msg_error;
    QTextCharFormat m_msg_fatal;

    QTextCharFormat m_fmt_hex;
    QTextCharFormat m_fmt_string;
    QTextCharFormat m_fmt_url;
    QTextCharFormat m_fmt_success;
    QTextCharFormat m_fmt_fail;

    // Full-row background tints (warn / error / fatal)
    QTextCharFormat m_row_warn;
    QTextCharFormat m_row_error;
    QTextCharFormat m_row_fatal;
};

using HiLogHighlighter = LogHighlighter;

} // namespace harmonycast
