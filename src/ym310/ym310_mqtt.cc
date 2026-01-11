#include "ym310_mqtt.h"
#include "ym310_at_modem.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "Ym310Mqtt";

Ym310Mqtt::Ym310Mqtt(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id)
    : at_uart_(at_uart), modem_(modem), connect_id_(connect_id) {
    
    event_group_handle_ = xEventGroupCreate();
    
    // 注册 URC 回调
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            // TCP 连接成功 URC: 
            // - 单连接模式: CONNECTOK
            // - 多连接模式: 7,CONNECT OK (MQTT 专用连接 ID=7)
            // 使用 find 来匹配，因为可能带有连接 ID 前缀
            if (command.find("CONNECT OK") != std::string::npos ||
                command.find("CONNECTOK") != std::string::npos) {
                // 排除 CONNACK OK
                if (command.find("CONNACK") == std::string::npos) {
                    ESP_LOGD(TAG, "MQTT TCP connected");
                    xEventGroupSetBits(event_group_handle_, YM310_MQTT_TCP_CONNECTED);
                }
            } 
            // MQTT 会话建立成功 URC: CONNACKOK (MCONNECT 后)
            if (command.find("CONNACK OK") != std::string::npos ||
                command.find("CONNACKOK") != std::string::npos) {
                ESP_LOGD(TAG, "MQTT session OK");
                connected_ = true;
                xEventGroupSetBits(event_group_handle_, YM310_MQTT_CONNECTED);
                if (on_connected_callback_) {
                    on_connected_callback_();
                }
            } else if (command == "MCONNECT") {
                // +MCONNECT: <result>,<ret_code> (备用格式)
                if (arguments.size() >= 2) {
                    int result = arguments[0].int_value;
                    int ret_code = arguments[1].int_value;
                    ESP_LOGI(TAG, "MQTT connect result: %d, code: %d", result, ret_code);
                    
                    // result=0 表示成功 (根据手册示例)
                    if (result == 0 && ret_code == 0) {
                        connected_ = true;
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_CONNECTED);
                        if (on_connected_callback_) {
                            on_connected_callback_();
                        }
                    } else {
                        last_error_ = ret_code;
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_ERROR);
                        if (on_error_callback_) {
                            on_error_callback_("Connect failed: " + std::to_string(ret_code));
                        }
                    }
                }
            } 
            // 断开连接 URC
            if (command == "MDISCONNECT" || command == "MQTT DISCONNECT" ||
                command.find("MDISCONNECT") != std::string::npos) {
                ESP_LOGW(TAG, "MQTT disconnected (%s)", command.c_str());
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, YM310_MQTT_DISCONNECTED);
                if (on_disconnected_callback_) {
                    on_disconnected_callback_();
                }
            } 
            // TCP 连接关闭 URC (支持多连接格式如 "7,CLOSED")
            if (command.find("CLOSED") != std::string::npos) {
                // TCP 连接关闭（被服务器断开或网络问题）
                ESP_LOGW(TAG, "TCP connection closed (%s)", command.c_str());
                connected_ = false;
                xEventGroupSetBits(event_group_handle_, YM310_MQTT_DISCONNECTED);
                if (on_disconnected_callback_) {
                    on_disconnected_callback_();
                }
            } else if (command == "MPUB") {
                // +MPUB: <result>
                if (arguments.size() >= 1) {
                    int result = arguments[0].int_value;
                    if (result == 1) {
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_PUB_OK);
                    } else {
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_ERROR);
                    }
                }
            } else if (command == "SUBACK") {
                // 订阅确认 URC: SUBACK
                ESP_LOGI(TAG, "Subscribe acknowledged (SUBACK)");
                xEventGroupSetBits(event_group_handle_, YM310_MQTT_SUB_OK);
            } else if (command == "MSUB") {
                // +MSUB: "<topic>",<len>,<data>
                // 订阅消息接收 (直接上报模式)
                // 例如: +MSUB:"mqtt/topic",9 byte,SSSSddddd
                if (arguments.size() >= 3) {
                    std::string topic = arguments[0].string_value;
                    std::string payload = arguments[2].string_value;
                    
                    ESP_LOGD(TAG, "MQTT message received: topic=%s", topic.c_str());
                    
                    if (on_message_callback_) {
                        on_message_callback_(topic, payload);
                    }
                } else if (arguments.size() == 1) {
                    // Cache 模式: +MSUB: 0 表示有消息需要用 MQTTMSGGET 读取
                    ESP_LOGD(TAG, "MQTT message cached, use MQTTMSGGET to read");
                }
            } else if (command == "MUNSUB") {
                // +MUNSUB: <result>
                if (arguments.size() >= 1) {
                    int result = arguments[0].int_value;
                    if (result == 1) {
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_UNSUB_OK);
                    } else {
                        xEventGroupSetBits(event_group_handle_, YM310_MQTT_ERROR);
                    }
                }
            }
        }
    );
}

