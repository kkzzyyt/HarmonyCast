#include "harmonycast/InspectionPage.h"
#include "harmonycast/InspectionInventory.h"
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
#include <QSet>
#include <QScrollBar>
#include <algorithm>

namespace harmonycast {

InspectionPage::InspectionPage(std::shared_ptr<DeviceClient> hdc, QWidget* parent, std::shared_ptr<DeviceClient> android)
    : QWidget(parent), m_runner(new InspectionRunner(hdc, this)),
      m_harmony_client(std::move(hdc)), m_android_client(std::move(android)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->addWidget(IconManager::label("导入设备清单，最多 8 台并发巡检；安装、卸载和清理逐台执行", "Muted", true));

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
    inputHeading->addWidget(IconManager::label("设备清单", "CardTitle"));
    inputHeading->addStretch();
    m_import_button = IconManager::button("导入清单");
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
    m_task->addItem("批量安装应用", int(InspectionTask::InstallApplications));
    m_task->addItem("批量卸载应用", int(InspectionTask::UninstallApplications));
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
    m_application_heading = IconManager::label("选择要清理的应用", "Section");
    cleanupLayout->addWidget(m_application_heading);
    for (const auto& package : InspectionReport::cleanupPackages()) {
        auto* checkbox = new QCheckBox(package, m_cleanup_options);
        checkbox->setProperty("cleanupPackage", package);
        checkbox->setChecked(true);
        checkbox->setToolTip(package);
        cleanupLayout->addWidget(checkbox);
        m_cleanup_packages.push_back(checkbox);
    }
    m_application_hint = IconManager::label("清除应用数据目录，包含本地配置和登录状态。仅处理设备已安装的所选应用。", "Muted", true);
    cleanupLayout->addWidget(m_application_hint);
    inputLayout->addWidget(m_cleanup_options);
    m_cleanup_options->hide();
    m_install_options = new QWidget(this);
    auto* installLayout = new QVBoxLayout(m_install_options);
    installLayout->setContentsMargins(0, 0, 0, 0); installLayout->setSpacing(8);
    m_select_install_button = IconManager::button("选择安装包");
    m_select_install_button->setObjectName("InspectionSelectPackages");
    m_select_install_button->setFixedHeight(32);
    installLayout->addWidget(m_select_install_button);
    m_install_file_label = IconManager::label("未选择安装包", "Muted", true);
    m_install_file_label->setTextFormat(Qt::PlainText);
    installLayout->addWidget(m_install_file_label);
    installLayout->addWidget(IconManager::label("按选择顺序逐个安装，所有目标设备使用同一组安装包", "Muted", true));
    inputLayout->addWidget(m_install_options);
    m_install_options->hide();
    m_file_label = IconManager::label("导入 Excel / CSV / TXT 或粘贴 IP", "Muted", true);
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
    m_table->setColumnCount(16);
    m_table->setHorizontalHeaderLabels({"IP / 端口", "设备类型", "清单型号", "科室", "床位", "房间", "状态", "运行时间",
        "MAC 地址", "内存（MiB）", "内存使用率", "硬件型号", "单片机版本", "系统镜像版本", "巡检时间", "备注"});
    m_table->horizontalHeaderItem(11)->setToolTip("设备实际读取的 ICU 硬件型号，与清单型号分开保留");
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setStyleSheet("QTableWidget { border: none; border-radius: 0; } QTableWidget::item { border-bottom: 1px solid #d4dbe5; }");
    m_table->horizontalHeaderItem(9)->setToolTip("已用内存 / 总内存（MiB）");
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    const int widths[] = {148, 112, 112, 160, 80, 80, 88, 128, 164, 144, 112, 120, 136, 352, 176, 320};
    for (int i = 0; i < 16; ++i) m_table->setColumnWidth(i, widths[i]);
    results->addWidget(m_table, 1);
    m_summary = IconManager::label("采集运行时长、网络、内存、型号、单片机和系统版本", "Muted", true);
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
        const QString path = QFileDialog::getOpenFileName(this, "导入设备清单", {}, "设备清单 (*.xlsx *.xls *.csv *.tsv *.txt);;所有文件 (*)");
        if (path.isEmpty()) return;
        QString error;
        if (!importFile(path, &error)) QMessageBox::warning(this, "导入失败", error);
    });
    connect(m_select_install_button, &QPushButton::clicked, this, [this] {
        const bool android = DevicePlatform(m_platform->currentData().toInt()) == DevicePlatform::Android;
        const auto paths = QFileDialog::getOpenFileNames(this, "选择批量安装的应用", {},
            android ? "Android 安装包 (*.apk)" : "鸿蒙安装包 (*.hap *.app)");
        if (paths.isEmpty()) return;
        QString error;
        if (!setInstallationFiles(paths, &error)) QMessageBox::warning(this, "安装包不可用", error);
    });
    connect(m_start_button, &QPushButton::clicked, this, &InspectionPage::startInspection);
    connect(m_stop_button, &QPushButton::clicked, this, [this] {
        m_stop_button->setEnabled(false);
        m_runner->stop();
    });
    connect(m_export_button, &QPushButton::clicked, this, [this] {
        const auto prefix = QString("批量%1_").arg(InspectionReport::taskName(m_results_task));
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
        const auto task = InspectionTask(m_task->currentData().toInt());
        m_platform->setEnabled(!running && (task == InspectionTask::Inspect || task == InspectionTask::InstallApplications));
        m_select_install_button->setEnabled(!running);
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
        m_progress->setFormat("%v / %m 条记录");
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
    if (QFileInfo(path).suffix().compare("txt", Qt::CaseInsensitive) != 0) {
        const auto inventory = InspectionInventory::read(path, DevicePlatform(m_platform->currentData().toInt()), error);
        if (!inventory.errors.isEmpty()) return fail(inventory.errors.mid(0, 5).join('\n'));
        if (inventory.targets.empty()) return fail(error && !error->isEmpty() ? *error : QString("清单中没有有效设备"));
        m_inventory_targets = inventory.targets;
        QStringList endpoints; QSet<QString> seen;
        for (const auto& target : inventory.targets) {
            if (!seen.contains(target.endpoint())) { endpoints << target.endpoint(); seen.insert(target.endpoint()); }
        }
        m_input->setPlainText(endpoints.join('\n'));
        updateInput();
        m_file_label->setText(QFileInfo(path).fileName()); m_file_label->setToolTip(path);
        return true;
    }
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
    m_inventory_targets.clear();
    m_input->setPlainText(text);
    updateInput();
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
    const auto task = InspectionTask(m_task->currentData().toInt());
    const auto deviceCount = m_import.targets.size();
    if (!m_inventory_targets.empty()) {
        QMap<QString, std::vector<const InspectionTarget*>> byEndpoint;
        for (const auto& inventory : m_inventory_targets)
            if (inventory.platform == DevicePlatform(m_platform->currentData().toInt())) byEndpoint[inventory.endpoint()].push_back(&inventory);
        std::vector<InspectionTarget> expanded;
        for (const auto& target : m_import.targets) {
            bool matched = false;
            for (const auto* inventory : byEndpoint.value(target.endpoint())) {
                expanded.push_back(*inventory); matched = true;
                if (task != InspectionTask::Inspect) break;
            }
            if (!matched) expanded.push_back(target);
        }
        m_import.targets = std::move(expanded);
    }
    m_input_summary->setText(QString("有效 %1 台 · 清单 %2 条 · 无效 %3")
        .arg(deviceCount).arg(m_import.targets.size()).arg(m_import.errors.size()));
    m_input_errors->setVisible(!m_import.errors.isEmpty());
    m_input_errors->setText(m_import.errors.mid(0, 3).join('\n') +
        (m_import.errors.isEmpty() ? QString{} : QStringLiteral("\n请修正无效地址后开始巡检")));
    m_input_errors->setToolTip(m_import.errors.join('\n'));
    const bool packagesValid = task == InspectionTask::InstallApplications ?
        !InspectionReport::installationFiles(m_install_files, DevicePlatform(m_platform->currentData().toInt())).isEmpty() :
        task == InspectionTask::Inspect || !selectedCleanupPackages().isEmpty();
    m_start_button->setEnabled(!isRunning() && !m_import.targets.empty() && m_import.errors.isEmpty() && packagesValid);
    refreshResults();
}

QStringList InspectionPage::selectedCleanupPackages() const {
    QStringList selected;
    for (auto* checkbox : m_cleanup_packages) if (checkbox->isChecked()) selected << checkbox->property("cleanupPackage").toString();
    return selected;
}

bool InspectionPage::setInstallationFiles(const QStringList& paths, QString* error) {
    if (isRunning()) { if (error) *error = "请先停止当前任务"; return false; }
    const auto files = InspectionReport::installationFiles(paths, DevicePlatform(m_platform->currentData().toInt()), error);
    if (files.isEmpty()) return false;
    m_install_files = files;
    QStringList names; for (const auto& file : files) names << QFileInfo(file).fileName();
    m_install_file_label->setText(QString("已选 %1 个安装包\n%2%3").arg(files.size()).arg(names.mid(0, 3).join('\n'))
        .arg(files.size() > 3 ? QString("\n另有 %1 个，悬停查看完整路径").arg(files.size() - 3) : QString{}));
    m_install_file_label->setToolTip(files.join('\n'));
    updateInput();
    return true;
}

void InspectionPage::updateTaskControls() {
    const auto task = InspectionTask(m_task->currentData().toInt());
    const bool cleanup = task == InspectionTask::ClearApplicationData || task == InspectionTask::UninstallApplications;
    if (cleanup) m_platform->setCurrentIndex(0);
    m_platform->setEnabled(!isRunning() && !cleanup);
    m_cleanup_options->setVisible(cleanup);
    m_install_options->setVisible(task == InspectionTask::InstallApplications);
    m_application_heading->setText(task == InspectionTask::UninstallApplications ? "选择要卸载的应用" : "选择要清理的应用");
    m_application_hint->setText(task == InspectionTask::UninstallApplications ? "卸载应用并删除应用数据，未安装的应用自动跳过" :
        "清除应用数据目录，包含本地配置和登录状态。仅处理设备已安装的所选应用。");
    m_start_button->setText("开始" + InspectionReport::taskName(task));
    m_input->parentWidget()->setMinimumWidth(task != InspectionTask::Inspect ? 328 : 224);
    updateInput();
}

void InspectionPage::configureResults(InspectionTask task) {
    m_table_task = task;
    m_table->clear();
    m_table->setRowCount(0);
    if (task == InspectionTask::InstallApplications) {
        m_table_install_files = m_install_files;
        QStringList columns{"IP / 端口", "状态", "执行时间"};
        for (const auto& file : m_install_files) columns << QFileInfo(file).fileName();
        columns << "备注";
        m_table->setColumnCount(columns.size()); m_table->setHorizontalHeaderLabels(columns);
        for (int i = 0; i < columns.size(); ++i) {
            m_table->setColumnWidth(i, i == 0 ? 148 : i == 1 ? 88 : i == 2 ? 160 : 240);
            m_table->horizontalHeaderItem(i)->setToolTip(i >= 3 && i < 3 + m_install_files.size() ? m_install_files[i - 3] : columns[i]);
        }
    } else if (task == InspectionTask::ClearApplicationData || task == InspectionTask::UninstallApplications) {
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
        m_table->setColumnCount(16);
        m_table->setHorizontalHeaderLabels({"IP / 端口", "设备类型", "清单型号", "科室", "床位", "房间", "状态", "运行时间",
            "MAC 地址", "内存（MiB）", "内存使用率", "硬件型号", "单片机版本", "系统镜像版本", "巡检时间", "备注"});
        const int widths[] = {148, 112, 112, 160, 80, 80, 88, 128, 164, 144, 112, 120, 136, 352, 176, 320};
        for (int i = 0; i < 16; ++i) m_table->setColumnWidth(i, widths[i]);
        m_table->horizontalHeaderItem(9)->setToolTip("已用内存 / 总内存（MiB）");
        m_table->horizontalHeaderItem(11)->setToolTip("设备实际读取的 ICU 硬件型号，与清单型号分开保留");
    }
}

void InspectionPage::refreshResults() {
    const auto task = InspectionTask(m_task->currentData().toInt());
    if (m_table_task != task || (task == InspectionTask::InstallApplications && m_table_install_files != m_install_files)) configureResults(task);
    const auto& results = m_runner->results();
    bool matches = !results.empty() && results.front().task == task && results.size() == m_import.targets.size();
    for (size_t i = 0; matches && i < results.size(); ++i) {
        matches = results[i].target == m_import.targets[i];
    }
    const auto selected = selectedCleanupPackages();
    if (matches && task != InspectionTask::Inspect) {
        QStringList previous;
        for (const auto& app : results.front().applications) previous << (task == InspectionTask::InstallApplications ? app.sourcePath : app.package);
        matches = previous == (task == InspectionTask::InstallApplications ? m_install_files : selected);
    }
    m_table->setRowCount(static_cast<int>(matches ? results.size() : m_import.targets.size()));
    for (int i = 0; i < m_table->rowCount(); ++i) {
        if (matches) renderRow(i, results[i]);
        else {
            InspectionResult pending; pending.target = m_import.targets[i]; pending.task = task;
            if (task == InspectionTask::InstallApplications) {
                for (const auto& file : m_install_files)
                    pending.applications.push_back({QFileInfo(file).fileName(), ApplicationCleanupStatus::Pending, {}, file});
            } else if (task != InspectionTask::Inspect)
                for (const auto& package : selected) pending.applications.push_back({package, ApplicationCleanupStatus::Pending, {}});
            renderRow(i, pending);
        }
    }
    m_export_button->setText(!results.empty() && !matches ? "导出上次结果" : "导出表格");
    if (matches) updateSummary(false);
    else {
        m_activity->setText("待开始" + InspectionReport::taskName(task) + "，右侧显示待处理设备");
        m_summary->setText(QString("待处理 %1 台%2").arg(m_import.targets.size())
            .arg(task == InspectionTask::InstallApplications ? QString(" · 已选 %1 个安装包").arg(m_install_files.size()) :
                task != InspectionTask::Inspect ? QString(" · 已选 %1 个应用").arg(selected.size()) : QString{}));
    }
}

void InspectionPage::startInspection() {
    if (isRunning() || m_import.targets.empty() || !m_import.errors.isEmpty()) return;
    const auto targets = m_import.targets;
    const auto task = InspectionTask(m_task->currentData().toInt());
    const auto packages = selectedCleanupPackages();
    if (task == InspectionTask::InstallApplications) {
        QString error;
        if (InspectionReport::installationFiles(m_install_files, DevicePlatform(m_platform->currentData().toInt()), &error).isEmpty()) {
            QMessageBox::warning(this, "安装包不可用", error); return;
        }
        const auto confirmation = QString("将在 %1 台设备上依次安装：\n\n%2\n\n确认开始？")
            .arg(targets.size()).arg(m_install_files.join('\n'));
        if (QMessageBox::question(this, "批量安装应用", confirmation, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    } else if (task == InspectionTask::UninstallApplications) {
        if (packages.isEmpty()) return;
        const auto confirmation = QString("将在 %1 台设备上卸载以下应用，并删除应用数据：\n\n%2\n\n未安装的应用会跳过，已完成的卸载无法撤销。确认开始？")
            .arg(targets.size()).arg(packages.join('\n'));
        if (QMessageBox::warning(this, "批量卸载应用", confirmation, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    } else if (task == InspectionTask::ClearApplicationData) {
        if (packages.isEmpty()) return;
        const auto confirmation = QString("将在列表中的 %1 台设备上，清除以下应用的数据目录：\n\n%2\n\n"
            "这会删除应用的本地配置和登录状态。未安装的应用会跳过，已完成的清理无法撤销。\n\n确认开始？")
            .arg(targets.size()).arg(packages.join('\n'));
        if (QMessageBox::warning(this, "批量清理应用数据", confirmation,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    configureResults(task);
    m_results_task = task;
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(targets.size()));
    const bool started = task == InspectionTask::ClearApplicationData ? m_runner->startApplicationCleanup(targets, packages) :
        task == InspectionTask::UninstallApplications ? m_runner->startApplicationUninstall(targets, packages) :
        task == InspectionTask::InstallApplications ? m_runner->startApplicationInstall(targets, m_install_files) : m_runner->start(targets);
    if (!started) return;
    updateSummary(false);
}

void InspectionPage::updateRow(int index) {
    if (index < 0 || index >= static_cast<int>(m_runner->results().size())) return;
    const auto& row = m_runner->results()[index];
    if (row.task != m_table_task) return;
    renderRow(index, row);
    if (isRunning()) {
        const int statusColumn = row.task == InspectionTask::Inspect ? 6 : 1;
        const int horizontalPosition = m_table->horizontalScrollBar()->value();
        m_table->setCurrentCell(index, statusColumn);
        m_table->scrollToItem(m_table->item(index, statusColumn), QAbstractItemView::EnsureVisible);
        m_table->horizontalScrollBar()->setValue(horizontalPosition);
        updateSummary(false);
    }
}

void InspectionPage::renderRow(int index, const InspectionResult& row) {
    const auto cells = InspectionReport::cells(row);
    QStringList visible;
    if (row.task == InspectionTask::InstallApplications) {
        visible = QStringList{row.target.endpoint(), row.status == InspectionStatus::Pending ? "待处理" : cells[2], cells[3]};
        for (const auto& file : m_table_install_files) {
            const auto app = std::find_if(row.applications.begin(), row.applications.end(), [&](const ApplicationCleanupResult& value) { return value.sourcePath == file; });
            visible << (app == row.applications.end() ? QString("未选择") : InspectionReport::applicationCleanupText(*app, row.task));
        }
        visible << row.notes;
    } else if (row.task == InspectionTask::ClearApplicationData || row.task == InspectionTask::UninstallApplications) {
        visible = QStringList{row.target.endpoint(), cells[2], cells[3], cells[4], cells[5], cells[6], cells[7]};
        if (row.status == InspectionStatus::Pending) visible[1] = "待处理";
        const auto packages = InspectionReport::cleanupPackages();
        for (int appIndex = 0; appIndex < packages.size(); ++appIndex) {
            const auto app = std::find_if(row.applications.begin(), row.applications.end(),
                [&](const ApplicationCleanupResult& value) { return value.package == packages[appIndex]; });
            if (app == row.applications.end()) visible[3 + appIndex] = "未选择";
            else if (app->status == ApplicationCleanupStatus::Pending &&
                     (row.status == InspectionStatus::Pending || row.status == InspectionStatus::Connecting || row.status == InspectionStatus::Cleaning || row.status == InspectionStatus::Uninstalling))
                visible[3 + appIndex] = "待执行";
        }
    } else {
        const auto headers = InspectionReport::headers();
        const auto field = [&](const QString& name) { return cells.value(headers.indexOf(name)); };
        const auto totalMemory = field("总内存（MiB）"), usedMemory = field("已用内存（MiB）");
        const QString memory = totalMemory.isEmpty() ? QString{} :
            (usedMemory.isEmpty() ? "—" : QString::number(usedMemory.toDouble(), 'f', 0)) + " / " + QString::number(totalMemory.toDouble(), 'f', 0);
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
        visible = QStringList{row.target.endpoint(), row.target.deviceType, row.target.inventoryModel, row.target.department,
            row.target.bed, row.target.room, cells[2], runtime, mac, memory,
            field("内存使用率（%）").isEmpty() ? QString{} : field("内存使用率（%）") + "%",
            row.model, row.mcuVersion, row.systemVersion, field("巡检时间"), row.notes};
    }
    const int statusColumn = row.task == InspectionTask::Inspect ? 6 : 1;
    if (!row.activity.isEmpty()) visible[statusColumn] = row.activity;
    for (int column = 0; column < visible.size(); ++column) {
        auto* item = m_table->item(index, column);
        if (!item) {
            item = new QTableWidgetItem;
            m_table->setItem(index, column, item);
        }
        item->setText(visible[column]);
        item->setToolTip(visible[column]);
        if (row.task == InspectionTask::Inspect) {
            if (column == 7) item->setToolTip(cells[3]);
            if (column == 8) item->setToolTip(cells[5]);
            if (column == 9 || column == 10) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (column == statusColumn) {
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
    const auto name = InspectionReport::taskName(m_results_task);
    if (!isRunning()) m_activity->setText(name + (stopped ? "已停止，可导出当前结果" : "结束，可导出表格"));
    m_summary->setText(QString("完成 %1 · 部分完成 %2 · 失败 %3 · 已停止 %4").arg(succeeded).arg(partial).arg(failed).arg(cancelled) +
        ((m_results_task == InspectionTask::ClearApplicationData || m_results_task == InspectionTask::UninstallApplications) ? QString(" · 无匹配应用 %1").arg(skipped) : QString{}) +
        (isRunning() ? QString(" · 待处理 %1").arg(std::count_if(m_runner->results().begin(), m_runner->results().end(),
            [](const InspectionResult& row) { return row.status == InspectionStatus::Pending; })) : QString{}));
}

} // namespace harmonycast
