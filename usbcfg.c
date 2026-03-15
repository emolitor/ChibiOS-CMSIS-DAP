/*
 * Copyright (C) 2026 Eric Molitor <github.com/emolitor>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * USB composite device configuration for CMSIS-DAP v2 + CDC ACM.
 *
 * Descriptor layout:
 *   Interface 0: CMSIS-DAP v2 (Vendor class, Bulk EP1 IN/OUT)
 *   Interface 1: CDC Control (EP3 IN interrupt)
 *   Interface 2: CDC Data (EP2 IN/OUT bulk)
 *
 * Includes BOS descriptor with MS OS 2.0 Platform Capability for
 * automatic WinUSB driver binding on Windows.
 */

#include "hal.h"
#include "usbcfg.h"

/*===========================================================================*/
/* Forward declarations for callbacks defined in main.c.                     */
/*===========================================================================*/

extern void dap_usb_out_cb(USBDriver *usbp, usbep_t ep);
extern void dap_usb_in_cb(USBDriver *usbp, usbep_t ep);

SerialUSBDriver SDU1;

/*===========================================================================*/
/* USB Device Descriptor.                                                    */
/*===========================================================================*/

static const uint8_t device_descriptor_data[] = {
  USB_DESC_DEVICE(
    0x0201,          /* bcdUSB (2.01 for BOS) */
    0xEF,            /* bDeviceClass (Misc — IAD) */
    0x02,            /* bDeviceSubClass (Common) */
    0x01,            /* bDeviceProtocol (IAD) */
    0x40,            /* bMaxPacketSize (64) */
    0x2E8A,          /* idVendor (Raspberry Pi) */
    0x000C,          /* idProduct (Debug Probe) */
    0x0220,          /* bcdDevice (>= 2.2.0 disables OpenOCD quirk_mode) */
    1,               /* iManufacturer */
    2,               /* iProduct */
    3,               /* iSerialNumber */
    1)               /* bNumConfigurations */
};

static const USBDescriptor device_descriptor = {
  sizeof device_descriptor_data,
  device_descriptor_data
};

/*===========================================================================*/
/* Configuration Descriptor.                                                 */
/*===========================================================================*/

/*
 * Total length:
 *   Config(9) +
 *   IF0(9) + EP1_OUT(7) + EP1_IN(7) = 23 (CMSIS-DAP) +
 *   IAD(8) +
 *   IF1(9) + CDC_Header(5) + CDC_CallMgmt(5) + CDC_ACM(4) + CDC_Union(5) + EP3_IN(7) = 35 +
 *   IF2(9) + EP2_OUT(7) + EP2_IN(7) = 23 (CDC Data)
 *   = 9 + 23 + 8 + 35 + 23 = 98
 */
#define CONFIG_DESC_SIZE    98U

static const uint8_t config_descriptor_data[] = {
  /* Configuration Descriptor. */
  USB_DESC_CONFIGURATION(CONFIG_DESC_SIZE, 3, 1, 0, 0xC0, 50),

  /* === Interface 0: CMSIS-DAP v2 (Vendor class) === */
  USB_DESC_INTERFACE(0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 4),
  /* EP1 OUT (Bulk). */
  USB_DESC_ENDPOINT(DAP_EP, 0x02, 0x0040, 0x00),
  /* EP1 IN (Bulk). */
  USB_DESC_ENDPOINT(DAP_EP | 0x80, 0x02, 0x0040, 0x00),

  /* === IAD for CDC (interfaces 1-2) === */
  USB_DESC_INTERFACE_ASSOCIATION(0x01, 0x02, 0x02, 0x02, 0x00, 0),

  /* === Interface 1: CDC Control === */
  USB_DESC_INTERFACE(0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0),
  /* CDC Header Functional Descriptor. */
  USB_DESC_BYTE(5), USB_DESC_BYTE(0x24), USB_DESC_BYTE(0x00),
  USB_DESC_BCD(0x0110),
  /* CDC Call Management Functional Descriptor. */
  USB_DESC_BYTE(5), USB_DESC_BYTE(0x24), USB_DESC_BYTE(0x01),
  USB_DESC_BYTE(0x00), USB_DESC_BYTE(0x02),
  /* CDC ACM Functional Descriptor. */
  USB_DESC_BYTE(4), USB_DESC_BYTE(0x24), USB_DESC_BYTE(0x02),
  USB_DESC_BYTE(0x02),
  /* CDC Union Functional Descriptor. */
  USB_DESC_BYTE(5), USB_DESC_BYTE(0x24), USB_DESC_BYTE(0x06),
  USB_DESC_BYTE(0x01), USB_DESC_BYTE(0x02),
  /* EP3 IN (Interrupt). */
  USB_DESC_ENDPOINT(CDC_INT_EP | 0x80, 0x03, 0x0008, 0xFF),

  /* === Interface 2: CDC Data === */
  USB_DESC_INTERFACE(0x02, 0x00, 0x02, 0x0A, 0x00, 0x00, 0),
  /* EP2 OUT (Bulk). */
  USB_DESC_ENDPOINT(CDC_DATA_EP, 0x02, 0x0040, 0x00),
  /* EP2 IN (Bulk). */
  USB_DESC_ENDPOINT(CDC_DATA_EP | 0x80, 0x02, 0x0040, 0x00)
};

