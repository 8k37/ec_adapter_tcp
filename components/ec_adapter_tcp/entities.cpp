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
  // REG_W_* registers are write-only on ectoControl: reading them returns 0
  // and would clobber the value we just wrote. The official HA integration
  // (sokolovs/ec-adapter) never polls them either. State is optimistic: the
  // hub caches the value it wrote once the adapter acknowledges the write.
  // The last written value is persisted in flash and pushed back to the
  // adapter once after boot, because it cannot be read from it.
  this->pref_ = global_preferences->make_preference<uint16_t>(this->get_object_id_hash() ^ 0x9E3779B9u);
  uint16_t stored = 0;
  if (this->pref_.load(&stored)) {
    float divisor = this->scale_ == 0.0f ? 1.0f : this->scale_;
    this->publish_state((float) stored / divisor);
    this->hub_->add_restore_write(this->address_, stored);
    return;
  }
  // Empty flash (first boot after flashing): fall back to initial_value so the
  // entity is not unknown and the adapter gets a defined value. Like the
  // stored one, it is only pushed once the adapter acknowledges the write.
  if (this->has_initial_value_) {
    float multiplier = this->scale_ == 0.0f ? 1.0f : this->scale_;
    uint16_t raw = (uint16_t) lroundf(this->initial_value_ * multiplier);
    this->publish_state(this->initial_value_);
    this->hub_->add_restore_write(this->address_, raw);
    ESP_LOGD(TAG, "Using initial_value %.3f (raw %u) for 0x%04X", this->initial_value_,
             (unsigned) raw, this->address_);
  }
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
  this->pref_.save(&raw);
}

// ---------------------------------------------------------------- select

void EcAdapterTcpSelect::setup() {
  if (this->hub_ == nullptr)
    return;
  this->hub_->add_listener(this);
  // Write-only register (0x0030 connect type, 0x0039 work mode): the adapter
  // answers reads with 0, so polling would overwrite the value we wrote.
  // Keep the optimistic value cached by the hub after a successful write, and
  // persist it in flash so the entity is not "unknown" after a reboot.
  this->pref_ = global_preferences->make_preference<uint16_t>(this->get_object_id_hash() ^ 0x5E1EC7u);
  uint16_t stored = 0;
  if (this->pref_.load(&stored)) {
    for (auto &option : this->options_) {
      if (option.second == stored) {
        this->publish_state(option.first);
        this->hub_->add_restore_write(this->address_, stored);
        return;
      }
    }
  }
  if (!this->initial_option_.empty()) {
    for (auto &option : this->options_) {
      if (option.first == this->initial_option_) {
        this->publish_state(option.first);
        this->hub_->add_restore_write(this->address_, option.second);
        return;
      }
    }
  }
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
      if (this->hub_->write_register(this->address_, option.second)) {
        this->publish_state(value);
        this->pref_.save(&option.second);
      }
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

  if (source == nullptr) {
    ESP_LOGW(TAG, "Temperature source '%s' has no sensor", name.c_str());
    return;
  }

  source->add_on_state_callback([this, index](float value) { this->on_source_value_(index, value); });

  // The sensor may already hold a value (restored from flash or delivered by
  // the API before setup()): add_on_state_callback() only fires on the *next*
  // publish, so without this the climate would stay without a current
  // temperature until the sensor happens to update again.
  if (source->has_state() && !std::isnan(source->state)) {
    this->source_values_[index] = source->state;
    ESP_LOGD(TAG, "Temperature source '%s' seeded with %.1f", name.c_str(), source->state);
  }
}

void EcAdapterTcpClimate::set_current_temperature_sensor(sensor::Sensor *source) {
  this->add_temperature_source("current", source);
  this->explicit_source_ = (int8_t) (this->sources_.size() - 1);
  this->active_source_ = (uint8_t) this->explicit_source_;
}

