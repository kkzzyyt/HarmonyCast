#pragma once

#include "DeviceClient.h"
#include <QObject>
#include <QPointer>
#include <memory>

namespace harmonycast {

class AdbClient final : public DeviceClient {
public:
    explicit AdbClient(std::string adbPath = "", QObject* callbackContext = nullptr);
    [[nodiscard]] DevicePlatform platform() const override { return DevicePlatform::Android; }
    [[nodiscard]] const std::string& path() const override { return m_path; }
    [[nodiscard]] bool isAvailable() const override;
    static std::string findAdb();
    static CommandResult runProcess(const std::string& path, const std::vector<std::string>& args,
                                    const std::string& serial = "", int timeout_ms = 10000);
    CommandResult run(const std::vector<std::string>& args, const std::string& serial = "",
                      int timeout_ms = 10000) const override;
    void executeAsync(const std::vector<std::string>& args, const std::string& serial,
                      std::function<void(CommandResult)> callback) const override;
    std::optional<std::vector<Device>> listDevicesChecked() const override;
    static std::vector<Device> parseDeviceList(const std::string& text);
    static std::vector<std::string> commandArguments(QString input, bool shellByDefault = true);
    static QImage screenshotImage(const CommandResult& result);
    bool forwardPort(const std::string& serial, int local, int remote) const override;
    bool removeForward(const std::string& serial, int local, int remote = 0) const override;
    bool enableTcpMode(const std::string& serial = "", int port = 5555) const override;
    bool restartServer() const override;
    bool fileSend(const std::string& serial, const std::string& local, const std::string& remote) const override;
    bool fileRecv(const std::string& serial, const std::string& remote, const std::string& local) const override;
    bool wakeup(const std::string& serial) const override;
    void wakeupAsync(const std::string& serial) const override;
    bool home(const std::string& serial) const override;
    bool back(const std::string& serial) const override;
    std::optional<DeviceInfo> getDeviceInfo(const std::string& serial) const override;
    static DeviceInfo parseDeviceInfo(const std::string& serial, const std::string& text);
    static std::string deviceInfoScript();
private:
    std::string m_path;
    // Worker callbacks are delivered through a retained GUI-thread receiver.
    std::shared_ptr<QObject> m_delivery;
    QPointer<QObject> m_callback_context;
};

} // namespace harmonycast
