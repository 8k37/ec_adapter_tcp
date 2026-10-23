#include "ec_adapter_tcp.h"

#include <arpa/inet.h>
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

#include "esphome/core/hal.h"
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

void EcAdapterTcp::close_socket_() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
  this->connected_ = false;
  this->tx_buf_.clear();
  this->tx_sent_ = 0;
  this->rx_buf_.clear();
  this->rx_expected_ = 0;
}

void EcAdapterTcp::fail_cycle_(const char *reason) {
  ESP_LOGW(TAG, "Modbus cycle aborted: %s", reason);
  this->queue_.clear();
  this->close_socket_();
  this->state_ = State::IDLE;
}

void EcAdapterTcp::start_connect_() {
  this->close_socket_();

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);

  if (inet_pton(AF_INET, this->host_.c_str(), &addr.sin_addr) != 1) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned) this->port_);

    struct addrinfo *res = nullptr;
    if (getaddrinfo(this->host_.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
      ESP_LOGW(TAG, "Cannot resolve '%s'", this->host_.c_str());
      return;
    }
    memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);
  }

  int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGW(TAG, "socket() failed: %d", errno);
    return;
  }

  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  int nodelay = 1;
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  int ret = ::connect(sock, (struct sockaddr *) &addr, sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS) {
    ESP_LOGW(TAG, "connect() failed: %d", errno);
    ::close(sock);
    return;
  }

  this->sock_ = sock;
  if (ret == 0) {
    this->connected_ = true;
    this->state_ = State::IDLE;
    ESP_LOGI(TAG, "Connected to %s:%u", this->host_.c_str(), (unsigned) this->port_);
  } else {
    this->state_ = State::CONNECTING;
    this->deadline_ = millis() + this->timeout_ms_;
  }
}

