#pragma once

#include <QDialog>
#include "harmonycast/UpdateChecker.h"

namespace harmonycast {

class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(const UpdateInfo& info, QWidget* parent = nullptr);
    ~UpdateDialog() override = default;

    [[nodiscard]] bool wasIgnored() const { return m_ignored; }

private slots:
    void onDownloadClicked();
    void onIgnoreClicked();

private:
    UpdateInfo m_info;
    bool m_ignored{false};

    void setupUi();
};

} // namespace harmonycast
