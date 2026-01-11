#ifndef _YM310_AT_MODEM_H_
#define _YM310_AT_MODEM_H_

#include "at_modem.h"
#include "tcp.h"
#include "udp.h"
#include "http.h"
#include "mqtt.h"
#include "web_socket.h"

/**
 * @brief YM310 Cat.1 模组 AT Modem 实现
 * 
 * 域格 YM310 模组基于移芯 EC716S 平台，AT 命令采用类 SIM800 风格。
 * 主要特点：
 * - TCP/UDP 使用 CIPSTART/CIPSEND 命令
 * - HTTP 使用 SAPBR + HTTPINIT/HTTPACTION 命令
 * - MQTT 使用 MCONFIG/MIPSTART/MCONNECT 命令
 */
class Ym310AtModem : public AtModem {
public:
    Ym310AtModem(std::shared_ptr<AtUart> at_uart);
    ~Ym310AtModem() override;

    // 重写基类方法
    void Reboot() override;
    bool SetSleepMode(bool enable, int delay_seconds = 0) override;
    NetworkStatus WaitForNetworkReady(int timeout_ms = -1) override;

    // 实现基类的纯虚函数
    std::unique_ptr<Http> CreateHttp(int connect_id) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id) override;

    // YM310 特有方法
    bool ActivatePdpContext();      // 激活 TCPIP PDP 上下文 (CSTT/CIICR/CIFSR)
    bool DeactivatePdpContext();    // 去激活 PDP 上下文 (CIPSHUT)
    bool ActivateSapbr();           // 激活 SAPBR 承载（HTTP 用）
    bool DeactivateSapbr();         // 去激活 SAPBR
    bool IsPdpActivated() const { return pdp_activated_; }
    bool IsSapbrActivated() const { return sapbr_activated_; }
    const std::string& GetLocalIp() const { return local_ip_; }

protected:
    void HandleUrc(const std::string& command, 
                   const std::vector<AtArgumentValue>& arguments) override;

private:
    bool pdp_activated_ = false;
    bool sapbr_activated_ = false;
    std::string local_ip_;
    
    // 连接状态跟踪（多连接模式，支持 0-5 共 6 个连接）
    static const int MAX_CONNECTIONS = 6;
    bool connection_states_[MAX_CONNECTIONS] = {false};
    
    // 初始化模组配置
    void InitModem();
};

#endif // _YM310_AT_MODEM_H_
