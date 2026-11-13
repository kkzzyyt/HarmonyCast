#include "harmonycast/InspectionPage.h"
#include "harmonycast/IconManager.h"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QComboBox>
#include <QCheckBox>

namespace harmonycast {

InspectionPage::InspectionPage(std::shared_ptr<DeviceClient> hdc, QWidget* parent, std::shared_ptr<DeviceClient> android)
    : QWidget(parent), m_runner(new InspectionRunner(hdc, this)),
      m_harmony_client(std::move(hdc)), m_android_client(std::move(android)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->addWidget(IconManager::label("导入设备列表，逐台巡检或清理应用数据，并导出执行结果", "Muted", true));

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    auto* inputCard = new QFrame;
    inputCard->setObjectName("InfoCard");
    inputCard->setMinimumWidth(224);
    auto* inputLayout = new QVBoxLayout(inputCard);
    inputLayout->setContentsMargins(16, 16, 16, 16);
    inputLayout->setSpacing(12);
    auto* inputHeading = new QHBoxLayout;
    inputHeading->setSpacing(8);
    inputHeading->addWidget(IconManager::label("巡检目标", "CardTitle"));
    inputHeading->addStretch();
    m_import_button = IconManager::button("导入 TXT");
    m_import_button->setFixedHeight(32);
    m_import_button->setIcon(IconManager::makeSvgIcon(
        "<path d=\"M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><polyline points=\"14 2 14 8 20 8\"/>",
        "#8295a9", "none", 2, 16));
    inputHeading->addWidget(m_import_button);
    inputLayout->addLayout(inputHeading);
    m_task = new QComboBox(this);
    m_task->setObjectName("InspectionTask");
    m_task->addItem("设备巡检", int(InspectionTask::Inspect));
    m_task->addItem("清理应用数据", int(InspectionTask::ClearApplicationData));
    m_task->setFixedHeight(32);
    IconManager::setupComboBoxPopup(m_task, 180);
    inputLayout->addWidget(m_task);
    m_platform = new QComboBox(this);
    m_platform->setObjectName("InspectionPlatform");
    m_platform->addItem("鸿蒙 · HDC", int(DevicePlatform::OpenHarmony));
    if (m_android_client) m_platform->addItem("Android · ADB", int(DevicePlatform::Android));
    m_platform->setFixedHeight(32);
    IconManager::setupComboBoxPopup(m_platform, 180);
    inputLayout->addWidget(m_platform);
    m_cleanup_options = new QWidget(this);
    m_cleanup_options->setObjectName("InspectionCleanupOptions");
    m_cleanup_options->setStyleSheet("QWidget#InspectionCleanupOptions { background: transparent; }");
    auto* cleanupLayout = new QVBoxLayout(m_cleanup_options);
    cleanupLayout->setContentsMargins(0, 0, 0, 0);
    cleanupLayout->setSpacing(8);
    cleanupLayout->addWidget(IconManager::label("选择要清理的应用", "Section"));
    for (const auto& package : InspectionReport::cleanupPackages()) {
        auto* checkbox = new QCheckBox(package, m_cleanup_options);
        checkbox->setProperty("cleanupPackage", package);
        checkbox->setChecked(true);
        checkbox->setToolTip(package);
        cleanupLayout->addWidget(checkbox);
        m_cleanup_packages.push_back(checkbox);
    }
    cleanupLayout->addWidget(IconManager::label("清除应用数据目录，包含本地配置和登录状态。仅处理设备已安装的所选应用。", "Muted", true));
    inputLayout->addWidget(m_cleanup_options);
    m_cleanup_options->hide();
    m_file_label = IconManager::label("导入 TXT 或直接粘贴 IP", "Muted", true);
    m_file_label->setTextFormat(Qt::PlainText);
    inputLayout->addWidget(m_file_label);
    m_input = new QPlainTextEdit;
    m_input->setObjectName("InspectionTargets");
    m_input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_input->setPlaceholderText("192.168.1.10\n192.168.1.11\n192.168.1.12:9000");
    m_input->setLineWrapMode(QPlainTextEdit::NoWrap);
    inputLayout->addWidget(m_input, 1);
    m_port_hint = IconManager::label("默认端口 8710\n支持 IP:端口、# 注释，重复地址自动去重", "Muted", true);
    inputLayout->addWidget(m_port_hint);
    m_input_summary = IconManager::label("有效 0 台", "Muted", true);
    inputLayout->addWidget(m_input_summary);
    m_input_errors = IconManager::label("", "Muted", true);
    m_input_errors->setTextFormat(Qt::PlainText);
    m_input_errors->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_input_errors->setStyleSheet("color: #80652f;");
    inputLayout->addWidget(m_input_errors);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_start_button = IconManager::button("开始巡检", "Primary", "play");
    m_start_button->setObjectName("InspectionStart");
    m_start_button->setFixedHeight(36);
    m_stop_button = IconManager::button("停止");
    m_stop_button->setFixedHeight(36);
    m_stop_button->setEnabled(false);
    m_stop_button->setToolTip("停止巡检，并断开当前巡检设备");
    actions->addWidget(m_start_button, 1);
    actions->addWidget(m_stop_button);
    inputLayout->addLayout(actions);
    splitter->addWidget(inputCard);

    auto* resultCard = new QFrame;
    resultCard->setObjectName("InfoCard");
    resultCard->setMinimumWidth(300);
    auto* results = new QVBoxLayout(resultCard);
    results->setContentsMargins(16, 16, 16, 16);
    results->setSpacing(12);
    auto* resultHeading = new QHBoxLayout;
    resultHeading->setSpacing(8);
    resultHeading->addWidget(IconManager::label("执行结果", "CardTitle"));
    resultHeading->addStretch();
    m_export_button = IconManager::button("导出表格", "", "export");
    m_export_button->setObjectName("InspectionExport");
    m_export_button->setFixedHeight(32);
    m_export_button->setEnabled(false);
    m_export_button->setToolTip("导出 CSV 表格，可用 Excel 或 WPS 打开，包含成功、失败和停止的设备");
    resultHeading->addWidget(m_export_button);
    results->addLayout(resultHeading);
    m_activity = IconManager::label("导入 IP 列表后开始巡检", "Muted", true);
    m_activity->setTextFormat(Qt::PlainText);
    results->addWidget(m_activity);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1);
    m_progress->setValue(0);
    m_progress->setFormat("未开始");
    m_progress->setFixedHeight(4);
    m_progress->setTextVisible(false);
    m_progress->hide();
    m_progress->setAlignment(Qt::AlignCenter);
    m_progress->setStyleSheet(
        "QProgressBar { background: #d1d5dc; border: none; border-radius: 2px; }"
        "QProgressBar::chunk { background: #566b8b; border-radius: 2px; }");
    results->addWidget(m_progress);
    m_table = new QTableWidget;
    m_table->setObjectName("InspectionResults");
    m_table->setColumnCount(10);
    m_table->setHorizontalHeaderLabels({"IP / 端口", "状态", "运行时间", "MAC 地址", "内存（MiB）",
                                       "内存使用率", "型号", "系统版本", "巡检时间", "备注"});
    m_table->horizontalHeaderItem(6)->setToolTip("ICU 硬件与看门狗中的硬件型号");
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setStyleSheet("QTableWidget { border: none; border-radius: 0; } QTableWidget::item { border-bottom: 1px solid #d4dbe5; }");
    m_table->horizontalHeaderItem(4)->setToolTip("已用内存 / 总内存（MiB）");
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const int widths[] = {148, 72, 128, 164, 144, 112, 120, 192, 176, 320};
    for (int i = 0; i < 10; ++i) m_table->setColumnWidth(i, widths[i]);
    results->addWidget(m_table, 1);
    m_summary = IconManager::label("采集运行时长、网络、内存、存储、系统和 CPU 信息", "Muted", true);
    results->addWidget(m_summary);
    splitter->addWidget(resultCard);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({248, 760});
    layout->addWidget(splitter, 1);

