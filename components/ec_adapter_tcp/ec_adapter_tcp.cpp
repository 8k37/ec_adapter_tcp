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

static void log_hex_(const char *prefix, const uint8_t *data, size_t len) {
  char buf[3 * 32 + 1];
  size_t shown = len > 32 ? 32 : len;
  size_t pos = 0;
  for (size_t i = 0; i < shown && pos + 4 < sizeof(buf); i++)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
  ESP_LOGV(TAG, "%s (%u bytes%s): %s", prefix, (unsigned) len,
           len > 32 ? ", truncated" : "", buf);
}

static std::vector<uint8_t> build_write_pdu(uint16_t address, uint16_t value, bool use_fc16) {
  if (!use_fc16) {
    return {0x06, (uint8_t) (address >> 8), (uint8_t) (address & 0xFF), (uint8_t) (value >> 8),
            (uint8_t) (value & 0xFF)};
  }
  // FC16 (Write Multiple Registers), quantity 1. Widely supported even on
  // servers that reject FC06.
  return {0x10, (uint8_t) (address >> 8), (uint8_t) (address & 0xFF), 0x00, 0x01, 0x02,
          (uint8_t) (value >> 8), (uint8_t) (value & 0xFF)};
}

static std::vector<uint8_t> build_read_pdu(uint16_t address, uint16_t count) {
  return {0x03, (uint8_t) (address >> 8), (uint8_t) (address & 0xFF), (uint8_t) (count >> 8),
          (uint8_t) (count & 0xFF)};
}

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

void EcAdapterTcp::fail_cycle_(const char *reason, bool peer_closed) {
  ESP_LOGW(TAG, "Modbus cycle aborted: %s%s", reason, peer_closed ? " (peer closed)" : "");
  this->close_socket_();
  this->state_ = State::IDLE;

  if (peer_closed) {
    // The server actively dropped the connection. Give it time to clean up
    // the old socket before reconnecting, otherwise the next connect is
    // accepted but immediately reset again (the modbus_bridge on the same
    // ESP needs to free the previous client slot first).
    this->last_connect_attempt_ = millis();
  }

  if (this->queue_.empty())
    return;

  Request &front = this->queue_.front();

  // Log the exact request that failed so wire-level problems are visible.
  if (front.is_write) {
    ESP_LOGW(TAG, "Failed write 0x%04X = %u (FC%02X) after %s", front.address,
             (unsigned) front.value, front.use_fc16 ? 0x10 : 0x06, reason);
  } else {
    ESP_LOGW(TAG, "Failed read 0x%04X (%u regs) after %s", front.address, (unsigned) front.count,
             reason);
  }
  if (!front.pdu.empty())
    log_hex_("Failed PDU", front.pdu.data(), front.pdu.size());

  if (front.is_write) {
    // Writes are user commands, never drop them on the first failure:
    //  - if we haven't tried FC16 yet, fall back to it;
    //  - otherwise log the failure and move on.
    if (!front.use_fc16) {
      front.use_fc16 = true;
      front.pdu = build_write_pdu(front.address, front.value, true);
      ESP_LOGD(TAG, "Write 0x%04X = %u retrying with FC16 after %s", front.address,
               (unsigned) front.value, reason);
      return;
    }
    ESP_LOGW(TAG, "Write 0x%04X = %u failed: %s", front.address, (unsigned) front.value, reason);
    this->queue_.erase(this->queue_.begin());
  } else {
    // Reads: retry once on a peer-closed connection, drop on a timeout (the
    // server is simply not answering, retrying would just waste another full
    // timeout window).
    if (peer_closed && front.retries > 0) {
      front.retries--;
      ESP_LOGD(TAG, "Read 0x%04X retrying after %s", front.address, reason);
      return;
    }
    this->queue_.erase(this->queue_.begin());
  }

  // Drop the remaining reads; next update() will queue them again. Keep
  // pending writes so a failing poll never swallows a user command.
  std::vector<Request> writes;
  for (auto &r : this->queue_)
    if (r.is_write)
      writes.push_back(r);
  this->queue_ = writes;
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
      this->fail_cycle_("connect timeout", false);
    }
    return;
  }
  if (sel < 0) {
    if (errno == EINTR)
      return;
    ESP_LOGW(TAG, "select() failed: %d", errno);
    this->close_socket_();
    this->state_ = State::IDLE;
    this->fail_cycle_("select failed", false);
    return;
  }

  int so_error = 0;
  socklen_t len = sizeof(so_error);
  if (getsockopt(this->sock_, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
    ESP_LOGW(TAG, "connect failed: %d", so_error);
    this->close_socket_();
    this->state_ = State::IDLE;
    this->fail_cycle_("connect failed", false);
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
    req.retries = 1;
    req.pdu = build_read_pdu(req.address, req.count);
    this->queue_.push_back(req);
  }
}

