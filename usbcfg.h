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
/* Extern declarations.                                                      */
/*===========================================================================*/

extern const USBConfig usbcfg;
extern const SerialUSBConfig serusbcfg;
extern SerialUSBDriver SDU1;

/* DAP endpoint callbacks (called from USB ISR). */
void dap_usb_out_cb(USBDriver *usbp, usbep_t ep);
void dap_usb_in_cb(USBDriver *usbp, usbep_t ep);

/* Set USB serial number string descriptor from hex string (max 16 chars). */
void usb_set_serial_string(const char *serial);

#endif /* USBCFG_H */
