#include "ym310_at_modem.h"
#include <esp_log.h>
#include <esp_err.h>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <cstring>
#include "ym310_tcp.h"
#include "ym310_ssl.h"
#include "ym310_udp.h"
#include "ym310_http.h"
#include "ym310_mqtt.h"
#include "web_socket.h"

#define TAG "Ym310AtModem"

Ym310AtModem::Ym310AtModem(std::shared_ptr<AtUart> at_uart) : AtModem(at_uart) {
    ESP_LOGI(TAG, "Initializing YM310 AT Modem");
    InitModem();
}

Ym310AtModem::~Ym310AtModem() {
    // 清理资源
    if (pdp_activated_) {
        DeactivatePdpContext();
    }
    if (sapbr_activated_) {
        DeactivateSapbr();
    }
}

void Ym310AtModem::InitModem() {
    // 关闭 echo
    at_uart_->SendCommand("ATE0");
    
    // 设置多连接模式
    at_uart_->SendCommand("AT+CIPMUX=1");
    
    // 设置快发模式（不等待 SEND OK）
    at_uart_->SendCommand("AT+CIPQSEND=1");
    
    // 设置接收数据时显示数据头
    at_uart_->SendCommand("AT+CIPHEAD=1");
    
    // 关闭透传模式
    at_uart_->SendCommand("AT+CIPMODE=0");
}

void Ym310AtModem::Reboot() {
    at_uart_->SendCommand("AT+RESET");
}

bool Ym310AtModem::SetSleepMode(bool enable, int delay_seconds) {
    // YM310 使用 AT+CSCLK 控制睡眠模式
    // 0: 禁用睡眠
    // 1: 允许通过 DTR 控制睡眠
    // 2: 自动睡眠
    if (enable) {
        return at_uart_->SendCommand("AT+CSCLK=1");
    } else {
        return at_uart_->SendCommand("AT+CSCLK=0");
    }
}

NetworkStatus Ym310AtModem::WaitForNetworkReady(int timeout_ms) {
    ESP_LOGI(TAG, "Waiting for YM310 network ready...");
    
    // 调用基类方法检查网络注册状态
    NetworkStatus status = AtModem::WaitForNetworkReady(timeout_ms);
    
    if (status == NetworkStatus::Ready) {
        ESP_LOGI(TAG, "Network registered");
        // 注意：不在这里激活 PDP/SAPBR
        // - HTTP 需要 SAPBR，在 CreateHttp() 时按需激活
        // - TCP/UDP/SSL 需要 PDP，在 CreateTcp/CreateSsl/CreateUdp() 时按需激活
        // - MQTT 使用自己的连接方式，在 CreateMqtt() 时处理
    }
    
    return status;
}

bool Ym310AtModem::ActivatePdpContext() {
    if (pdp_activated_) {
        ESP_LOGD(TAG, "PDP context already activated");
        return true;
    }
    
    ESP_LOGD(TAG, "Activating PDP...");
    
    // 先关闭之前的连接
    at_uart_->SendCommand("AT+CIPSHUT", 5000);
    
    // 设置 APN（使用自动获取的 APN）
    if (!at_uart_->SendCommand("AT+CSTT", 5000)) {
        ESP_LOGE(TAG, "Failed to set APN (CSTT)");
        return false;
    }
    
    // 激活移动场景
    if (!at_uart_->SendCommand("AT+CIICR", 30000)) {
        ESP_LOGE(TAG, "Failed to activate GPRS (CIICR)");
        return false;
    }
    
    // 获取本地 IP 地址
    // 注意：AT+CIFSR 不返回 OK，直接返回 IP 地址
    // 使用 timeout_ms=0 发送命令，然后读取响应
    at_uart_->SendCommand("AT+CIFSR", 0);
    
    // 等待响应（IP 地址）
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 读取响应
    local_ip_ = at_uart_->GetResponse();
    
    // 去除可能的空白字符和换行符
    local_ip_.erase(0, local_ip_.find_first_not_of(" \t\r\n"));
    if (!local_ip_.empty()) {
        size_t last_valid = local_ip_.find_last_not_of(" \t\r\n");
        if (last_valid != std::string::npos) {
            local_ip_.erase(last_valid + 1);
        }
    }
    
    // 验证 IP 地址格式（应该是数字和点）
    if (local_ip_.empty() || local_ip_.find("ERROR") != std::string::npos ||
        local_ip_.find('.') == std::string::npos) {
        ESP_LOGE(TAG, "Invalid IP address: %s", local_ip_.c_str());
        return false;
    }
    
    ESP_LOGD(TAG, "Local IP: %s", local_ip_.c_str());
    
    pdp_activated_ = true;
    ESP_LOGD(TAG, "PDP activated, IP: %s", local_ip_.c_str());
    return true;
}

