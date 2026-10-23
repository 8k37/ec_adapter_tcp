#include "entities.h"

#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace ec_adapter_tcp {

static const char *const TAG = "ec_adapter_tcp";

static bool decode_value(EcAdapterTcp *hub, uint16_t address, EcValueType type, float *out) {
  uint16_t raw = 0;
  if (!hub->read_register(address, &raw))
    return false;
  switch (type) {
    case EC_VALUE_UINT8:
      *out = (float) (raw & 0xFF);
      break;
    case EC_VALUE_INT8:
      *out = (float) (int8_t) (raw & 0xFF);
      break;
    case EC_VALUE_UINT16:
      *out = (float) raw;
      break;
    case EC_VALUE_INT16:
      *out = (float) (int16_t) raw;
      break;
    case EC_VALUE_UINT32:
    case EC_VALUE_INT32: {
      uint16_t lo = 0;
      if (!hub->read_register((uint16_t) (address + 1), &lo))
        return false;
      uint32_t u = ((uint32_t) raw << 16) | lo;
      *out = (type == EC_VALUE_UINT32) ? (float) u : (float) (int32_t) u;
      break;
    }
    default:
      return false;
  }
  return true;
}

static uint8_t register_count(EcValueType type) {
  return (type == EC_VALUE_UINT32 || type == EC_VALUE_INT32) ? 2 : 1;
}

// ---------------------------------------------------------------- sensor

void EcAdapterTcpSensor::setup() {
  if (this->hub_ == nullptr)
    return;
  this->hub_->add_listener(this);
  this->hub_->add_read_register(this->address_, register_count(this->type_));
}

void EcAdapterTcpSensor::dump_config() {
  LOG_SENSOR("", "ectoControl sensor", this);
  ESP_LOGCONFIG(TAG, "  Register: 0x%04X", this->address_);
}

void EcAdapterTcpSensor::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;
  float value = 0.0f;
  if (this->mask_ != 0) {
    uint16_t raw = 0;
    if (!this->hub_->read_register(this->address_, &raw))
      return;
    value = (float) ((raw & this->mask_) >> this->shift_);
  } else {
    if (!decode_value(this->hub_, this->address_, this->type_, &value))
      return;
  }
  this->publish_state(value * this->scale_);
}

// --------------------------------------------------------- binary sensor

void EcAdapterTcpBinarySensor::setup() {
  if (this->hub_ == nullptr)
    return;
  this->hub_->add_listener(this);
  this->hub_->add_read_register(this->address_, 1);
}

void EcAdapterTcpBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "ectoControl binary sensor", this);
  ESP_LOGCONFIG(TAG, "  Register: 0x%04X", this->address_);
}

void EcAdapterTcpBinarySensor::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;
  uint16_t raw = 0;
  if (!this->hub_->read_register(this->address_, &raw))
    return;
  bool state = (raw & this->mask_) != 0;
  if (this->inverted_)
    state = !state;
  this->publish_state(state);
}

// ---------------------------------------------------------------- number

void EcAdapterTcpNumber::setup() {
  if (this->hub_ == nullptr)
    return;
  this->hub_->add_listener(this);
  this->hub_->add_read_register(this->address_, 1);
}

void EcAdapterTcpNumber::dump_config() {
  LOG_NUMBER("", "ectoControl number", this);
  ESP_LOGCONFIG(TAG, "  Register: 0x%04X (scale x%.3f)", this->address_, this->scale_);
}

void EcAdapterTcpNumber::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;
  uint16_t raw = 0;
  if (!this->hub_->read_register(this->address_, &raw))
    return;
  float divisor = this->scale_ == 0.0f ? 1.0f : this->scale_;
  this->publish_state((float) raw / divisor);
}

void EcAdapterTcpNumber::control(float value) {
  if (this->hub_ == nullptr)
    return;
  float multiplier = this->scale_ == 0.0f ? 1.0f : this->scale_;
  uint16_t raw = (uint16_t) lroundf(value * multiplier);
  if (!this->hub_->write_register(this->address_, raw))
    return;
  this->publish_state(value);
}

// ---------------------------------------------------------------- select

void EcAdapterTcpSelect::setup() {
  if (this->hub_ == nullptr)
    return;
  this->hub_->add_listener(this);
  this->hub_->add_read_register(this->address_, 1);
}

void EcAdapterTcpSelect::dump_config() {
  LOG_SELECT("", "ectoControl select", this);
  ESP_LOGCONFIG(TAG, "  Register: 0x%04X", this->address_);
}

void EcAdapterTcpSelect::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;
  uint16_t raw = 0;
  if (!this->hub_->read_register(this->address_, &raw))
    return;
  for (auto &option : this->options_) {
    if (option.second == raw) {
      this->publish_state(option.first);
      return;
    }
  }
}

void EcAdapterTcpSelect::control(const std::string &value) {
  if (this->hub_ == nullptr)
    return;
  for (auto &option : this->options_) {
    if (option.first == value) {
      if (this->hub_->write_register(this->address_, option.second))
        this->publish_state(value);
      return;
    }
  }
}

// ---------------------------------------------------------------- button

void EcAdapterTcpButton::dump_config() {
  LOG_BUTTON("", "ectoControl button", this);
  ESP_LOGCONFIG(TAG, "  Register: 0x%04X value %u", this->address_, (unsigned) this->value_);
}

