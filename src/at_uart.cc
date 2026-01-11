#include "at_uart.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>

#define TAG "AtUart"


// AtUart 构造函数实现
AtUart::AtUart(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), ri_pin_(ri_pin), uart_num_(UART_NUM),
      baud_rate_(115200), initialized_(false), dtr_pin_state_(false),
      pm_lock_(nullptr), ri_pm_lock_(nullptr), ri_pm_lock_acquired_(false),
      event_task_handle_(nullptr), receive_task_handle_(nullptr),
      event_queue_handle_(nullptr), event_group_handle_(nullptr) {
    // Create power management lock for DTR operations
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_pm_lock", &pm_lock_);
    // Create power management lock for RI pin operations
    if (ri_pin_ != GPIO_NUM_NC) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_ri_pm_lock", &ri_pm_lock_);
    }
}

AtUart::~AtUart() {
    if (receive_task_handle_) {
        vTaskDelete(receive_task_handle_);
    }
    if (event_task_handle_) {
        vTaskDelete(event_task_handle_);
    }
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
    if (initialized_) {
        // Remove RI pin ISR handler if configured
        if (ri_pin_ != GPIO_NUM_NC) {
            gpio_isr_handler_remove(ri_pin_);
        }
        uart_driver_delete(uart_num_);
    }
    if (ri_pm_lock_) {
        if (ri_pm_lock_acquired_) {
            esp_pm_lock_release(ri_pm_lock_);
        }
        esp_pm_lock_delete(ri_pm_lock_);
    }
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void AtUart::Initialize() {
    if (initialized_) {
        return;
    }
    
    event_group_handle_ = xEventGroupCreate();
    if (!event_group_handle_) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate_;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    
    ESP_ERROR_CHECK(uart_driver_install(uart_num_, 8192, 0, 100, &event_queue_handle_, ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    if (dtr_pin_ != GPIO_NUM_NC) {
        gpio_config_t config = {};
        config.pin_bit_mask = (1ULL << dtr_pin_);
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&config);
        gpio_set_level(dtr_pin_, 0);
        dtr_pin_state_ = false;  // 记录初始状态为低电平
    }

    // Configure RI pin as input with interrupt
    if (ri_pin_ != GPIO_NUM_NC) {
        gpio_config_t ri_config = {};
        ri_config.pin_bit_mask = (1ULL << ri_pin_);
        ri_config.mode = GPIO_MODE_INPUT;
        ri_config.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable pull-up for RI pin
        ri_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ri_config.intr_type = GPIO_INTR_LOW_LEVEL;  // Trigger on falling edge (low level)
        gpio_config(&ri_config);

        gpio_wakeup_enable(ri_pin_, GPIO_INTR_LOW_LEVEL);

        // Add ISR handler for RI pin
        gpio_isr_handler_add(ri_pin_, RiPinIsrHandler, this);
    }

    xTaskCreatePinnedToCore([](void* arg) {
        auto ml307_at_modem = (AtUart*)arg;
        ml307_at_modem->EventTask();
        vTaskDelete(NULL);
    }, "modem_event", 2048, this, configMAX_PRIORITIES - 1, &event_task_handle_, 0);

    xTaskCreatePinnedToCore([](void* arg) {
        auto ml307_at_modem = (AtUart*)arg;
        ml307_at_modem->ReceiveTask();
        vTaskDelete(NULL);
    }, "modem_receive", 2048 * 3, this, configMAX_PRIORITIES - 2, &receive_task_handle_, 0);
    initialized_ = true;
}

void AtUart::EventTask() {
    uart_event_t event;
    while (true) {
        if (xQueueReceive(event_queue_handle_, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type)
            {
            case UART_DATA:
                xEventGroupSetBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE);
                break;
            case UART_BREAK:
                xEventGroupSetBits(event_group_handle_, AT_EVENT_BREAK);
                break;
            case UART_BUFFER_FULL:
                xEventGroupSetBits(event_group_handle_, AT_EVENT_BUFFER_FULL);
                break;
            case UART_FIFO_OVF:
                xEventGroupSetBits(event_group_handle_, AT_EVENT_FIFO_OVF);
                break;
            default:
                ESP_LOGE(TAG, "unknown event type: %d", event.type);
                break;
            }
        }
    }
}

void AtUart::ReceiveTask() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_DATA_AVAILABLE | AT_EVENT_FIFO_OVF |
            AT_EVENT_BUFFER_FULL | AT_EVENT_BREAK | AT_EVENT_RI_PIN_INT, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & AT_EVENT_DATA_AVAILABLE) {
            size_t available;
            uart_get_buffered_data_len(uart_num_, &available);
            if (available > 0) {
                // Extend rx_buffer_ and read into buffer
                rx_buffer_.resize(rx_buffer_.size() + available);
                char* rx_buffer_ptr = &rx_buffer_[rx_buffer_.size() - available];
                uart_read_bytes(uart_num_, rx_buffer_ptr, available, portMAX_DELAY);
                while (ParseResponse()) {}
            }
        }
        if (bits & AT_EVENT_FIFO_OVF) {
            ESP_LOGE(TAG, "FIFO overflow");
            HandleUrc("FIFO_OVERFLOW", {});
        }
        if (bits & AT_EVENT_BREAK) {
            ESP_LOGE(TAG, "Break");
        }
        if (bits & AT_EVENT_BUFFER_FULL) {
            ESP_LOGE(TAG, "Buffer full");
        }

        if (ri_pin_ != GPIO_NUM_NC) {
            if (bits & AT_EVENT_RI_PIN_INT) {
                // RI pin went low - acquire PM lock to prevent sleep
                if (!ri_pm_lock_acquired_) {
                    esp_pm_lock_acquire(ri_pm_lock_);
                    ri_pm_lock_acquired_ = true;
                    ESP_LOGD(TAG, "RI pin went low, PM lock acquired");
                }
            } else {
                // Release RI PM lock when data is available (modem has data to send)
                if (ri_pm_lock_acquired_) {
                    esp_pm_lock_release(ri_pm_lock_);
                    ri_pm_lock_acquired_ = false;
                    gpio_intr_enable(ri_pin_);
                    ESP_LOGD(TAG, "Data available, RI PM lock released");
                }
            }
        }
    }
}

static bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit) && s.length() < 10;
}

bool AtUart::ParseResponse() {
    // 处理数据发送提示符 '>'
    if (wait_for_response_ && rx_buffer_[0] == '>') {
        rx_buffer_.erase(0, 1);
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
        return true;
    }
    
    // 处理 YM310 HTTP 数据发送提示符 'DOWNLOAD'
    if (wait_for_response_ && rx_buffer_.size() >= 10 && 
        rx_buffer_.substr(0, 8) == "DOWNLOAD") {
        // 跳过 DOWNLOAD 和可能的换行符
        size_t skip = 8;
        while (skip < rx_buffer_.size() && 
               (rx_buffer_[skip] == '\r' || rx_buffer_[skip] == '\n')) {
            skip++;
        }
        rx_buffer_.erase(0, skip);
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
        return true;
    }

    auto end_pos = rx_buffer_.find("\r\n");
    if (end_pos == std::string::npos) {
        // FIXME: for +MHTTPURC: "ind", missing newline
        if (rx_buffer_.size() >= 16 && memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"ind\"", 16) == 0) {
            // Find the end of this line and add \r\n if missing
            auto next_plus = rx_buffer_.find("+", 1);
            if (next_plus != std::string::npos) {
                // Insert \r\n before the next + command
                rx_buffer_.insert(next_plus, "\r\n");
            } else {
                // Append \r\n at the end
                rx_buffer_.append("\r\n");
            }
            end_pos = rx_buffer_.find("\r\n");
        } else {
            return false;
        }
    }

    // Ignore empty lines
    if (end_pos == 0) {
        rx_buffer_.erase(0, 2);
        return true;
    }

    if (debug_) {
        ESP_LOGI(TAG, "<< %.64s (%u bytes) [%02x%02x%02x]", rx_buffer_.substr(0, end_pos).c_str(), end_pos,
            rx_buffer_[0], rx_buffer_[1], rx_buffer_[2]);
    }
    // print last 64 bytes before end_pos if available
    // if (end_pos > 64) {
    //     ESP_LOGI(TAG, "<< LAST: %.64s", rx_buffer_.c_str() + end_pos - 64);
    // }

    // Parse "+CME ERROR: 123,456,789"
    if (rx_buffer_[0] == '+') {
        std::string command, values;
        
        // 特殊处理 YM310 的 +RECEIVE,<n>,<length>: 格式
        // 格式: +RECEIVE,<n>,<length>:\r\n<data>\r\n
        if (rx_buffer_.size() >= 9 && rx_buffer_.substr(0, 9) == "+RECEIVE,") {
            // 查找冒号位置
            auto colon_pos = rx_buffer_.find(':');
            if (colon_pos != std::string::npos && colon_pos < end_pos) {
                // 解析 +RECEIVE,<n>,<length>
                std::string receive_params = rx_buffer_.substr(9, colon_pos - 9);
                std::vector<AtArgumentValue> arguments;
                
                // 解析参数 (n,length)
                std::istringstream iss(receive_params);
                std::string item;
                int param_idx = 0;
                int data_length = 0;
                while (std::getline(iss, item, ',')) {
                    AtArgumentValue arg;
                    arg.type = AtArgumentValue::Type::Int;
                    arg.int_value = std::stoi(item);
                    arg.string_value = item;
                    arguments.push_back(arg);
                    if (param_idx == 1) {
                        data_length = arg.int_value;
                    }
                    param_idx++;
                }
                
                // 删除 +RECEIVE,<n>,<length>:\r\n 部分
                rx_buffer_.erase(0, end_pos + 2);
                
                // 等待数据到达（数据在下一行）
                if (data_length > 0) {
                    // 检查是否有足够的数据
                    if (rx_buffer_.size() >= (size_t)data_length) {
                        // 读取数据
                        std::string data = rx_buffer_.substr(0, data_length);
                        rx_buffer_.erase(0, data_length);
                        
                        // 跳过可能的 \r\n
                        while (rx_buffer_.size() >= 1 && 
                               (rx_buffer_[0] == '\r' || rx_buffer_[0] == '\n')) {
                            rx_buffer_.erase(0, 1);
                        }
                        
                        // 添加数据到参数
                        AtArgumentValue data_arg;
                        data_arg.type = AtArgumentValue::Type::String;
                        data_arg.string_value = data;
                        arguments.push_back(data_arg);
                        
                        ESP_LOGD(TAG, "RECEIVE: conn=%d, len=%d", 
                                 arguments[0].int_value, data_length);
                    } else {
                        // 数据不完整，需要等待更多数据
                        // 把 +RECEIVE 行放回去
                        std::string receive_line = "+RECEIVE," + receive_params + ":\r\n";
                        rx_buffer_.insert(0, receive_line);
                        return false;
                    }
                }
                
                HandleUrc("RECEIVE", arguments);
                return true;
            }
        }
        
        // 特殊处理 YM310 MQTT 订阅消息: +MSUB:"topic",<len> byte,<data>
        // 格式: +MSUB:"server-device",156 byte,{"type":"hello",...}
        if (rx_buffer_.size() >= 6 && rx_buffer_.substr(0, 6) == "+MSUB:") {
            // 找到 " byte," 标记
            auto byte_marker = rx_buffer_.find(" byte,");
            if (byte_marker != std::string::npos) {
                // 解析长度：找到 " byte," 前面的数字
                size_t len_start = rx_buffer_.rfind(',', byte_marker);
                if (len_start != std::string::npos) {
                    std::string len_str = rx_buffer_.substr(len_start + 1, byte_marker - len_start - 1);
                    int data_length = std::stoi(len_str);
                    
                    // 数据起始位置
                    size_t data_start = byte_marker + 6;  // " byte," 长度为 6
                    
                    // 检查数据是否完整
                    if (rx_buffer_.size() >= data_start + data_length) {
                        // 解析 topic
                        size_t topic_start = rx_buffer_.find('"') + 1;
                        size_t topic_end = rx_buffer_.find('"', topic_start);
                        std::string topic = rx_buffer_.substr(topic_start, topic_end - topic_start);
                        
                        // 提取数据
                        std::string data = rx_buffer_.substr(data_start, data_length);
                        
                        // 构建参数
                        std::vector<AtArgumentValue> arguments;
                        AtArgumentValue topic_arg;
                        topic_arg.type = AtArgumentValue::Type::String;
                        topic_arg.string_value = topic;
                        arguments.push_back(topic_arg);
                        
                        AtArgumentValue len_arg;
                        len_arg.type = AtArgumentValue::Type::Int;
                        len_arg.int_value = data_length;
                        len_arg.string_value = len_str;
                        arguments.push_back(len_arg);
                        
                        AtArgumentValue data_arg;
                        data_arg.type = AtArgumentValue::Type::String;
                        data_arg.string_value = data;
                        arguments.push_back(data_arg);
                        
                        // 删除已处理的数据
                        size_t total_len = data_start + data_length;
                        while (rx_buffer_.size() > total_len && 
                               (rx_buffer_[total_len] == '\r' || rx_buffer_[total_len] == '\n')) {
                            total_len++;
                        }
                        rx_buffer_.erase(0, total_len);
                        
                        ESP_LOGD(TAG, "MSUB: topic=%s, len=%d", topic.c_str(), data_length);
                        HandleUrc("MSUB", arguments);
                        return true;
                    }
                }
            }
            // 数据不完整，等待更多数据
            return false;
        }
        
        auto pos = rx_buffer_.find(": ");
        if (pos == std::string::npos || pos > end_pos) {
            command = rx_buffer_.substr(1, end_pos - 1);
        } else {
            command = rx_buffer_.substr(1, pos - 1);
            values = rx_buffer_.substr(pos + 2, end_pos - pos - 2);
        }
        rx_buffer_.erase(0, end_pos + 2);

        // Parse "string", int, int, ... into AtArgumentValue
        std::vector<AtArgumentValue> arguments;
        std::istringstream iss(values);
        std::string item;
        while (std::getline(iss, item, ',')) {
            AtArgumentValue argument;
            if (item.front() == '"') {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = item.substr(1, item.size() - 2);
            } else if (item.find(".") != std::string::npos) {
                argument.type = AtArgumentValue::Type::Double;
                argument.double_value = std::stod(item);
            } else if (is_number(item)) {
                argument.type = AtArgumentValue::Type::Int;
                argument.int_value = std::stoi(item);
                argument.string_value = std::move(item);
            } else {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = std::move(item);
            }
            arguments.push_back(argument);
        }

        HandleUrc(command, arguments);
        return true;
    } else if (rx_buffer_.size() >= 4 && rx_buffer_[0] == 'O' && rx_buffer_[1] == 'K' && rx_buffer_[2] == '\r' && rx_buffer_[3] == '\n') {
        rx_buffer_.erase(0, 4);
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
        return true;
    } else if (rx_buffer_.size() >= 7 && rx_buffer_[0] == 'E' && rx_buffer_[1] == 'R' && rx_buffer_[2] == 'R' && rx_buffer_[3] == 'O' && rx_buffer_[4] == 'R' && rx_buffer_[5] == '\r' && rx_buffer_[6] == '\n') {
        rx_buffer_.erase(0, 7);
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
        return true;
    } else if (rx_buffer_[0] == 0xE0) { // 4G wake up MCU, just ignore
        rx_buffer_.erase(0, end_pos + 2);
        return true;
    } else {
        // 检查是否是 YM310 多连接模式的 URC 格式: "n,COMMAND" 或 "COMMAND"
        // 例如: "7,CONNECT OK", "CONNECTOK", "7,CLOSED", "CONNACKOK"
        std::string line = rx_buffer_.substr(0, end_pos);
        std::string urc_command;
        std::vector<AtArgumentValue> urc_args;
        
        // 尝试解析多连接格式 "n,COMMAND" 
        if (line.size() >= 3 && std::isdigit(line[0]) && line[1] == ',') {
            // 提取连接 ID
            AtArgumentValue conn_id_arg;
            conn_id_arg.type = AtArgumentValue::Type::Int;
            conn_id_arg.int_value = line[0] - '0';
            urc_args.push_back(conn_id_arg);
            
            // 命令部分（包含连接 ID 前缀，方便匹配）
            urc_command = line;
        } else {
            urc_command = line;
        }
        
        // 检查是否是已知的 URC 格式
        bool is_urc = false;
        if (urc_command.find("CONNECT OK") != std::string::npos ||
            urc_command.find("CONNECTOK") != std::string::npos ||
            urc_command.find("CONNECT FAIL") != std::string::npos ||
            urc_command.find("CONNECTFAIL") != std::string::npos ||
            urc_command.find("CONNACK OK") != std::string::npos ||
            urc_command.find("CONNACKOK") != std::string::npos ||
            urc_command.find("CLOSED") != std::string::npos ||
            urc_command.find("SEND OK") != std::string::npos ||
            urc_command.find("SENDOK") != std::string::npos ||
            urc_command.find("ALREADY CONNECT") != std::string::npos ||
            urc_command.find("ALREADYCONNECT") != std::string::npos ||
            urc_command.find("SUBACK") != std::string::npos ||
            urc_command.find("PUBACK") != std::string::npos ||
            urc_command.find("PUBCOMP") != std::string::npos ||
            urc_command.find("DATA ACCEPT") != std::string::npos ||
            urc_command.find("DATAACCEPT") != std::string::npos) {
            is_urc = true;
        }
        
        if (is_urc) {
            rx_buffer_.erase(0, end_pos + 2);
            
            // DATA ACCEPT 在快发模式下代替 OK，需要触发命令完成事件
            if (urc_command.find("DATA ACCEPT") != std::string::npos ||
                urc_command.find("DATAACCEPT") != std::string::npos) {
                xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            }
            
            HandleUrc(urc_command, urc_args);
            return true;
        }
        
        std::lock_guard<std::mutex> lock(mutex_);
        response_ = rx_buffer_.substr(0, end_pos);
        rx_buffer_.erase(0, end_pos + 2);
        return true;
    }
    return false;
}

