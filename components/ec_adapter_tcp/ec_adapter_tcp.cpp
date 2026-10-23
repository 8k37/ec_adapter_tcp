#include "ec_adapter_tcp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esphome/core/log.h"

namespace esphome {
namespace ec_adapter_tcp {

static const char *const TAG = "ec_adapter_tcp";

void EcAdapterTcp::setup() { this->transaction_id_ = 0; }

void EcAdapterTcp::dump_config() {
  ESP_LOGCONFIG(TAG, "ectoControl adapter (Modbus TCP):");
  ESP_LOGCONFIG(TAG, "  Host: %s", this->host_.c_str());
  ESP_LOGCONFIG(TAG, "  Port: %u", (unsigned) this->port_);
  ESP_LOGCONFIG(TAG, "  Unit ID: %u", (unsigned) this->unit_id_);
  ESP_LOGCONFIG(TAG, "  Response timeout: %u ms", (unsigned) this->timeout_ms_);

  std::string addrs;
  for (uint16_t address : this->addresses_) {
    char item[8];
    snprintf(item, sizeof(item), "0x%04X", address);
    if (!addrs.empty())
      addrs += ", ";
    addrs += item;
  }
  ESP_LOGCONFIG(TAG, "  Polled registers (%u): %s", (unsigned) this->addresses_.size(), addrs.c_str());
}

void EcAdapterTcp::add_read_register(uint16_t address, uint8_t count) {
  for (uint8_t i = 0; i < count; i++)
    this->addresses_.insert((uint16_t) (address + i));
}

bool EcAdapterTcp::read_register(uint16_t address, uint16_t *value) const {
  auto it = this->registers_.find(address);
  if (it == this->registers_.end())
    return false;
  *value = it->second;
  return true;
}

void EcAdapterTcp::close_connection_() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
  this->connected_ = false;
}

bool EcAdapterTcp::ensure_connected_() {
  if (this->sock_ >= 0)
    return true;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned) this->port_);

  struct addrinfo *res = nullptr;
  int gai = getaddrinfo(this->host_.c_str(), port_str, &hints, &res);
  if (gai != 0 || res == nullptr) {
    ESP_LOGW(TAG, "Cannot resolve '%s'", this->host_.c_str());
    return false;
  }

  int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    freeaddrinfo(res);
    ESP_LOGW(TAG, "socket() failed: %d", errno);
    return false;
  }

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  int ret = ::connect(sock, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (ret < 0 && errno != EINPROGRESS) {
    ::close(sock);
    ESP_LOGW(TAG, "connect() failed: %d", errno);
    return false;
  }

  if (ret < 0) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv;
    tv.tv_sec = this->timeout_ms_ / 1000;
    tv.tv_usec = (this->timeout_ms_ % 1000) * 1000;
    int sel = ::select(sock + 1, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
      ::close(sock);
      ESP_LOGW(TAG, "connect timeout to %s:%u", this->host_.c_str(), (unsigned) this->port_);
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
      ::close(sock);
      ESP_LOGW(TAG, "connect failed: %d", so_error);
      return false;
    }
  }

  fcntl(sock, F_SETFL, flags);

  struct timeval tv;
  tv.tv_sec = this->timeout_ms_ / 1000;
  tv.tv_usec = (this->timeout_ms_ % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  this->sock_ = sock;
  this->connected_ = true;
  ESP_LOGI(TAG, "Connected to %s:%u", this->host_.c_str(), (unsigned) this->port_);
  return true;
}

bool EcAdapterTcp::send_all_(const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(this->sock_, data + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    sent += (size_t) n;
  }
  return true;
}

bool EcAdapterTcp::recv_exact_(uint8_t *data, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = ::recv(this->sock_, data + got, len - got, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    got += (size_t) n;
  }
  return true;
}

bool EcAdapterTcp::transact_(const uint8_t *pdu, size_t pdu_len, std::vector<uint8_t> &response) {
  // The adapter silently drops idle TCP connections. The stale socket still
  // looks fine (sock_ >= 0) until the first recv() returns 0, which used to
  // surface as a bogus "Read of 0x.... failed". Retry once on a fresh socket.
  for (int attempt = 0; attempt < 2; attempt++) {
    this->last_exception_ = 0;
    if (!this->ensure_connected_())
      return false;
    if (this->transact_once_(pdu, pdu_len, response))
      return true;
    if (this->last_exception_ != 0)
      return false;  // the adapter answered properly, retrying will not help
    this->close_connection_();
    if (attempt == 0)
      ESP_LOGD(TAG, "Connection lost, reconnecting");
  }
  return false;
}

bool EcAdapterTcp::transact_once_(const uint8_t *pdu, size_t pdu_len, std::vector<uint8_t> &response) {
  this->transaction_id_++;

  uint8_t header[7];
  header[0] = (uint8_t) (this->transaction_id_ >> 8);
  header[1] = (uint8_t) (this->transaction_id_ & 0xFF);
  header[2] = 0x00;
  header[3] = 0x00;
  uint16_t len = (uint16_t) (pdu_len + 1);
  header[4] = (uint8_t) (len >> 8);
  header[5] = (uint8_t) (len & 0xFF);
  header[6] = this->unit_id_;

  if (!this->send_all_(header, sizeof(header)) || !this->send_all_(pdu, pdu_len))
    return false;

  uint8_t rheader[7];
  if (!this->recv_exact_(rheader, sizeof(rheader)))
    return false;

  uint16_t r_transaction = (uint16_t) ((rheader[0] << 8) | rheader[1]);
  uint16_t r_protocol = (uint16_t) ((rheader[2] << 8) | rheader[3]);
  uint16_t rlen = (uint16_t) ((rheader[4] << 8) | rheader[5]);
  if (r_protocol != 0 || rlen < 2) {
    ESP_LOGW(TAG, "Malformed MBAP header (protocol %u, length %u)", r_protocol, rlen);
    return false;
  }
  if (r_transaction != this->transaction_id_) {
    ESP_LOGW(TAG, "Transaction id mismatch (sent %u, got %u)", (unsigned) this->transaction_id_,
             (unsigned) r_transaction);
    return false;
  }
  if (rheader[6] != this->unit_id_) {
    ESP_LOGW(TAG, "Unit id mismatch (sent %u, got %u)", (unsigned) this->unit_id_,
             (unsigned) rheader[6]);
    return false;
  }

  response.resize(rlen - 1);
  if (!this->recv_exact_(response.data(), response.size()))
    return false;

  if (response[0] & 0x80) {
    this->last_exception_ = response.size() > 1 ? response[1] : 0xFF;
    ESP_LOGD(TAG, "Modbus exception 0x%02X for function 0x%02X", this->last_exception_, pdu[0]);
    return false;
  }
  return true;
}

