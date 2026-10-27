#pragma once

#include "Types.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace harmonycast {

struct CommandResult {
    int exit_code{-1};
    std::string out;
    std::string err;

    [[nodiscard]] bool success() const { return exit_code == 0; }
};

class HdcClient {
public:
    explicit HdcClient(std::string hdc_path = "");
    ~HdcClient() = default;

    static std::string findHdc();

    [[nodiscard]] const std::string& path() const { return m_hdc_path; }
    void setPath(std::string hdc_path) { m_hdc_path = std::move(hdc_path); }
    [[nodiscard]] bool isAvailable() const;

    static CommandResult runProcess(const std::string& hdc_path,
                                    const std::vector<std::string>& args,
                                    const std::string& serial = "",
                                    int timeout_ms = 10000);

    CommandResult run(const std::vector<std::string>& args,
                      const std::string& serial = "",
                      int timeout_ms = 10000) const;

    void executeAsync(const std::vector<std::string>& args,
                      const std::string& serial,
                      std::function<void(CommandResult)> callback) const;

    std::vector<Device> listDevices() const;

    bool forwardPort(const std::string& serial, int local_port, int remote_port) const;
    bool removeForward(const std::string& serial, int local_port, int remote_port) const;

    bool enableTcpMode(const std::string& serial = "", int port = 8710) const;
    bool restartServer() const;

    bool fileSend(const std::string& serial, const std::string& local_path, const std::string& remote_path) const;
    bool fileRecv(const std::string& serial, const std::string& remote_path, const std::string& local_path) const;

    bool wakeup(const std::string& serial) const;
    void wakeupAsync(const std::string& serial) const;
    bool home(const std::string& serial) const;
    bool back(const std::string& serial) const;

    std::optional<DeviceInfo> getDeviceInfo(const std::string& serial) const;

private:
    std::string m_hdc_path;
};

} // namespace harmonycast
