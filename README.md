WeatherClock2.ino draws its weather data from Open-Meteo.com and does not require a key. The sketch includes an attribution banner crediting the weather data source as required by the free license agreement.

Required Arduino IDE settings:

Set Tools > Partition Scheme to "Huge APP (3MB no OTA/1MB SPIFFS)"

lv\_conf.h is required, and the following font sizes are to be enabled:

#define LV\_FONT\_MONTSERRAT\_14 1

#define LV\_FONT\_MONTSERRAT\_24 1

#define LV\_FONT\_MONTSERRAT\_28 1

#define LV\_FONT\_MONTSERRAT\_34 1

#define LV\_FONT\_MONTSERRAT\_48 1

This sketch is provided for personal/hobby use only, not for commercial purposes.

3D printable case can be found here: https://www.thingiverse.com/thing:7436303

