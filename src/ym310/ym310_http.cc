#include "ym310_http.h"
#include "ym310_at_modem.h"
#include <esp_log.h>
#include <cstring>
#include <algorithm>

static const char* TAG = "Ym310Http";

Ym310Http::Ym310Http(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem)
    : at_uart_(at_uart), modem_(modem) {
    
    event_group_handle_ = xEventGroupCreate();
    
    // 注册 URC 回调
    urc_callback_it_ = at_uart_->RegisterUrcCallback(
        [this](const std::string& command, const std::vector<AtArgumentValue>& arguments) {
            if (command == "HTTPACTION") {
                // +HTTPACTION: <method>,<status>,<len>
                if (arguments.size() >= 3) {
                    status_code_ = arguments[1].int_value;
                    body_length_ = arguments[2].int_value;
                    if (status_code_ >= 600) {
                        // 模组错误码：600=非HTTP PDU, 601=网络错误, 602=内存不足, 603=DNS错误, 604=栈忙, 605=SSL失败
                        ESP_LOGE(TAG, "HTTP module error: %d (len=%d)", status_code_, (int)body_length_);
                    } else {
                        ESP_LOGI(TAG, "HTTP response: status=%d, len=%d", status_code_, (int)body_length_);
                    }
                    xEventGroupSetBits(event_group_handle_, YM310_HTTP_ACTION_DONE);
                }
            }
        }
    );
}

Ym310Http::~Ym310Http() {
    Close();
    
    at_uart_->UnregisterUrcCallback(urc_callback_it_);
    
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
}

void Ym310Http::SetTimeout(int timeout_ms) {
    timeout_ms_ = timeout_ms;
}

void Ym310Http::SetHeader(const std::string& key, const std::string& value) {
    headers_[key] = value;
}

void Ym310Http::SetContent(std::string&& content) {
    content_ = std::move(content);
}

void Ym310Http::SetKeepAlive(bool enable) {
    keep_alive_ = enable;
}

bool Ym310Http::Initialize() {
    // 先尝试关闭可能存在的 HTTP 会话（避免内存不足错误 602）
    at_uart_->SendCommand("AT+HTTPTERM", 2000);
    
    // 等待一下让模组释放资源
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 初始化 HTTP 服务
    if (!at_uart_->SendCommand("AT+HTTPINIT", 5000)) {
        ESP_LOGE(TAG, "Failed to initialize HTTP");
        last_error_ = -1;
        return false;
    }
    
    // 启用重定向支持（处理 30x 跳转）
    if (!at_uart_->SendCommand("AT+HTTPPARA=\"REDIR\",1", 2000)) {
        ESP_LOGW(TAG, "Failed to enable redirect");
    }
    
    // 设置较长的超时时间（默认 120 秒，增加到 300 秒用于大文件下载）
    at_uart_->SendCommand("AT+HTTPPARA=\"TIMEOUT\",300", 1000);
    
    initialized_ = true;
    ESP_LOGI(TAG, "HTTP: Initialized");
    return true;
}

