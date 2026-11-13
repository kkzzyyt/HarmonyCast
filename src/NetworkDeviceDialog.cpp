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
#include <QSaveFile>
#include <QScrollBar>
#include <QHostAddress>
#include <QIntValidator>
#include <QSignalBlocker>
#include <algorithm>
#include <stdexcept>

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
        const auto platform = obj.value("platform").toString();
        if (!platform.isEmpty() && platform != "android" && platform != "openharmony") continue;
        dev.platform = platform == "android" ? DevicePlatform::Android : DevicePlatform::OpenHarmony;
        dev.ip = ip;
        dev.name = obj["name"].toString().trimmed().toStdString();
        dev.port = obj.contains("port") ? obj["port"].toInt(defaultDevicePort(dev.platform)) : defaultDevicePort(dev.platform);
        if (dev.port <= 0 || dev.port > 65535) dev.port = defaultDevicePort(dev.platform);
        dev.updated_at = obj.value("updated_at").toVariant().toLongLong();
        result.push_back(dev);
    }
    return result;
}

namespace {

void writeSavedDevices(const std::vector<SavedNetworkDevice>& devices) {
    QJsonArray array;
    for (const auto& device : devices) {
        array.append(QJsonObject{{"ip", QString::fromStdString(device.ip)},
                                 {"name", QString::fromStdString(device.name)},
                                 {"port", device.port}, {"updated_at", static_cast<qint64>(device.updated_at)},
                                 {"platform", device.platform == DevicePlatform::Android ? "android" : "openharmony"}});
    }
    QSaveFile file(NetworkDeviceDialog::savedDevicesFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        const auto bytes = QJsonDocument(array).toJson(QJsonDocument::Indented);
        if (file.write(bytes) == bytes.size()) file.commit();
    }
}

std::pair<std::string, int> splitEndpoint(const std::string& raw, int defaultPort) {
    QString host = QString::fromStdString(raw).trimmed();
    int port = defaultPort > 0 && defaultPort <= 65535 ? defaultPort : 8710;
    const int colon = host.lastIndexOf(':');
    if (colon > 0) {
        bool ok = false;
        const int parsed = host.mid(colon + 1).toInt(&ok);
        if (!ok || parsed <= 0 || parsed > 65535 || host.left(colon).contains(':')) return {};
        port = parsed;
        host = host.left(colon).trimmed();
    }
    return {host.toStdString(), port};
}

} // namespace

void NetworkDeviceDialog::saveDevice(const std::string& raw_ip, const std::string& name, int default_port, DevicePlatform platform) {
    if (isAndroidDevice(raw_ip)) platform = DevicePlatform::Android;
    const auto [ip, port] = splitEndpoint(deviceSerial(raw_ip), default_port > 0 ? default_port : defaultDevicePort(platform));
    if (ip.empty()) return;
    auto devices = loadSavedDevices();
    QString preservedName = QString::fromStdString(name).trimmed();
    for (const auto& saved : devices) {
        if (saved.platform == platform && saved.ip == ip && saved.port == port && preservedName.isEmpty()) preservedName = QString::fromStdString(saved.name);
    }
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const SavedNetworkDevice& saved) {
        return saved.platform == platform && saved.ip == ip && saved.port == port;
    }), devices.end());
    SavedNetworkDevice saved;
    saved.platform = platform;
    saved.ip = ip;
    saved.port = port;
    saved.name = preservedName.toStdString();
    saved.updated_at = QDateTime::currentSecsSinceEpoch();
    devices.insert(devices.begin(), std::move(saved));
    writeSavedDevices(devices);
}

void NetworkDeviceDialog::deleteSavedDevice(const std::string& raw_ip) {
    const auto platform = isAndroidDevice(raw_ip) ? DevicePlatform::Android : DevicePlatform::OpenHarmony;
    const auto serial = deviceSerial(raw_ip);
    const auto [ip, port] = splitEndpoint(serial, defaultDevicePort(platform));
    if (ip.empty()) return;
    const bool exactEndpoint = serial.find(':') != std::string::npos;
    auto devices = loadSavedDevices();
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const SavedNetworkDevice& saved) {
        return saved.platform == platform && saved.ip == ip && (!exactEndpoint || saved.port == port);
    }), devices.end());
    writeSavedDevices(devices);
}

