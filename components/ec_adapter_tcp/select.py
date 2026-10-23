import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_OPTIONS = "options"
CONF_INITIAL_VALUE = "initial_value"

EcAdapterTcpSelect = ec_adapter_tcp_ns.class_("EcAdapterTcpSelect", cg.Component, select.Select)

CONFIG_SCHEMA = select.select_schema(EcAdapterTcpSelect).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Required(CONF_OPTIONS): cv.Schema({cv.string: cv.int_range(min=0, max=0xFFFF)}),
        # Option published on boot when nothing has been written yet. The
        # register is write-only, so its real value cannot be read back; the
        # last value written from this device is persisted in flash instead.
        cv.Optional(CONF_INITIAL_VALUE): cv.string_strict,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await select.register_select(
        var,
        config,
        options=list(config[CONF_OPTIONS].keys()),
    )
    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    for name, value in config[CONF_OPTIONS].items():
        cg.add(var.add_option(name, value))
    if CONF_INITIAL_VALUE in config:
        initial = config[CONF_INITIAL_VALUE]
        if initial not in config[CONF_OPTIONS]:
            raise cv.Invalid(
                f"initial_value '{initial}' is not one of the options "
                f"({', '.join(config[CONF_OPTIONS])})"
            )
        cg.add(var.set_initial_option(initial))