void EcAdapterTcp::update() {
  if (this->addresses_.empty())
    return;
  if (this->state_ != State::IDLE)
    return;  // previous cycle still in progress, retry at the next interval
  for (auto &r : this->queue_) {
    if (!r.is_write)
      return;  // reads are already queued
  }
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
    uint32_t now = millis();
    if (this->last_connect_attempt_ != 0 &&
        (uint32_t) (now - this->last_connect_attempt_) < 1000)
      return;  // throttled, try again on the next loop iteration
    this->last_connect_attempt_ = now;
    this->start_connect_();
    if (this->sock_ < 0)
      return;  // connect failed; wait out the throttle window
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
    this->fail_cycle_("socket closed", true);
    return;
  }

  while (this->tx_sent_ < this->tx_buf_.size()) {
    ssize_t n = ::send(this->sock_, this->tx_buf_.data() + this->tx_sent_,
                       this->tx_buf_.size() - this->tx_sent_, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      bool peer_closed = (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE);
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if ((int32_t) (millis() - this->deadline_) >= 0) {
          this->fail_cycle_("send timeout", false);
          return;
        }
        return;  // socket buffer full, continue in the next loop iteration
      }
      ESP_LOGW(TAG, "send failed: %d", errno);
      this->fail_cycle_("send failed", peer_closed);
      return;
    }
    if (n == 0) {
      this->fail_cycle_("send returned 0", true);
      return;
    }
    this->tx_sent_ += (size_t) n;
  }

  log_hex_("TX", this->tx_buf_.data(), this->tx_buf_.size());
  this->deadline_ = millis() + this->timeout_ms_;
  this->state_ = State::RECEIVING;
}