NetworkDeviceDialog::NetworkDeviceDialog(
    QWidget* parent,
    std::shared_ptr<DeviceClient> hdc,
    const std::vector<std::string>& connected_targets,
    const std::string& current_target,
    std::shared_ptr<AdbClient> adb
)
    : QDialog(parent)
    , m_hdc(std::move(hdc))
    , m_adb(std::move(adb))
    , m_current_target(current_target)
{
    for (const auto& target : connected_targets) {
        const auto parsed = isAndroidDevice(target) ? AdbClient::parseDeviceList(deviceSerial(target) + " device") :
            HdcClient::parseDeviceList(target + " TCP Connected");
        if (!parsed.empty()) m_live_devices.push_back(parsed.front());
    }
    setWindowTitle("设备中心");
    setMinimumSize(600, 440);
    resize(720, 520);
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
    hdr_top->addWidget(IconManager::label("设备中心", "CardTitle"));
    hdr_top->addStretch();
    auto* refresh = IconManager::button("刷新", "", "refresh");
    refresh->setFixedHeight(32);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshData(); emit refreshRequested(); });
    hdr_top->addWidget(refresh);
    hdr_layout->addLayout(hdr_top);

    m_target_status_label = IconManager::label("默认端口 8710 · 双击切换设备", "Muted", true);
    hdr_layout->addWidget(m_target_status_label);
    layout->addLayout(hdr_layout);

    m_search = new QLineEdit(this);
    m_search->setObjectName("DeviceHubSearch");
    m_search->setPlaceholderText("搜索 IP、端口、名称或状态");
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(32);
    m_search->addAction(IconManager::getIcon("search"), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this, &NetworkDeviceDialog::filterDevices);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            if (!m_table->isRowHidden(row) && row < static_cast<int>(m_displayed_devices.size())) {
                onTableDoubleClicked(row, 0);
                return;
            }
        }
    });
    layout->addWidget(m_search);

    m_table = new QTableWidget(this);
    m_table->setObjectName("DeviceHubTable");
    IconManager::setupScrollArea(m_table);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"设备 / IP", "端口", "状态", "操作"});
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
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
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

    m_platform = new QComboBox(this);
    m_platform->setObjectName("DeviceHubPlatform");
    m_platform->addItem("鸿蒙", int(DevicePlatform::OpenHarmony));
    m_platform->addItem("Android", int(DevicePlatform::Android));
    m_platform->setFixedSize(104, 32);
    IconManager::setupComboBoxPopup(m_platform, 120);
    input_row->addWidget(m_platform);

    m_ip_input = new QLineEdit();
    m_ip_input->setObjectName("DeviceHubAddress");
    m_ip_input->setPlaceholderText("设备 IP 地址，例如 10.10.220.41");
    connect(m_ip_input, &QLineEdit::returnPressed, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_ip_input, 1);

    input_row->addWidget(IconManager::label(":", "Muted"));

    m_port_input = new QLineEdit("8710");
    m_port_input->setObjectName("DeviceHubPort");
    m_port_input->setValidator(new QIntValidator(1, 65535, m_port_input));
    m_port_input->setPlaceholderText("8710");
    m_port_input->setFixedWidth(68);
    m_port_input->setAlignment(Qt::AlignCenter);
    m_port_input->setToolTip("连接端口 (默认 8710，支持自定义修改)");
    connect(m_port_input, &QLineEdit::returnPressed, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_port_input);
    connect(m_platform, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const int port = defaultDevicePort(DevicePlatform(m_platform->currentData().toInt()));
        if (m_port_input->text() == "8710" || m_port_input->text() == "5555" || m_port_input->text().isEmpty())
            m_port_input->setText(QString::number(port));
        m_port_input->setPlaceholderText(QString::number(port));
        m_port_input->setToolTip(QString("连接端口，默认 %1").arg(port));
        filterDevices(m_search->text());
    });
    if (isAndroidDevice(m_current_target)) m_platform->setCurrentIndex(1);

    m_btn_disconnect_input = IconManager::button("断开", "Danger", "disconnect");
    m_btn_disconnect_input->setToolTip("断开目标设备的网络调试连接");
    connect(m_btn_disconnect_input, &QPushButton::clicked, this, &NetworkDeviceDialog::onDisconnectInput);
    input_row->addWidget(m_btn_disconnect_input);

    m_btn_connect = IconManager::button("连接", "Primary", "connect");
    m_btn_connect->setToolTip("连接该设备并自动存入列表");
    connect(m_btn_connect, &QPushButton::clicked, this, &NetworkDeviceDialog::onConnectAndSave);
    input_row->addWidget(m_btn_connect);

    card_layout->addLayout(input_row);

    m_error_label = IconManager::label("", "Muted");
    m_error_label->setStyleSheet("color: #be4251; font-size: 12px;");
    card_layout->addWidget(m_error_label);

    layout->addWidget(card);
}

