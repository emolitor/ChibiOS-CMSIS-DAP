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
 * USB composite device configuration for CMSIS-DAP probe.
 *
 * Two USB functions:
 *   1. CMSIS-DAP v2 (Vendor class, Bulk EP1)
 *   2. CDC ACM UART bridge (EP2 data, EP3 interrupt)
 */

#ifndef USBCFG_H
#define USBCFG_H

/*===========================================================================*/
/* Endpoint numbers.                                                         */
/*===========================================================================*/

#define DAP_EP                  1U  /* CMSIS-DAP bulk IN/OUT */
#define CDC_DATA_EP             2U  /* CDC data bulk IN/OUT */
#define CDC_INT_EP              3U  /* CDC notification interrupt IN */

/*===========================================================================*/
/* MS OS 2.0 vendor request code.                                            */
/*===========================================================================*/

#define MS_OS_20_VENDOR_CODE    0x01U

/*===========================================================================*/
/* USB event flags (broadcast via evt_usb).                                  */
/*===========================================================================*/

#define EVT_USB_CONFIGURED              (1U << 0)
#define EVT_USB_RESET                   (1U << 1)
#define EVT_USB_SUSPENDED               (1U << 2)
#define EVT_USB_WAKEUP                  (1U << 3)

/*===========================================================================*/
/* Extern declarations.                                                      */
/*===========================================================================*/

extern event_source_t evt_usb;

extern const USBConfig usbcfg;
extern const SerialUSBConfig serusbcfg;
extern SerialUSBDriver SDU1;

/* DAP endpoint callbacks (called from USB ISR). */
void dap_usb_out_cb(USBDriver *usbp, usbep_t ep);
void dap_usb_in_cb(USBDriver *usbp, usbep_t ep);

/* Set USB serial number string descriptor from hex string (max 16 chars). */
void usb_set_serial_string(const char *serial);

/* CDC line coding / DTR accessors (called from UartThread). */
bool usb_linecoding_changed(void);
void usb_get_linecoding(cdc_linecoding_t *lc);
bool usb_dtr_active(void);

#endif /* USBCFG_H */
