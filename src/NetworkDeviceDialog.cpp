#include "harmonycast/NetworkDeviceDialog.h"
#include "harmonycast/IconManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <algorithm>
#include <unordered_set>

namespace harmonycast {

QString NetworkDeviceDialog::savedDevicesFilePath() {
    QString p = QDir::homePath() + "/Library/Application Support/HarmonyCast/saved_devices.json";
    if (!QStandardPaths::isTestModeEnabled() && QFile::exists(p)) {
        return p;
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/HarmonyCast";
    QDir().mkpath(dir);
    return dir + "/saved_devices.json";
}

std::vector<SavedNetworkDevice> NetworkDeviceDialog::loadSavedDevices() {
    std::vector<SavedNetworkDevice> result;
    QFile file(savedDevicesFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) {
        return result;
    }

    QJsonArray arr = doc.array();
    for (const auto& val : arr) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        std::string ip = obj["ip"].toString().trimmed().toStdString();
        if (ip.empty()) continue;

        SavedNetworkDevice dev;
        dev.ip = ip;
        dev.name = obj["name"].toString().trimmed().toStdString();
        dev.port = obj.contains("port") ? obj["port"].toInt(8710) : 8710;
        if (dev.port <= 0 || dev.port > 65535) dev.port = 8710;
        dev.updated_at = obj.value("updated_at").toVariant().toLongLong();
        result.push_back(dev);
    }
    return result;
}

void NetworkDeviceDialog::saveDevice(const std::string& raw_ip, const std::string& name, int default_port) {
    std::string ip = raw_ip;
    int port = default_port;

    // Trim
    while (!ip.empty() && (ip.front() == ' ' || ip.front() == '\t')) ip.erase(0, 1);
    while (!ip.empty() && (ip.back() == ' ' || ip.back() == '\t')) ip.pop_back();

    if (size_t col = ip.find(':'); col != std::string::npos) {
        std::string port_str = ip.substr(col + 1);
        ip = ip.substr(0, col);
        if (!port_str.empty()) {
            try {
                int p = std::stoi(port_str);
                if (p > 0 && p <= 65535) port = p;
            } catch (...) {}
        }
    }

    if (ip.empty()) return;

    auto list = loadSavedDevices();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedNetworkDevice& d) {
        return d.ip == ip;
    }), list.end());

    SavedNetworkDevice new_dev;
    new_dev.ip = ip;
    new_dev.name = name;
    new_dev.port = port;
    new_dev.updated_at = QDateTime::currentSecsSinceEpoch();
    list.insert(list.begin(), new_dev);

    QJsonArray arr;
    for (const auto& d : list) {
        QJsonObject obj;
        obj["ip"] = QString::fromStdString(d.ip);
        obj["name"] = QString::fromStdString(d.name);
        obj["port"] = d.port;
        obj["updated_at"] = static_cast<qint64>(d.updated_at);
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    QFile file(savedDevicesFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

void NetworkDeviceDialog::deleteSavedDevice(const std::string& raw_ip) {
    std::string ip = raw_ip;
    if (size_t col = ip.find(':'); col != std::string::npos) {
        ip = ip.substr(0, col);
    }

    auto list = loadSavedDevices();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SavedNetworkDevice& d) {
        return d.ip == ip;
    }), list.end());

    QJsonArray arr;
    for (const auto& d : list) {
        QJsonObject obj;
        obj["ip"] = QString::fromStdString(d.ip);
        obj["name"] = QString::fromStdString(d.name);
        obj["port"] = d.port;
        obj["updated_at"] = static_cast<qint64>(d.updated_at);
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    QFile file(savedDevicesFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

NetworkDeviceDialog::NetworkDeviceDialog(
    QWidget* parent,
    std::shared_ptr<HdcClient> hdc,
    const std::vector<std::string>& connected_targets,
    const std::string& current_target
)
    : QDialog(parent)
    , m_hdc(std::move(hdc))
    , m_connected_targets(connected_targets)
    , m_current_target(current_target)
{
    setWindowTitle("设备维护与连接中心");
    setMinimumSize(600, 440);
    resize(660, 480);
    initUi();
    refreshData();
}

void NetworkDeviceDialog::initUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    auto* hdr_layout = new QVBoxLayout();
    hdr_layout->setSpacing(6);

    auto* hdr_top = new QHBoxLayout();
    hdr_top->addWidget(IconManager::label("设备维护与连接中心", "CardTitle"));
    hdr_top->addStretch();
    hdr_layout->addLayout(hdr_top);

    m_target_status_label = IconManager::label("默认端口 8710 · 单设备独占连接 · 双击列表项快速切换", "Muted");
    hdr_layout->addWidget(m_target_status_label);
    layout->addLayout(hdr_layout);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"设备 IP", "端口", "状态", "操作"});
    auto* header = m_table->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    header->setDefaultAlignment(Qt::AlignCenter);
    m_table->setColumnWidth(1, 85);
    m_table->setColumnWidth(2, 105);
    m_table->setColumnWidth(3, 140);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &NetworkDeviceDialog::onTableDoubleClicked);
    layout->addWidget(m_table, 1);

    auto* card = new QFrame(this);
    card->setObjectName("InfoCard");
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(14, 12, 14, 12);
    card_layout->setSpacing(10);
    card_layout->addWidget(IconManager::label("快速连接与断开", "Section"));

    auto* input_row = new QHBoxLayout();
    input_row->setSpacing(8);

    m_ip_input = new QLineEdit();
    m_ip_input->setPlaceholderText("设备 IP 地址，例如 10.10.220.41");
    connect(m_ip_input, &QLineEdit::returnPressed, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_ip_input, 1);

    input_row->addWidget(IconManager::label(":", "Muted"));

    m_port_input = new QLineEdit("8710");
    m_port_input->setPlaceholderText("8710");
    m_port_input->setFixedWidth(68);
    m_port_input->setAlignment(Qt::AlignCenter);
    m_port_input->setToolTip("连接端口 (默认 8710，支持自定义修改)");
    connect(m_port_input, &QLineEdit::returnPressed, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_port_input);

    m_btn_disconnect_input = IconManager::button("断开", "Danger", "disconnect");
    m_btn_disconnect_input->setToolTip("断开目标连接并从底层 HDC 中彻底删除 (-remove)");
    connect(m_btn_disconnect_input, &QPushButton::clicked, this, &NetworkDeviceDialog::onDisconnectInput);
    input_row->addWidget(m_btn_disconnect_input);

    m_btn_connect = IconManager::button("连接", "Primary", "connect");
    m_btn_connect->setToolTip("连接该设备并自动存入列表");
    connect(m_btn_connect, &QPushButton::clicked, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_btn_connect);

    card_layout->addLayout(input_row);

    auto* tools_row = new QHBoxLayout();
    tools_row->setSpacing(8);

    m_btn_enable_tcp = IconManager::button("开启 USB 无线调试端口 (8710)", "", "device");
    m_btn_enable_tcp->setToolTip("为当前已连接的 USB 设备开启 TCP 模式 (hdc tmode port 8710)，开启后拔掉数据线即可无线连接");
    connect(m_btn_enable_tcp, &QPushButton::clicked, this, &NetworkDeviceDialog::onEnableUsbTcpMode);
    tools_row->addWidget(m_btn_enable_tcp);

    m_btn_restart_hdc = IconManager::button("重启 HDC 服务", "Secondary", "refresh");
    m_btn_restart_hdc->setToolTip("重启底层 HDC 服务进程 (hdc kill && hdc start -r)，修复服务假死或连接异常");
    connect(m_btn_restart_hdc, &QPushButton::clicked, this, &NetworkDeviceDialog::onRestartHdcService);
    tools_row->addWidget(m_btn_restart_hdc);

    tools_row->addStretch();
    card_layout->addLayout(tools_row);

    m_error_label = IconManager::label("", "Muted");
    m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
    card_layout->addWidget(m_error_label);

    layout->addWidget(card);
}