    connect(m_input, &QPlainTextEdit::textChanged, this, &InspectionPage::updateInput);
    connect(m_task, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &InspectionPage::updateTaskControls);
    for (auto* checkbox : m_cleanup_packages) connect(checkbox, &QCheckBox::toggled, this, &InspectionPage::updateInput);
    connect(m_platform, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const auto platform = DevicePlatform(m_platform->currentData().toInt());
        m_runner->setClient(platform == DevicePlatform::Android ? m_android_client : m_harmony_client);
        m_port_hint->setText(QString("默认端口 %1\n支持 IP:端口、# 注释，重复地址自动去重").arg(defaultDevicePort(platform)));
        updateInput();
    });
    connect(m_import_button, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, "导入巡检 IP 列表", {}, "IP 列表 (*.txt);;所有文件 (*)");
        if (path.isEmpty()) return;
        QString error;
        if (!importFile(path, &error)) QMessageBox::warning(this, "导入失败", error);
    });
    connect(m_start_button, &QPushButton::clicked, this, &InspectionPage::startInspection);
    connect(m_stop_button, &QPushButton::clicked, this, [this] {
        m_stop_button->setEnabled(false);
        m_runner->stop();
    });
    connect(m_export_button, &QPushButton::clicked, this, [this] {
        const auto prefix = m_results_task == InspectionTask::ClearApplicationData ? QStringLiteral("应用数据清理_") : QStringLiteral("设备巡检_");
        QString path = QFileDialog::getSaveFileName(this, "导出执行结果",
            prefix + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".csv", "CSV 表格 (*.csv)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".csv", Qt::CaseInsensitive)) path += ".csv";
        QString error;
        if (!exportCsv(path, &error)) QMessageBox::warning(this, "导出失败", error);
        else m_activity->setText("已导出：" + path);
    });
    connect(m_runner, &InspectionRunner::runningChanged, this, [this](bool running) {
        m_progress->setVisible(running || !m_runner->results().empty());
        m_input->setReadOnly(running);
        m_platform->setEnabled(!running && m_task->currentData().toInt() == int(InspectionTask::Inspect));
        m_task->setEnabled(!running);
        for (auto* checkbox : m_cleanup_packages) checkbox->setEnabled(!running);
        m_import_button->setEnabled(!running);
        m_stop_button->setEnabled(running);
        m_export_button->setEnabled(!running && !m_runner->results().empty());
        updateInput();
        emit runningChanged(running);
    });
    connect(m_runner, &InspectionRunner::targetChanged, this, &InspectionPage::updateRow);
    connect(m_runner, &InspectionRunner::activityChanged, m_activity, &QLabel::setText);
    connect(m_runner, &InspectionRunner::progressChanged, this, [this](int completed, int total) {
        m_progress->setFormat("%v / %m 台");
        m_progress->setRange(0, total);
        m_progress->setValue(completed);
    });
    connect(m_runner, &InspectionRunner::finished, this, &InspectionPage::updateSummary);
    updateInput();
}

