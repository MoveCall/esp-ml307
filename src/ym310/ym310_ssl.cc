#include "ym310_ssl.h"
#include "ym310_at_modem.h"
#include <esp_log.h>

static const char* TAG = "Ym310Ssl";

Ym310Ssl::Ym310Ssl(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id)
    : Ym310Tcp(at_uart, modem, connect_id) {
}

Ym310Ssl::~Ym310Ssl() {
    // 关闭 SSL
    if (connected_) {
        Disconnect();
    }
    
    // 禁用 SSL
    at_uart_->SendCommand("AT+CIPSSL=0");
}

void Ym310Ssl::SetClientCert(const std::string& cert, const std::string& key) {
    client_cert_ = cert;
    client_key_ = key;
}

void Ym310Ssl::SetCaCert(const std::string& ca_cert) {
    ca_cert_ = ca_cert;
}

bool Ym310Ssl::ConfigureConnection() {
    return ConfigureSsl();
}

bool Ym310Ssl::ConfigureSsl() {
    ESP_LOGI(TAG, "Configuring SSL for connection %d", connect_id_);
    
    // 启用 SSL
    // AT+CIPSSL=<ssl_mode>
    // 0: 禁用 SSL
    // 1: 启用 SSL，用于下一个 CIPSTART
    if (!at_uart_->SendCommand("AT+CIPSSL=1")) {
        ESP_LOGE(TAG, "Failed to enable SSL");
        return false;
    }
    
    // 配置 SSL 版本
    // AT+SSLCFG="sslversion",<n>,<sslversion>
    // <n>: SSL 上下文 ID，与 CIPSTART 中的连接号绑定
    // <sslversion>: 0=SSL3.0, 1=TLS1.0, 2=TLS1.1, 3=TLS1.2, 4=ALL
    std::string cmd = "AT+SSLCFG=\"sslversion\"," + std::to_string(connect_id_) + ",4";
    if (!at_uart_->SendCommand(cmd)) {
        ESP_LOGW(TAG, "Failed to set SSL version, continuing...");
    }
    
    // 配置安全等级 (证书验证)
    // AT+SSLCFG="seclevel",<n>,<seclevel>
    // 0: 不验证服务器证书
    // 1: 验证服务器证书（需要设置 CA 证书）
    // 2: 双向认证（需要设置 CA 证书和客户端证书）
    int sec_level = ssl_verify_ ? 1 : 0;
    cmd = "AT+SSLCFG=\"seclevel\"," + std::to_string(connect_id_) + "," + std::to_string(sec_level);
    if (!at_uart_->SendCommand(cmd)) {
        ESP_LOGW(TAG, "Failed to set SSL security level, continuing...");
    }
    
    // 如果有 CA 证书，设置证书路径
    if (!ca_cert_.empty()) {
        // AT+SSLCFG="cacert",<n>,<cacertpath>
        // 需要先通过 AT+FSWRITE 写入文件系统
        ESP_LOGW(TAG, "CA certificate upload not implemented yet");
    }
    
    // 如果有客户端证书，设置证书路径
    if (!client_cert_.empty() && !client_key_.empty()) {
        // AT+SSLCFG="clientcert",<n>,<cert_path>
        // AT+SSLCFG="clientkey",<n>,<key_path>
        ESP_LOGW(TAG, "Client certificate upload not implemented yet");
    }
    
    ESP_LOGI(TAG, "SSL configured: seclevel=%d", sec_level);
    return true;
}
