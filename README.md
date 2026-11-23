WeatherClock2.ino draws its weather data from Open-Meteo.com and does not require a key. The sketch includes an attribution banner crediting the weather data source as required by the free license agreement.

See sketch for required libraries

Required Arduino IDE settings:

Set Tools > Partition Scheme to "Huge APP (3MB no OTA/1MB SPIFFS)"

lv_conf.h is required: (Windows users should find it here: C:\Users\{username}\Documents\Arduino\libraries\lvgl\src\)

The following font sizes are to be enabled in this file:

#define LV\_FONT\_MONTSERRAT\_14 1

#define LV\_FONT\_MONTSERRAT\_24 1

#define LV\_FONT\_MONTSERRAT\_28 1

#define LV\_FONT\_MONTSERRAT\_34 1

#define LV\_FONT\_MONTSERRAT\_48 1

This sketch is provided for personal/hobby use only, not for commercial purposes.

A 3D printable case along with the hardware requirements can be found here: https://www.thingiverse.com/thing:7436303