bool InspectionPage::importFile(const QString& path, QString* error) {
    if (error) error->clear();
    auto fail = [&](const QString& reason) { if (error) *error = reason; return false; };
    if (isRunning()) return fail("请先停止当前巡检");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    const auto bytes = file.read(InspectionReport::MaxImportBytes + 1);
    if (file.error() != QFile::NoError) return fail(file.errorString());
    if (bytes.size() > InspectionReport::MaxImportBytes) return fail("IP 列表文件不能超过 1 MiB");
    QString text;
    if (bytes.startsWith(QByteArray::fromHex("fffe")) || bytes.startsWith(QByteArray::fromHex("feff"))) {
        if (bytes.size() % 2 != 0) return fail("UTF-16 文件内容不完整");
        const bool littleEndian = static_cast<unsigned char>(bytes[0]) == 0xff;
        for (int i = 2; i < bytes.size(); i += 2) {
            const auto a = static_cast<unsigned char>(bytes[i]);
            const auto b = static_cast<unsigned char>(bytes[i + 1]);
            text += QChar(static_cast<ushort>(littleEndian ? (a | b << 8) : (a << 8 | b)));
        }
    } else {
        text = QString::fromUtf8(bytes);
        if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    }
    m_input->setPlainText(text);
    m_file_label->setText(QFileInfo(path).fileName());
    m_file_label->setToolTip(path);
    return true;
}

