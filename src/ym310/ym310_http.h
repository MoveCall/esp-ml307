#ifndef _YM310_HTTP_H_
#define _YM310_HTTP_H_

#include "http.h"
#include "tcp.h"
#include "at_uart.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string>
#include <map>
#include <memory>

// 事件位定义
#define YM310_HTTP_ACTION_DONE  BIT0
#define YM310_HTTP_ERROR        BIT1

class Ym310AtModem;

/**
 * @brief YM310 HTTP 客户端实现
 * 
 * 对于 POST 请求：使用 HTTPINIT/HTTPPARA/HTTPACTION/HTTPREAD 等命令
 * 对于 GET 请求：使用 TCP 直接下载（绕过 HTTP 引擎的内存限制）
 * 需要先通过 SAPBR/PDP 激活承载
 */
class Ym310Http : public Http {
public:
    Ym310Http(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem);
    ~Ym310Http() override;

    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    void SetKeepAlive(bool enable) override;
    
    bool Open(const std::string& method, const std::string& url) override;
    void Close() override;
    
    int Read(char* buffer, size_t buffer_size) override;
    int Write(const char* buffer, size_t buffer_size) override;
    
    int GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;
    int GetLastError() override;

private:
    std::shared_ptr<AtUart> at_uart_;
    Ym310AtModem* modem_;
    
    int timeout_ms_ = 30000;
    bool keep_alive_ = false;
    std::map<std::string, std::string> headers_;
    std::string content_;
    
    int status_code_ = 0;
    size_t body_length_ = 0;
    std::string response_body_;
    std::map<std::string, std::string> response_headers_;
    size_t read_offset_ = 0;
    int last_error_ = 0;
    bool initialized_ = false;
    bool use_streaming_ = false;
    bool use_tcp_mode_ = false;  // 使用 TCP 直接下载
    
    // TCP 模式相关
    std::unique_ptr<Tcp> tcp_;
    std::string tcp_buffer_;
    bool headers_parsed_ = false;
    
    EventGroupHandle_t event_group_handle_;
    std::list<UrcCallback>::iterator urc_callback_it_;
    
    // HTTP 模组模式
    bool Initialize();
    bool SetParameters(const std::string& url);
    bool PerformAction(int method);
    bool ReadResponse();
    int MethodToCode(const std::string& method);
    
    // TCP 模式
    bool OpenViaTcp(const std::string& method, const std::string& url);
    bool ParseHttpHeaders();
    int ReadViaTcp(char* buffer, size_t buffer_size);
};

#endif // _YM310_HTTP_H_
