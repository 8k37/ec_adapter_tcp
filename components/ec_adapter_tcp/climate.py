import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_TEMPERATURE_ADDRESS = "temperature_address"
CONF_TEMPERATURE_SCALE = "temperature_scale"
CONF_ON_VALUE = "on_value"
CONF_OFF_VALUE = "off_value"
CONF_MIN_TEMPERATURE = "min_temperature"
CONF_MAX_TEMPERATURE = "max_temperature"
CONF_STEP = "step"
CONF_INITIAL_TARGET_TEMPERATURE = "initial_target_temperature"
CONF_SOURCES = "sources"
CONF_NAME = "name"
CONF_SENSOR = "sensor"

# Defaults match the historical behaviour of this component:
#   address 0x0030  -- Connect Type (REG_W_CONNECT_TYPE: 0 = adapter, 1 = boiler panel)
#   on_value 0      -- Adapter  -> HEAT
#   off_value 1     -- Boiler panel -> OFF
# Both registers are write-only on ectoControl; the hub caches the value it
# wrote once the adapter acknowledges it and the entity publishes that.
EcAdapterTcpClimate = ec_adapter_tcp_ns.class_(
    "EcAdapterTcpClimate", cg.Component, climate.Climate
)

CONFIG_SCHEMA = climate.climate_schema(EcAdapterTcpClimate).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Optional(CONF_ADDRESS, default=0x0030): cv.hex_uint16_t,
        cv.Optional(CONF_TEMPERATURE_ADDRESS, default=0x0031): cv.hex_uint16_t,
        cv.Optional(CONF_TEMPERATURE_SCALE, default=10.0): cv.float_,
        cv.Optional(CONF_ON_VALUE, default=0): cv.int_range(min=0, max=0xFFFF),
        cv.Optional(CONF_OFF_VALUE, default=1): cv.int_range(min=0, max=0xFFFF),
        cv.Optional(CONF_MIN_TEMPERATURE, default=5.0): cv.float_,
        cv.Optional(CONF_MAX_TEMPERATURE, default=30.0): cv.float_,
        cv.Optional(CONF_STEP, default=0.5): cv.float_,
        cv.Optional(CONF_INITIAL_TARGET_TEMPERATURE, default=20.0): cv.float_,
        cv.Optional(CONF_SOURCES, default=[]): cv.ensure_list(
            cv.Schema(
                {
                    cv.Required(CONF_NAME): cv.string_strict,
                    cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
                }
            )
        ),
    }
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

    for source in config[CONF_SOURCES]:
        sens = await cg.get_variable(source[CONF_SENSOR])
        cg.add(var.add_temperature_source(source[CONF_NAME], sens))