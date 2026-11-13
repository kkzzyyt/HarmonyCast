#pragma once

#include "Types.h"
#include <functional>
#include <optional>

namespace harmonycast {

struct CommandResult {
    int exit_code{-1};
    std::string out;
    std::string err;
    [[nodiscard]] bool success() const { return exit_code == 0; }
};

// Device operations shared by the workbench. Shell commands remain platform-specific.
class DeviceClient {
public:
    virtual ~DeviceClient() = default;
    [[nodiscard]] virtual DevicePlatform platform() const = 0;
    [[nodiscard]] virtual const std::string& path() const = 0;
    [[nodiscard]] virtual bool isAvailable() const = 0;
    virtual CommandResult run(const std::vector<std::string>& args, const std::string& serial = "",
                              int timeout_ms = 10000) const = 0;
    virtual CommandResult runWithOutputLimit(const std::vector<std::string>& args, const std::string& serial,
                                            int timeout_ms, int max_bytes) const {
        auto result = run(args, serial, timeout_ms);
        if (max_bytes > 0 && result.out.size() + result.err.size() > size_t(max_bytes)) return {-4, {}, "设备命令返回内容超过限制"};
        return result;
    }
    virtual void executeAsync(const std::vector<std::string>& args, const std::string& serial,
                              std::function<void(CommandResult)> callback) const = 0;
    virtual std::optional<std::vector<Device>> listDevicesChecked() const = 0;
    std::vector<Device> listDevices() const { return listDevicesChecked().value_or(std::vector<Device>{}); }
    virtual bool forwardPort(const std::string& serial, int local_port, int remote_port) const = 0;
    virtual bool removeForward(const std::string& serial, int local_port, int remote_port) const = 0;
    bool removeSocketForward(const std::string& serial, int localPort, const std::string& remote) const {
        const auto local = "tcp:" + std::to_string(localPort);
        if (platform() == DevicePlatform::Android) return run({"forward", "--remove", local}, serial, 2000).success();
        const auto result = run({"fport", "rm", local + " " + remote}, serial, 1500);
        if (result.success() && result.out.find("[Fail]") == std::string::npos) return true;
        return run({"fport", "rm", local, remote}, serial, 1500).success();
    }
    virtual bool enableTcpMode(const std::string& serial = "", int port = 8710) const = 0;
    virtual bool restartServer() const = 0;
    virtual bool fileSend(const std::string& serial, const std::string& local, const std::string& remote) const = 0;
    virtual bool fileRecv(const std::string& serial, const std::string& remote, const std::string& local) const = 0;
    virtual bool wakeup(const std::string& serial) const = 0;
    virtual void wakeupAsync(const std::string& serial) const = 0;
    virtual bool home(const std::string& serial) const = 0;
    virtual bool back(const std::string& serial) const = 0;
    virtual std::optional<DeviceInfo> getDeviceInfo(const std::string& serial) const = 0;
};

} // namespace harmonycast
