#include "ym310_udp.h"
#include "ym310_at_modem.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "Ym310Udp";

Ym310Udp::Ym310Udp(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id)
    : at_uart_(at_uart), modem_(modem), connect_id_(connect_id) {
    
    event_group_handle_ = xEventGroupCreate();
    
    // 注册 URC 回调
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            // UDP 连接状态处理（与 TCP 类似）
            // 根据手册：CONNECTOK / CONNECTFAIL（无空格）
            if (command == "CONNECTOK" || command == "CONNECT OK") {
                if (connect_id_ == 0) {
                    xEventGroupSetBits(event_group_handle_, YM310_UDP_CONNECTED);
                }
            } else if (command == "CONNECTFAIL" || command == "CONNECT FAIL") {
                if (connect_id_ == 0) {
                    xEventGroupSetBits(event_group_handle_, YM310_UDP_ERROR);
                }
            } else if (command == "CLOSED") {
                if (arguments.size() >= 1 && arguments[0].int_value == connect_id_) {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, YM310_UDP_DISCONNECTED);
                }
            } 
            // 发送成功 URC (使用 find 匹配，支持 "DATA ACCEPT:0,17" 等格式)
            if (command.find("SENDOK") != std::string::npos || 
                command.find("SEND OK") != std::string::npos ||
                command.find("DATAACCEPT") != std::string::npos || 
                command.find("DATA ACCEPT") != std::string::npos) {
                // 根据手册：DATAACCEPT:<length> 或 DATAACCEPT:<n>,<length>
                ESP_LOGD(TAG, "UDP send OK");
                xEventGroupSetBits(event_group_handle_, YM310_UDP_SEND_OK);
            } else if (command == "RECEIVE") {
                // UDP 数据接收
                if (arguments.size() >= 2) {
                    int conn_id = arguments[0].int_value;
                    if (conn_id == connect_id_ && arguments.size() >= 3) {
                        HandleReceivedData(arguments[2].string_value);
                    }
                }
            } else if (command == "+IPD") {
                // 数据接收
                if (arguments.size() >= 2) {
                    if (arguments[0].int_value == connect_id_ || connect_id_ == 0) {
                        if (arguments.size() >= 3) {
                            HandleReceivedData(arguments[2].string_value);
                        }
                    }
                }
            }
            
            // 多连接模式的连接结果
            // 格式：<n>,CONNECTOK (根据手册)
            if (arguments.size() >= 1 && arguments[0].int_value == connect_id_) {
                if (command.find("CONNECTOK") != std::string::npos || 
                    command.find("CONNECT OK") != std::string::npos) {
                    xEventGroupSetBits(event_group_handle_, YM310_UDP_CONNECTED);
                } else if (command.find("CONNECTFAIL") != std::string::npos || 
                           command.find("CONNECT FAIL") != std::string::npos) {
                    xEventGroupSetBits(event_group_handle_, YM310_UDP_ERROR);
                }
            }
        }
    );
}

Ym310Udp::~Ym310Udp() {
    if (connected_) {
        Disconnect();
    }
    
    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool Ym310Udp::Connect(const std::string& host, int port) {
    if (connected_) {
        ESP_LOGW(TAG, "Already connected, disconnect first");
        Disconnect();
    }
    
    ESP_LOGI(TAG, "Connecting UDP to %s:%d (connection %d)", 
             host.c_str(), port, connect_id_);
    
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, 
                         YM310_UDP_CONNECTED | YM310_UDP_ERROR | YM310_UDP_DISCONNECTED);
    
    // 发送连接命令
    // 多连接模式：AT+CIPSTART=<n>,"UDP","<host>",<port>
    std::string cmd = "AT+CIPSTART=" + std::to_string(connect_id_) + 
                      ",\"UDP\",\"" + host + "\"," + std::to_string(port);
    
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to send CIPSTART command");
        last_error_ = -1;
        return false;
    }
    
    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_UDP_CONNECTED | YM310_UDP_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_UDP_CONNECT_TIMEOUT_MS)
    );
    
    if (bits & YM310_UDP_CONNECTED) {
        connected_ = true;
        ESP_LOGI(TAG, "UDP connected successfully");
        return true;
    } else if (bits & YM310_UDP_ERROR) {
        ESP_LOGE(TAG, "UDP connection failed");
        last_error_ = -2;
        return false;
    } else {
        ESP_LOGE(TAG, "UDP connection timeout");
        last_error_ = -3;
        return false;
    }
}

void Ym310Udp::Disconnect() {
    if (!connected_) {
        return;
    }
    
    ESP_LOGI(TAG, "Disconnecting UDP connection %d", connect_id_);
    
    // 多连接模式：AT+CIPCLOSE=<n>
    std::string cmd = "AT+CIPCLOSE=" + std::to_string(connect_id_);
    at_uart_->SendCommand(cmd, 5000);
    
    connected_ = false;
    
    // 清空接收缓冲区
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_buffer_.clear();
}

int Ym310Udp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }
    
    if (data.empty()) {
        return 0;
    }
    
    ESP_LOGD(TAG, "UDP send %d bytes on conn %d", (int)data.size(), connect_id_);
    
    // 清除发送事件位
    xEventGroupClearBits(event_group_handle_, YM310_UDP_SEND_OK);
    
    // 多连接模式：AT+CIPSEND=<n>,<len>
    std::string cmd = "AT+CIPSEND=" + std::to_string(connect_id_) + "," + 
                      std::to_string(data.size());
    
    if (!at_uart_->SendCommandWithData(cmd, YM310_UDP_SEND_TIMEOUT_MS, true, 
                                        data.c_str(), data.size())) {
        ESP_LOGE(TAG, "Failed to send UDP data (SendCommandWithData returned false)");
        last_error_ = -4;
        return -1;
    }
    
    // 等待发送确认
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_UDP_SEND_OK,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_UDP_SEND_TIMEOUT_MS)
    );
    
    if (bits & YM310_UDP_SEND_OK) {
        ESP_LOGD(TAG, "UDP send OK");
        return data.size();
    }
    
    // 超时也认为发送成功（快发模式）
    ESP_LOGD(TAG, "UDP send timeout, assuming success");
    return data.size();
}

int Ym310Udp::GetLastError() {
    return last_error_;
}

void Ym310Udp::HandleReceivedData(const std::string& data) {
    ESP_LOGD(TAG, "Received %zu bytes on UDP connection %d", data.size(), connect_id_);
    
    if (message_callback_) {
        message_callback_(data);
    } else {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_buffer_.append(data);
        xEventGroupSetBits(event_group_handle_, YM310_UDP_DATA_READY);
    }
}