bool Ym310Http::SetParameters(const std::string& url) {
    // 检查是否是 HTTPS，需要启用 SSL
    bool is_https = (url.substr(0, 5) == "https");
    if (is_https) {
        // 启用 HTTP SSL
        if (!at_uart_->SendCommand("AT+HTTPSSL=1", 3000)) {
            ESP_LOGW(TAG, "Failed to enable HTTPSSL");
        }
        
        // 从 URL 中提取主机名
        size_t host_start = url.find("://") + 3;
        size_t host_end = url.find("/", host_start);
        if (host_end == std::string::npos) host_end = url.length();
        size_t port_pos = url.find(":", host_start);
        if (port_pos != std::string::npos && port_pos < host_end) {
            host_end = port_pos;
        }
        std::string hostname = url.substr(host_start, host_end - host_start);
        
        // 配置 SSL 参数（HTTPS 使用 SSL 上下文 ID = 153）
        std::string ssl_hostname = "AT+SSLCFG=\"hostname\",153,\"" + hostname + "\"";
        at_uart_->SendCommand(ssl_hostname, 1000);
        at_uart_->SendCommand("AT+SSLCFG=\"sslversion\",153,4", 1000);  // TLS 1.2
        at_uart_->SendCommand("AT+SSLCFG=\"seclevel\",153,0", 1000);    // 不验证证书
        
        ESP_LOGD(TAG, "HTTPS: %s", hostname.c_str());
    } else {
        // 禁用 HTTP SSL
        at_uart_->SendCommand("AT+HTTPSSL=0", 1000);
    }
    
    // 设置 CID（使用 SAPBR 承载 1）- 必须在 SSL 配置之后
    if (!at_uart_->SendCommand("AT+HTTPPARA=\"CID\",1")) {
        ESP_LOGE(TAG, "Failed to set HTTP CID");
        last_error_ = -2;
        return false;
    }
    
    // 设置 URL
    std::string cmd = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to set URL");
        last_error_ = -3;
        return false;
    }
    
    // 设置 Content-Type（如果有内容或有指定）
    auto it = headers_.find("Content-Type");
    if (it != headers_.end()) {
        ESP_LOGD(TAG, "Content-Type: %s", it->second.c_str());
        cmd = "AT+HTTPPARA=\"CONTENT\",\"" + it->second + "\"";
        at_uart_->SendCommand(cmd);
    } else if (!content_.empty()) {
        // 只在有内容时设置默认 Content-Type
        at_uart_->SendCommand("AT+HTTPPARA=\"CONTENT\",\"application/json\"");
    }
    
    // 设置 User-Agent（总是设置默认值）
    it = headers_.find("User-Agent");
    if (it != headers_.end()) {
        cmd = "AT+HTTPPARA=\"UA\",\"" + it->second + "\"";
    } else {
        // 默认 User-Agent
        cmd = "AT+HTTPPARA=\"UA\",\"ESP32-YM310/1.0\"";
    }
    at_uart_->SendCommand(cmd);
    
    // 设置其他自定义头部（通过 USERDATA）
    // 根据手册：一条一条地输入，后面输入的不会覆盖以前的
    for (const auto& header : headers_) {
        // 跳过已单独处理的头部
        if (header.first == "Content-Type" || header.first == "User-Agent") {
            continue;
        }
        std::string header_line = header.first + ": " + header.second;
        cmd = "AT+HTTPPARA=\"USERDATA\",\"" + header_line + "\"";
        at_uart_->SendCommand(cmd);
    }
    
    return true;
}

int Ym310Http::MethodToCode(const std::string& method) {
    if (method == "GET") return 0;
    if (method == "POST") return 1;
    if (method == "HEAD") return 2;
    if (method == "DELETE") return 3;
    return 0;  // 默认 GET
}

bool Ym310Http::PerformAction(int method) {
    // 清除事件位
    xEventGroupClearBits(event_group_handle_, YM310_HTTP_ACTION_DONE | YM310_HTTP_ERROR);
    
    // 如果是 POST 且有数据，先写入数据
    if (method == 1) {
        if (!content_.empty()) {
            ESP_LOGD(TAG, "POST data: %d bytes", (int)content_.size());
            // AT+HTTPDATA=<size>,<time>
            std::string cmd = "AT+HTTPDATA=" + std::to_string(content_.size()) + ",10000";
            if (!at_uart_->SendCommandWithData(cmd, 10000, true, 
                                                content_.c_str(), content_.size())) {
                ESP_LOGE(TAG, "Failed to send HTTP data");
                last_error_ = -4;
                return false;
            }
            ESP_LOGI(TAG, "POST data sent successfully");
        } else {
            ESP_LOGW(TAG, "POST method but no content to send");
        }
    }
    
    // 执行 HTTP 操作
    // 统一使用 HTTPACTION，然后通过 HTTPREAD 分块读取响应
    // HTTPEXACTION 用于流式读取但实现复杂，改用简单的分块读取
    std::string cmd = "AT+HTTPACTION=" + std::to_string(method);
    use_streaming_ = false;  // 不使用流式模式，统一用分块读取
    
    if (!at_uart_->SendCommand(cmd, 5000)) {
        ESP_LOGE(TAG, "Failed to send HTTPACTION");
        last_error_ = -5;
        return false;
    }
    
    // 等待响应（通过 +HTTPACTION 或 +HTTPEXACTION URC）
    EventBits_t bits = xEventGroupWaitBits(
        event_group_handle_,
        YM310_HTTP_ACTION_DONE | YM310_HTTP_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms_)
    );
    
    if (bits & YM310_HTTP_ACTION_DONE) {
        return true;
    } else {
        ESP_LOGE(TAG, "HTTP action timeout");
        last_error_ = -6;
        return false;
    }
}

