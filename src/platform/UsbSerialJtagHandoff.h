#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart. No-op on targets
// without USB Drive (ESP32-S3 boards built with FREEINK_CAP_USB_MSC).
void handoffUsbOtgToSerialJtag();
