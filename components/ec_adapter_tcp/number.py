import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_MAX_VALUE, CONF_MIN_VALUE, CONF_STEP

from . import CONF_EC_ADAPTER_TCP_ID, EcAdapterTcp, ec_adapter_tcp_ns

DEPENDENCIES = ["ec_adapter_tcp"]

CONF_ADDRESS = "address"
CONF_SCALE = "scale"
CONF_INITIAL_VALUE = "initial_value"

EcAdapterTcpNumber = ec_adapter_tcp_ns.class_("EcAdapterTcpNumber", cg.Component, number.Number)

CONFIG_SCHEMA = number.number_schema(EcAdapterTcpNumber).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Optional(CONF_MIN_VALUE, default=0.0): cv.float_,
        cv.Optional(CONF_MAX_VALUE, default=100.0): cv.float_,
        cv.Optional(CONF_STEP, default=1.0): cv.float_,
        cv.Optional(CONF_SCALE, default=1.0): cv.float_,
        # Published and pushed to the adapter when nothing has been written from
        # this device yet (empty flash). The register is write-only, so this is
        # the only way to give it a defined state on a fresh flash.
        cv.Optional(CONF_INITIAL_VALUE): cv.float_,
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EC_ADAPTER_TCP_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await number.register_number(
        var,
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    cg.add(var.set_hub(hub))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_scale(config[CONF_SCALE]))
    cg.add(var.set_range(config[CONF_MIN_VALUE], config[CONF_MAX_VALUE], config[CONF_STEP]))
    if CONF_INITIAL_VALUE in config:
        initial = config[CONF_INITIAL_VALUE]
        if initial < config[CONF_MIN_VALUE] or initial > config[CONF_MAX_VALUE]:
            raise cv.Invalid(
                f"initial_value {initial} is outside the range "
                f"{config[CONF_MIN_VALUE]}..{config[CONF_MAX_VALUE]}"
            )
        cg.add(var.set_initial_value(initial))