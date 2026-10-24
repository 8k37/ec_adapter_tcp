#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ec_adapter_tcp.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace ec_adapter_tcp {

enum EcValueType : uint8_t {
  EC_VALUE_UINT8 = 0,
  EC_VALUE_INT8 = 1,
  EC_VALUE_UINT16 = 2,
  EC_VALUE_INT16 = 3,
  EC_VALUE_UINT32 = 4,
  EC_VALUE_INT32 = 5,
};

class EcAdapterTcpSensor : public Component, public sensor::Sensor, public EcAdapterTcpListener {
 public:
  void setup() override;
  void dump_config() override;
  void on_registers_updated() override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void set_value_type(EcValueType type) { this->type_ = type; }
  void set_scale(float scale) { this->scale_ = scale; }
  void set_bitmask(uint16_t mask) { this->mask_ = mask; }
  void set_shift(uint8_t shift) { this->shift_ = shift; }

 protected:
  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0};
  EcValueType type_{EC_VALUE_UINT16};
  float scale_{1.0f};
  uint16_t mask_{0};
  uint8_t shift_{0};
};

class EcAdapterTcpBinarySensor : public Component, public binary_sensor::BinarySensor, public EcAdapterTcpListener {
 public:
  void setup() override;
  void dump_config() override;
  void on_registers_updated() override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void set_bitmask(uint16_t mask) { this->mask_ = mask; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }

 protected:
  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0};
  uint16_t mask_{0};
  bool inverted_{false};
};

class EcAdapterTcpNumber : public Component, public number::Number, public EcAdapterTcpListener {
 public:
  void setup() override;
  void dump_config() override;
  void on_registers_updated() override;
  void control(float value) override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void set_scale(float scale) { this->scale_ = scale; }
  void set_range(float min_value, float max_value, float step) {
    this->min_value_ = min_value;
    this->max_value_ = max_value;
    this->step_ = step;
  }
  // Value published and pushed to the adapter when nothing has been written
  // from this device yet (empty flash, e.g. right after the first flash). The
  // register is write-only, so without it the entity stays unknown until the
  // user sets a value by hand.
  void set_initial_value(float value) {
    this->initial_value_ = value;
    this->has_initial_value_ = true;
  }

 protected:
  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0};
  float scale_{1.0f};
  float min_value_{0.0f};
  float max_value_{100.0f};
  float step_{1.0f};
  float initial_value_{0.0f};
  bool has_initial_value_{false};
  // Last value written, persisted in flash: the register is write-only, so it
  // cannot be read back after a reboot.
  ESPPreferenceObject pref_;
};

class EcAdapterTcpSelect : public Component, public select::Select, public EcAdapterTcpListener {
 public:
  void setup() override;
  void dump_config() override;
  void on_registers_updated() override;
  void control(const std::string &value) override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void add_option(const std::string &name, uint16_t value) { this->options_.emplace_back(name, value); }
  void set_initial_option(const std::string &name) { this->initial_option_ = name; }

 protected:
  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0};
  // Publish this option when nothing has been written yet (no flash value).
  std::string initial_option_;
  // Last value successfully written to the adapter, persisted in flash: the
  // register is write-only, so it cannot be read back after a reboot.
  ESPPreferenceObject pref_;
  std::vector<std::pair<std::string, uint16_t>> options_;
};

class EcAdapterTcpButton : public Component, public button::Button {
 public:
  void setup() override {}
  void dump_config() override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void set_value(uint16_t value) { this->value_ = value; }

 protected:
  void press_action() override;

  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0};
  uint16_t value_{0};
};