bool Ym310Http::ReadResponse() {
    response_body_.clear();
    read_offset_ = 0;
    
    // 对于小响应（<= 4KB），预读到 response_body_
    // 对于大响应，通过 Read() 方法按需分块读取
    const size_t PREREAD_THRESHOLD = 4096;
    
    if (body_length_ == 0) {
        return true;
    }
    
    if (body_length_ <= PREREAD_THRESHOLD) {
        // 小响应：预读到内存
        ESP_LOGD(TAG, "Pre-reading small response: %d bytes", (int)body_length_);
        
        const size_t CHUNK_SIZE = 1024;
        size_t offset = 0;
        response_body_.reserve(body_length_);
        
        while (offset < body_length_) {
            size_t to_read = std::min(CHUNK_SIZE, body_length_ - offset);
            std::string cmd = "AT+HTTPREAD=" + std::to_string(offset) + "," + 
                              std::to_string(to_read);
            
            if (!at_uart_->SendCommand(cmd, 10000)) {
                ESP_LOGE(TAG, "Failed to read HTTP response at offset %d", (int)offset);
                last_error_ = -7;
                return false;
            }
            
            std::string response = at_uart_->GetResponse();
            
            // 响应格式：+HTTPREAD: <len>\r\n<data>
            size_t data_start = response.find("\r\n");
            if (data_start != std::string::npos) {
                response_body_.append(response.substr(data_start + 2));
            } else {
                response_body_.append(response);
            }
            
            offset += to_read;
        }
    } else {
        // 大响应：不预读，通过 Read() 分块读取
        ESP_LOGI(TAG, "Large response (%d bytes), will read via Read()", (int)body_length_);
        use_streaming_ = true;  // 标记为流式模式，Read() 会使用 HTTPREAD
    }
    
    return true;
}

bool Ym310Http::Open(const std::string& method, const std::string& url) {
    ESP_LOGI(TAG, "HTTP %s %s (content: %d bytes, headers: %d)", 
             method.c_str(), url.c_str(), (int)content_.size(), (int)headers_.size());
    
    // 对于 GET 请求，使用 TCP 直接下载（绕过 HTTP 引擎的内存限制）
    if (method == "GET") {
        return OpenViaTcp(method, url);
    }
    
    // POST 等请求使用 HTTP 模组
    use_tcp_mode_ = false;
    
    // 初始化
    if (!Initialize()) {
        return false;
    }
    
    // 设置参数
    if (!SetParameters(url)) {
        return false;
    }
    
    // 执行请求
    int method_code = MethodToCode(method);
    if (!PerformAction(method_code)) {
        return false;
    }
    
    // 读取响应（即使状态码非 2xx 也读取）
    if (!ReadResponse()) {
        return false;
    }
    
    // 调试：打印响应体（特别是错误响应）
    if (status_code_ >= 400 && !response_body_.empty()) {
        ESP_LOGW(TAG, "HTTP error response body: %.*s", 
                 (int)std::min(response_body_.size(), (size_t)256), 
                 response_body_.c_str());
    }
    
    return true;
}

void Ym310Http::Close() {
    if (use_tcp_mode_) {
        // TCP 模式：断开 TCP 连接
        if (tcp_) {
            tcp_->Disconnect();
            tcp_.reset();
        }
    } else {
        // HTTP 模组模式：终止 HTTP 会话
        at_uart_->SendCommand("AT+HTTPTERM", 3000);
    }
    
    initialized_ = false;
    
    // 清理状态
    headers_.clear();
    content_.clear();
    response_body_.clear();
    response_headers_.clear();
    tcp_buffer_.clear();
    status_code_ = 0;
    body_length_ = 0;
    read_offset_ = 0;
    use_streaming_ = false;
    use_tcp_mode_ = false;
    headers_parsed_ = false;
}