static const USBDescriptor config_descriptor = {
  sizeof config_descriptor_data,
  config_descriptor_data
};

/*===========================================================================*/
/* BOS Descriptor with MS OS 2.0 Platform Capability.                        */
/*===========================================================================*/

/*
 * MS OS 2.0 descriptor set length:
 *   Header(10) + Config subset header(8) + Function subset header(8) +
 *   Compatible ID(20) + Registry Property(132) = 178
 */
#define MS_OS_20_DESC_SET_SIZE  178U

static const uint8_t bos_descriptor_data[] = {
  /* BOS Descriptor. */
  USB_DESC_BYTE(5),                     /* bLength */
  USB_DESC_BYTE(15),                    /* bDescriptorType (BOS) */
  USB_DESC_WORD(5 + 28),               /* wTotalLength (BOS + Platform Cap) */
  USB_DESC_BYTE(1),                     /* bNumDeviceCaps */

  /* MS OS 2.0 Platform Capability Descriptor.
   * bLength(1) + bDescriptorType(1) + bDevCapabilityType(1) + bReserved(1)
   * + UUID(16) + dwWindowsVersion(4) + wTotalLength(2)
   * + bMS_VendorCode(1) + bAltEnumCode(1) = 28 bytes. */
  USB_DESC_BYTE(28),                    /* bLength */
  USB_DESC_BYTE(16),                    /* bDescriptorType (DEVICE_CAPABILITY) */
  USB_DESC_BYTE(5),                     /* bDevCapabilityType (PLATFORM) */
  USB_DESC_BYTE(0),                     /* bReserved */
  /* PlatformCapabilityUUID: {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} (MS OS 2.0) */
  0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
  0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
  /* MS OS 2.0 descriptor information. */
  0x00, 0x00, 0x03, 0x06,              /* dwWindowsVersion (0x06030000 LE = Win 8.1+) */
  USB_DESC_WORD(MS_OS_20_DESC_SET_SIZE), /* wMSOSDescriptorSetTotalLength */
  USB_DESC_BYTE(MS_OS_20_VENDOR_CODE),  /* bMS_VendorCode */
  USB_DESC_BYTE(0)                      /* bAltEnumCode */
};

static const USBDescriptor bos_descriptor = {
  sizeof bos_descriptor_data,
  bos_descriptor_data
};

/*===========================================================================*/
/* MS OS 2.0 Descriptor Set.                                                 */
/*===========================================================================*/