void EcAdapterTcpClimate::setup() {
  if (this->hub_ != nullptr) {
    this->hub_->add_listener(this);
    // 0x0039 (work mode) and 0x0031 (coolant setpoint) are write-only: reads
    // return 0, not the applied value. Do not poll them; the hub caches the
    // value from the write acknowledgement and on_registers_updated() reads
    // that cache, mirroring the HA integration's optimistic behaviour.
    //
    // The current temperature, in contrast, is a normal readable register:
    // poll it when the user pointed us at one, otherwise current_temperature
    // stays NaN until an external source publishes a value.
    if (this->current_temperature_address_ != 0)
      this->hub_->add_read_register(this->current_temperature_address_, 1);
  }

  // The setpoint is a local (virtual) room temperature; persist it across reboots.
  this->pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
  float stored = 0.0f;
  bool have_setpoint = this->pref_.load(&stored);
  if (have_setpoint) {
    // Older builds could persist a setpoint read back from the write-only
    // 0x0031 cache, which another entity may have filled with a differently
    // scaled value (e.g. 45). Reject anything outside the configured range
    // instead of publishing it, and fall back to the initial setpoint.
    if (stored < this->min_temperature_ - 0.01f || stored > this->max_temperature_ + 0.01f) {
      ESP_LOGW(TAG, "Stored setpoint %.2f is outside %.1f..%.1f, using %.1f", stored,
               this->min_temperature_, this->max_temperature_, this->initial_target_temperature_);
      stored = this->initial_target_temperature_;
      have_setpoint = false;
    }
    this->target_temperature = stored;
  } else {
    this->target_temperature = this->initial_target_temperature_;
  }
  // The sleep preset publishes its own setpoint; keep the user's setpoint
  // aside so leaving the night window restores it.
  this->normal_target_ = this->target_temperature;

  // Work mode is write-only too: persist the last commanded mode.
  this->mode_pref_ =
      global_preferences->make_preference<uint8_t>(this->get_object_id_hash() ^ 0x1F123BB5u);
  uint8_t stored_mode = 0;
  bool have_mode = this->mode_pref_.load(&stored_mode);

  // Remember which external temperature source the user picked.
  this->source_pref_ =
      global_preferences->make_preference<uint8_t>(this->get_object_id_hash() ^ 0x5A5A5A5Au);
  uint8_t stored_source = 0;
  if (this->explicit_source_ >= 0) {
    // current_temperature_sensor: wins over any stored selection.
    this->active_source_ = (uint8_t) this->explicit_source_;
  } else if (this->source_pref_.load(&stored_source) && stored_source < this->sources_.size()) {
    this->active_source_ = stored_source;
  }
  if (this->active_source_ >= this->sources_.size())
    this->active_source_ = 0;

  // Take the already known value of the active source right away, so the
  // climate never sits on a NaN current temperature when a value exists.
  if (this->active_source_ < this->source_values_.size()) {
    float value = this->source_values_[this->active_source_];
    if (!std::isnan(value))
      this->current_temperature = value;
  }

  // Restore the commanded mode: it is the user's choice, not a readback of the
  // write-only work-mode register.
  this->mode = have_mode ? (climate::ClimateMode) stored_mode : climate::CLIMATE_MODE_OFF;
  this->action = (this->mode == climate::CLIMATE_MODE_OFF) ? climate::CLIMATE_ACTION_OFF
                                                           : climate::CLIMATE_ACTION_IDLE;
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->publish_state();

  // Push the last known values back to the adapter on the first cycle after
  // boot: these registers are write-only, so the adapter would otherwise keep
  // whatever it had while the UI shows the stored value. Only values that were
  // actually set at least once are restored (never an implicit default).
  if (this->hub_ != nullptr) {
    if (have_setpoint) {
      float multiplier = this->temperature_scale_ == 0.0f ? 1.0f : this->temperature_scale_;
      uint16_t raw = (uint16_t) lroundf(this->target_temperature * multiplier);
      this->hub_->add_restore_write(this->temperature_address_, raw);
    }
    if (have_mode) {
      // With auto_mode the register is re-evaluated on the first update, so a
      // warm room does not get the adapter switched on just because it was on
      // before the reboot: restore the idle state and let the thermostat decide.
      uint16_t raw = (stored_mode == (uint8_t) climate::CLIMATE_MODE_HEAT && !this->auto_mode_)
                         ? this->on_value_
                         : this->off_value_;
      this->hub_->add_restore_write(this->address_, raw);
    }
  }
}

void EcAdapterTcpClimate::set_active_source(const std::string &name) {
  for (size_t i = 0; i < this->sources_.size(); i++) {
    if (this->sources_[i].first == name) {
      this->active_source_ = (uint8_t) i;
      this->source_pref_.save(&this->active_source_);
      if (!std::isnan(this->source_values_[i]))
        this->current_temperature = this->source_values_[i];
      // Re-evaluate right away: switching to a source with a known value must
      // not wait for the next poll to engage or release the adapter.
      this->apply_auto_mode_();
      this->publish_state();
      ESP_LOGI(TAG, "Active temperature source: %s", name.c_str());
      return;
    }
  }
  ESP_LOGW(TAG, "Unknown temperature source '%s'", name.c_str());
}

