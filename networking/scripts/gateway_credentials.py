import hashlib
import json
from pathlib import Path
import re
from tempfile import NamedTemporaryFile

Import("env")

device_id = env.GetProjectOption("custom_device_id", "")
end_devices = env.GetProjectOption("custom_end_devices", "").split()
device_pattern = re.compile(r"custom-([1-9][0-9]{0,2})")
device_tags = {}
for registered_id in end_devices:
    match = device_pattern.fullmatch(registered_id)
    if not match or int(match[1]) > 255 or registered_id in device_tags:
        raise RuntimeError("List unique IDs from custom-1 through custom-255 in custom_end_devices.")
    device_tags[registered_id] = int(match[1])

if device_id not in device_tags:
    raise RuntimeError("Set custom_device_id to this board's ID from custom_end_devices.")

credentials_path = Path(env.subst("$PROJECT_DIR")).parent / "end devices" / f"{device_id}.json"
try:
    credentials = json.loads(credentials_path.read_text(encoding="utf-8"))
except (OSError, ValueError):
    raise RuntimeError(f"Cannot read LoRaWAN credentials from {credentials_path}") from None

if not isinstance(credentials, dict) or credentials.get("device_id") != device_id:
    raise RuntimeError(f"device_id in {credentials_path} must match custom_device_id.")


def read_hex(field, size):
    value = credentials.get(field)
    if not isinstance(value, str) or not re.fullmatch(rf"[0-9a-fA-F]{{{size * 2}}}", value):
        raise RuntimeError(f"{field} in {credentials_path} must contain {size * 2} hex digits")
    return value.upper()


def cpp_bytes(value):
    return ", ".join(f"0x{byte:02X}" for byte in bytes.fromhex(value))


def read_receive_option(name, minimum, maximum):
    value = env.GetProjectOption(name, "")
    if not re.fullmatch(r"[0-9]{1,2}", value) or not minimum <= int(value) <= maximum:
        raise RuntimeError(f"Set {name} in platformio.ini to a value from {minimum} to {maximum}.")
    return int(value)


address = read_hex("device_address", 4)
network_key = read_hex("nwk_s_key", 16)
application_key = read_hex("app_s_key", 16)
session_id = hashlib.sha256(bytes.fromhex(address + network_key + application_key)).hexdigest()[:16]

initial_rx1_delay = read_receive_option("custom_initial_rx1_delay", 1, 15)
initial_rx2_data_rate = read_receive_option("custom_initial_rx2_data_rate", 0, 7)
device_list = "".join(f'    {{"{name}", 0x{tag:02X}}},\n' for name, tag in device_tags.items())

build_dir = Path(env.subst("$BUILD_DIR"))
build_dir.mkdir(parents=True, exist_ok=True)
header = build_dir / "gateway_credentials.h"
contents = (
    "#pragma once\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n\n"
    f"constexpr uint32_t deviceAddress = 0x{address}UL;\n"
    f"constexpr uint8_t networkSessionKey[] = {{{cpp_bytes(network_key)}}};\n"
    f"constexpr uint8_t applicationSessionKey[] = {{{cpp_bytes(application_key)}}};\n"
    f'constexpr char sessionPath[] = "/ttn-{session_id}.bin";\n'
    "struct EndDevice { const char* id; uint8_t tag; };\n"
    "constexpr EndDevice endDevices[] = {\n"
    f"{device_list}"
    "};\n"
    f"constexpr size_t localDeviceIndex = {end_devices.index(device_id)};\n"
    "constexpr const char* deviceId = endDevices[localDeviceIndex].id;\n"
    f"constexpr uint8_t initialRx1DelaySeconds = {initial_rx1_delay};\n"
    f"constexpr uint8_t initialRx2DataRate = {initial_rx2_data_rate};\n"
)
if not header.exists() or header.read_text(encoding="utf-8") != contents:
    with NamedTemporaryFile(mode="w", dir=build_dir, encoding="utf-8", delete=False) as output:
        output.write(contents)
    Path(output.name).replace(header)

env.Append(CPPPATH=[str(build_dir)])
