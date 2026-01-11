#ifndef _YM310_TCP_H_
#define _YM310_TCP_H_

#include "tcp.h"
#include "at_uart.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string>
#include <mutex>

// 事件位定义
#define YM310_TCP_CONNECTED     BIT0
#define YM310_TCP_DISCONNECTED  BIT1
#define YM310_TCP_ERROR         BIT2
#define YM310_TCP_SEND_OK       BIT3
#define YM310_TCP_SEND_FAIL     BIT4
#define YM310_TCP_DATA_READY    BIT5

#define YM310_TCP_CONNECT_TIMEOUT_MS 30000
#define YM310_TCP_SEND_TIMEOUT_MS    10000

class Ym310AtModem;  // 前向声明

/**
 * @brief YM310 TCP 客户端实现
 * 
 * 使用 AT+CIPSTART/AT+CIPSEND/AT+CIPCLOSE 命令
 */
class Ym310Tcp : public Tcp {
public:
    Ym310Tcp(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id);
    virtual ~Ym310Tcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;
    int GetLastError() override;

protected:
    std::shared_ptr<AtUart> at_uart_;
    Ym310AtModem* modem_;
    int connect_id_;
    int last_error_ = 0;
    
    EventGroupHandle_t event_group_handle_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    
    std::string rx_buffer_;
    std::mutex rx_mutex_;
    
    // 虚函数允许子类（SSL）自定义配置
    virtual bool ConfigureConnection();
    virtual std::string GetProtocol() { return "TCP"; }
    
    // 处理接收到的数据
    void HandleReceivedData(const std::string& data);
};

#endif // _YM310_TCP_H_
