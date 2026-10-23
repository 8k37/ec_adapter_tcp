import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_VALUE = "value"

EcAdapterTcpButton = ec_adapter_tcp_ns.class_(
    "EcAdapterTcpButton", cg.Component, button.Button
)

CONFIG_SCHEMA = button.button_schema(EcAdapterTcpButton).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Required(CONF_VALUE): cv.hex_uint16_t,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await button.register_button(var, config)
    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_value(config[CONF_VALUE]))