std::pair<std::string, int> NetworkDeviceDialog::parseInputEndpoint() {
    const QString raw = m_ip_input->text().trimmed();
    if (raw.isEmpty()) throw std::runtime_error("请输入设备 IP 地址");
    QString host = raw;
    QString portText = m_port_input->text().trimmed();
    const int colon = raw.lastIndexOf(':');
    if (colon >= 0) {
        host = raw.left(colon).trimmed();
        portText = raw.mid(colon + 1).trimmed();
    }
    QHostAddress address;
    if (!address.setAddress(host) || address.protocol() != QAbstractSocket::IPv4Protocol)
        throw std::runtime_error("请输入有效的 IPv4 地址");
    bool valid = false;
    const int port = portText.toInt(&valid);
    if (!valid || port < 1 || port > 65535) throw std::runtime_error("端口必须为 1–65535");
    return {address.toString().toStdString(), port};
}

void NetworkDeviceDialog::setDevices(const std::vector<Device>& devices, const std::string& current_target) {
    m_live_devices = devices;
    m_current_target = current_target;
    refreshData();
}

void NetworkDeviceDialog::refreshData() {
    QString selected;
    if (const auto* item = m_table->item(m_table->currentRow(), 0)) selected = item->data(Qt::UserRole).toString();
    if (selected.isEmpty()) selected = QString::fromStdString(m_current_target);
    const int oldScroll = m_table->verticalScrollBar()->value();
    m_displayed_devices = DeviceCatalog::merge(m_live_devices, loadSavedDevices());
    const QSignalBlocker blocker(m_table);
    m_table->setUpdatesEnabled(false);
    m_table->clearSpans();
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(m_displayed_devices.size()));
    for (int row = 0; row < static_cast<int>(m_displayed_devices.size()); ++row) {
        const auto& entry = m_displayed_devices[row];
        const auto& device = entry.device;
        const auto endpoint = device.id();
        const QString serial = QString::fromStdString(device.serial);
        const bool connected = device.state == DeviceState::Connected;
        QString display = device.is_network ? QString::fromStdString(device.ip) : "USB: " + serial;
        if (!entry.name.isEmpty() && entry.name != display && entry.name != serial) display += "\n" + entry.name;
        if (device.platform == DevicePlatform::Android) display = "Android · " + display;
        auto* address = new QTableWidgetItem(display);
        address->setData(Qt::UserRole, QString::fromStdString(endpoint));
        address->setData(Qt::UserRole + 1, entry.searchText());
        address->setToolTip(entry.label());
        m_table->setItem(row, 0, address);
        auto* port = new QTableWidgetItem(device.is_network ? QString::number(device.port) : QString{});
        port->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 1, port);
        auto* state = new QTableWidgetItem(entry.stateText());
        state->setTextAlignment(Qt::AlignCenter);
        state->setForeground(QColor(device.state == DeviceState::Unauthorized ? "#a25a60" : connected ? "#4b6585" : "#617087"));
        m_table->setItem(row, 2, state);
        auto* actions = new QWidget();
        auto* actionLayout = new QHBoxLayout(actions);
        actionLayout->setContentsMargins(4, 0, 4, 0);
        actionLayout->setSpacing(6);
        actionLayout->setAlignment(Qt::AlignCenter);
        if (connected) {
            auto* select = IconManager::iconButton("mirror", "切换至 " + serial, "IconBtnPrimary", 32, 28);
            connect(select, &QPushButton::clicked, this, [this, endpoint] { selectTarget(endpoint); });
            actionLayout->addWidget(select);
        } else if (device.is_network) {
            auto* connectButton = IconManager::iconButton("connect", "连接 " + serial, "IconBtnPrimary", 32, 28);
            connect(connectButton, &QPushButton::clicked, this, [this, endpoint] { connectTarget(endpoint); });
            actionLayout->addWidget(connectButton);
        }
        if (device.is_network && (connected || device.state == DeviceState::Offline || device.state == DeviceState::Unauthorized)) {
            auto* disconnectButton = IconManager::iconButton("disconnect", "断开 " + serial, "IconBtnDanger", 32, 28);
            connect(disconnectButton, &QPushButton::clicked, this, [this, endpoint] { disconnectTarget(endpoint); });
            actionLayout->addWidget(disconnectButton);
        }
        if (entry.saved) {
            auto* remove = IconManager::iconButton("delete", "删除保存记录 " + serial, "IconBtnMuted", 32, 28);
            connect(remove, &QPushButton::clicked, this, [this, endpoint] { deleteTarget(endpoint); });
            actionLayout->addWidget(remove);
        }
        m_table->setCellWidget(row, 3, actions);
        m_table->setRowHeight(row, entry.name.isEmpty() ? 40 : 48);
        if (QString::fromStdString(endpoint) == selected) m_table->setCurrentCell(row, 0);
    }
    filterDevices(m_search->text());
    m_table->verticalScrollBar()->setValue(oldScroll);
    m_table->setUpdatesEnabled(true);
}