void EcAdapterTcpButton::press_action() {
  if (this->hub_ == nullptr)
    return;
  if (!this->hub_->write_register(this->address_, this->value_))
    ESP_LOGW(TAG, "Command 0x%04X = %u failed", this->address_, (unsigned) this->value_);
}

// --------------------------------------------------------------- climate

void EcAdapterTcpClimate::add_temperature_source(const std::string &name, sensor::Sensor *source) {
  uint8_t index = (uint8_t) this->sources_.size();
  this->sources_.emplace_back(name, source);
  this->source_values_.push_back(NAN);
  if (source != nullptr)
    source->add_on_state_callback([this, index](float value) { this->on_source_value_(index, value); });
}

void EcAdapterTcpClimate::setup() {
  if (this->hub_ != nullptr) {
    this->hub_->add_listener(this);
    this->hub_->add_read_register(this->address_, 1);
    this->hub_->add_read_register(this->temperature_address_, 1);
  }

  // The setpoint is a local (virtual) room temperature; persist it across reboots.
  this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
  float stored = 0.0f;
  if (this->pref_.load(&stored)) {
    this->target_temperature = stored;
  } else {
    this->target_temperature = this->initial_target_temperature_;
  }

  // Remember which external temperature source the user picked.
  this->source_pref_ =
      global_preferences->make_preference<uint8_t>(this->get_object_id_hash() ^ 0x5A5A5A5Au);
  uint8_t stored_source = 0;
  if (this->source_pref_.load(&stored_source) && stored_source < this->sources_.size())
    this->active_source_ = stored_source;

  this->mode = climate::CLIMATE_MODE_OFF;
  this->publish_state();
}

void EcAdapterTcpClimate::set_active_source(const std::string &name) {
  for (size_t i = 0; i < this->sources_.size(); i++) {
    if (this->sources_[i].first == name) {
      this->active_source_ = (uint8_t) i;
      this->source_pref_.save(&this->active_source_);
      if (!std::isnan(this->source_values_[i]))
        this->current_temperature = this->source_values_[i];
      this->publish_state();
      ESP_LOGI(TAG, "Active temperature source: %s", name.c_str());
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown temperature source '%s'", name.c_str());
}

void EcAdapterTcpClimate::on_source_value_(uint8_t index, float value) {
  if (index >= this->source_values_.size())
    return;
  this->source_values_[index] = value;
  if (index != this->active_source_ || std::isnan(value))
    return;
  this->current_temperature = value;
  this->publish_state();
}

void EcAdapterTcpClimate::dump_config() {
  LOG_CLIMATE("", "ectoControl climate", this);
  ESP_LOGCONFIG(TAG, "  On/off register: 0x%04X (on=%u, off=%u)", this->address_, (unsigned) this->on_value_,
                (unsigned) this->off_value_);
  ESP_LOGCONFIG(TAG, "  Target temperature register: 0x%04X (scale x%.3f)", this->temperature_address_,
                this->temperature_scale_);
  ESP_LOGCONFIG(TAG, "  Setpoint: local, initial %.1f, range %.1f..%.1f step %.1f",
                this->initial_target_temperature_, this->min_temperature_, this->max_temperature_, this->step_);
}

climate::ClimateTraits EcAdapterTcpClimate::traits() {
  climate::ClimateTraits traits;
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
  traits.set_visual_min_temperature(this->min_temperature_);
  traits.set_visual_max_temperature(this->max_temperature_);
  traits.set_visual_temperature_step(this->step_);
  return traits;
}

void EcAdapterTcpClimate::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;

  uint16_t raw = 0;
  if (this->hub_->read_register(this->address_, &raw))
    this->mode = (raw == this->on_value_) ? climate::CLIMATE_MODE_HEAT : climate::CLIMATE_MODE_OFF;

  uint16_t target_raw = 0;
  if (this->hub_->read_register(this->temperature_address_, &target_raw)) {
    float divisor = this->temperature_scale_ == 0.0f ? 1.0f : this->temperature_scale_;
    this->target_temperature = (float) target_raw / divisor;
  }

  // current_temperature is fed externally via set_current_temperature().
  this->publish_state();
}

void EcAdapterTcpClimate::control(const climate::ClimateCall &call) {
  bool changed = false;

  if (call.get_mode().has_value()) {
    climate::ClimateMode mode = *call.get_mode();
    uint16_t value = (mode == climate::CLIMATE_MODE_OFF) ? this->off_value_ : this->on_value_;
    if (this->hub_ == nullptr || this->hub_->write_register(this->address_, value)) {
      this->mode = (mode == climate::CLIMATE_MODE_OFF) ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT;
      changed = true;
    }
  }

  if (call.get_target_temperature().has_value()) {
    float value = *call.get_target_temperature();
    if (value < this->min_temperature_)
      value = this->min_temperature_;
    if (value > this->max_temperature_)
      value = this->max_temperature_;
    if (this->hub_ != nullptr) {
      float multiplier = this->temperature_scale_ == 0.0f ? 1.0f : this->temperature_scale_;
      uint16_t raw = (uint16_t) lroundf(value * multiplier);
      if (this->hub_->write_register(this->temperature_address_, raw)) {
        this->target_temperature = value;
        this->pref_.save(&value);
        changed = true;
      }
    }
  }

  if (changed)
    this->publish_state();
}

}  // namespace ec_adapter_tcp
}  // namespace esphome