class EcAdapterTcpClimate : public Component, public climate::Climate, public EcAdapterTcpListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_registers_updated() override;
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void set_hub(EcAdapterTcp *hub) { this->hub_ = hub; }
  void set_address(uint16_t address) { this->address_ = address; }
  void set_temperature_address(uint16_t address) { this->temperature_address_ = address; }
  void set_temperature_scale(float scale) { this->temperature_scale_ = scale; }
  // Readable register that reports the boiler's actual current temperature
  // (e.g. the coolant sensor). Optional: when left at 0 the current
  // temperature can only come from an external source (add_temperature_source).
  void set_current_temperature_address(uint16_t address) { this->current_temperature_address_ = address; }
  void set_current_temperature_scale(float scale) { this->current_temperature_scale_ = scale; }
  void set_on_value(uint16_t value) { this->on_value_ = value; }
  void set_off_value(uint16_t value) { this->off_value_ = value; }
  void set_visual_temperatures(float min_temperature, float max_temperature, float step) {
    this->min_temperature_ = min_temperature;
    this->max_temperature_ = max_temperature;
    this->step_ = step;
  }
  void set_initial_target_temperature(float value) { this->initial_target_temperature_ = value; }
  // Thermostat. `mode` always stays the user's command: the component never
  // turns the climate on or off by itself. While the climate is on, the
  // thermostat only decides who drives the boiler: the adapter (on_value) when
  // the current temperature is below the target, the boiler's own panel
  // (off_value) once the target plus the hysteresis is reached. The state is
  // reported through `action`, so a warm room leaves the thermostat idle
  // instead of starting the adapter.
  void set_auto_mode(bool auto_mode) { this->auto_mode_ = auto_mode; }
  void set_auto_mode_hysteresis(float hysteresis) { this->auto_mode_hysteresis_ = hysteresis; }
  // Night preset. When configured, the climate reports the standard `sleep`
  // preset and uses `temperature` as its setpoint between `start_hour`
  // (inclusive) and `end_hour` (exclusive) of the local time. The normal
  // setpoint is kept untouched and restored when the window ends.
  // `temperature` is only the initial value: a setpoint change made while the
  // preset is active edits it and persists the new value in flash, so the
  // preset stays on and is reused with the new temperature at the next window.
  void set_sleep_config(float temperature, uint8_t start_hour, uint8_t end_hour) {
    this->sleep_temperature_ = temperature;
    this->sleep_start_hour_ = start_hour;
    this->sleep_end_hour_ = end_hour;
    this->sleep_enabled_ = true;
  }
  void set_time(time::RealTimeClock *clock) { this->time_ = clock; }

  // Register an external temperature source (e.g. a Home Assistant sensor).
  void add_temperature_source(const std::string &name, sensor::Sensor *source);

  // Shortcut for the common case: one sensor (e.g. a Home Assistant
  // temperature sensor) is the only current temperature source. It is added
  // as a source and pinned as the active one, ignoring the stored selection.
  void set_current_temperature_sensor(sensor::Sensor *source);

  // Switch the active temperature source by its name (used by the select).
  void set_active_source(const std::string &name);

  // Direct value injection, kept for simple lambdas.
  void set_current_temperature(float value) {
    this->current_temperature = value;
    this->publish_state();
  }

 protected:
  void on_source_value_(uint8_t index, float value);
  // Thermostat step: hands the boiler to the adapter or back to its own panel
  // and updates `action`. Never changes `mode`. No-op when auto_mode_ is off.
  void apply_auto_mode_();
  // True when `hour` (0..23, local time) falls inside the night window. The
  // window may wrap around midnight (the default 23 -> 7 does).
  bool is_sleep_window_(uint8_t hour) const;
  // Switch the sleep preset on or off: swaps the published setpoint (local to
  // this thermostat, never written to the adapter's setpoint register) and
  // re-evaluates the thermostat. No-op when unchanged.
  void set_sleep_active_(bool active);

  EcAdapterTcp *hub_{nullptr};
  uint16_t address_{0x0030};
  uint16_t temperature_address_{0x0031};
  float temperature_scale_{10.0f};
  uint16_t current_temperature_address_{0};
  float current_temperature_scale_{1.0f};
  uint16_t on_value_{0};
  uint16_t off_value_{1};
  float min_temperature_{5.0f};
  float max_temperature_{30.0f};
  float step_{0.5f};
  float initial_target_temperature_{20.0f};
  bool auto_mode_{false};
  float auto_mode_hysteresis_{0.3f};
  // Night preset state. `normal_target_` is the user's setpoint outside the
  // night window; while sleeping the climate publishes `sleep_temperature_`
  // instead, so leaving the window restores the previous setpoint.
  bool sleep_enabled_{false};
  bool sleep_active_{false};
  float sleep_temperature_{18.0f};
  uint8_t sleep_start_hour_{23};
  uint8_t sleep_end_hour_{7};
  float normal_target_{20.0f};
  time::RealTimeClock *time_{nullptr};
  uint32_t last_sleep_check_{0};
  // Last evaluated state of the night window. The preset only changes on a
  // window edge, so a manual override (user preset or setpoint change) is not
  // fought by the scheduler until the next edge.
  bool last_window_known_{false};
  bool last_window_state_{false};
  ESPPreferenceObject pref_;
  ESPPreferenceObject mode_pref_;
  ESPPreferenceObject source_pref_;
  // The sleep setpoint is persisted separately from the normal one, so a change
  // made while the preset is active survives a reboot and the night window.
  ESPPreferenceObject sleep_pref_;

  std::vector<std::pair<std::string, sensor::Sensor *>> sources_;
  std::vector<float> source_values_;
  uint8_t active_source_{0};
  // Index of the source pinned by set_current_temperature_sensor(), or -1 when
  // the user relies on sources: plus the stored selection.
  int8_t explicit_source_{-1};
};

}  // namespace ec_adapter_tcp
}  // namespace esphome