bool EcAdapterTcp::read_holding_(uint16_t start, uint16_t count, std::vector<uint16_t> &out) {
  uint8_t pdu[5];
  pdu[0] = 0x03;
  pdu[1] = (uint8_t) (start >> 8);
  pdu[2] = (uint8_t) (start & 0xFF);
  pdu[3] = (uint8_t) (count >> 8);
  pdu[4] = (uint8_t) (count & 0xFF);

  std::vector<uint8_t> resp;
  if (!this->transact_(pdu, sizeof(pdu), resp))
    return false;
  if (resp.size() < 2 || resp[1] != count * 2 || resp.size() < (size_t) (2 + count * 2)) {
    ESP_LOGW(TAG, "Malformed read response for 0x%04X (%u regs): byte count %u, payload %u", start,
             (unsigned) count, resp.size() > 1 ? (unsigned) resp[1] : 0u, (unsigned) resp.size());
    return false;
  }

  out.clear();
  out.reserve(count);
  for (uint16_t i = 0; i < count; i++)
    out.push_back((uint16_t) ((resp[2 + i * 2] << 8) | resp[3 + i * 2]));
  return true;
}

bool EcAdapterTcp::poll_range_(uint16_t start, uint16_t count) {
  std::vector<uint16_t> values;
  if (!this->read_holding_(start, count, values)) {
    if (this->last_exception_ == 0)
      this->warned_ranges_.erase(start);
    return false;
  }
  this->warned_ranges_.erase(start);
  for (size_t i = 0; i < values.size(); i++)
    this->registers_[(uint16_t) (start + i)] = values[i];
  return true;
}

void EcAdapterTcp::log_missing_register_(uint16_t address) {
  if (this->warned_ranges_.insert(address).second) {
    ESP_LOGW(TAG, "Register 0x%04X is not available (Modbus exception 0x%02X), skipping", address,
             this->last_exception_);
  } else {
    ESP_LOGV(TAG, "Register 0x%04X still unavailable (exception 0x%02X)", address,
             this->last_exception_);
  }
}

bool EcAdapterTcp::write_register(uint16_t address, uint16_t value) {
  uint8_t pdu[5];
  pdu[0] = 0x06;
  pdu[1] = (uint8_t) (address >> 8);
  pdu[2] = (uint8_t) (address & 0xFF);
  pdu[3] = (uint8_t) (value >> 8);
  pdu[4] = (uint8_t) (value & 0xFF);

  std::vector<uint8_t> resp;
  if (!this->transact_(pdu, sizeof(pdu), resp)) {
    this->close_connection_();
    ESP_LOGW(TAG, "Write 0x%04X = %u failed", address, (unsigned) value);
    return false;
  }
  this->registers_[address] = value;
  return true;
}

void EcAdapterTcp::update() {
  if (this->addresses_.empty())
    return;

  if (!this->ensure_connected_()) {
    this->close_connection_();
    return;
  }

  // Merge the requested addresses into contiguous ranges (max 120 registers per request).
  std::vector<std::pair<uint16_t, uint16_t>> ranges;
  uint16_t start = 0;
  uint16_t prev = 0;
  bool have = false;
  for (uint16_t address : this->addresses_) {
    if (!have) {
      start = address;
      prev = address;
      have = true;
      continue;
    }
    if (address == (uint16_t) (prev + 1) && (uint16_t) (address - start) < 120) {
      prev = address;
      continue;
    }
    ranges.emplace_back(start, (uint16_t) (prev - start + 1));
    start = address;
    prev = address;
  }
  if (have)
    ranges.emplace_back(start, (uint16_t) (prev - start + 1));

  for (auto &range : ranges) {
    if (this->poll_range_(range.first, range.second))
      continue;
    if (this->last_exception_ == 0) {
      // Transport level failure: the connection is in an unknown state.
      this->close_connection_();
      ESP_LOGW(TAG, "Read of 0x%04X (%u regs) failed", range.first, (unsigned) range.second);
      return;
    }
    // The adapter answered with a Modbus exception. Split the range so a single
    // unsupported register does not hide the valid ones around it.
    if (range.second == 1) {
      this->log_missing_register_(range.first);
      continue;
    }
    for (uint16_t i = 0; i < range.second; i++) {
      uint16_t addr = (uint16_t) (range.first + i);
      if (this->poll_range_(addr, 1))
        continue;
      if (this->last_exception_ == 0) {
        this->close_connection_();
        ESP_LOGW(TAG, "Read of 0x%04X failed", addr);
        return;
      }
      this->log_missing_register_(addr);
    }
  }

  for (auto *listener : this->listeners_)
    listener->on_registers_updated();
}

}  // namespace ec_adapter_tcp
}  // namespace esphome