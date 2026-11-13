#pragma once

#include "DeviceClient.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <QPointer>
#include <QObject>
#include <QString>

class QProcess;

namespace harmonycast {

class HdcClient : public DeviceClient {
public:
    explicit HdcClient(std::string hdc_path = "", QObject* callbackContext = nullptr);
    ~HdcClient() override = default;
    [[nodiscard]] DevicePlatform platform() const override { return DevicePlatform::OpenHarmony; }

    static std::string findHdc();

    [[nodiscard]] const std::string& path() const override { return m_hdc_path; }
    void setPath(std::string hdc_path) { m_hdc_path = std::move(hdc_path); }
    [[nodiscard]] bool isAvailable() const override;

    static CommandResult runProcess(const std::string& hdc_path,
                                    const std::vector<std::string>& args,
                                    const std::string& serial = "",
                                    int timeout_ms = 10000);
    static void configureProcess(QProcess& process, const std::string& hdc_path);

    CommandResult run(const std::vector<std::string>& args,
                      const std::string& serial = "",
                      int timeout_ms = 10000) const override;

    void executeAsync(const std::vector<std::string>& args,
                      const std::string& serial,
                      std::function<void(CommandResult)> callback) const override;

    std::vector<Device> listDevices() const;
    std::optional<std::vector<Device>> listDevicesChecked() const override;
    static std::vector<Device> parseDeviceList(const std::string& output);
    static std::vector<std::string> commandArguments(QString input, bool shellByDefault = true);

    bool forwardPort(const std::string& serial, int local_port, int remote_port) const override;
    bool removeForward(const std::string& serial, int local_port, int remote_port) const override;

    bool enableTcpMode(const std::string& serial = "", int port = 8710) const override;
    bool restartServer() const override;

    bool fileSend(const std::string& serial, const std::string& local_path, const std::string& remote_path) const override;
    bool fileRecv(const std::string& serial, const std::string& remote_path, const std::string& local_path) const override;

    bool wakeup(const std::string& serial) const override;
    void wakeupAsync(const std::string& serial) const override;
    bool home(const std::string& serial) const override;
    bool back(const std::string& serial) const override;

    std::optional<DeviceInfo> getDeviceInfo(const std::string& serial) const override;

private:
    std::string m_hdc_path;
    QPointer<QObject> m_callback_context;
};

} // namespace harmonycast