/* DeviceInterfaceGUID for WinUSB: {CDB3B5AD-293B-4663-AA36-1AAE46463776} */
static const uint8_t ms_os_20_descriptor_set[] = {
  /* MS OS 2.0 Descriptor Set Header (10 bytes). */
  USB_DESC_WORD(10),                    /* wLength */
  USB_DESC_WORD(0x0000),                /* wDescriptorType (SET_HEADER) */
  0x00, 0x00, 0x03, 0x06,              /* dwWindowsVersion (0x06030000 LE = Win 8.1+) */
  USB_DESC_WORD(MS_OS_20_DESC_SET_SIZE), /* wTotalLength */

  /* MS OS 2.0 Configuration Subset Header (8 bytes). */
  USB_DESC_WORD(8),                     /* wLength */
  USB_DESC_WORD(0x0001),                /* wDescriptorType (SUBSET_HEADER_CONFIGURATION) */
  USB_DESC_BYTE(0),                     /* bConfigurationValue */
  USB_DESC_BYTE(0),                     /* bReserved */
  USB_DESC_WORD(8 + 8 + 20 + 132),     /* wTotalLength (config subset contents) */

  /* MS OS 2.0 Function Subset Header (for interface 0, 8 bytes). */
  USB_DESC_WORD(8),                     /* wLength */
  USB_DESC_WORD(0x0002),                /* wDescriptorType (SUBSET_HEADER_FUNCTION) */
  USB_DESC_BYTE(0),                     /* bFirstInterface */
  USB_DESC_BYTE(0),                     /* bReserved */
  USB_DESC_WORD(8 + 20 + 132),         /* wSubsetLength */

  /* MS OS 2.0 Compatible ID Descriptor (WinUSB). */
  USB_DESC_WORD(20),                    /* wLength */
  USB_DESC_WORD(0x0003),                /* wDescriptorType (COMPATIBLE_ID) */
  'W', 'I', 'N', 'U', 'S', 'B', 0, 0, /* CompatibleID */
  0, 0, 0, 0, 0, 0, 0, 0,             /* SubCompatibleID */

  /* MS OS 2.0 Registry Property Descriptor (DeviceInterfaceGUID). */
  USB_DESC_WORD(132),                   /* wLength */
  USB_DESC_WORD(0x0004),                /* wDescriptorType (REG_PROPERTY) */
  USB_DESC_WORD(0x0007),                /* wPropertyDataType (REG_MULTI_SZ) */
  USB_DESC_WORD(42),                    /* wPropertyNameLength */
  /* "DeviceInterfaceGUIDs\0" in UTF-16LE (21 chars * 2 = 42 bytes). */
  'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
  'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
  'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
  USB_DESC_WORD(80),                    /* wPropertyDataLength */
  /* "{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0" in UTF-16LE. */
  '{', 0, 'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0,
  'D', 0, '-', 0, '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0, '4', 0,
  '6', 0, '6', 0, '3', 0, '-', 0, 'A', 0, 'A', 0, '3', 0, '6', 0,
  '-', 0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0,
  '6', 0, '3', 0, '7', 0, '7', 0, '6', 0, '}', 0, 0, 0, 0, 0
};

/*===========================================================================*/
/* USB Strings.                                                              */
/*===========================================================================*/

/* String 0: Language ID. */
static const uint8_t string0[] = {
  USB_DESC_BYTE(4), USB_DESC_BYTE(USB_DESCRIPTOR_STRING),
  USB_DESC_WORD(0x0409)
};

/* String 1: Manufacturer. */
static const uint8_t string1[] = {
  USB_DESC_BYTE(30), USB_DESC_BYTE(USB_DESCRIPTOR_STRING),
  'R', 0, 'a', 0, 's', 0, 'p', 0, 'b', 0, 'e', 0, 'r', 0, 'r', 0,
  'y', 0, ' ', 0, 'P', 0, 'i', 0, ' ', 0, ' ', 0
};

/* String 2: Product (must contain "CMSIS-DAP" for OpenOCD auto-detect). */
static const uint8_t string2[] = {
  USB_DESC_BYTE(52), USB_DESC_BYTE(USB_DESCRIPTOR_STRING),
  'C', 0, 'h', 0, 'i', 0, 'b', 0, 'i', 0, 'O', 0, 'S', 0, ' ', 0,
  'P', 0, 'r', 0, 'o', 0, 'b', 0, 'e', 0, ' ', 0, '(', 0, 'C', 0,
  'M', 0, 'S', 0, 'I', 0, 'S', 0, '-', 0, 'D', 0, 'A', 0, 'P', 0,
  ')', 0
};

/* String 3: Serial Number (mutable, populated at boot from flash unique ID). */
static uint8_t string3[34] = {
  34, 0x03, /* bLength, bDescriptorType (STRING) */
  '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0,
  '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0
};

/* String 4: CMSIS-DAP interface name (must contain "CMSIS-DAP"). */
static const uint8_t string4[] = {
  USB_DESC_BYTE(26), USB_DESC_BYTE(USB_DESCRIPTOR_STRING),
  'C', 0, 'M', 0, 'S', 0, 'I', 0, 'S', 0, '-', 0, 'D', 0, 'A', 0,
  'P', 0, ' ', 0, 'v', 0, '2', 0
};

