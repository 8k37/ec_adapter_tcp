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
  bool poll_range_(uint16_t start, uint16_t count);
  void log_missing_register_(uint16_t address);
  bool host_is_self_() const;
  bool ensure_connected_();
  void close_connection_();
  bool transact_(const uint8_t *pdu, size_t pdu_len, std::vector<uint8_t> &response);
  bool transact_once_(const uint8_t *pdu, size_t pdu_len, std::vector<uint8_t> &response);
  bool read_holding_(uint16_t start, uint16_t count, std::vector<uint16_t> &out);
  bool send_all_(const uint8_t *data, size_t len);
  bool recv_exact_(uint8_t *data, size_t len);

  std::string host_;
  uint16_t port_{502};
  uint8_t unit_id_{1};
  uint32_t timeout_ms_{2000};
  int sock_{-1};
  bool connected_{false};
  uint16_t transaction_id_{0};
  std::vector<EcAdapterTcpListener *> listeners_;
  std::set<uint16_t> addresses_;
  std::map<uint16_t, uint16_t> registers_;
  uint8_t last_exception_{0};
  bool last_peer_closed_{false};
  std::set<uint16_t> warned_ranges_;
};

}  // namespace ec_adapter_tcp
}  // namespace esphome