bool Ym310AtModem::DeactivatePdpContext() {
    if (!pdp_activated_) {
        return true;
    }
    
    ESP_LOGI(TAG, "Deactivating PDP context...");
    
    if (at_uart_->SendCommand("AT+CIPSHUT", 5000)) {
        pdp_activated_ = false;
        local_ip_.clear();
        
        // 重置所有连接状态
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            connection_states_[i] = false;
        }
        
        return true;
    }
    
    return false;
}

bool Ym310AtModem::ActivateSapbr() {
    if (sapbr_activated_) {
        ESP_LOGD(TAG, "SAPBR already activated");
        return true;
    }
    
    ESP_LOGI(TAG, "Activating SAPBR bearer...");
    
    
    // 使用 URC 回调捕获 +SAPBR 响应（因为 +开头的响应被当作 URC 处理）
    std::string sapbr_response;
    auto urc_callback = at_uart_->RegisterUrcCallback(
        [&sapbr_response](const std::string& command, const std::vector<AtArgumentValue>& args) {
            if (command == "SAPBR" && args.size() >= 3) {
                // +SAPBR: <bearer_id>,<status>,<ip>
                // status: 1=connected, 3=closing, 0=connecting
                sapbr_response = command + ":";
                for (size_t i = 0; i < args.size(); i++) {
                    if (i > 0) sapbr_response += ",";
                    if (args[i].type == AtArgumentValue::Type::String) {
                        sapbr_response += args[i].string_value;
                    } else if (args[i].type == AtArgumentValue::Type::Int) {
                        sapbr_response += std::to_string(args[i].int_value);
                    }
                }
                ESP_LOGD(TAG, "SAPBR response: %s", sapbr_response.c_str());
            }
        });
    
    // 第一步：配置承载类型（必须先配置才能激活）
    ESP_LOGD(TAG, "SAPBR: Setting CONTYPE...");
    if (!at_uart_->SendCommand("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 3000)) {
        ESP_LOGE(TAG, "Failed to set SAPBR CONTYPE");
        at_uart_->UnregisterUrcCallback(urc_callback);
        return false;
    }
    
    // 第二步：设置 APN（使用 cmnet）
    ESP_LOGD(TAG, "SAPBR: Setting APN...");
    if (!at_uart_->SendCommand("AT+SAPBR=3,1,\"APN\",\"cmnet\"", 3000)) {
        ESP_LOGE(TAG, "Failed to set SAPBR APN");
        at_uart_->UnregisterUrcCallback(urc_callback);
        return false;
    }
    
    // 第三步：激活承载（最多等待 65 秒）
    // 注意：如果已经激活，会返回 +CME ERROR: 3，这是正常的
    ESP_LOGD(TAG, "SAPBR: Activating...");
    bool activate_ok = at_uart_->SendCommand("AT+SAPBR=1,1", 65000);
    if (!activate_ok) {
        ESP_LOGW(TAG, "AT+SAPBR=1,1 returned error (may already be activated), checking status...");
    }
    
    // 第四步：查询承载状态确认激活成功
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGD(TAG, "SAPBR: Checking status...");
    sapbr_response.clear();  // 清除之前的响应
    
    if (at_uart_->SendCommand("AT+SAPBR=2,1", 5000)) {
        // 等待一下让 URC 回调处理完成
        vTaskDelay(pdMS_TO_TICKS(100));
        
        ESP_LOGD(TAG, "SAPBR: %s", sapbr_response.c_str());
        
        // 解析 URC 响应：SAPBR:<bearer_id>,<status>,<ip>
        // status=1 表示 connected，且 IP 不是 0.0.0.0
        // 例如：SAPBR:1,1,10.219.121.75
        if (!sapbr_response.empty()) {
            // 检查 status=1 (connected)
            size_t first_comma = sapbr_response.find(',');
            if (first_comma != std::string::npos) {
                size_t second_comma = sapbr_response.find(',', first_comma + 1);
                if (second_comma != std::string::npos) {
                    std::string status_str = sapbr_response.substr(first_comma + 1, second_comma - first_comma - 1);
                    std::string ip_str = sapbr_response.substr(second_comma + 1);
                    
                    ESP_LOGD(TAG, "SAPBR status=%s, IP=%s", status_str.c_str(), ip_str.c_str());
                    
                    if (status_str == "1" && ip_str != "0.0.0.0") {
                        sapbr_activated_ = true;
                        at_uart_->UnregisterUrcCallback(urc_callback);
                        ESP_LOGI(TAG, "SAPBR bearer activated successfully, IP: %s", ip_str.c_str());
                        return true;
                    }
                }
            }
        }
    }
    
    at_uart_->UnregisterUrcCallback(urc_callback);
    at_uart_->SetDebug(false);
    ESP_LOGE(TAG, "Failed to activate SAPBR bearer");
    return false;
}

bool Ym310AtModem::DeactivateSapbr() {
    if (!sapbr_activated_) {
        return true;
    }
    
    ESP_LOGI(TAG, "Deactivating SAPBR bearer...");
    
    if (at_uart_->SendCommand("AT+SAPBR=0,1", 5000)) {
        sapbr_activated_ = false;
        return true;
    }
    
    return false;
}

void Ym310AtModem::HandleUrc(const std::string& command, 
                              const std::vector<AtArgumentValue>& arguments) {
    // 调用基类处理通用 URC
    AtModem::HandleUrc(command, arguments);
    
    // YM310 特有 URC 处理
    if (command == "RECEIVE") {
        // +RECEIVE,<n>,<len>:<data>
        // TCP 数据接收，转发给对应的 TCP 连接处理
        if (arguments.size() >= 2) {
            int conn_id = arguments[0].int_value;
            int data_len = arguments[1].int_value;
            ESP_LOGD(TAG, "TCP data received on connection %d, length: %d", conn_id, data_len);
        }
    } else if (command == "CLOSED") {
        // 连接关闭通知
        if (arguments.size() >= 1) {
            int conn_id = arguments[0].int_value;
            if (conn_id >= 0 && conn_id < MAX_CONNECTIONS) {
                connection_states_[conn_id] = false;
                ESP_LOGI(TAG, "Connection %d closed", conn_id);
            }
        }
    } else if (command == "CONNECTOK" || command == "CONNECT OK") {
        // 连接成功（单连接模式），根据手册使用 CONNECTOK（无空格）
        ESP_LOGD(TAG, "Connection established");
    } else if (command == "CONNECTFAIL" || command == "CONNECT FAIL") {
        // 连接失败
        ESP_LOGE(TAG, "Connection failed");
    } else if (command == "ALREADY CONNECT") {
        // 连接已存在
        ESP_LOGW(TAG, "Connection already exists");
    } else if (command == "SEND OK") {
        // 数据发送成功
        ESP_LOGD(TAG, "Data sent successfully");
    } else if (command == "SEND FAIL") {
        // 数据发送失败
        ESP_LOGE(TAG, "Data send failed");
    } else if (command == "+PDP" || command == "PDP DEACT") {
        // PDP 上下文去激活
        ESP_LOGW(TAG, "PDP context deactivated");
        pdp_activated_ = false;
        network_ready_ = false;
        if (on_network_state_changed_) {
            on_network_state_changed_(false);
        }
    } else if (command == "HTTPACTION") {
        // HTTP 响应通知
        // +HTTPACTION: <method>,<status>,<len>
        if (arguments.size() >= 3) {
            int method = arguments[0].int_value;
            int status = arguments[1].int_value;
            int len = arguments[2].int_value;
            ESP_LOGD(TAG, "HTTP done: method=%d, status=%d, len=%d", 
                     method, status, len);
        }
    } else if (command == "MSUB") {
        // MQTT 消息接收
        // +MSUB: "<topic>",<len>,<data>
        if (arguments.size() >= 2) {
            ESP_LOGD(TAG, "MQTT message received on topic: %s", 
                     arguments[0].string_value.c_str());
        }
    } else if (command == "MCONNECT") {
        // MQTT 连接状态
        // +MCONNECT: <result>,<ret_code>
        if (arguments.size() >= 2) {
            int result = arguments[0].int_value;
            int ret_code = arguments[1].int_value;
            ESP_LOGI(TAG, "MQTT connect result: %d, ret_code: %d", result, ret_code);
        }
    }
}

// ==================== 创建网络接口 ====================

std::unique_ptr<Http> Ym310AtModem::CreateHttp(int connect_id) {
    // 确保 SAPBR 已激活
    if (!sapbr_activated_) {
        if (!ActivateSapbr()) {
            ESP_LOGE(TAG, "Failed to activate SAPBR for HTTP");
            return nullptr;
        }
    }
    return std::make_unique<Ym310Http>(at_uart_, this);
}

std::unique_ptr<Tcp> Ym310AtModem::CreateTcp(int connect_id) {
    assert(connect_id >= 0 && connect_id < MAX_CONNECTIONS);
    
    // 确保 PDP 已激活
    if (!pdp_activated_) {
        if (!ActivatePdpContext()) {
            ESP_LOGE(TAG, "Failed to activate PDP for TCP");
            return nullptr;
        }
    }
    
    return std::make_unique<Ym310Tcp>(at_uart_, this, connect_id);
}

std::unique_ptr<Tcp> Ym310AtModem::CreateSsl(int connect_id) {
    assert(connect_id >= 0 && connect_id < MAX_CONNECTIONS);
    
    // 确保 PDP 已激活
    if (!pdp_activated_) {
        if (!ActivatePdpContext()) {
            ESP_LOGE(TAG, "Failed to activate PDP for SSL");
            return nullptr;
        }
    }
    
    return std::make_unique<Ym310Ssl>(at_uart_, this, connect_id);
}

std::unique_ptr<Udp> Ym310AtModem::CreateUdp(int connect_id) {
    assert(connect_id >= 0 && connect_id < MAX_CONNECTIONS);
    
    // 确保 PDP 已激活
    if (!pdp_activated_) {
        if (!ActivatePdpContext()) {
            ESP_LOGE(TAG, "Failed to activate PDP for UDP");
            return nullptr;
        }
    }
    
    return std::make_unique<Ym310Udp>(at_uart_, this, connect_id);
}

std::unique_ptr<Mqtt> Ym310AtModem::CreateMqtt(int connect_id) {
    // 根据 AT 手册：模块开机注册后缺省就有一个激活的 PDP 承载
    // MQTT 不需要额外激活 PDP，可以直接使用 MQTT AT 命令
    // 注意：如果之前手动激活过 PDP，可能会影响 MQTT 连接
    
    return std::make_unique<Ym310Mqtt>(at_uart_, this, connect_id);
}

std::unique_ptr<WebSocket> Ym310AtModem::CreateWebSocket(int connect_id) {
    assert(connect_id >= 0 && connect_id < MAX_CONNECTIONS);
    // WebSocket 复用通用实现，基于 TCP/SSL
    return std::make_unique<WebSocket>(this, connect_id);
}