std::pair<std::string, int> NetworkDeviceDialog::parseInputEndpoint() {
    QString raw_ip = m_ip_input->text().trimmed();
    if (raw_ip.isEmpty()) {
        throw std::runtime_error("请输入设备 IP 地址");
    }

    QString clean_ip = raw_ip;
    int port = 8710;
    if (raw_ip.contains(':')) {
        int idx = raw_ip.lastIndexOf(':');
        clean_ip = raw_ip.left(idx).trimmed();
        QString port_str = raw_ip.mid(idx + 1).trimmed();
        bool ok = false;
        int p = port_str.toInt(&ok);
        if (ok && p >= 1 && p <= 65535) {
            port = p;
        }
    } else if (m_port_input) {
        QString port_str = m_port_input->text().trimmed();
        bool ok = false;
        int p = port_str.toInt(&ok);
        if (ok && p >= 1 && p <= 65535) {
            port = p;
        }
    }
    return {clean_ip.toStdString(), port};
}

void NetworkDeviceDialog::refreshData() {
    auto saved_list = loadSavedDevices();
    std::unordered_set<std::string> saved_ips;
    for (const auto& d : saved_list) {
        saved_ips.insert(d.ip);
    }

    // Merge online network targets from HDC to ensure live devices are never omitted
    std::vector<SavedNetworkDevice> merged = saved_list;
    int conn_count = 0;
    for (const auto& target : m_connected_targets) {
        if (target.find(':') != std::string::npos) {
            conn_count++;
            std::string host = target.substr(0, target.find(':'));
            std::string port_str = target.substr(target.find(':') + 1);
            int port = 8710;
            try { port = std::stoi(port_str); } catch (...) {}
            if (saved_ips.find(host) == saved_ips.end()) {
                SavedNetworkDevice d;
                d.ip = host;
                d.port = port;
                merged.push_back(d);
                saved_ips.insert(host);
            }
        }
    }

    if (m_target_status_label) {
        m_target_status_label->setText(
            QString("在线目标: %1 台 · 端口固定 8710 · 单设备独占连接 · 双击列表项快速切换").arg(conn_count)
        );
    }

    m_displayed_devices = merged;

    if (merged.empty()) {
        m_table->clearSpans();
        m_table->setRowCount(1);
        auto* item = new QTableWidgetItem("（暂无设备，请在下方输入 IP 连接）");
        item->setForeground(QColor("#8295a9"));
        item->setTextAlignment(Qt::AlignCenter);
        item->setFlags(Qt::NoItemFlags);
        m_table->setItem(0, 0, item);
        m_table->setSpan(0, 0, 1, 4);
        return;
    }

    m_table->clearSpans();
    m_table->setRowCount(static_cast<int>(merged.size()));

    for (size_t row = 0; row < merged.size(); ++row) {
        const auto& dev = merged[row];
        std::string ep = dev.ip + ":" + std::to_string(dev.port);
        bool is_conn = false;
        for (const auto& t : m_connected_targets) {
            if (t == ep || t == dev.ip) {
                is_conn = true;
                break;
            }
        }

        auto* item_ip = new QTableWidgetItem(QString::fromStdString(dev.ip));
        item_ip->setForeground(QColor("#e7edf4"));
        item_ip->setTextAlignment(Qt::AlignCenter);
        QFont font = item_ip->font();
        font.setBold(true);
        item_ip->setFont(font);
        item_ip->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_table->setItem(static_cast<int>(row), 0, item_ip);

        auto* item_port = new QTableWidgetItem(QString::number(dev.port));
        item_port->setTextAlignment(Qt::AlignCenter);
        item_port->setForeground(QColor("#8295a9"));
        item_port->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_table->setItem(static_cast<int>(row), 1, item_port);

        auto* item_status = new QTableWidgetItem(is_conn ? "● 已连接" : "○ 未连接");
        item_status->setTextAlignment(Qt::AlignCenter);
        item_status->setForeground(QColor(is_conn ? "#5ce0c1" : "#8295a9"));
        item_status->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        m_table->setItem(static_cast<int>(row), 2, item_status);

        auto* actions = new QWidget();
        auto* act_layout = new QHBoxLayout(actions);
        act_layout->setContentsMargins(4, 0, 4, 0);
        act_layout->setSpacing(6);
        act_layout->setAlignment(Qt::AlignCenter);

        if (is_conn) {
            auto* btn_select = IconManager::iconButton(
                "mirror",
                QString("切换至 %1 并投屏").arg(QString::fromStdString(ep)),
                "IconBtnPrimary",
                34,
                28
            );
            connect(btn_select, &QPushButton::clicked, this, [this, ep]{ selectTarget(ep); });
            act_layout->addWidget(btn_select);

            auto* btn_disconn = IconManager::iconButton(
                "disconnect",
                QString("断开 %1 连接并从 HDC 目标彻底删除 (-remove)").arg(QString::fromStdString(ep)),
                "IconBtnDanger",
                34,
                28
            );
            connect(btn_disconn, &QPushButton::clicked, this, [this, ep]{ disconnectTarget(ep); });
            act_layout->addWidget(btn_disconn);
        } else {
            auto* btn_conn = IconManager::iconButton(
                "connect",
                QString("连接设备 %1").arg(QString::fromStdString(ep)),
                "IconBtnPrimary",
                34,
                28
            );
            connect(btn_conn, &QPushButton::clicked, this, [this, ep]{ connectTarget(ep); });
            act_layout->addWidget(btn_conn);
        }

        auto* btn_del = IconManager::iconButton(
            "delete",
            QString("从设备列表中删除 %1").arg(QString::fromStdString(dev.ip)),
            "IconBtnMuted",
            34,
            28
        );
        connect(btn_del, &QPushButton::clicked, this, [this, ip=dev.ip]{ deleteTarget(ip); });
        act_layout->addWidget(btn_del);

        m_table->setCellWidget(static_cast<int>(row), 3, actions);
        m_table->setRowHeight(static_cast<int>(row), 44);
    }
}

