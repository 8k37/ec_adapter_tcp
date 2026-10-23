#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace ec_adapter_tcp {

class EcAdapterTcp;

class EcAdapterTcpListener {
 public:
  virtual void on_registers_updated() = 0;
};

class EcAdapterTcp : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_unit_id(uint8_t unit_id) { this->unit_id_ = unit_id; }
  void set_response_timeout(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }

  void add_listener(EcAdapterTcpListener *listener) { this->listeners_.push_back(listener); }
  void add_read_register(uint16_t address, uint8_t count);

  bool read_register(uint16_t address, uint16_t *value) const;
  bool write_register(uint16_t address, uint16_t value);
  bool is_connected() const { return this->connected_; }

 protected:
  enum class State : uint8_t { IDLE, CONNECTING, SENDING, RECEIVING };

  struct Request {
    std::vector<uint8_t> pdu;
    uint16_t address{0};
    uint16_t count{0};
    uint16_t value{0};
    bool is_write{false};
  };

  void start_connect_();
  void poll_connect_();
  void start_next_request_();
  void send_step_();
  void recv_step_();
  void finish_request_();
  void fail_cycle_(const char *reason);
  void enqueue_reads_();
  void log_missing_register_(uint16_t address);
  void close_socket_();

  std::string host_;
  uint16_t port_{502};
  uint8_t unit_id_{1};
  uint32_t timeout_ms_{2000};
  int sock_{-1};
  bool connected_{false};
  uint16_t transaction_id_{0};
  uint16_t current_transaction_{0};

  State state_{State::IDLE};
  uint32_t deadline_{0};
  uint32_t last_connect_attempt_{0};

  std::vector<uint8_t> tx_buf_;
  size_t tx_sent_{0};
  std::vector<uint8_t> rx_buf_;
  size_t rx_expected_{0};

  std::vector<Request> queue_;

  std::vector<EcAdapterTcpListener *> listeners_;
  std::set<uint16_t> addresses_;
  std::map<uint16_t, uint16_t> registers_;
  uint8_t last_exception_{0};
  std::set<uint16_t> warned_ranges_;
};

}  // namespace ec_adapter_tcp
}  // namespace esphome