void EcAdapterTcp::poll_connect_() {
  if (this->sock_ < 0) {
    this->state_ = State::IDLE;
    return;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(this->sock_, &wfds);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  int sel = ::select(this->sock_ + 1, nullptr, &wfds, nullptr, &tv);
  if (sel == 0) {
    if ((int32_t) (millis() - this->deadline_) >= 0) {
      ESP_LOGW(TAG, "connect timeout to %s:%u", this->host_.c_str(), (unsigned) this->port_);
      this->close_socket_();
      this->state_ = State::IDLE;
    }
    return;
  }
  if (sel < 0) {
    if (errno == EINTR)
      return;
    ESP_LOGW(TAG, "select() failed: %d", errno);
    this->close_socket_();
    this->state_ = State::IDLE;
    return;
  }

  int so_error = 0;
  socklen_t len = sizeof(so_error);
  if (getsockopt(this->sock_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
    ESP_LOGW(TAG, "connect failed: %d", so_error);
    this->close_socket_();
    this->state_ = State::IDLE;
    return;
  }

  this->connected_ = true;
  this->state_ = State::IDLE;
  ESP_LOGI(TAG, "Connected to %s:%u", this->host_.c_str(), (unsigned) this->port_);
}

void EcAdapterTcp::enqueue_reads_() {
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
    Request req;
    req.address = range.first;
    req.count = range.second;
    req.pdu = {0x03, (uint8_t) (range.first >> 8), (uint8_t) (range.first & 0xFF),
               (uint8_t) (range.second >> 8), (uint8_t) (range.second & 0xFF)};
    this->queue_.push_back(req);
  }
}

void EcAdapterTcp::update() {
  if (this->addresses_.empty())
    return;
  if (this->state_ != State::IDLE || !this->queue_.empty())
    return;  // the previous cycle is still running
  this->enqueue_reads_();
}

void EcAdapterTcp::loop() {
  PollingComponent::loop();

  switch (this->state_) {
    case State::IDLE:
      if (!this->queue_.empty())
        this->start_next_request_();
      break;
    case State::CONNECTING:
      this->poll_connect_();
      break;
    case State::SENDING:
      this->send_step_();
      break;
    case State::RECEIVING:
      this->recv_step_();
      break;
  }
}

void EcAdapterTcp::start_next_request_() {
  if (this->queue_.empty()) {
    this->state_ = State::IDLE;
    return;
  }

  if (this->sock_ < 0) {
    if (this->last_connect_attempt_ != 0 && millis() - this->last_connect_attempt_ < 1000) {
      this->state_ = State::IDLE;
      return;
    }
    this->last_connect_attempt_ = millis();
    this->start_connect_();
    if (this->sock_ < 0) {
      this->fail_cycle_("cannot connect");
      return;
    }
    if (this->state_ != State::IDLE)
      return;  // connection in progress, resume from loop()
  }

  const Request &req = this->queue_.front();
  this->current_transaction_ = ++this->transaction_id_;
  uint16_t len = (uint16_t) (req.pdu.size() + 1);

  this->tx_buf_.clear();
  this->tx_buf_.push_back((uint8_t) (this->current_transaction_ >> 8));
  this->tx_buf_.push_back((uint8_t) (this->current_transaction_ & 0xFF));
  this->tx_buf_.push_back(0x00);
  this->tx_buf_.push_back(0x00);
  this->tx_buf_.push_back((uint8_t) (len >> 8));
  this->tx_buf_.push_back((uint8_t) (len & 0xFF));
  this->tx_buf_.push_back(this->unit_id_);
  this->tx_buf_.insert(this->tx_buf_.end(), req.pdu.begin(), req.pdu.end());

  this->tx_sent_ = 0;
  this->rx_buf_.clear();
  this->rx_expected_ = 0;
  this->deadline_ = millis() + this->timeout_ms_;
  this->state_ = State::SENDING;
}

void EcAdapterTcp::send_step_() {
  if (this->sock_ < 0) {
    this->fail_cycle_("socket closed");
    return;
  }

  while (this->tx_sent_ < this->tx_buf_.size()) {
    ssize_t n = ::send(this->sock_, this->tx_buf_.data() + this->tx_sent_,
                       this->tx_buf_.size() - this->tx_sent_, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;  // socket buffer full, continue in the next loop iteration
      this->fail_cycle_("send failed");
      return;
    }
    if (n == 0) {
      this->fail_cycle_("send returned 0");
      return;
    }
    this->tx_sent_ += (size_t) n;
  }

  this->deadline_ = millis() + this->timeout_ms_;
  this->state_ = State::RECEIVING;
}

void EcAdapterTcp::recv_step_() {
  if (this->sock_ < 0) {
    this->fail_cycle_("socket closed");
    return;
  }

  uint8_t buf[64];
  ssize_t n = ::recv(this->sock_, buf, sizeof(buf), 0);
  if (n < 0) {
    if (errno == EINTR)
      return;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if ((int32_t) (millis() - this->deadline_) >= 0) {
        ESP_LOGW(TAG, "No response from %s:%u within %u ms (%u bytes received)", this->host_.c_str(),
                 (unsigned) this->port_, (unsigned) this->timeout_ms_, (unsigned) this->rx_buf_.size());
        this->fail_cycle_("timeout");
      }
      return;
    }
    this->fail_cycle_("recv failed");
    return;
  }
  if (n == 0) {
    this->fail_cycle_("connection closed by peer");
    return;
  }

  this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);

  if (this->rx_expected_ == 0 && this->rx_buf_.size() >= 7) {
    uint16_t rlen = (uint16_t) ((this->rx_buf_[4] << 8) | this->rx_buf_[5]);
    if (rlen < 2) {
      this->fail_cycle_("malformed MBAP length");
      return;
    }
    this->rx_expected_ = (size_t) 6 + rlen;
  }

  if (this->rx_expected_ != 0 && this->rx_buf_.size() >= this->rx_expected_)
    this->finish_request_();
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

