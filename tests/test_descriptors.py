"""USB descriptor tests.

These validate the bytes the device actually emits: the USB_DESC_* helper
macros in usbcfg.c are expanded to concrete bytes (see descriptor_bytes.py)
and checked for internal consistency — bLength fields, wTotalLength sums,
endpoint/interface layout, and string lengths. This catches descriptor
inconsistencies (e.g. a wrong string bLength) that source-text token matching
misses, without a full ChibiOS host build. A couple of light identity smoke
checks over the source remain.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import descriptor_bytes as db  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "usbcfg.c").read_text(encoding="utf-8")

STRING = db.USB_DESC_TYPES["USB_DESCRIPTOR_STRING"]
CONFIGURATION = db.USB_DESC_TYPES["USB_DESCRIPTOR_CONFIGURATION"]
INTERFACE = db.USB_DESC_TYPES["USB_DESCRIPTOR_INTERFACE"]
ENDPOINT = db.USB_DESC_TYPES["USB_DESCRIPTOR_ENDPOINT"]
IAD = db.USB_DESC_TYPES["USB_DESCRIPTOR_INTERFACE_ASSOCIATION"]


def _u16(desc, off):
    return desc[off] | (desc[off + 1] << 8)


def test_source_identity_smoke():
    # Cheap sanity checks that are awkward to derive from the byte stream.
    assert "WinUSB" in SOURCE
    assert "MS_OS_20_VENDOR_CODE" in SOURCE


def test_device_descriptor_bytes():
    dev = db.descriptor("device_descriptor_data")
    assert len(dev) == 18
    assert dev[0] == 18                       # bLength
    assert dev[1] == db.USB_DESC_TYPES["USB_DESCRIPTOR_DEVICE"]
    assert _u16(dev, 2) == 0x0201             # bcdUSB (2.01 for BOS)
    assert _u16(dev, 8) == 0x2E8A             # idVendor (Raspberry Pi)
    assert _u16(dev, 10) == 0x000C            # idProduct (Debug Probe)
    assert dev[17] == 1                       # bNumConfigurations


def test_configuration_descriptor_bytes():
    cfg = db.descriptor("config_descriptor_data")
    assert cfg[0] == db.SYMBOLS["USB_DESC_CONFIGURATION_SIZE"]
    assert cfg[1] == CONFIGURATION
    # wTotalLength must equal the real array length and the declared constant.
    assert _u16(cfg, 2) == len(cfg) == db.SYMBOLS["CONFIG_DESC_SIZE"]

    kinds = {}
    interfaces, endpoints = [], []
    for off, blen, dtype in db.walk_children(cfg, 0):  # asserts children tile
        kinds[dtype] = kinds.get(dtype, 0) + 1
        if dtype == INTERFACE:
            interfaces.append(cfg[off + 2])            # bInterfaceNumber
        elif dtype == ENDPOINT:
            endpoints.append((cfg[off + 2], cfg[off + 3], _u16(cfg, off + 4)))

    assert kinds[CONFIGURATION] == 1
    assert kinds[INTERFACE] == 3               # DAP + CDC control + CDC data
    assert kinds[IAD] == 1
    assert cfg[4] == len(interfaces) == 3      # bNumInterfaces
    assert interfaces == [0x00, 0x01, 0x02]

    # Exactly the five expected endpoints, no duplicates (checked before the
    # dict() below would silently collapse any).
    dap, cdc_data, cdc_int = (
        db.SYMBOLS["DAP_EP"],
        db.SYMBOLS["CDC_DATA_EP"],
        db.SYMBOLS["CDC_INT_EP"],
    )
    addresses = [addr for addr, _attr, _mps in endpoints]
    assert len(addresses) == len(set(addresses)) == 5
    assert set(addresses) == {
        dap, dap | 0x80, cdc_data, cdc_data | 0x80, cdc_int | 0x80,
    }
    eps = dict((addr, (attr, mps)) for addr, attr, mps in endpoints)
    assert eps[dap] == (0x02, 0x40)            # bulk OUT 64
    assert eps[dap | 0x80] == (0x02, 0x40)     # bulk IN 64
    assert eps[cdc_data] == (0x02, 0x40)       # bulk OUT
    assert eps[cdc_data | 0x80] == (0x02, 0x40)  # bulk IN
    assert eps[cdc_int | 0x80][0] == 0x03      # interrupt IN


def test_bos_and_ms_os_20_contract():
    bos = db.descriptor("bos_descriptor_data")
    assert bos[0] == 5                         # bLength of the BOS header
    assert bos[1] == 0x0F                      # bDescriptorType == BOS
    assert _u16(bos, 2) == len(bos)            # wTotalLength == emitted length
    assert bos[4] == 1                         # bNumDeviceCaps

    # The single device capability is the MS OS 2.0 platform capability. Its
    # advertised set length must match the actual descriptor set, or Windows
    # WinUSB binding breaks.
    cap = bos[5:5 + bos[5]]
    assert cap[1] == 0x10 and cap[2] == 0x05   # DEVICE_CAPABILITY / PLATFORM
    ms_os_total = cap[24] | (cap[25] << 8)     # wMSOSDescriptorSetTotalLength
    ms = db.descriptor("ms_os_20_descriptor_set")
    assert (
        len(ms)
        == ms_os_total
        == db.SYMBOLS["MS_OS_20_DESC_SET_SIZE"]
        == _u16(ms, 8)                         # SET_HEADER wTotalLength
    )


def test_string_descriptors_bytes():
    expected_text = {
        "string1": "Raspberry Pi",
        "string2": "ChibiOS Probe (CMSIS-DAP)",
        "string4": "CMSIS-DAP v2",
    }
    for name in ("string0", "string1", "string2", "string3", "string4"):
        s = db.descriptor(name)
        assert s[0] == len(s), f"{name}: bLength {s[0]} != emitted {len(s)}"
        assert s[1] == STRING, f"{name}: type {s[1]:#x} != STRING"
        assert s[0] % 2 == 0                    # 2-byte header + UTF-16 units
    for name, text in expected_text.items():
        # Decode the whole payload as UTF-16LE (not just the low bytes) so a
        # corrupted high byte is caught rather than silently discarded.
        chars = bytes(db.descriptor(name)[2:]).decode("utf-16-le")
        assert chars == text, f"{name}: {chars!r} != {text!r}"
    # OpenOCD auto-detects the probe by the "CMSIS-DAP" product string.
    assert "CMSIS-DAP" in bytes(db.descriptor("string2")[2:]).decode("utf-16-le")

    # String 3 holds the serial number; usb_set_serial_string() writes a
    # 16-character serial through byte 33, so the array must be exactly 34
    # bytes regardless of its initializer, or the boot-time write overruns it.
    assert db.declared_size("string3") == 34
    assert db.descriptor("string3")[0] == 34