static const USBDescriptor strings[] = {
  {sizeof string0, string0},
  {sizeof string1, string1},
  {sizeof string2, string2},
  {sizeof string3, string3},
  {sizeof string4, string4}
};

void usb_set_serial_string(const char *serial) {
  uint32_t i;

  for (i = 0U; i < 16U && serial[i] != '\0'; i++) {
    string3[2U + i * 2U] = (uint8_t)serial[i];
    string3[3U + i * 2U] = 0U;
  }
}

/*===========================================================================*/
/* Descriptor callback.                                                      */
/*===========================================================================*/

static const USBDescriptor *get_descriptor(USBDriver *usbp,
                                           uint8_t dtype,
                                           uint8_t dindex,
                                           uint16_t lang) {
  (void)usbp;
  (void)lang;

  switch (dtype) {
  case USB_DESCRIPTOR_DEVICE:
    return &device_descriptor;
  case USB_DESCRIPTOR_CONFIGURATION:
    return &config_descriptor;
  case USB_DESCRIPTOR_STRING:
    if (dindex < 5U)
      return &strings[dindex];
    break;
  case 15U:  /* BOS descriptor type */
    return &bos_descriptor;
  }
  return NULL;
}

/*===========================================================================*/
/* Endpoint configurations.                                                  */
/*===========================================================================*/

/* EP1: CMSIS-DAP bulk. */
static USBInEndpointState  dap_ep_in_state;
static USBOutEndpointState dap_ep_out_state;

static const USBEndpointConfig dap_ep_config = {
  .ep_mode    = USB_EP_MODE_TYPE_BULK,
  .setup_cb   = NULL,
  .in_cb      = dap_usb_in_cb,
  .out_cb     = dap_usb_out_cb,
  .in_maxsize = 0x0040,
  .out_maxsize= 0x0040,
  .in_state   = &dap_ep_in_state,
  .out_state  = &dap_ep_out_state
};

/* EP2: CDC data bulk. */
static USBInEndpointState  cdc_data_in_state;
static USBOutEndpointState cdc_data_out_state;

static const USBEndpointConfig cdc_data_ep_config = {
  .ep_mode    = USB_EP_MODE_TYPE_BULK,
  .setup_cb   = NULL,
  .in_cb      = sduDataTransmitted,
  .out_cb     = sduDataReceived,
  .in_maxsize = 0x0040,
  .out_maxsize= 0x0040,
  .in_state   = &cdc_data_in_state,
  .out_state  = &cdc_data_out_state
};

/* EP3: CDC interrupt. */
static USBInEndpointState cdc_int_in_state;

static const USBEndpointConfig cdc_int_ep_config = {
  .ep_mode    = USB_EP_MODE_TYPE_INTR,
  .setup_cb   = NULL,
  .in_cb      = sduInterruptTransmitted,
  .out_cb     = NULL,
  .in_maxsize = 0x0008,
  .out_maxsize= 0x0000,
  .in_state   = &cdc_int_in_state,
  .out_state  = NULL
};

/*===========================================================================*/
/* USB event handler.                                                        */
/*===========================================================================*/

static void usb_event(USBDriver *usbp, usbevent_t event) {
  switch (event) {
  case USB_EVENT_ADDRESS:
    return;
  case USB_EVENT_CONFIGURED:
    chSysLockFromISR();
    usbInitEndpointI(usbp, DAP_EP, &dap_ep_config);
    usbInitEndpointI(usbp, CDC_DATA_EP, &cdc_data_ep_config);
    usbInitEndpointI(usbp, CDC_INT_EP, &cdc_int_ep_config);
    sduConfigureHookI(&SDU1);
    chEvtBroadcastFlagsI(&evt_usb, EVT_USB_CONFIGURED);
    chSysUnlockFromISR();
    return;
  case USB_EVENT_RESET:
  case USB_EVENT_UNCONFIGURED:
    chSysLockFromISR();
    sduSuspendHookI(&SDU1);
    chEvtBroadcastFlagsI(&evt_usb, EVT_USB_RESET);
    chSysUnlockFromISR();
    return;
  case USB_EVENT_SUSPEND:
    chSysLockFromISR();
    sduSuspendHookI(&SDU1);
    chEvtBroadcastFlagsI(&evt_usb, EVT_USB_SUSPENDED);
    chSysUnlockFromISR();
    return;
  case USB_EVENT_WAKEUP:
    chSysLockFromISR();
    sduWakeupHookI(&SDU1);
    chEvtBroadcastFlagsI(&evt_usb, EVT_USB_WAKEUP);
    chSysUnlockFromISR();
    return;
  case USB_EVENT_STALLED:
    return;
  }
}

