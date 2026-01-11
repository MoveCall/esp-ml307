#ifndef _YM310_SSL_H_
#define _YM310_SSL_H_

#include "ym310_tcp.h"

/**
 * @brief YM310 SSL/TLS 客户端实现
 * 
 * 继承自 Ym310Tcp，使用 AT+CIPSSL 启用 SSL
 * 以及 AT+SSLCFG 配置证书验证
 */
class Ym310Ssl : public Ym310Tcp {
public:
    Ym310Ssl(std::shared_ptr<AtUart> at_uart, Ym310AtModem* modem, int connect_id);
    ~Ym310Ssl() override;

    // SSL 配置方法
    void SetSslVerify(bool verify) { ssl_verify_ = verify; }
    void SetClientCert(const std::string& cert, const std::string& key);
    void SetCaCert(const std::string& ca_cert);

protected:
    bool ConfigureConnection() override;
    std::string GetProtocol() override { return "TCP"; }  // 仍使用 TCP，通过 CIPSSL 启用加密

private:
    bool ssl_verify_ = false;  // 默认不验证证书
    std::string client_cert_;
    std::string client_key_;
    std::string ca_cert_;
    
    bool ConfigureSsl();
};

#endif // _YM310_SSL_H_