void EcAdapterTcp::recv_step_() {
  if (this->sock_ < 0) {
    this->fail_cycle_("socket closed", true);
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
        this->fail_cycle_("timeout", false);
      }
      return;
    }
    bool peer_closed = (errno == ECONNRESET || errno == ENOTCONN || errno == EPIPE);
    ESP_LOGW(TAG, "recv failed: %d", errno);
    this->fail_cycle_("recv failed", peer_closed);
    return;
  }
  if (n == 0) {
    this->fail_cycle_("connection closed by peer", true);
    return;
  }

  log_hex_("RX", buf, (size_t) n);
  this->rx_buf_.insert(this->rx_buf_.end(), buf, buf + n);

  if (this->rx_expected_ == 0 && this->rx_buf_.size() >= 7) {
    uint16_t rlen = (uint16_t) ((this->rx_buf_[4] << 8) | this->rx_buf_[5]);
    if (rlen < 2) {
      this->fail_cycle_("malformed MBAP length", false);
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
    this->fail_cycle_("MBAP mismatch", false);
    return;
  }

  std::vector<uint8_t> pdu(this->rx_buf_.begin() + 7, this->rx_buf_.begin() + this->rx_expected_);

  Request req = this->queue_.front();

  this->tx_buf_.clear();
  this->tx_sent_ = 0;
  this->rx_buf_.clear();
  this->rx_expected_ = 0;
  this->state_ = State::IDLE;

  if (pdu.empty()) {
    this->fail_cycle_("empty response", false);
    return;
  }

  // Exception response: 0x80 | function code, then the exception code.
  if (pdu[0] & 0x80) {
    uint8_t code = pdu.size() > 1 ? pdu[1] : 0xFF;
    this->last_exception_ = code;
    ESP_LOGV(TAG, "Modbus exception 0x%02X for function 0x%02X", code, pdu[0] & 0x7F);

    if (req.is_write) {
      if (!req.use_fc16) {
        // Many servers accept FC16 but reject FC06 (or close the connection
        // on it). Fall back once.
        this->queue_.front().use_fc16 = true;
        this->queue_.front().pdu = build_write_pdu(req.address, req.value, true);
        ESP_LOGD(TAG, "FC06 rejected (exception 0x%02X), retrying 0x%04X with FC16", code, req.address);
        return;
      }
      ESP_LOGW(TAG, "Write 0x%04X = %u rejected (Modbus exception 0x%02X)", req.address,
               (unsigned) req.value, code);
      this->queue_.erase(this->queue_.begin());
    } else if (req.count > 1) {
      // Split the range so one unsupported register does not hide the rest.
      this->queue_.erase(this->queue_.begin());
      std::vector<Request> singles;
      singles.reserve(req.count);
      for (uint16_t i = 0; i < req.count; i++) {
        Request single;
        single.address = (uint16_t) (req.address + i);
        single.count = 1;
        single.retries = 1;
        single.pdu = build_read_pdu(single.address, 1);
        singles.push_back(single);
      }
      this->queue_.insert(this->queue_.begin(), singles.begin(), singles.end());
      return;
    } else {
      this->log_missing_register_(req.address);
      this->queue_.erase(this->queue_.begin());
    }

    if (this->queue_.empty())
      for (auto *listener : this->listeners_)
        listener->on_registers_updated();
    return;
  }

  bool ok = false;
  if (pdu[0] == 0x03 && req.count > 0) {
    if (pdu.size() >= 2 && pdu[1] == req.count * 2 && pdu.size() >= (size_t) (2 + req.count * 2)) {
      for (uint16_t i = 0; i < req.count; i++)
        this->registers_[(uint16_t) (req.address + i)] =
            (uint16_t) ((pdu[2 + i * 2] << 8) | pdu[3 + i * 2]);
      this->warned_ranges_.erase(req.address);
      ok = true;
    } else {
      ESP_LOGW(TAG, "Malformed read response for 0x%04X (%u regs)", req.address, (unsigned) req.count);
    }
  } else if ((pdu[0] == 0x06 || pdu[0] == 0x10) && req.is_write) {
    this->registers_[req.address] = req.value;
    ESP_LOGI(TAG, "Write 0x%04X = %u applied", req.address, (unsigned) req.value);
    ok = true;
  } else {
    ESP_LOGW(TAG, "Unexpected function 0x%02X in response to 0x%04X", pdu[0], req.address);
  }

  if (!ok) {
    if (req.is_write && !req.use_fc16) {
      this->queue_.front().use_fc16 = true;
      this->queue_.front().pdu = build_write_pdu(req.address, req.value, true);
      ESP_LOGD(TAG, "Unexpected write response, retrying 0x%04X with FC16", req.address);
      return;
    }
    if (req.is_write)
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
  req.count = 0;
  req.retries = 1;
  req.use_fc16 = false;
  req.pdu = build_write_pdu(address, value, false);
  // Writes are user commands and must not wait behind a full polling cycle.
  // While state_ != IDLE, queue_.front() is the in-flight request, so insert
  // right after it and before any pending reads. Insertion time between
  // multiple writes is preserved.
  size_t pos = (this->state_ == State::IDLE) ? 0 : 1;
  while (pos < this->queue_.size() && this->queue_[pos].is_write)
    pos++;
  this->queue_.insert(this->queue_.begin() + pos, req);
  ESP_LOGD(TAG, "Queued write 0x%04X = %u at position %u", address, (unsigned) value,
           (unsigned) pos);
  return true;
}

}  // namespace ec_adapter_tcp
}  // namespace esphome