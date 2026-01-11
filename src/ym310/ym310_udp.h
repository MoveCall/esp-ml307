#ifndef _YM310_UDP_H_
#define _YM310_UDP_H_

#include "udp.h"
#include "at_uart.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string>
#include <mutex>

// 事件位定义
#define YM310_UDP_CONNECTED     BIT0
#define YM310_UDP_DISCONNECTED  BIT1
#define YM310_UDP_ERROR         BIT2
#define YM310_UDP_SEND_OK       BIT3
#define YM310_UDP_DATA_READY    BIT4

#define YM310_UDP_CONNECT_TIMEOUT_MS 30000
#define YM310_UDP_SEND_TIMEOUT_MS    10000

class Ym310AtModem;

/**
 * @brief YM310 UDP 客户端实现
 * 
 * 使用 AT+CIPSTART/AT+CIPSEND/AT+CIPCLOSE 命令
 * 协议类型为 "UDP"
 */
class Ym310Udp : public Udp {
public:
    Ym310Udp(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id);
    ~Ym310Udp() override;

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;
    int GetLastError() override;

private:
    std::shared_ptr<AtUart> at_uart_;
    Ym310AtModem* modem_;
    int connect_id_;
    int last_error_ = 0;
    
    EventGroupHandle_t event_group_handle_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    
    std::string rx_buffer_;
    std::mutex rx_mutex_;
    
    void HandleReceivedData(const std::string& data);
};

#endif // _YM310_UDP_H_