Ym310Mqtt::~Ym310Mqtt() {
    if (connected_) {
        Disconnect();
    }
    
    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

void Ym310Mqtt::SetWillMessage(const std::string& topic, const std::string& message, int qos) {
    will_topic_ = topic;
    will_message_ = message;
    will_qos_ = qos;
}

bool Ym310Mqtt::Configure(const std::string& client_id, const std::string& username, 
                           const std::string& password) {
    ESP_LOGD(TAG, "MQTT config: client_id=%s", client_id.c_str());
    
    // 配置 MQTT 参数
    // AT+MCONFIG=<clientid>[,<username>,<password>[,<will_qos>,<will_retain>,<will_topic>,<will_message>]]
    std::string cmd;
    
    if (!will_topic_.empty()) {
        // 带遗嘱消息的配置
        cmd = "AT+MCONFIG=\"" + client_id + "\",\"" + username + "\",\"" + password + "\"," +
              std::to_string(will_qos_) + ",0,\"" + will_topic_ + "\",\"" + will_message_ + "\"";
    } else {
        // 普通配置
        cmd = "AT+MCONFIG=\"" + client_id + "\",\"" + username + "\",\"" + password + "\"";
    }
    
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to configure MQTT");
        last_error_ = -1;
        return false;
    }
    
    // 注意: Keep-Alive 和 Clean Session 在 AT+MCONNECT 命令中设置
    // AT+MCONNECT=<clean_session>,<keep_alive>
    
    return true;
}

bool Ym310Mqtt::ConnectToBroker(const std::string& host, int port) {
    ESP_LOGD(TAG, "MQTT connecting to %s:%d (SSL=%d)", host.c_str(), port, use_ssl_);
    
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, 
                         YM310_MQTT_TCP_CONNECTED | YM310_MQTT_CONNECTED | 
                         YM310_MQTT_ERROR | YM310_MQTT_DISCONNECTED);
    
    // 如果使用 SSL，需要配置 SSL 参数
    // 根据手册，MQTT 功能使用 SSL 上下文 ID = 88
    if (use_ssl_) {
        ESP_LOGD(TAG, "MQTT SSL config (ctx=88)");
        // 配置 SSL hostname
        std::string ssl_hostname = "AT+SSLCFG=\"hostname\",88,\"" + host + "\"";
        at_uart_->SendCommand(ssl_hostname, 1000);
        // 配置 SSL 版本: TLS 1.2
        at_uart_->SendCommand("AT+SSLCFG=\"sslversion\",88,4", 1000);
        // 配置安全等级: 0=不验证证书
        at_uart_->SendCommand("AT+SSLCFG=\"seclevel\",88,0", 1000);
    }
    
    // 建立 TCP 连接到 Broker
    // 普通连接: AT+MIPSTART="<host>",<port> （注意：根据手册，端口号也可以用引号括住）
    // SSL 连接: AT+SSLMIPSTART="<host>",<port>
    std::string cmd;
    if (use_ssl_) {
        cmd = "AT+SSLMIPSTART=\"" + host + "\"," + std::to_string(port);
    } else {
        cmd = "AT+MIPSTART=\"" + host + "\"," + std::to_string(port);
    }
    
    ESP_LOGD(TAG, ">> %s", cmd.c_str());
    
    if (!at_uart_->SendCommand(cmd, 30000)) {
        ESP_LOGE(TAG, "Failed to start MQTT IP connection");
        last_error_ = -2;
        return false;
    }
    
    ESP_LOGD(TAG, "Waiting for CONNECT URC...");
    
    // 等待 TCP 连接成功 (CONNECTOK URC)
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_MQTT_TCP_CONNECTED | YM310_MQTT_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_MQTT_CONNECT_TIMEOUT_MS)
    );
    
    if (!(bits & YM310_MQTT_TCP_CONNECTED)) {
        ESP_LOGE(TAG, "MQTT TCP connection failed or timeout");
        last_error_ = -2;
        return false;
    }
    
    ESP_LOGD(TAG, "TCP OK, sending MCONNECT...");
    
    // 发送 MQTT CONNECT
    // AT+MCONNECT=<clean_session>,<keep_alive>
    // 根据手册，在 CONNECTOK 后要立即发送，否则会被服务器踢掉
    cmd = "AT+MCONNECT=" + std::to_string(clean_session_ ? 1 : 0) + "," + 
          std::to_string(keep_alive_seconds_);
    
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to send MQTT CONNECT");
        last_error_ = -3;
        return false;
    }
    
    // 等待 MQTT 会话建立成功 (CONNACKOK URC)
    bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_MQTT_CONNECTED | YM310_MQTT_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_MQTT_CONNECT_TIMEOUT_MS)
    );
    
    if (bits & YM310_MQTT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected successfully");
        return true;
    } else if (bits & YM310_MQTT_ERROR) {
        ESP_LOGE(TAG, "MQTT connection failed");
        return false;
    } else {
        ESP_LOGE(TAG, "MQTT connection timeout");
        last_error_ = -4;
        return false;
    }
}

