#include "ym310_tcp.h"
#include "ym310_at_modem.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "Ym310Tcp";

Ym310Tcp::Ym310Tcp(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id)
    : at_uart_(at_uart), modem_(modem), connect_id_(connect_id) {
    
    event_group_handle_ = xEventGroupCreate();
    
    // 注册 URC 回调处理
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            // 处理连接状态 URC
            // 根据手册：
            // 单连接模式：CONNECTOK / CONNECTFAIL / CLOSED
            // 多连接模式：<n>,CONNECTOK / <n>,CONNECTFAIL / <n>,CLOSED
            
            if (command == "CONNECTOK" || command == "CONNECT OK") {
                // 单连接模式的连接成功
                if (connect_id_ == 0) {
                    xEventGroupSetBits(event_group_handle_, YM310_TCP_CONNECTED);
                }
            } else if (command == "CONNECTFAIL" || command == "CONNECT FAIL") {
                if (connect_id_ == 0) {
                    xEventGroupSetBits(event_group_handle_, YM310_TCP_ERROR);
                }
            } else if (command == "CLOSED") {
                // 连接关闭
                if (arguments.size() >= 1 && arguments[0].int_value == connect_id_) {
                    connected_ = false;
                    xEventGroupSetBits(event_group_handle_, YM310_TCP_DISCONNECTED);
                    if (disconnect_callback_) {
                        disconnect_callback_();
                    }
                }
            } 
            // 发送成功 URC (使用 find 匹配，支持 "DATA ACCEPT:0,58" 等格式)
            if (command.find("SENDOK") != std::string::npos || 
                command.find("SEND OK") != std::string::npos ||
                command.find("DATAACCEPT") != std::string::npos || 
                command.find("DATA ACCEPT") != std::string::npos) {
                // 根据手册：DATAACCEPT:<length> 或 DATAACCEPT:<n>,<length>
                ESP_LOGI("Ym310Tcp", "Send OK URC: %s", command.c_str());
                xEventGroupSetBits(event_group_handle_, YM310_TCP_SEND_OK);
            } 
            // 发送失败 URC
            if (command.find("SENDFAIL") != std::string::npos || 
                command.find("SEND FAIL") != std::string::npos) {
                xEventGroupSetBits(event_group_handle_, YM310_TCP_SEND_FAIL);
            } else if (command == "RECEIVE") {
                // +RECEIVE,<n>,<len>:<data>
                // 多连接模式下的数据接收
                if (arguments.size() >= 2) {
                    int conn_id = arguments[0].int_value;
                    if (conn_id == connect_id_) {
                        // 数据在第三个参数或需要继续读取
                        if (arguments.size() >= 3) {
                            HandleReceivedData(arguments[2].string_value);
                        }
                    }
                }
            } else if (command == "+IPD") {
                // 单连接模式：+IPD,<len>:<data>
                // 多连接模式：+IPD,<n>,<len>:<data>（带 CIPHEAD）
                if (arguments.size() >= 2) {
                    // 检查是否是我们的连接
                    if (arguments[0].int_value == connect_id_ || connect_id_ == 0) {
                        if (arguments.size() >= 3) {
                            HandleReceivedData(arguments[2].string_value);
                        }
                    }
                }
            }
            
            // 处理多连接模式的连接结果
            // 格式：<n>,CONNECTOK (根据手册)
            if (arguments.size() >= 1 && arguments[0].int_value == connect_id_) {
                if (command.find("CONNECTOK") != std::string::npos || 
                    command.find("CONNECT OK") != std::string::npos) {
                    xEventGroupSetBits(event_group_handle_, YM310_TCP_CONNECTED);
                } else if (command.find("CONNECTFAIL") != std::string::npos || 
                           command.find("CONNECT FAIL") != std::string::npos) {
                    xEventGroupSetBits(event_group_handle_, YM310_TCP_ERROR);
                }
            }
        }
    );
}

