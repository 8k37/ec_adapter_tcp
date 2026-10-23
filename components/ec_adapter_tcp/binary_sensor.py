import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_BITMASK = "bitmask"
CONF_INVERTED = "inverted"

EcAdapterTcpBinarySensor = ec_adapter_tcp_ns.class_(
    "EcAdapterTcpBinarySensor", cg.Component, binary_sensor.BinarySensor
)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(EcAdapterTcpBinarySensor).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Required(CONF_BITMASK): cv.hex_uint16_t,
        cv.Optional(CONF_INVERTED, default=False): cv.boolean,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await binary_sensor.register_binary_sensor(var, config)
    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_bitmask(config[CONF_BITMASK]))
    cg.add(var.set_inverted(config[CONF_INVERTED]))