bool Ym310Mqtt::Connect(const std::string broker_address, int broker_port, 
                         const std::string client_id, const std::string username, 
                         const std::string password) {
    // 清理任何残留的 MQTT 连接（处理 ESP32 复位但 4G 模组未复位的情况）
    // 无论 connected_ 状态如何，都尝试清理
    ESP_LOGD(TAG, "Cleaning up any existing MQTT connections...");
    at_uart_->SendCommand("AT+MDISCONNECT", 1000);
    at_uart_->SendCommand("AT+MIPCLOSE", 1000);
    connected_ = false;
    
    // 端口 8883 是标准 MQTTS 端口，自动启用 SSL
    if (broker_port == 8883) {
        ESP_LOGI(TAG, "Port 8883 detected, enabling SSL automatically");
        use_ssl_ = true;
    }
    
    // 配置 MQTT 参数
    if (!Configure(client_id, username, password)) {
        return false;
    }
    
    // 连接到 Broker
    return ConnectToBroker(broker_address, broker_port);
}

void Ym310Mqtt::Disconnect() {
    if (!connected_) {
        return;
    }
    
    ESP_LOGI(TAG, "Disconnecting MQTT");
    
    // AT+MDISCONNECT
    at_uart_->SendCommand("AT+MDISCONNECT", 5000);
    
    // 关闭 TCP 连接
    at_uart_->SendCommand("AT+MIPCLOSE", 5000);
    
    connected_ = false;
}

bool Ym310Mqtt::Publish(const std::string topic, const std::string payload, int qos) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return false;
    }
    
    ESP_LOGD(TAG, "Pub %s, qos=%d, len=%d", topic.c_str(), qos, (int)payload.size());
    
    // 使用 AT+MPUBEX 发送定长消息，避免 JSON 中的双引号问题
    // AT+MPUBEX="<topic>",<qos>,<retain>,<len>
    // 返回 > 后发送数据
    std::string cmd = "AT+MPUBEX=\"" + topic + "\"," + std::to_string(qos) + 
                      ",0," + std::to_string(payload.size());
    
    if (!at_uart_->SendCommandWithData(cmd, YM310_MQTT_OPERATION_TIMEOUT_MS, true,
                                        payload.c_str(), payload.size())) {
        ESP_LOGE(TAG, "Failed to publish message");
        last_error_ = -5;
        return false;
    }
    
    // QoS 0 直接返回成功，不等待确认
    if (qos == 0) {
        ESP_LOGD(TAG, "QoS 0 sent");
        return true;
    }
    
    // QoS 1/2 等待发布确认（可选）
    ESP_LOGD(TAG, "Published OK");
    return true;
}

bool Ym310Mqtt::Subscribe(const std::string topic, int qos) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return false;
    }
    
    ESP_LOGI(TAG, "Subscribing to %s, qos=%d", topic.c_str(), qos);
    
    
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, YM310_MQTT_SUB_OK | YM310_MQTT_ERROR);
    
    // AT+MSUB="<topic>",<qos>
    std::string cmd = "AT+MSUB=\"" + topic + "\"," + std::to_string(qos);
    
    if (!at_uart_->SendCommand(cmd, YM310_MQTT_OPERATION_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Failed to send subscribe command");
        last_error_ = -8;
        return false;
    }
    
    // 等待订阅确认
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_MQTT_SUB_OK | YM310_MQTT_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_MQTT_OPERATION_TIMEOUT_MS)
    );
    
    at_uart_->SetDebug(false);
    
    if (bits & YM310_MQTT_SUB_OK) {
        ESP_LOGI(TAG, "Subscribed to %s successfully", topic.c_str());
        return true;
    } else if (bits & YM310_MQTT_ERROR) {
        ESP_LOGE(TAG, "Subscribe failed");
        last_error_ = -9;
        return false;
    }
    
    ESP_LOGE(TAG, "Subscribe timeout");
    last_error_ = -10;
    return false;
}

bool Ym310Mqtt::Unsubscribe(const std::string topic) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return false;
    }
    
    ESP_LOGI(TAG, "Unsubscribing from %s", topic.c_str());
    
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, YM310_MQTT_UNSUB_OK | YM310_MQTT_ERROR);
    
    // AT+MUNSUB="<topic>"
    std::string cmd = "AT+MUNSUB=\"" + topic + "\"";
    
    if (!at_uart_->SendCommand(cmd, YM310_MQTT_OPERATION_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Failed to send unsubscribe command");
        last_error_ = -11;
        return false;
    }
    
    // 等待取消订阅确认
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_MQTT_UNSUB_OK | YM310_MQTT_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_MQTT_OPERATION_TIMEOUT_MS)
    );
    
    if (bits & YM310_MQTT_UNSUB_OK) {
        ESP_LOGI(TAG, "Unsubscribed from %s successfully", topic.c_str());
        return true;
    } else if (bits & YM310_MQTT_ERROR) {
        ESP_LOGE(TAG, "Unsubscribe failed");
        last_error_ = -12;
        return false;
    }
    
    ESP_LOGE(TAG, "Unsubscribe timeout");
    last_error_ = -13;
    return false;
}

bool Ym310Mqtt::IsConnected() {
    return connected_;
}

int Ym310Mqtt::GetLastError() {
    return last_error_;
}