bool EcAdapterTcpClimate::is_sleep_window_(uint8_t hour) const {
  if (this->sleep_start_hour_ == this->sleep_end_hour_)
    return false;
  if (this->sleep_start_hour_ < this->sleep_end_hour_)
    return hour >= this->sleep_start_hour_ && hour < this->sleep_end_hour_;
  // The window wraps around midnight (the default 23 -> 7 does).
  return hour >= this->sleep_start_hour_ || hour < this->sleep_end_hour_;
}

void EcAdapterTcpClimate::set_sleep_active_(bool active) {
  if (this->sleep_active_ == active)
    return;
  this->sleep_active_ = active;
  if (active) {
    this->preset = climate::CLIMATE_PRESET_SLEEP;
    this->target_temperature = this->sleep_temperature_;
  } else {
    this->preset = climate::CLIMATE_PRESET_NONE;
    this->target_temperature = this->normal_target_;
  }

  // The preset only changes the setpoint published by this thermostat: the
  // adapter's setpoint register is left alone, so the night value never
  // reaches the boiler. With auto_mode the new target is still what decides
  // whether the adapter is engaged.
  this->apply_auto_mode_();
  this->publish_state();
  ESP_LOGI(TAG, "Sleep preset %s, target %.1f", active ? "on" : "off",
           this->target_temperature);
}

void EcAdapterTcpClimate::loop() {
  if (!this->sleep_enabled_ || this->time_ == nullptr)
    return;
  // The clock is read at most every 30 s: the preset only changes twice a day.
  uint32_t now_ms = millis();
  if (this->last_sleep_check_ != 0 && now_ms - this->last_sleep_check_ < 30000)
    return;
  this->last_sleep_check_ = now_ms;

  auto now = this->time_->now();
  if (!now.is_valid())
    return;

  bool in_window = this->is_sleep_window_(now.hour);
  if (!this->last_window_known_) {
    // First valid time after boot: adopt the schedule right away, so a reboot
    // at 02:00 keeps the night setpoint.
    this->last_window_known_ = true;
    this->last_window_state_ = in_window;
    this->set_sleep_active_(in_window);
    return;
  }
  // Only react to a window edge: a manual preset or setpoint change inside the
  // same window is not overwritten until the next edge.
  if (in_window == this->last_window_state_)
    return;
  this->last_window_state_ = in_window;
  this->set_sleep_active_(in_window);
}

void EcAdapterTcpClimate::apply_auto_mode_() {
  if (!this->auto_mode_)
    return;

  // `mode` is the user's command: the thermostat never turns the climate on or
  // off by itself, it only decides who drives the boiler.
  if (this->mode != climate::CLIMATE_MODE_HEAT) {
    this->action = climate::CLIMATE_ACTION_OFF;
    return;
  }
  if (this->hub_ == nullptr)
    return;

  // The work-mode register is write-only, so the hub cache holds the value of
  // the last acknowledged write: use it to avoid rewriting the same value.
  uint16_t cached = 0;
  bool have_cached = this->hub_->read_register(this->address_, &cached);

  if (std::isnan(this->current_temperature) || std::isnan(this->target_temperature)) {
    // Fail safe: without a known temperature the thermostat cannot decide, so
    // it must not leave the adapter heating. Hand the boiler back to its own
    // panel and report idle until a temperature is available again. This also
    // covers the boot case where the Home Assistant sensor has not published
    // yet: the adapter is not engaged on stale information.
    this->action = climate::CLIMATE_ACTION_IDLE;
    if (!have_cached || cached != this->off_value_)
      this->hub_->write_register(this->address_, this->off_value_);
    return;
  }

  // Below the setpoint the boiler is handed to the adapter (on_value). At or
  // above the setpoint plus the hysteresis it goes back to its own panel
  // (off_value), which is the thermostat's idle state: the climate is still on,
  // but the adapter does not heat. Inside the dead band nothing is written, so
  // the register is not toggled on every poll.
  uint16_t desired;
  if (this->current_temperature < this->target_temperature) {
    desired = this->on_value_;
    this->action = climate::CLIMATE_ACTION_HEATING;
  } else if (this->current_temperature >= this->target_temperature + this->auto_mode_hysteresis_) {
    desired = this->off_value_;
    this->action = climate::CLIMATE_ACTION_IDLE;
  } else {
    this->action = (have_cached && cached == this->on_value_) ? climate::CLIMATE_ACTION_HEATING
                                                              : climate::CLIMATE_ACTION_IDLE;
    return;
  }

  if (have_cached && cached == desired)
    return;
  if (!this->hub_->write_register(this->address_, desired)) {
    ESP_LOGW(TAG, "Auto mode: writing 0x%04X = %u failed", this->address_, (unsigned) desired);
    return;
  }
  ESP_LOGI(TAG, "Auto mode: current %.1f, target %.1f -> %s", this->current_temperature,
           this->target_temperature,
           desired == this->on_value_ ? "adapter (heating)" : "boiler panel (idle)");
}

