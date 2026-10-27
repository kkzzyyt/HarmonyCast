#pragma once

#include "Types.h"
#include "HdcClient.h"

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

struct SavedNetworkDevice {
    std::string ip;
    std::string name;
    int port{8710};
    int64_t updated_at{0};
};

class NetworkDeviceDialog : public QDialog {
    Q_OBJECT
public:
    explicit NetworkDeviceDialog(
        QWidget* parent = nullptr,
        std::shared_ptr<HdcClient> hdc = nullptr,
        const std::vector<std::string>& connected_targets = {},
        const std::string& current_target = ""
    );
    ~NetworkDeviceDialog() override = default;

    // Static persistence helpers
    static QString savedDevicesFilePath();
    static std::vector<SavedNetworkDevice> loadSavedDevices();
    static void saveDevice(const std::string& ip, const std::string& name = "", int port = 8710);
    static void deleteSavedDevice(const std::string& ip);

    // Callbacks
    void setOnConnect(std::function<void(const std::string&)> cb) { m_on_connect = std::move(cb); }
    void setOnDisconnect(std::function<void(const std::string&)> cb) { m_on_disconnect = std::move(cb); }
    void setOnSelect(std::function<void(const std::string&)> cb) { m_on_select = std::move(cb); }

    void refreshData();

private slots:
    void onTableDoubleClicked(int row, int column);
    void onConnectAndSave();
    void onDisconnectInput();
    void onEnableUsbTcpMode();
    void onRestartHdcService();

private:
    void initUi();
    std::pair<std::string, int> parseInputEndpoint();
    void selectTarget(const std::string& endpoint);
    void connectTarget(const std::string& endpoint);
    void disconnectTarget(const std::string& endpoint);
    void deleteTarget(const std::string& ip);

    std::shared_ptr<HdcClient> m_hdc;
    std::vector<std::string> m_connected_targets;
    std::string m_current_target;

    std::vector<SavedNetworkDevice> m_displayed_devices;

    QLabel* m_target_status_label{nullptr};
    QTableWidget* m_table{nullptr};
    QLineEdit* m_ip_input{nullptr};
    QLineEdit* m_port_input{nullptr};
    QPushButton* m_btn_disconnect_input{nullptr};
    QPushButton* m_btn_connect{nullptr};
    QPushButton* m_btn_enable_tcp{nullptr};
    QPushButton* m_btn_restart_hdc{nullptr};
    QLabel* m_error_label{nullptr};

    std::function<void(const std::string&)> m_on_connect;
    std::function<void(const std::string&)> m_on_disconnect;
    std::function<void(const std::string&)> m_on_select;
};

using DeviceHubDialog = NetworkDeviceDialog;

} // namespace harmonycast