void AtUart::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "CME ERROR") {
        cme_error_code_ = arguments[0].int_value;
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& callback : urc_callbacks_) {
        callback(command, arguments);
    }
}

bool AtUart::DetectBaudRate(int timeout_ms) {
    int baud_rates[] = {115200, 921600, 460800, 230400, 57600, 38400, 19200, 9600};
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    while (true) {
        ESP_LOGI(TAG, "Detecting baud rate...");
        for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++) {
            int rate = baud_rates[i];
            uart_set_baudrate(uart_num_, rate);
            if (SendCommand("AT", 20)) {
                ESP_LOGI(TAG, "Detected baud rate: %d", rate);
                baud_rate_ = rate;
                return true;
            }
        }
        
        // Check timeout before delay if specified
        if (timeout_ms != -1) {
            TickType_t elapsed = xTaskGetTickCount() - start_time;
            if (elapsed >= timeout_ticks) {
                ESP_LOGE(TAG, "Baud rate detection timeout");
                return false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool AtUart::SetBaudRate(int new_baud_rate, int timeout_ms) {
    if (!DetectBaudRate(timeout_ms)) {
        ESP_LOGE(TAG, "Failed to detect baud rate");
        return false;
    }
    if (new_baud_rate == baud_rate_) {
        return true;
    }
    // Set new baud rate
    if (!SendCommand(std::string("AT+IPR=") + std::to_string(new_baud_rate))) {
        ESP_LOGI(TAG, "Failed to set baud rate to %d", new_baud_rate);
        return false;
    }
    uart_set_baudrate(uart_num_, new_baud_rate);
    baud_rate_ = new_baud_rate;
    ESP_LOGI(TAG, "Set baud rate to %d", new_baud_rate);
    return true;
}

bool AtUart::SendData(const char* data, size_t length) {
    if (!initialized_) {
        ESP_LOGE(TAG, "UART未初始化");
        return false;
    }
    
    int ret = uart_write_bytes(uart_num_, data, length);
    if (ret < 0) {
        ESP_LOGE(TAG, "uart_write_bytes failed: %d", ret);
        return false;
    }
    return true;
}

bool AtUart::SendCommandWithData(const std::string& command, size_t timeout_ms, bool add_crlf, const char* data, size_t data_length) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (debug_) {
        ESP_LOGI(TAG, ">> %.64s (%u bytes)", command.data(), command.length());
    }

    xEventGroupClearBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR);
    wait_for_response_ = true;
    cme_error_code_ = 0;
    {
        std::lock_guard<std::mutex> response_lock(mutex_);
        response_.clear();
    }

    if (add_crlf) {
        if (!SendData((command + "\r\n").data(), command.length() + 2)) {
            return false;
        }
    } else {
        if (!SendData(command.data(), command.length())) {
            return false;
        }
    }
    if (timeout_ms > 0) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    } else {
        wait_for_response_ = false;
    }

    if (data && data_length > 0) {
        wait_for_response_ = true;
        if (!SendData(data, data_length)) {
            return false;
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    }
    return true;
}

bool AtUart::SendCommand(const std::string& command, size_t timeout_ms, bool add_crlf) {
    return SendCommandWithData(command, timeout_ms, add_crlf, nullptr, 0);
}

std::string AtUart::GetResponse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_;
}

