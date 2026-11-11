#pragma once

#include "DeviceInspection.h"
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace harmonycast {

class InspectionPage : public QWidget {
    Q_OBJECT
public:
    explicit InspectionPage(std::shared_ptr<HdcClient> hdc, QWidget* parent = nullptr);
    [[nodiscard]] bool isRunning() const { return m_runner->isRunning(); }
    bool importFile(const QString& path, QString* error = nullptr);
    bool exportCsv(const QString& path, QString* error = nullptr) const;

signals:
    void runningChanged(bool running);

private:
    void updateInput();
    void startInspection();
    void updateRow(int index);
    void updateSummary(bool stopped);

    InspectionRunner* m_runner;
    InspectionImport m_import;
    QPlainTextEdit* m_input;
    QLabel* m_file_label;
    QLabel* m_input_summary;
    QLabel* m_input_errors;
    QLabel* m_activity;
    QLabel* m_summary;
    QProgressBar* m_progress;
    QTableWidget* m_table;
    QPushButton* m_import_button;
    QPushButton* m_start_button;
    QPushButton* m_stop_button;
    QPushButton* m_export_button;
};

} // namespace harmonycast