void EcAdapterTcpClimate::on_source_value_(uint8_t index, float value) {
  if (index >= this->source_values_.size())
    return;
  this->source_values_[index] = value;
  if (index != this->active_source_)
    return;
  if (std::isnan(value)) {
    // The source went unavailable; keep the last known value rather than
    // dropping the current temperature from the UI.
    ESP_LOGD(TAG, "Active temperature source '%s' is unavailable, keeping %.1f",
             this->sources_[index].first.c_str(), this->current_temperature);
    return;
  }
  ESP_LOGV(TAG, "Current temperature %.2f from source '%s'", value,
           this->sources_[index].first.c_str());
  this->current_temperature = value;
  this->apply_auto_mode_();
  this->publish_state();
}

void EcAdapterTcpClimate::dump_config() {
  LOG_CLIMATE("", "ectoControl climate", this);
  ESP_LOGCONFIG(TAG, "  Work mode register: 0x%04X (on=%u, off=%u, write-only)", this->address_,
                (unsigned) this->on_value_, (unsigned) this->off_value_);
  ESP_LOGCONFIG(TAG, "  Target temperature register: 0x%04X (scale x%.3f, write-only)",
                this->temperature_address_, this->temperature_scale_);
  if (!this->sources_.empty()) {
    ESP_LOGCONFIG(TAG, "  Current temperature: %u external source(s), active '%s'",
                  (unsigned) this->sources_.size(),
                  this->active_source_ < this->sources_.size()
                      ? this->sources_[this->active_source_].first.c_str()
                      : "?");
  } else if (this->current_temperature_address_ != 0) {
    ESP_LOGCONFIG(TAG, "  Current temperature register: 0x%04X (scale x%.3f)",
                  this->current_temperature_address_, this->current_temperature_scale_);
  } else {
    ESP_LOGCONFIG(TAG, "  Current temperature: no source configured");
  }
  ESP_LOGCONFIG(TAG, "  Setpoint: local, initial %.1f, range %.1f..%.1f step %.1f",
                this->initial_target_temperature_, this->min_temperature_, this->max_temperature_, this->step_);
  if (this->auto_mode_)
    ESP_LOGCONFIG(TAG, "  Auto mode: on, hysteresis %.1f (mode stays as commanded, state in action)",
                  this->auto_mode_hysteresis_);
  if (this->sleep_enabled_)
    ESP_LOGCONFIG(TAG, "  Sleep preset: %.1f from %02u:00 to %02u:00 (%s), currently %s",
                  this->sleep_temperature_, (unsigned) this->sleep_start_hour_,
                  (unsigned) this->sleep_end_hour_,
                  this->time_ != nullptr ? "time configured" : "no time source",
                  this->sleep_active_ ? "active" : "inactive");
}

climate::ClimateTraits EcAdapterTcpClimate::traits() {
  climate::ClimateTraits traits;
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
  if (this->sleep_enabled_)
    traits.set_supported_presets({climate::CLIMATE_PRESET_NONE, climate::CLIMATE_PRESET_SLEEP});
  traits.set_visual_min_temperature(this->min_temperature_);
  traits.set_visual_max_temperature(this->max_temperature_);
  traits.set_visual_temperature_step(this->step_);
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                         climate::CLIMATE_SUPPORTS_ACTION);
  return traits;
}