std::list<UrcCallback>::iterator AtUart::RegisterUrcCallback(UrcCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    return urc_callbacks_.insert(urc_callbacks_.end(), callback);
}

void AtUart::UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator) {
    std::lock_guard<std::mutex> lock(mutex_);
    urc_callbacks_.erase(iterator);
}

void AtUart::SetDtrPin(bool high) {
    if (dtr_pin_ != GPIO_NUM_NC) {
        if (debug_) {
            ESP_LOGI(TAG, "Set DTR pin %d to %d", dtr_pin_, high ? 1 : 0);
        }
        gpio_set_level(dtr_pin_, high ? 1 : 0);
        dtr_pin_state_ = high;  // 记录DTR pin的状态
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

void AtUart::EncodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length * 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i++) {
        dest.push_back(hex_chars[(data[i] & 0xF0) >> 4]);
        dest.push_back(hex_chars[data[i] & 0x0F]);
    }
}

void AtUart::DecodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length / 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i += 2) {
        char byte = (CharToHex(data[i]) << 4) | CharToHex(data[i + 1]);
        dest.push_back(byte);
    }
}

std::string AtUart::EncodeHex(const std::string& data) {
    std::string encoded;
    EncodeHexAppend(encoded, data.c_str(), data.size());
    return encoded;
}

std::string AtUart::DecodeHex(const std::string& data) {
    std::string decoded;
    DecodeHexAppend(decoded, data.c_str(), data.size());
    return decoded;
}

void AtUart::SetDebug(bool enable) {
    debug_ = enable;
}

// RI pin ISR handler (runs in IRAM)
void IRAM_ATTR AtUart::RiPinIsrHandler(void* arg) {
    AtUart* at_uart = static_cast<AtUart*>(arg);
    // Disable interrupt
    gpio_intr_disable(at_uart->ri_pin_);
    // Notify the task to handle the interrupt
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(at_uart->event_group_handle_, AT_EVENT_RI_PIN_INT, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