bool InspectionPage::exportCsv(const QString& path, QString* error) const {
    if (isRunning() || m_runner->results().empty()) {
        if (error) *error = "巡检结束或停止后可导出结果";
        return false;
    }
    return InspectionReport::writeCsv(path, m_runner->results(), error);
}

void InspectionPage::updateInput() {
    const QString text = m_input->toPlainText();
    if (text.toUtf8().size() > InspectionReport::MaxImportBytes) {
        m_import = {};
        m_import.errors << "IP 列表不能超过 1 MiB";
    } else {
        m_import = InspectionReport::parseTargets(text, DevicePlatform(m_platform->currentData().toInt()));
    }
    m_input_summary->setText(QString("有效 %1 台 · 重复 %2 · 无效 %3")
        .arg(m_import.targets.size()).arg(m_import.duplicates).arg(m_import.errors.size()));
    m_input_errors->setVisible(!m_import.errors.isEmpty());
    m_input_errors->setText(m_import.errors.mid(0, 3).join('\n') +
        (m_import.errors.isEmpty() ? QString{} : QStringLiteral("\n请修正无效地址后开始巡检")));
    m_input_errors->setToolTip(m_import.errors.join('\n'));
    const bool packagesValid = m_task->currentData().toInt() != int(InspectionTask::ClearApplicationData) || !selectedCleanupPackages().isEmpty();
    m_start_button->setEnabled(!isRunning() && !m_import.targets.empty() && m_import.errors.isEmpty() && packagesValid);
}

QStringList InspectionPage::selectedCleanupPackages() const {
    QStringList selected;
    for (auto* checkbox : m_cleanup_packages) if (checkbox->isChecked()) selected << checkbox->property("cleanupPackage").toString();
    return selected;
}

void InspectionPage::updateTaskControls() {
    const bool cleanup = m_task->currentData().toInt() == int(InspectionTask::ClearApplicationData);
    if (cleanup) m_platform->setCurrentIndex(0);
    m_platform->setEnabled(!isRunning() && !cleanup);
    m_cleanup_options->setVisible(cleanup);
    m_start_button->setText(cleanup ? "开始清理" : "开始巡检");
    m_input->parentWidget()->setMinimumWidth(cleanup ? 328 : 224);
    updateInput();
}

void InspectionPage::configureResults(InspectionTask task) {
    m_results_task = task;
    m_table->clear();
    if (task == InspectionTask::ClearApplicationData) {
        m_table->setColumnCount(7);
        QStringList columns{"IP / 端口", "状态", "执行时间"};
        for (const auto& package : InspectionReport::cleanupPackages()) columns << package.section('.', -1);
        columns << "备注";
        m_table->setHorizontalHeaderLabels(columns);
        const int widths[] = {140, 88, 160, 112, 104, 144, 320};
        for (int i = 0; i < 7; ++i) {
            m_table->setColumnWidth(i, widths[i]);
            m_table->horizontalHeaderItem(i)->setToolTip(i >= 3 && i <= 5 ? InspectionReport::cleanupPackages()[i - 3] : columns[i]);
        }
    } else {
        m_table->setColumnCount(10);
        m_table->setHorizontalHeaderLabels({"IP / 端口", "状态", "运行时间", "MAC 地址", "内存（MiB）",
            "内存使用率", "型号", "系统版本", "巡检时间", "备注"});
        const int widths[] = {148, 72, 128, 164, 144, 112, 120, 192, 176, 320};
        for (int i = 0; i < 10; ++i) m_table->setColumnWidth(i, widths[i]);
        m_table->horizontalHeaderItem(4)->setToolTip("已用内存 / 总内存（MiB）");
        m_table->horizontalHeaderItem(6)->setToolTip("ICU 硬件与看门狗中的硬件型号");
    }
}

