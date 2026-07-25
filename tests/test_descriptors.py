import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "usbcfg.c").read_text(encoding="utf-8")


def test_device_identity_and_bos_contract():
    assert "0x2E8A" in SOURCE
    assert "0x000C" in SOURCE
    assert "0x0201" in SOURCE
    assert "MS_OS_20_DESC_SET_SIZE  178U" in SOURCE
    assert "MS_OS_20_VENDOR_CODE" in SOURCE
    assert "WinUSB" in SOURCE


def test_endpoint_and_interface_contract():
    assert "#define CONFIG_DESC_SIZE    98U" in SOURCE
    assert "USB_DESC_INTERFACE(0x00" in SOURCE
    assert "USB_DESC_INTERFACE(0x01" in SOURCE
    assert "USB_DESC_INTERFACE(0x02" in SOURCE
    assert "USB_DESC_ENDPOINT(DAP_EP, 0x02, 0x0040" in SOURCE
    assert "USB_DESC_ENDPOINT(CDC_DATA_EP, 0x02, 0x0040" in SOURCE
    assert "USB_DESC_ENDPOINT(CDC_INT_EP | 0x80, 0x03" in SOURCE


def test_descriptor_sizes_match_declared_arrays():
    sizes = {
        name: int(size)
        for name, size in re.findall(
            r"static (?:const )?uint8_t (string\d)\[(\d+)\]", SOURCE
        )
    }
    assert sizes.get("string3") == 34
    manufacturer = re.search(
        r"static const uint8_t string1\[\] = \{(.*?)\};", SOURCE, re.DOTALL
    )
    assert manufacturer is not None
    descriptor = manufacturer.group(1)
    assert (
        "USB_DESC_BYTE(26), USB_DESC_BYTE(USB_DESCRIPTOR_STRING)" in descriptor
    )
    characters = re.findall(r"'(.)', 0", descriptor)
    assert "".join(characters) == "Raspberry Pi"
    assert len(characters) * 2 == 24
    assert "'C', 0, 'M', 0, 'S', 0, 'I', 0, 'S', 0" in SOURCE