void NetworkDeviceDialog::selectTarget(const std::string& endpoint) {
    accept();
    if (m_on_select) {
        m_on_select(endpoint);
    } else if (m_on_connect) {
        m_on_connect(endpoint);
    }
}

void NetworkDeviceDialog::connectTarget(const std::string& endpoint) {
    accept();
    if (m_on_connect) {
        m_on_connect(endpoint);
    }
}

void NetworkDeviceDialog::onTableDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    if (row < 0 || row >= static_cast<int>(m_displayed_devices.size())) {
        return;
    }
    const auto& dev = m_displayed_devices[row];
    std::string ep = dev.ip + ":" + std::to_string(dev.port);
    connectTarget(ep);
}

void NetworkDeviceDialog::onConnectAndSave() {
    try {
        auto [clean_ip, port] = parseInputEndpoint();
        saveDevice(clean_ip, "", port);
        m_error_label->setText("");
        std::string ep = clean_ip + ":" + std::to_string(port);
        connectTarget(ep);
    } catch (const std::exception& e) {
        m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
        m_error_label->setText(QString::fromUtf8(e.what()));
    }
}

void NetworkDeviceDialog::onDisconnectInput() {
    try {
        auto [clean_ip, port] = parseInputEndpoint();
        std::string ep = clean_ip + ":" + std::to_string(port);
        disconnectTarget(ep);
    } catch (const std::exception& e) {
        m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
        m_error_label->setText(QString::fromUtf8(e.what()));
    }
}

