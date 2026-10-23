import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_VALUE_TYPE = "value_type"
CONF_SCALE = "scale"
CONF_BITMASK = "bitmask"
CONF_SHIFT = "shift"

EcAdapterTcpSensor = ec_adapter_tcp_ns.class_("EcAdapterTcpSensor", cg.Component, sensor.Sensor)

VALUE_TYPES = {
    "uint8": cg.RawExpression("ec_adapter_tcp::EC_VALUE_UINT8"),
    "int8": cg.RawExpression("ec_adapter_tcp::EC_VALUE_INT8"),
    "uint16": cg.RawExpression("ec_adapter_tcp::EC_VALUE_UINT16"),
    "int16": cg.RawExpression("ec_adapter_tcp::EC_VALUE_INT16"),
    "uint32": cg.RawExpression("ec_adapter_tcp::EC_VALUE_UINT32"),
    "int32": cg.RawExpression("ec_adapter_tcp::EC_VALUE_INT32"),
}

CONFIG_SCHEMA = sensor.sensor_schema(EcAdapterTcpSensor).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Optional(CONF_VALUE_TYPE, default="uint16"): cv.enum(VALUE_TYPES, lower=True),
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        cv.Optional(CONF_BITMASK): cv.hex_uint16_t,
        cv.Optional(CONF_SHIFT, default=0): cv.int_range(min=0, max=15),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)
    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_value_type(config[CONF_VALUE_TYPE]))
    cg.add(var.set_scale(config[CONF_SCALE]))
    if CONF_BITMASK in config:
        cg.add(var.set_bitmask(config[CONF_BITMASK]))
        cg.add(var.set_shift(config[CONF_SHIFT]))