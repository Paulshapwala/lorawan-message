#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <bluefruit.h>

#include "ble_text_line.h"

namespace {
constexpr char deviceName[] = "Hello BLE";
constexpr uint8_t oledAddress = 0x3C;
constexpr size_t maxMessageBytes = 80;

Adafruit_SSD1306 oled(128, 64, &Wire);
BLEUart bleUart;
bool oledReady = false;
bool bleReady = false;
bool phoneConnected = false;
bool greetingSent = false;
BleTextLine<maxMessageBytes> phoneLine;

void show(const char* status, const char* text = "") {
    if (!oledReady) {
        return;
    }
    oled.clearDisplay();
    oled.setCursor(0, 0);
    oled.println(deviceName);
    oled.println(status);
    oled.println(text);
    oled.display();
}

}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_3V3_EN, OUTPUT);
    digitalWrite(PIN_3V3_EN, HIGH);
    delay(100);

    Wire.begin();
    Wire.beginTransmission(oledAddress);
    oledReady = Wire.endTransmission() == 0 &&
                oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false);
    if (oledReady) {
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
    } else {
        Serial.println("OLED did not initialize.");
    }

    if (!Bluefruit.begin()) {
        show("BLE could not start");
        Serial.println("BLE could not start.");
        return;
    }
    Bluefruit.setName(deviceName);
    if (bleUart.begin() != ERROR_NONE) {
        show("BLE UART failed");
        Serial.println("BLE UART could not start.");
        return;
    }

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(bleUart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    bleReady = Bluefruit.Advertising.start(0);
    show(bleReady ? "Waiting for phone" : "BLE advertising failed");
}

void loop() {
    if (!bleReady) {
        delay(20);
        return;
    }

    const bool connected = Bluefruit.connected() > 0;
    if (connected != phoneConnected) {
        phoneConnected = connected;
        show(connected ? "Phone connected" : "Waiting for phone");
        if (!connected) {
            greetingSent = false;
            phoneLine.reset();
            bleUart.flush();
        }
    }

    if (connected && bleUart.notifyEnabled()) {
        if (!greetingSent) {
            greetingSent = bleUart.print("Hello BLE\r\n") > 0;
        }
        switch (phoneLine.read(bleUart)) {
            case BleLineResult::Ready:
                show("Phone connected", phoneLine.text());
                bleUart.print("Echo: ");
                bleUart.println(phoneLine.text());
                break;
            case BleLineResult::TooLong:
                show("Phone connected", "Message over 80 bytes");
                bleUart.println("Try 80 bytes or fewer.");
                break;
            case BleLineResult::Pending:
                break;
        }
    }
    delay(10);
}