void EcAdapterTcp::finish_request_() {
  uint16_t r_transaction = (uint16_t) ((this->rx_buf_[0] << 8) | this->rx_buf_[1]);
  uint16_t r_protocol = (uint16_t) ((this->rx_buf_[2] << 8) | this->rx_buf_[3]);
  uint8_t r_unit = this->rx_buf_[6];

  if (r_protocol != 0 || r_transaction != this->current_transaction_ || r_unit != this->unit_id_) {
    ESP_LOGW(TAG, "Unexpected MBAP header (protocol %u, transaction %u/%u, unit %u/%u)", r_protocol,
             (unsigned) r_transaction, (unsigned) this->current_transaction_, (unsigned) r_unit,
             (unsigned) this->unit_id_);
    this->fail_cycle_("MBAP mismatch");
    return;
  }

  std::vector<uint8_t> pdu(this->rx_buf_.begin() + 7, this->rx_buf_.begin() + this->rx_expected_);

  Request req = this->queue_.front();
  bool ok = false;
  uint8_t exception = 0;

  if (pdu.empty()) {
    this->fail_cycle_("empty response");
    return;
  }

  if (pdu[0] & 0x80) {
    exception = pdu.size() > 1 ? pdu[1] : 0xFF;
    this->last_exception_ = exception;
    ESP_LOGV(TAG, "Modbus exception 0x%02X for function 0x%02X", exception, pdu[0] & 0x7F);
  } else if (pdu[0] == 0x03 && req.count > 0) {
    if (pdu.size() >= 2 && pdu[1] == req.count * 2 && pdu.size() >= (size_t) (2 + req.count * 2)) {
      for (uint16_t i = 0; i < req.count; i++)
        this->registers_[(uint16_t) (req.address + i)] =
            (uint16_t) ((pdu[2 + i * 2] << 8) | pdu[3 + i * 2]);
      this->warned_ranges_.erase(req.address);
      ok = true;
    } else {
      ESP_LOGW(TAG, "Malformed read response for 0x%04X (%u regs)", req.address, (unsigned) req.count);
    }
  } else if (pdu[0] == 0x06 && req.is_write) {
    this->registers_[req.address] = req.value;
    ok = true;
  } else {
    ESP_LOGW(TAG, "Unexpected function 0x%02X in response to 0x%04X", pdu[0], req.address);
  }

  this->tx_buf_.clear();
  this->tx_sent_ = 0;
  this->rx_buf_.clear();
  this->rx_expected_ = 0;
  this->state_ = State::IDLE;

  if (!ok && exception != 0) {
    if (!req.is_write && req.count > 1) {
      // The adapter rejected the whole range. Split it into single-register
      // reads so one unsupported register does not hide the valid ones.
      this->queue_.erase(this->queue_.begin());
      std::vector<Request> singles;
      singles.reserve(req.count);
      for (uint16_t i = 0; i < req.count; i++) {
        Request single;
        single.address = (uint16_t) (req.address + i);
        single.count = 1;
        single.pdu = {0x03, (uint8_t) (single.address >> 8), (uint8_t) (single.address & 0xFF), 0x00,
                      0x01};
        singles.push_back(single);
      }
      this->queue_.insert(this->queue_.begin(), singles.begin(), singles.end());
      return;
    }
    this->log_missing_register_(req.address);
  } else if (!ok && req.is_write) {
    ESP_LOGW(TAG, "Write 0x%04X = %u failed", req.address, (unsigned) req.value);
  }

  this->queue_.erase(this->queue_.begin());

  if (this->queue_.empty())
    for (auto *listener : this->listeners_)
      listener->on_registers_updated();
}

bool EcAdapterTcp::write_register(uint16_t address, uint16_t value) {
  Request req;
  req.is_write = true;
  req.address = address;
  req.value = value;
  req.pdu = {0x06, (uint8_t) (address >> 8), (uint8_t) (address & 0xFF), (uint8_t) (value >> 8),
             (uint8_t) (value & 0xFF)};
  this->queue_.push_back(req);
  return true;
}

}  // namespace ec_adapter_tcp
}  // namespace esphome