void EcAdapterTcpClimate::on_registers_updated() {
  if (this->hub_ == nullptr)
    return;

  // The work-mode register is write-only and `mode` is the user's command,
  // persisted in flash: it is never derived from the register cache here,
  // otherwise the thermostat's own writes would flip the climate on and off.
  //
  // The setpoint register 0x0031 is write-only as well, so its cache only ever
  // holds a value *we* wrote. Reading it back was actively harmful: any other
  // entity writing the same register with a different scale (e.g. a number
  // with scale 10 setting 45 -> raw 450) made the climate publish 45.0 as its
  // target, far outside min/max. The setpoint stays local: set by control(),
  // persisted in flash and restored in setup().

  // Current temperature: an external source (e.g. a Home Assistant sensor) has
  // priority; the adapter's own readable register is only used when no source
  // is configured. Without either, current_temperature stays NaN and Home
  // Assistant shows no current temperature at all.
  if (!this->sources_.empty()) {
    if (this->active_source_ < this->source_values_.size()) {
      float value = this->source_values_[this->active_source_];
      if (!std::isnan(value))
        this->current_temperature = value;
    }
  } else if (this->current_temperature_address_ != 0) {
    uint16_t current_raw = 0;
    if (this->hub_->read_register(this->current_temperature_address_, &current_raw)) {
      float divisor =
          this->current_temperature_scale_ == 0.0f ? 1.0f : this->current_temperature_scale_;
      // Temperatures are signed: 0xFFF6 is -1.0 C, not 6552.6.
      this->current_temperature = (float) (int16_t) current_raw / divisor;
    }
  }

  this->apply_auto_mode_();

  this->publish_state();
}

void EcAdapterTcpClimate::control(const climate::ClimateCall &call) {
  bool changed = false;

  if (call.get_preset().has_value() && this->sleep_enabled_) {
    // Manual preset selection. The schedule re-applies sleep (or the normal
    // setpoint) at the next window edge, so the night window keeps working.
    if (*call.get_preset() == climate::CLIMATE_PRESET_SLEEP) {
      this->set_sleep_active_(true);
      changed = true;
    } else if (*call.get_preset() == climate::CLIMATE_PRESET_NONE) {
      this->set_sleep_active_(false);
      changed = true;
    }
  }

  if (call.get_mode().has_value()) {
    climate::ClimateMode mode = *call.get_mode();
    if (this->auto_mode_ && mode != climate::CLIMATE_MODE_OFF) {
      // Turning the climate on only records the wish to heat. Whether the
      // adapter is actually engaged is decided by apply_auto_mode_(): in an
      // already warm room the thermostat stays idle and the adapter is left
      // alone until the temperature drops below the setpoint.
      this->mode = climate::CLIMATE_MODE_HEAT;
      uint8_t mode_byte = (uint8_t) this->mode;
      this->mode_pref_.save(&mode_byte);
      this->apply_auto_mode_();
      changed = true;
    } else {
      uint16_t value = (mode == climate::CLIMATE_MODE_OFF) ? this->off_value_ : this->on_value_;
      if (this->hub_ == nullptr || this->hub_->write_register(this->address_, value)) {
        this->mode =
            (mode == climate::CLIMATE_MODE_OFF) ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT;
        this->action = (this->mode == climate::CLIMATE_MODE_OFF) ? climate::CLIMATE_ACTION_OFF
                                                                 : climate::CLIMATE_ACTION_HEATING;
        uint8_t mode_byte = (uint8_t) this->mode;
        this->mode_pref_.save(&mode_byte);
        changed = true;
      }
    }
  }

  if (call.get_target_temperature().has_value()) {
    float value = *call.get_target_temperature();
    if (value < this->min_temperature_)
      value = this->min_temperature_;
    if (value > this->max_temperature_)
      value = this->max_temperature_;
    // The setpoint is local: store and publish it even when the adapter is
    // unreachable, so the UI never silently keeps an old value. The write is
    // queued and retried by the hub.
    // A manual setpoint change ends the night preset: the user just asked for
    // exactly this temperature. The schedule re-applies sleep at the next edge.
    if (this->sleep_active_) {
      this->sleep_active_ = false;
      this->preset = climate::CLIMATE_PRESET_NONE;
    }
    this->normal_target_ = value;
    this->target_temperature = value;
    this->pref_.save(&value);
    changed = true;
    if (this->hub_ != nullptr) {
      float multiplier = this->temperature_scale_ == 0.0f ? 1.0f : this->temperature_scale_;
      uint16_t raw = (uint16_t) lroundf(value * multiplier);
      if (!this->hub_->write_register(this->temperature_address_, raw))
        ESP_LOGW(TAG, "Could not queue setpoint 0x%04X = %u", this->temperature_address_,
                 (unsigned) raw);
    }
  }

  // A new setpoint takes effect immediately, without waiting for the next poll.
  if (this->auto_mode_)
    this->apply_auto_mode_();

  if (changed)
    this->publish_state();
}

}  // namespace ec_adapter_tcp
}  // namespace esphome
