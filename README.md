# Save the Bees Arduino code

## 1. Relayr Account
- User: bees@relayr.io
- Password: See bitbucket repo.

## 2. Hardware
- [Wemos D1 mini](http://www.wemos.cc/Products/d1_mini.html)
- Temperature and Humidity
    - [AM2315 - ENCASED I2C TEMPERATURE/HUMIDITY SENSOR](https://www.adafruit.com/product/1293)
    - [AM2315](https://cdn-shop.adafruit.com/datasheets/AM2315.pdf)
- Force & Load
    - [Force Sensors & Load Cells MF LC:COMP ONLY](http://www.mouser.de/ProductDetail/Measurement-Specialties/FC2231-0000-0010-L/?qs=sGAEpiMZZMvDU9HV27FC0fgNXcIVcGB0KWHdJUqMg9Q%3d)


## 3. Wiring

### 3.1 Temperature and Humidity: using Wemos D1 mini
- Connect RED of the AM2315 sensor to 5.0V
- Connect BLACK to Ground
- Connect WHITE to i2c clock - on Wemos D1 mini that's D1
- Connect YELLOW to i2c data - on Wemos D1 mini that's D2

## 4. Software

### 4.1 Libraries
- [Wemos to relayr.io](https://github.com/relayr/ESP8266_Arduino/tree/master/WeMos_D1)
- [Adafruit_AM2315](https://github.com/adafruit/Adafruit_AM2315)
- [EEPROM for ESP8266](http://www.esp8266.com/wiki/doku.php?id=arduino-docs)

