#pragma once

#include "DeviceInspection.h"
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QComboBox;
class QCheckBox;

namespace harmonycast {

class InspectionPage : public QWidget {
    Q_OBJECT
public:
    explicit InspectionPage(std::shared_ptr<DeviceClient> hdc, QWidget* parent = nullptr,
                            std::shared_ptr<DeviceClient> android = nullptr);
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
    void updateTaskControls();
    void configureResults(InspectionTask task);
    void refreshResults();
    void renderRow(int index, const InspectionResult& row);
    QStringList selectedCleanupPackages() const;

    InspectionRunner* m_runner;
    std::shared_ptr<DeviceClient> m_harmony_client, m_android_client;
    QComboBox* m_platform;
    QComboBox* m_task;
    QWidget* m_cleanup_options;
    std::vector<QCheckBox*> m_cleanup_packages;
    InspectionTask m_results_task{InspectionTask::Inspect};
    InspectionTask m_table_task{InspectionTask::Inspect};
    QLabel* m_port_hint;
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
