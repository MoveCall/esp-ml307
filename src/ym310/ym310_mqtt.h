#ifndef _YM310_MQTT_H_
#define _YM310_MQTT_H_

#include "mqtt.h"
#include "at_uart.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string>
#include <map>

// 事件位定义
#define YM310_MQTT_TCP_CONNECTED  BIT0  // TCP 连接成功 (CONNECTOK)
#define YM310_MQTT_CONNECTED      BIT1  // MQTT 会话建立成功 (CONNACKOK)
#define YM310_MQTT_DISCONNECTED   BIT2
#define YM310_MQTT_ERROR          BIT3
#define YM310_MQTT_PUB_OK         BIT4
#define YM310_MQTT_SUB_OK         BIT5
#define YM310_MQTT_UNSUB_OK       BIT6

#define YM310_MQTT_CONNECT_TIMEOUT_MS  30000
#define YM310_MQTT_OPERATION_TIMEOUT_MS 10000

class Ym310AtModem;

/**
 * @brief YM310 MQTT 客户端实现
 * 
 * 使用 MCONFIG/MIPSTART/MCONNECT/MPUB/MSUB 等命令
 */
class Ym310Mqtt : public Mqtt {
public:
    Ym310Mqtt(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id = 0);
    ~Ym310Mqtt() override;

    bool Connect(const std::string broker_address, int broker_port, 
                 const std::string client_id, const std::string username, 
                 const std::string password) override;
    void Disconnect() override;
    bool Publish(const std::string topic, const std::string payload, int qos = 0) override;
    bool Subscribe(const std::string topic, int qos = 0) override;
    bool Unsubscribe(const std::string topic) override;
    bool IsConnected() override;
    int GetLastError() override;

    // YM310 特有方法
    void SetSsl(bool enable) { use_ssl_ = enable; }
    void SetCleanSession(bool clean) { clean_session_ = clean; }
    void SetWillMessage(const std::string& topic, const std::string& message, int qos = 0);

private:
    std::shared_ptr<AtUart> at_uart_;
    Ym310AtModem* modem_;
    int connect_id_;
    int last_error_ = 0;
    bool connected_ = false;
    bool use_ssl_ = false;
    bool clean_session_ = true;
    
    std::string will_topic_;
    std::string will_message_;
    int will_qos_ = 0;
    
    EventGroupHandle_t event_group_handle_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    
    bool Configure(const std::string& client_id, const std::string& username, 
                   const std::string& password);
    bool ConnectToBroker(const std::string& host, int port);
};

#endif // _YM310_MQTT_H_