int Ym310Http::Read(char* buffer, size_t buffer_size) {
    if (use_tcp_mode_) {
        // TCP 模式
        return ReadViaTcp(buffer, buffer_size);
    } else if (use_streaming_) {
        // HTTP 模组流式模式：使用 AT+HTTPREAD 分块读取
        if (read_offset_ >= body_length_) {
            return 0;  // 已读完
        }
        
        // 计算要读取的大小（HTTPREAD 最大 3356 字节）
        size_t to_read = std::min(buffer_size, body_length_ - read_offset_);
        to_read = std::min(to_read, (size_t)2048);  // 保守使用 2048
        
        std::string cmd = "AT+HTTPREAD=" + std::to_string(read_offset_) + "," + 
                          std::to_string(to_read);
        if (!at_uart_->SendCommand(cmd, 30000)) {
            ESP_LOGE(TAG, "Failed to send HTTPREAD at offset %d", (int)read_offset_);
            return -1;
        }
        
        std::string response = at_uart_->GetResponse();
        
        // 响应格式：+HTTPREAD: <len>\r\n<data>
        size_t data_start = response.find("\r\n");
        if (data_start == std::string::npos) {
            ESP_LOGE(TAG, "Invalid HTTPREAD response");
            return -1;
        }
        
        data_start += 2;
        size_t data_len = response.size() - data_start;
        size_t copy_len = std::min(data_len, buffer_size);
        std::memcpy(buffer, response.data() + data_start, copy_len);
        read_offset_ += copy_len;
        
        return copy_len;
    } else {
        // 传统模式：从预读的 response_body_ 中读取
        if (read_offset_ >= response_body_.size()) {
            return 0;
        }
        
        size_t to_copy = std::min(buffer_size, response_body_.size() - read_offset_);
        std::memcpy(buffer, response_body_.data() + read_offset_, to_copy);
        read_offset_ += to_copy;
        
        return to_copy;
    }
}

int Ym310Http::Write(const char* buffer, size_t buffer_size) {
    // 追加到 content_
    content_.append(buffer, buffer_size);
    return buffer_size;
}

int Ym310Http::GetStatusCode() {
    return status_code_;
}

std::string Ym310Http::GetResponseHeader(const std::string& key) const {
    auto it = response_headers_.find(key);
    if (it != response_headers_.end()) {
        return it->second;
    }
    return "";
}

size_t Ym310Http::GetBodyLength() {
    return body_length_;
}

std::string Ym310Http::ReadAll() {
    return response_body_;
}

int Ym310Http::GetLastError() {
    return last_error_;
}

// ============== TCP 模式实现 ==============

bool Ym310Http::OpenViaTcp(const std::string& method, const std::string& url) {
    use_tcp_mode_ = true;
    headers_parsed_ = false;
    tcp_buffer_.clear();
    response_headers_.clear();
    
    // 解析 URL
    bool is_https = (url.substr(0, 5) == "https");
    int default_port = is_https ? 443 : 80;
    
    size_t host_start = url.find("://") + 3;
    size_t path_start = url.find("/", host_start);
    if (path_start == std::string::npos) path_start = url.length();
    
    std::string host_port = url.substr(host_start, path_start - host_start);
    std::string path = (path_start < url.length()) ? url.substr(path_start) : "/";
    
    // 解析主机和端口
    std::string host;
    int port = default_port;
    size_t colon_pos = host_port.find(":");
    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        host = host_port;
    }
    
    ESP_LOGI(TAG, "TCP HTTP: %s %s:%d%s", method.c_str(), host.c_str(), port, path.c_str());
    
    // 创建 TCP 或 SSL 连接
    if (is_https) {
        tcp_ = modem_->CreateSsl(0);
    } else {
        tcp_ = modem_->CreateTcp(0);
    }
    
    if (!tcp_) {
        ESP_LOGE(TAG, "Failed to create TCP connection");
        last_error_ = -10;
        return false;
    }
    
    // 设置数据接收回调
    tcp_->OnStream([this](const std::string& data) {
        tcp_buffer_.append(data);
    });
    
    // 连接服务器
    if (!tcp_->Connect(host, port)) {
        ESP_LOGE(TAG, "Failed to connect to %s:%d", host.c_str(), port);
        last_error_ = -11;
        return false;
    }
    
    // 构建 HTTP 请求
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "User-Agent: ESP32-YM310/1.0\r\n";
    request += "Connection: close\r\n";
    
    // 添加自定义头部
    for (const auto& header : headers_) {
        request += header.first + ": " + header.second + "\r\n";
    }
    
    request += "\r\n";
    
    // 发送请求
    if (tcp_->Send(request) <= 0) {
        ESP_LOGE(TAG, "Failed to send HTTP request");
        last_error_ = -12;
        return false;
    }
    
    // 等待并解析响应头
    if (!ParseHttpHeaders()) {
        return false;
    }
    
    return true;
}