void NetworkDeviceDialog::deleteTarget(const std::string& ip) {
    auto reply = QMessageBox::question(
        this,
        "确认删除设备",
        QString("确定要从列表中删除设备 %1 吗？\n删除后该设备将不再出现在设备列表和快捷下拉菜单中。").arg(QString::fromStdString(ip)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply != QMessageBox::Yes) {
        return;
    }
    deleteSavedDevice(ip);
    m_error_label->setStyleSheet("color: #79e6c9; font-size: 12px;");
    m_error_label->setText(QString("已删除设备 %1").arg(QString::fromStdString(ip)));
    refreshData();
}

void NetworkDeviceDialog::disconnectTarget(const std::string& endpoint) {
    if (m_on_disconnect) {
        m_on_disconnect(endpoint);
    } else if (m_hdc) {
        m_hdc->executeAsync({"tconn", endpoint, "-remove"}, "", [](const CommandResult&){});
    }

    std::string clean_ip = endpoint;
    if (size_t col = clean_ip.find(':'); col != std::string::npos) {
        clean_ip = clean_ip.substr(0, col);
    }
    m_connected_targets.erase(
        std::remove_if(m_connected_targets.begin(), m_connected_targets.end(),
            [&](const std::string& t) {
                return t == endpoint || t == clean_ip;
            }),
        m_connected_targets.end()
    );

    m_error_label->setStyleSheet("color: #79e6c9; font-size: 12px;");
    m_error_label->setText(QString("已执行 hdc tconn %1 -remove").arg(QString::fromStdString(endpoint)));
    refreshData();
}

void NetworkDeviceDialog::onEnableUsbTcpMode() {
    if (!m_hdc) return;

    m_error_label->setStyleSheet("color: #f3c969; font-size: 12px;");
    m_error_label->setText("正在检测 USB 设备并开启无线调试模式 (8710端口)…");

    auto devs = m_hdc->listDevices();
    std::string usb_serial;
    for (const auto& d : devs) {
        if (d.serial.find(':') == std::string::npos && d.state == DeviceState::Connected) {
            usb_serial = d.serial;
            break;
        }
    }

    if (usb_serial.empty()) {
        for (const auto& d : devs) {
            if (d.serial.find(':') == std::string::npos && d.state == DeviceState::Unauthorized) {
                m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
                m_error_label->setText("USB 设备未授权：请先在设备屏幕上点击【允许USB调试】或【始终允许】！");
                return;
            }
        }
        m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
        m_error_label->setText("未检测到已连接的 USB 设备。请先用 USB 数据线将设备与电脑连接！");
        return;
    }

    bool ok = m_hdc->enableTcpMode(usb_serial, 8710);
    if (ok) {
        m_error_label->setStyleSheet("color: #79e6c9; font-size: 12px;");
        m_error_label->setText(QString("成功为设备开启 8710 无线调试端口！拔出 USB 线后输入 IP 即可无线投屏。"));
        QMessageBox::information(this, "无线调试开启成功",
            QString("已成功为设备开启 8710 无线调试端口！\n\n设备标识: %1\n\n使用指南：\n1. 现在您可以安全拔下 USB 数据线；\n2. 确保手机与电脑在同一 Wi-Fi 局域网；\n3. 查看手机【设置 -> WLAN / 关于手机】中的 IP 地址并在上方输入连接即可。")
            .arg(QString::fromStdString(usb_serial)));
    } else {
        m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
        m_error_label->setText("开启无线调试模式失败，请确认设备已授权或尝试点击【重启 HDC 服务】。");
    }
}

void NetworkDeviceDialog::onRestartHdcService() {
    if (!m_hdc) return;

    m_error_label->setStyleSheet("color: #f3c969; font-size: 12px;");
    m_error_label->setText("正在重启底层 HDC 服务…");

    bool ok = m_hdc->restartServer();
    if (ok) {
        m_error_label->setStyleSheet("color: #79e6c9; font-size: 12px;");
        m_error_label->setText("HDC 服务已成功重启并重载设备连接。");
    } else {
        m_error_label->setStyleSheet("color: #ff9f9f; font-size: 12px;");
        m_error_label->setText("重启 HDC 服务失败，请检查 HDC 权限或进程。");
    }
    refreshData();
}

} // namespace harmonycast
