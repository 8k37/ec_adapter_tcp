import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, time as time_
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_TEMPERATURE_ADDRESS = "temperature_address"
CONF_TEMPERATURE_SCALE = "temperature_scale"
CONF_CURRENT_TEMPERATURE_ADDRESS = "current_temperature_address"
CONF_CURRENT_TEMPERATURE_SCALE = "current_temperature_scale"
CONF_CURRENT_TEMPERATURE_SENSOR = "current_temperature_sensor"
CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"
CONF_MIN_TEMPERATURE = "min_temperature"
CONF_MAX_TEMPERATURE = "max_temperature"
CONF_STEP = "step"
CONF_INITIAL_TARGET_TEMPERATURE = "initial_target_temperature"
CONF_AUTO_MODE = "auto_mode"
CONF_AUTO_MODE_HYSTERESIS = "auto_mode_hysteresis"
CONF_SOURCES = "sources"
CONF_NAME = "name"
CONF_SENSOR = "sensor"
CONF_SLEEP_TEMPERATURE = "sleep_temperature"
CONF_SLEEP_START_HOUR = "sleep_start_hour"
CONF_SLEEP_END_HOUR = "sleep_end_hour"
CONF_TIME_ID = "time_id"

# Defaults match the historical behaviour of this component:
#   address 0x0030  -- Connect Type (REG_W_CONNECT_TYPE: 0 = adapter, 1 = boiler panel)
#   on_value 0      -- Adapter  -> HEAT
#   off_value 1     -- Boiler panel -> OFF
# Both registers are write-only on ectoControl; the hub caches the value it
# wrote once the adapter acknowledges it and the entity publishes that.
EcAdapterTcpClimate = ec_adapter_tcp_ns.class_(
    "EcAdapterTcpClimate", cg.Component, climate.Climate
)


def _validate_sleep(config):
    """The sleep preset is time driven, so it needs a clock and a temperature."""
    if CONF_SLEEP_TEMPERATURE in config:
        if CONF_TIME_ID not in config:
            raise cv.Invalid(
                "time_id is required when sleep_temperature is set: the sleep "
                "preset is switched automatically by the local time"
            )
        if not (
            config[CONF_MIN_TEMPERATURE]
            <= config[CONF_SLEEP_TEMPERATURE]
            <= config[CONF_MAX_TEMPERATURE]
        ):
            raise cv.Invalid(
                f"sleep_temperature {config[CONF_SLEEP_TEMPERATURE]} is outside "
                f"{config[CONF_MIN_TEMPERATURE]}..{config[CONF_MAX_TEMPERATURE]}"
            )
    elif CONF_SLEEP_START_HOUR in config or CONF_SLEEP_END_HOUR in config:
        raise cv.Invalid(
            "sleep_start_hour/sleep_end_hour require sleep_temperature"
        )
    if (
        CONF_SLEEP_TEMPERATURE in config
        and config[CONF_SLEEP_START_HOUR] == config[CONF_SLEEP_END_HOUR]
    ):
        raise cv.Invalid(
            "sleep_start_hour and sleep_end_hour must differ, otherwise the "
            "night window is empty"
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(EcAdapterTcpClimate).extend(
        {
            cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
            cv.Optional(CONF_ADDRESS, default=0x0030): cv.hex_uint16_t,
            cv.Optional(CONF_TEMPERATURE_ADDRESS, default=0x0031): cv.hex_uint16_t,
            cv.Optional(CONF_TEMPERATURE_SCALE, default=10.0): cv.float_,
            cv.Optional(CONF_CURRENT_TEMPERATURE_ADDRESS): cv.hex_uint16_t,
            cv.Optional(CONF_CURRENT_TEMPERATURE_SCALE, default=1.0): cv.float_,
            # Single external sensor (typically a Home Assistant temperature
            # sensor) that provides the current temperature. Shorthand for one
            # sources: entry pinned as the active source.
            cv.Optional(CONF_CURRENT_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_ON_VALUE, default=0): cv.int_range(min=0, max=0xFFFF),
            cv.Optional(CONF_OFF_VALUE, default=1): cv.int_range(min=0, max=0xFFFF),
            cv.Optional(CONF_MIN_TEMPERATURE, default=5.0): cv.float_,
            cv.Optional(CONF_MAX_TEMPERATURE, default=30.0): cv.float_,
            cv.Optional(CONF_STEP, default=0.5): cv.float_,
            cv.Optional(CONF_INITIAL_TARGET_TEMPERATURE, default=20.0): cv.float_,
            # Thermostat: while the climate is on, hand the boiler to the adapter
            # (on_value) when the current temperature is below the target and back
            # to the boiler's own panel (off_value) once the target plus the
            # hysteresis is reached. The climate mode itself is never changed
            # automatically; the state is reported through `action`.
            cv.Optional(CONF_AUTO_MODE, default=False): cv.boolean,
            cv.Optional(CONF_AUTO_MODE_HYSTERESIS, default=0.3): cv.float_range(min=0.0, max=10.0),
            # Night preset: between sleep_start_hour (inclusive) and
            # sleep_end_hour (exclusive) the climate reports the standard
            # `sleep` preset and uses sleep_temperature instead of the normal
            # setpoint. The normal setpoint is kept and restored afterwards.
            cv.Optional(CONF_SLEEP_TEMPERATURE): cv.float_,
            cv.Optional(CONF_SLEEP_START_HOUR, default=23): cv.int_range(min=0, max=23),
            cv.Optional(CONF_SLEEP_END_HOUR, default=7): cv.int_range(min=0, max=23),
            cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_SOURCES, default=[]): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required(CONF_NAME): cv.string_strict,
                        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
                    }
                )
            ),
        }
    ),
    _validate_sleep,
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_temperature_address(config[CONF_TEMPERATURE_ADDRESS]))
    cg.add(var.set_temperature_scale(config[CONF_TEMPERATURE_SCALE]))
    if CONF_CURRENT_TEMPERATURE_ADDRESS in config:
        cg.add(
            var.set_current_temperature_address(
                config[CONF_CURRENT_TEMPERATURE_ADDRESS]
            )
        )
        cg.add(
            var.set_current_temperature_scale(
                config[CONF_CURRENT_TEMPERATURE_SCALE]
            )
        )
    cg.add(var.set_on_value(config[CONF_ON_VALUE]))
    cg.add(var.set_off_value(config[CONF_OFF_VALUE]))
    cg.add(
        var.set_visual_temperatures(
            config[CONF_MIN_TEMPERATURE],
            config[CONF_MAX_TEMPERATURE],
            config[CONF_STEP],
        )
    )
    cg.add(var.set_initial_target_temperature(config[CONF_INITIAL_TARGET_TEMPERATURE]))
    cg.add(var.set_auto_mode(config[CONF_AUTO_MODE]))
    cg.add(var.set_auto_mode_hysteresis(config[CONF_AUTO_MODE_HYSTERESIS]))

    if CONF_SLEEP_TEMPERATURE in config:
        cg.add(
            var.set_sleep_config(
                config[CONF_SLEEP_TEMPERATURE],
                config[CONF_SLEEP_START_HOUR],
                config[CONF_SLEEP_END_HOUR],
            )
        )
        clock = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time(clock))

    if CONF_CURRENT_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_SENSOR])
        cg.add(var.set_current_temperature_sensor(sens))

    for source in config[CONF_SOURCES]:
        sens = await cg.get_variable(source[CONF_SENSOR])
        cg.add(var.add_temperature_source(source[CONF_NAME], sens))