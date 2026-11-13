#pragma once

#include "Types.h"
#include "HdcClient.h"
#include "DeviceCatalog.h"
#include "AdbClient.h"
#include <QComboBox>

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace harmonycast {

class NetworkDeviceDialog : public QDialog {
    Q_OBJECT
public:
    explicit NetworkDeviceDialog(
        QWidget* parent = nullptr,
        std::shared_ptr<DeviceClient> hdc = nullptr,
        const std::vector<std::string>& connected_targets = {},
        const std::string& current_target = "",
        std::shared_ptr<AdbClient> adb = nullptr
    );
    ~NetworkDeviceDialog() override = default;

    // Static persistence helpers
    static QString savedDevicesFilePath();
    static std::vector<SavedNetworkDevice> loadSavedDevices();
    static void saveDevice(const std::string& ip, const std::string& name = "", int port = 0,
                           DevicePlatform platform = DevicePlatform::OpenHarmony);
    static void deleteSavedDevice(const std::string& ip);

    // Callbacks
    void setOnConnect(std::function<void(const std::string&)> cb) { m_on_connect = std::move(cb); }
    void setOnDisconnect(std::function<void(const std::string&)> cb) { m_on_disconnect = std::move(cb); }
    void setOnSelect(std::function<void(const std::string&)> cb) { m_on_select = std::move(cb); }

    void refreshData();
    void setDevices(const std::vector<Device>& devices, const std::string& current_target = "");

signals:
    void devicesChanged();
    void refreshRequested();

private slots:
    void onTableDoubleClicked(int row, int column);
    void onConnectAndSave();
    void onDisconnectInput();
    void filterDevices(const QString& query);

private:
    void initUi();
    std::pair<std::string, int> parseInputEndpoint();
    void selectTarget(const std::string& endpoint);
    void connectTarget(const std::string& endpoint);
    void disconnectTarget(const std::string& endpoint);
    void deleteTarget(const std::string& endpoint);

    std::shared_ptr<DeviceClient> m_hdc;
    std::shared_ptr<AdbClient> m_adb;
    QComboBox* m_platform{nullptr};
    std::vector<Device> m_live_devices;
    std::string m_current_target;

    std::vector<DeviceListEntry> m_displayed_devices;

    QLabel* m_target_status_label{nullptr};
    QTableWidget* m_table{nullptr};
    QLineEdit* m_search{nullptr};
    QLineEdit* m_ip_input{nullptr};
    QLineEdit* m_port_input{nullptr};
    QPushButton* m_btn_disconnect_input{nullptr};
    QPushButton* m_btn_connect{nullptr};
    QLabel* m_error_label{nullptr};

    std::function<void(const std::string&)> m_on_connect;
    std::function<void(const std::string&)> m_on_disconnect;
    std::function<void(const std::string&)> m_on_select;
};

using DeviceHubDialog = NetworkDeviceDialog;

} // namespace harmonycast
