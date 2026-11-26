/** @file
 * Bench sketch for checking the Pro Micro's OLED wiring and USB serial output.
 */
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

constexpr char greeting[] = "Hello, Paul";
constexpr uint8_t oledAddress = 0x3C;
Adafruit_SSD1306 oled(128, 64, &Wire);

/// Power the display, check its I2C address, and draw the greeting once.
void setup() {
    Serial.begin(115200);

    // Enable the Pro Micro's 3.3 V output to power the OLED.
    pinMode(PIN_3V3_EN, OUTPUT);
    digitalWrite(PIN_3V3_EN, HIGH);
    delay(100);

    // Wire uses the schematic's SDA (P1.04) and SCL (P0.11).
    Wire.begin();
    Wire.beginTransmission(oledAddress);
    if (Wire.endTransmission() != 0) {
        Serial.println("The OLED did not respond on I2C.");
        return;
    }

    if (!oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false)) {
        Serial.println("Could not initialize the OLED.");
        return;
    }

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println(greeting);
    oled.display();
}

/// Print the greeting to USB serial every two seconds.
void loop() {
    Serial.println(greeting);
    delay(2000);
}