void NetworkDeviceDialog::filterDevices(const QString& query) {
    int matched = 0, connected = 0;
    for (int row = 0; row < static_cast<int>(m_displayed_devices.size()); ++row) {
        const auto& entry = m_displayed_devices[row];
        const bool visible = DeviceCatalog::matchesSearchText(entry.searchText(), query);
        m_table->setRowHidden(row, !visible);
        if (visible) ++matched;
        if (entry.device.state == DeviceState::Connected) ++connected;
    }
    m_target_status_label->setText(QString("匹配 %1 / %2 台 · 已连接 %3 台 · 默认端口 %4")
        .arg(matched).arg(m_displayed_devices.size()).arg(connected)
        .arg(m_platform ? defaultDevicePort(DevicePlatform(m_platform->currentData().toInt())) : 8710));
    m_target_status_label->setToolTip(m_displayed_devices.empty() ? "暂无设备，请在下方输入 IP 连接" :
        matched == 0 ? "没有匹配的设备" : "按 IP、端口、名称和状态匹配；双击或搜索后按回车切换");
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
    const auto& device = m_displayed_devices[row].device;
    if (device.state == DeviceState::Connected) selectTarget(device.id());
    else if (device.is_network) connectTarget(device.id());
}

void NetworkDeviceDialog::onConnectAndSave() {
    try {
        auto [clean_ip, port] = parseInputEndpoint();
        const auto platform = DevicePlatform(m_platform->currentData().toInt());
        saveDevice(clean_ip, "", port, platform);
        emit devicesChanged();
        m_error_label->setText("");
        std::string ep = deviceId(platform, clean_ip + ":" + std::to_string(port));
        connectTarget(ep);
    } catch (const std::exception& e) {
        m_error_label->setStyleSheet("color: #be4251; font-size: 12px;");
        m_error_label->setText(QString::fromUtf8(e.what()));
    }
}

void NetworkDeviceDialog::onDisconnectInput() {
    try {
        auto [clean_ip, port] = parseInputEndpoint();
        std::string ep = deviceId(DevicePlatform(m_platform->currentData().toInt()), clean_ip + ":" + std::to_string(port));
        disconnectTarget(ep);
    } catch (const std::exception& e) {
        m_error_label->setStyleSheet("color: #be4251; font-size: 12px;");
        m_error_label->setText(QString::fromUtf8(e.what()));
    }
}

void NetworkDeviceDialog::deleteTarget(const std::string& endpoint) {
    const auto reply = QMessageBox::question(this, "删除保存记录",
        QString("删除 %1 的保存记录？\n仍在线的设备会继续显示，断开后不再保留。").arg(QString::fromStdString(endpoint)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    deleteSavedDevice(endpoint);
    m_error_label->setStyleSheet("color: #4b6585; font-size: 12px;");
    m_error_label->setText(QString("已删除保存记录 %1").arg(QString::fromStdString(endpoint)));
    refreshData();
    emit devicesChanged();
}

void NetworkDeviceDialog::disconnectTarget(const std::string& endpoint) {
    if (m_on_disconnect) m_on_disconnect(endpoint);
    else if (isAndroidDevice(endpoint) && m_adb) m_adb->executeAsync({"disconnect", deviceSerial(endpoint)}, "", [](const CommandResult&) {});
    else if (m_hdc) m_hdc->executeAsync({"tconn", endpoint, "-remove"}, "", [](const CommandResult&) {});
    m_error_label->setStyleSheet("color: #4b6585; font-size: 12px;");
    m_error_label->setText(QString("正在断开 %1").arg(QString::fromStdString(endpoint)));
    // Live state is owned by DeviceManager. Keep both lists on that snapshot
    // until the command completes and discovery reports the actual result.
    emit refreshRequested();
}

} // namespace harmonycast