bool Ym310Http::ParseHttpHeaders() {
    // 等待直到收到完整的头部（\r\n\r\n）
    const int HEADER_TIMEOUT_MS = 30000;
    int elapsed = 0;
    const int CHECK_INTERVAL = 50;
    
    while (elapsed < HEADER_TIMEOUT_MS) {
        // 检查是否已有完整的头部
        size_t header_end = tcp_buffer_.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            // 解析头部
            std::string headers_str = tcp_buffer_.substr(0, header_end);
            tcp_buffer_ = tcp_buffer_.substr(header_end + 4);  // 保留 body 部分
            
            // 解析状态行：HTTP/1.1 200 OK
            size_t first_line_end = headers_str.find("\r\n");
            if (first_line_end != std::string::npos) {
                std::string status_line = headers_str.substr(0, first_line_end);
                size_t code_start = status_line.find(" ");
                if (code_start != std::string::npos) {
                    status_code_ = std::stoi(status_line.substr(code_start + 1, 3));
                }
                headers_str = headers_str.substr(first_line_end + 2);
            }
            
            // 解析其他头部
            size_t pos = 0;
            while (pos < headers_str.length()) {
                size_t line_end = headers_str.find("\r\n", pos);
                if (line_end == std::string::npos) line_end = headers_str.length();
                
                std::string line = headers_str.substr(pos, line_end - pos);
                size_t colon = line.find(": ");
                if (colon != std::string::npos) {
                    std::string key = line.substr(0, colon);
                    std::string value = line.substr(colon + 2);
                    response_headers_[key] = value;
                    
                    // 获取 Content-Length（不区分大小写）
                    if (key == "Content-Length" || key == "content-length") {
                        body_length_ = std::stoul(value);
                    }
                }
                
                pos = line_end + 2;
            }
            
            ESP_LOGI(TAG, "TCP HTTP response: status=%d, body_length=%d", 
                     status_code_, (int)body_length_);
            headers_parsed_ = true;
            return true;
        }
        
        // 等待更多数据
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL));
        elapsed += CHECK_INTERVAL;
    }
    
    ESP_LOGE(TAG, "Timeout waiting for HTTP headers");
    last_error_ = -14;
    return false;
}

int Ym310Http::ReadViaTcp(char* buffer, size_t buffer_size) {
    if (!headers_parsed_) {
        return -1;
    }
    
    // 检查是否已读完（基于 Content-Length）
    if (body_length_ > 0 && read_offset_ >= body_length_) {
        return 0;
    }
    
    // 等待数据（如果缓冲区为空）
    const int READ_TIMEOUT_MS = 10000;
    int elapsed = 0;
    const int CHECK_INTERVAL = 50;
    
    while (tcp_buffer_.empty() && elapsed < READ_TIMEOUT_MS) {
        // 检查连接是否仍然有效
        if (!tcp_ || !tcp_->connected()) {
            // 连接已关闭，检查是否已读完所有数据
            if (body_length_ > 0 && read_offset_ >= body_length_) {
                return 0;
            }
            if (body_length_ == 0) {
                // 没有 Content-Length，连接关闭就是结束
                return 0;
            }
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL));
        elapsed += CHECK_INTERVAL;
    }
    
    // 从缓冲区读取
    if (!tcp_buffer_.empty()) {
        size_t to_copy = std::min(buffer_size, tcp_buffer_.size());
        std::memcpy(buffer, tcp_buffer_.data(), to_copy);
        tcp_buffer_ = tcp_buffer_.substr(to_copy);
        read_offset_ += to_copy;
        return to_copy;
    }
    
    return 0;
}