Ym310Tcp::~Ym310Tcp() {
    if (connected_) {
        Disconnect();
    }
    
    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool Ym310Tcp::ConfigureConnection() {
    // 确保禁用 SSL（上次可能启用了 SSL）
    at_uart_->SendCommand("AT+CIPSSL=0", 1000);
    return true;
}

bool Ym310Tcp::Connect(const std::string& host, int port) {
    if (connected_) {
        ESP_LOGW(TAG, "Already connected, disconnect first");
        Disconnect();
    }
    
    ESP_LOGI(TAG, "Connecting to %s:%d (connection %d, protocol: %s)", 
             host.c_str(), port, connect_id_, GetProtocol().c_str());
    
    // 配置连接（SSL 会在此配置证书等）
    if (!ConfigureConnection()) {
        ESP_LOGE(TAG, "Failed to configure connection");
        return false;
    }
    
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, 
                         YM310_TCP_CONNECTED | YM310_TCP_ERROR | YM310_TCP_DISCONNECTED);
    
    // 发送连接命令
    // 多连接模式：AT+CIPSTART=<n>,"TCP","<host>",<port>
    std::string cmd = "AT+CIPSTART=" + std::to_string(connect_id_) + ",\"" + 
                      GetProtocol() + "\",\"" + host + "\"," + std::to_string(port);
    
    ESP_LOGI(TAG, "Sending: %s", cmd.c_str());
    
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to send CIPSTART command");
        last_error_ = -1;
        return false;
    }
    
    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_TCP_CONNECTED | YM310_TCP_ERROR,
        pdTRUE,  // 清除事件位
        pdFALSE, // 等待任意一个
        pdMS_TO_TICKS(YM310_TCP_CONNECT_TIMEOUT_MS)
    );
    
    if (bits & YM310_TCP_CONNECTED) {
        connected_ = true;
        ESP_LOGI(TAG, "Connected successfully");
        return true;
    } else if (bits & YM310_TCP_ERROR) {
        ESP_LOGE(TAG, "Connection failed");
        last_error_ = -2;
        return false;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        last_error_ = -3;
        return false;
    }
}

void Ym310Tcp::Disconnect() {
    if (!connected_) {
        return;
    }
    
    ESP_LOGI(TAG, "Disconnecting connection %d", connect_id_);
    
    // 多连接模式：AT+CIPCLOSE=<n>
    std::string cmd = "AT+CIPCLOSE=" + std::to_string(connect_id_);
    at_uart_->SendCommand(cmd, 5000);
    
    connected_ = false;
    
    // 清空接收缓冲区
    std::lock_guard<std::mutex> lock(rx_mutex_);
    rx_buffer_.clear();
}

int Ym310Tcp::Send(const std::string& data) {
    if (!connected_) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }
    
    if (data.empty()) {
        return 0;
    }
    
    ESP_LOGI(TAG, "Sending %zu bytes on connection %d", data.size(), connect_id_);
    
    // 清除发送事件位
    xEventGroupClearBits(event_group_handle_, YM310_TCP_SEND_OK | YM310_TCP_SEND_FAIL);
    
    // 多连接模式：AT+CIPSEND=<n>,<len>
    std::string cmd = "AT+CIPSEND=" + std::to_string(connect_id_) + "," + 
                      std::to_string(data.size());
    
    ESP_LOGI(TAG, "Sending command: %s", cmd.c_str());
    
    // 发送命令，等待 > 提示符
    if (!at_uart_->SendCommandWithData(cmd, YM310_TCP_SEND_TIMEOUT_MS, true, 
                                        data.c_str(), data.size())) {
        ESP_LOGE(TAG, "Failed to send data (SendCommandWithData returned false)");
        last_error_ = -4;
        return -1;
    }
    
    // 快发模式下会返回 DATA ACCEPT: <n>,<len>
    // 等待发送确认
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_TCP_SEND_OK | YM310_TCP_SEND_FAIL,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(YM310_TCP_SEND_TIMEOUT_MS)
    );
    
    at_uart_->SetDebug(false);
    
    if (bits & YM310_TCP_SEND_OK) {
        ESP_LOGI(TAG, "Data sent successfully");
        return data.size();
    } else if (bits & YM310_TCP_SEND_FAIL) {
        ESP_LOGE(TAG, "Data send failed");
        last_error_ = -5;
        return -1;
    }
    
    // 超时也认为发送成功（快发模式可能不返回确认）
    ESP_LOGD(TAG, "Send timeout, assuming success");
    return data.size();
}

int Ym310Tcp::GetLastError() {
    return last_error_;
}

void Ym310Tcp::HandleReceivedData(const std::string& data) {
    ESP_LOGD(TAG, "Received %zu bytes on connection %d", data.size(), connect_id_);
    
    // 调用流回调
    if (stream_callback_) {
        stream_callback_(data);
    } else {
        // 缓存数据
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_buffer_.append(data);
        xEventGroupSetBits(event_group_handle_, YM310_TCP_DATA_READY);
    }
}
