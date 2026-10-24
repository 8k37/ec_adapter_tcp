import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@8k37"]
DEPENDENCIES = ["network"]
# entities.h includes the headers of all entity types unconditionally, so every
# one of them must be part of the build even when the YAML only uses a subset.
AUTO_LOAD = ["binary_sensor", "button", "climate", "number", "select", "sensor", "time"]
MULTI_CONF = True

# Local constants: CONF_HOST / CONF_PORT are not exposed by esphome.const in 2026.10.
CONF_HOST = "host"
CONF_PORT = "port"
CONF_UNIT_ID = "unit_id"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_PERSISTENT_CONNECTION = "persistent_connection"
CONF_EC_ADAPTER_TCP_ID = "ec_adapter_tcp_id"

ec_adapter_tcp_ns = cg.esphome_ns.namespace("ec_adapter_tcp")
EcAdapterTcp = ec_adapter_tcp_ns.class_("EcAdapterTcp", cg.PollingComponent)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EcAdapterTcp),
        cv.Required(CONF_HOST): cv.string_strict,
        cv.Optional(CONF_PORT, default=502): cv.port,
        cv.Optional(CONF_UNIT_ID, default=1): cv.int_range(min=1, max=247),
        cv.Optional(CONF_RESPONSE_TIMEOUT, default="2s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_PERSISTENT_CONNECTION, default=False): cv.boolean,
    }
).extend(cv.polling_component_schema("15s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_unit_id(config[CONF_UNIT_ID]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT].total_milliseconds))
    cg.add(var.set_persistent_connection(config[CONF_PERSISTENT_CONNECTION]))