/*===========================================================================*/
/* SOF handler.                                                              */
/*===========================================================================*/

static void sof_handler(USBDriver *usbp) {
  (void)usbp;
  osalSysLockFromISR();
  sduSOFHookI(&SDU1);
  osalSysUnlockFromISR();
}

/*===========================================================================*/
/* CDC line coding / control line state interception.                         */
/*===========================================================================*/

static cdc_linecoding_t uart_linecoding = {
  {0x00, 0xC2, 0x01, 0x00},  /* 115200 baud (little-endian) */
  LC_STOP_1, LC_PARITY_NONE, 8
};

static volatile bool uart_dtr;
static volatile bool uart_linecoding_changed;

static void set_linecoding_cb(USBDriver *usbp) {
  (void)usbp;

  if (uart_dtr)
    uart_linecoding_changed = true;
}

/*===========================================================================*/
/* Requests hook — handles MS OS 2.0 vendor requests + CDC.                  */
/*===========================================================================*/

static bool requests_hook(USBDriver *usbp) {
  /* Check for MS OS 2.0 vendor request. */
  if (((usbp->setup[0] & USB_RTYPE_TYPE_MASK) == USB_RTYPE_TYPE_VENDOR) &&
      (usbp->setup[1] == MS_OS_20_VENDOR_CODE)) {
    uint16_t wIndex = (uint16_t)usbp->setup[4] | ((uint16_t)usbp->setup[5] << 8);

    if (wIndex == 0x0007U) {
      /* MS OS 2.0 Descriptor Set request. */
      usbSetupTransfer(usbp, (uint8_t *)ms_os_20_descriptor_set,
                        sizeof ms_os_20_descriptor_set, NULL);
      return true;
    }
    return false;
  }

  /* Intercept CDC class requests. */
  if ((usbp->setup[0] & USB_RTYPE_TYPE_MASK) == USB_RTYPE_TYPE_CLASS) {
    switch (usbp->setup[1]) {
    case CDC_SET_LINE_CODING:
      usbSetupTransfer(usbp, (uint8_t *)&uart_linecoding,
                        sizeof(uart_linecoding), set_linecoding_cb);
      return true;
    case CDC_GET_LINE_CODING:
      usbSetupTransfer(usbp, (uint8_t *)&uart_linecoding,
                        sizeof(uart_linecoding), NULL);
      return true;
    case CDC_SET_CONTROL_LINE_STATE: {
      uint16_t wValue = (uint16_t)usbp->setup[2] |
                        ((uint16_t)usbp->setup[3] << 8);
      uart_dtr = (wValue & 1U) != 0U;
      usbSetupTransfer(usbp, NULL, 0, NULL);
      return true;
    }
    default:
      break;
    }
  }

  /* Delegate remaining requests to Serial USB driver. */
  return sduRequestsHook(usbp);
}

/*===========================================================================*/
/* CDC line coding / DTR accessors for UartThread.                           */
/*===========================================================================*/

bool usb_linecoding_changed(void) {
  chSysLock();
  bool changed = uart_linecoding_changed;
  uart_linecoding_changed = false;
  chSysUnlock();
  return changed;
}

void usb_get_linecoding(cdc_linecoding_t *lc) {
  chSysLock();
  *lc = uart_linecoding;
  chSysUnlock();
}

bool usb_dtr_active(void) {
  return uart_dtr;
}

/*===========================================================================*/
/* USB driver configuration.                                                 */
/*===========================================================================*/

const USBConfig usbcfg = {
  usb_event,
  get_descriptor,
  requests_hook,
  sof_handler
};

/*===========================================================================*/
/* Serial over USB driver configuration (CDC on EP2/EP3).                    */
/*===========================================================================*/

const SerialUSBConfig serusbcfg = {
  .usbp     = &USBD1,
  .bulk_in  = CDC_DATA_EP,
  .bulk_out = CDC_DATA_EP,
  .int_in   = CDC_INT_EP
};
