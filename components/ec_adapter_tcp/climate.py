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

# ectoControl register map (see custom_components/ectocontrol_adapter/registers.py
# in sokolovs/ec-adapter).
WORK_MODE_REGISTER = 0x0039
CONNECT_TYPE_REGISTER = 0x0030
COOLANT_SETPOINT_REGISTER = 0x0031

# REG_W_MODE choices.
WORK_MODE_VALUES = {
    0: "disabled",
    1: "heating_only",
    2: "dwh_only",
    3: "heating_dwh",
    4: "second_only",
    5: "heating_second",
}


def _validate_work_mode_address(value: int) -> int:
    if value == CONNECT_TYPE_REGISTER:
        raise cv.Invalid(
            "0x0030 is the Connect Type register (REG_W_CONNECT_TYPE: "
            "0=adapter, 1=boiler_panel), not the Work Mode register. A climate "
            "pointed at 0x0030 switches the temperature source instead of "
            "enabling heating (symptom: 'Adapter' = HEAT, 'Boiler Panel' = OFF, "
            "and the Connect Type select moves when you toggle the climate). "
            "Use address: 0x0039 (REG_W_MODE: 0=disabled, 1=heating_only, "
            "2=dwh_only, 3=heating_dwh, 4=second_only, 5=heating_second) and "
            "leave 0x0030 to the Connect Type select."
        )
    if value == COOLANT_SETPOINT_REGISTER:
        raise cv.Invalid(
            "0x0031 is the Coolant setpoint register, not the Work Mode "
            "register. Use address: 0x0039 for work mode and "
            "temperature_address: 0x0031 for the setpoint."
        )
    return value


def _validate_setpoint_address(value: int) -> int:
    if value == CONNECT_TYPE_REGISTER:
        raise cv.Invalid(
            "0x0030 is the Connect Type register, not the coolant setpoint. "
            "The coolant setpoint is 0x0031 (scale x10)."
        )
    if value == WORK_MODE_REGISTER:
        raise cv.Invalid(
            "0x0039 is the Work Mode register, not the coolant setpoint. "
            "The coolant setpoint is 0x0031 (scale x10)."
        )
    return value

EcAdapterTcpClimate = ec_adapter_tcp_ns.class_(
    "EcAdapterTcpClimate", cg.Component, climate.Climate
)

CONFIG_SCHEMA = climate.climate_schema(EcAdapterTcpClimate).extend(
    {
        cv.GenerateID(CONF_EC_ADAPTER_TCP_ID): cv.use_id(EcAdapterTcp),
        # Work mode register (0x0039): 0=disabled, 1=heating_only, 2=dwh_only,
        # 3=heating_dwh, 4=second_only, 5=heating_second. The value read back is
        # not always meaningful (many adapters are write-only here), so the
        # climate entity only trusts a readback of 0..5 and otherwise keeps the
        # state it set after a successful write.
        cv.Optional(CONF_ADDRESS, default=WORK_MODE_REGISTER): cv.All(
            cv.hex_uint16_t, _validate_work_mode_address
        ),
        # Coolant setpoint register, scaled by 10 (0x0031).
        cv.Optional(CONF_TEMPERATURE_ADDRESS, default=COOLANT_SETPOINT_REGISTER): cv.All(
            cv.hex_uint16_t, _validate_setpoint_address
        ),
        cv.Optional(CONF_TEMPERATURE_SCALE, default=10.0): cv.float_,
        cv.Optional(CONF_ON_VALUE, default=1): cv.int_range(min=0, max=0xFFFF),
        cv.Optional(CONF_OFF_VALUE, default=0): cv.int_range(min=0, max=0xFFFF),
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