void InspectionPage::startInspection() {
    if (isRunning() || m_import.targets.empty() || !m_import.errors.isEmpty()) return;
    const auto targets = m_import.targets;
    const auto task = InspectionTask(m_task->currentData().toInt());
    const auto packages = selectedCleanupPackages();
    if (task == InspectionTask::ClearApplicationData) {
        if (packages.isEmpty()) return;
        const auto confirmation = QString("将在列表中的 %1 台设备上，清除以下应用的数据目录：\n\n%2\n\n"
            "这会删除应用的本地配置和登录状态。未安装的应用会跳过，已完成的清理无法撤销。\n\n确认开始？")
            .arg(targets.size()).arg(packages.join('\n'));
        if (QMessageBox::warning(this, "批量清理应用数据", confirmation,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    configureResults(task);
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(targets.size()));
    const bool started = task == InspectionTask::ClearApplicationData ? m_runner->startApplicationCleanup(targets, packages) :
        m_runner->start(targets);
    if (!started) return;
    for (int i = 0; i < m_table->rowCount(); ++i) updateRow(i);
    m_summary->setText(task == InspectionTask::ClearApplicationData ? "逐台处理，逐应用记录结果；未安装的包跳过，单项失败继续后续任务" :
        "失败设备也会保留记录；缺失信息留空并在备注中说明");
}

void InspectionPage::updateRow(int index) {
    if (index < 0 || index >= static_cast<int>(m_runner->results().size())) return;
    const auto& row = m_runner->results()[index];
    const auto cells = InspectionReport::cells(row);
    QStringList visible;
    if (row.task == InspectionTask::ClearApplicationData) {
        visible = QStringList{row.target.endpoint(), cells[2], cells[3], cells[4], cells[5], cells[6], cells[7]};
    } else {
        const QString memory = cells[12].isEmpty() ? QString{} :
            (cells[13].isEmpty() ? "—" : QString::number(cells[13].toDouble(), 'f', 0)) + " / " + QString::number(cells[12].toDouble(), 'f', 0);
        QString runtime;
        if (row.uptimeSeconds) {
            const auto seconds = *row.uptimeSeconds;
            runtime = QString("%1天 %2:%3:%4").arg(seconds / 86400)
                .arg(seconds / 3600 % 24, 2, 10, QChar('0')).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
        }
        QString mac = cells[5];
        for (const auto& net : row.networks) {
            if (net.ips.contains(row.target.ip) && !net.mac.isEmpty()) { mac = net.mac; break; }
        }
        visible = QStringList{row.target.endpoint(), cells[2], runtime, mac, memory,
            cells[15].isEmpty() ? QString{} : cells[15] + "%",
            row.model, row.systemVersion, cells[20], row.notes};
    }
    for (int column = 0; column < visible.size(); ++column) {
        auto* item = m_table->item(index, column);
        if (!item) {
            item = new QTableWidgetItem;
            m_table->setItem(index, column, item);
        }
        item->setText(visible[column]);
        item->setToolTip(visible[column]);
        if (row.task == InspectionTask::Inspect) {
            if (column == 2) item->setToolTip(cells[3]);
            if (column == 3) item->setToolTip(cells[5]);
            if (column == 4 || column == 5) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (column == 1) {
            const char* color = row.status == InspectionStatus::Succeeded ? "#4b6585" :
                row.status == InspectionStatus::Failed ? "#c63d48" :
                row.status == InspectionStatus::Partial ? "#80652f" : "#617087";
            item->setForeground(QColor(color));
        }
    }
}

void InspectionPage::updateSummary(bool stopped) {
    int succeeded = 0, partial = 0, failed = 0, cancelled = 0, skipped = 0;
    for (const auto& row : m_runner->results()) {
        if (row.status == InspectionStatus::Succeeded) ++succeeded;
        else if (row.status == InspectionStatus::Partial) ++partial;
        else if (row.status == InspectionStatus::Failed) ++failed;
        else if (row.status == InspectionStatus::Cancelled) ++cancelled;
        else if (row.status == InspectionStatus::Skipped) ++skipped;
    }
    const auto name = m_results_task == InspectionTask::ClearApplicationData ? QStringLiteral("清理") : QStringLiteral("巡检");
    m_activity->setText(name + (stopped ? "已停止，可导出当前结果" : "结束，可导出表格"));
    m_summary->setText(QString("完成 %1 · 部分完成 %2 · 失败 %3 · 已停止 %4").arg(succeeded).arg(partial).arg(failed).arg(cancelled) +
        (m_results_task == InspectionTask::ClearApplicationData ? QString(" · 无匹配应用 %1").arg(skipped) : QString{}));
}

} // namespace harmonycast
