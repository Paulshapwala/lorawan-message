#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_TinyUSB.h>
#include <RadioLib.h>
#include <SPI.h>
#include <Wire.h>
#include <atomic>
#include <bluefruit.h>

#include "ble_text_line.h"
#include "gateway_credentials.h"
#include "gateway_session.h"

namespace {
// RA-01SH signals from the KiCad schematic. The board variant sets the SPI pins.
constexpr uint8_t radioInterrupt = 10; // P0.10, DIO1
constexpr uint8_t radioReset = 9;      // P0.09
constexpr uint8_t radioBusy = 29;      // P0.29
constexpr uint8_t radioRxEnable = 17;  // P0.17

constexpr uint8_t uplinkDataRate = 5; // EU868 SF7 / 125 kHz
constexpr uint8_t routingPort = 2; // Compact destination byte followed by text.
constexpr uint8_t pollPort = 1;
constexpr uint8_t oledAddress = 0x3C;
constexpr uint8_t oledLineHeight = 8;
constexpr uint8_t menuButtonPin = 32; // P1.00, SW2 pulls BTN to ground.
constexpr unsigned long buttonDebounceMs = 30;
constexpr unsigned long buttonHoldMs = 700;
constexpr uint16_t bleAdvertisingSeconds = 60;
// Leave room under the EU868 DR0 downlink limit for the slower receive window.
constexpr size_t maxPhoneTextBytes = 48;
constexpr size_t peerCount = sizeof(endDevices) / sizeof(endDevices[0]) - 1;
constexpr size_t targetsPerPage = 5;
// Keep the target and session files within one LittleFS directory block.
constexpr char targetSettingsPath[] = "/target";
constexpr char targetStagingPath[] = "/target.tmp";

enum class Screen { Home, Menu, Targets, BleControl, Status, Result };
enum class BleState { Off, Advertising, Pairing, Ready };

struct MenuItem {
    const char* label;
    char command;
};

constexpr MenuItem menuItems[] = {
    {"Send hello", 's'},
    {"Send hello + ACK", 'a'},
    {"Check messages", 'p'},
    {"Change target", 't'},
    {"BLE Control", 'b'},
    {"Back", '\0'},
};
constexpr size_t menuItemCount = sizeof(menuItems) / sizeof(menuItems[0]);

Module radioModule(SS, radioInterrupt, radioReset, radioBusy);
SX1262 radio(&radioModule);
GatewaySession node(&radio, &EU868);
Adafruit_SSD1306 oled(128, 64, &Wire);
BLEUart bleUart;
BleTextLine<maxPhoneTextBytes> phoneLine;
std::atomic<uint32_t> displayedPasskey{0}; // Six-digit value plus one, with zero meaning no code.
std::atomic<bool> bleRxOverflow{false};
bool bleSetupAttempted = false;
bool bleInitialized = false;
bool bleEnabled = false;
bool bleBackSelected = false;
BleState lastBleState = BleState::Off;
uint32_t lastPasskey = 0;
bool oledReady = false;
bool ready = false;
bool serialWasOpen = false;
Screen currentScreen = Screen::Status;
size_t selectedItem = 0;
size_t selectedTarget = peerCount;
size_t targetMenuItem = peerCount;
bool buttonRawPressed = false;
bool buttonPressed = false;
bool buttonHoldHandled = false;
unsigned long buttonChangedAt = 0;
unsigned long buttonPressedAt = 0;

const EndDevice& peerAt(size_t index) {
    return endDevices[index < localDeviceIndex ? index : index + 1];
}

const EndDevice* currentTarget() {
    return selectedTarget < peerCount ? &peerAt(selectedTarget) : nullptr;
}

uint8_t readTarget(const char* path) {
    uint8_t settings[2] = {};
    auto file = InternalFS.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ);
    const bool valid = file && file.size() == sizeof(settings) &&
                       file.read(settings, sizeof(settings)) == static_cast<int>(sizeof(settings)) &&
                       settings[0] == endDevices[localDeviceIndex].tag;
    file.close();
    return valid ? settings[1] : 0;
}

void restoreTarget() {
    selectedTarget = peerCount > 0 ? 0 : peerCount;
    const uint8_t storedTarget = readTarget(targetSettingsPath);
    for (size_t i = 0; i < peerCount; ++i) {
        if (peerAt(i).tag == storedTarget) {
            selectedTarget = i;
            break;
        }
    }
}

bool saveTarget(uint8_t tag) {
    if (readTarget(targetSettingsPath) == tag) {
        return true;
    }
    if (InternalFS.exists(targetStagingPath) && !InternalFS.remove(targetStagingPath)) {
        return false;
    }

    const uint8_t settings[] = {endDevices[localDeviceIndex].tag, tag};
    auto file = InternalFS.open(targetStagingPath, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
    const bool written = file && file.size() == 0 && file.write(settings, sizeof(settings)) == sizeof(settings);
    file.close();
    return written && readTarget(targetStagingPath) == tag &&
           InternalFS.rename(targetStagingPath, targetSettingsPath) && readTarget(targetSettingsPath) == tag;
}

bool initializeOled() {
    Wire.begin();
    Wire.beginTransmission(oledAddress);
    if (Wire.endTransmission() != 0 ||
        !oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false)) {
        return false;
    }

    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    return true;
}

bool startScreen(Screen screen) {
    currentScreen = screen;
    if (!oledReady) {
        return false;
    }
    oled.clearDisplay();
    oled.setCursor(0, 0);
    return true;
}

void showHome() {
    if (startScreen(Screen::Home)) {
        oled.println(deviceId);
        oled.println(ready ? "EU868 ready" : "Radio unavailable");
        if (ready) {
            oled.println();
            oled.println("> Open menu");
            oled.println("Hold: OK");
        }
        oled.display();
    }
}

void showMenu() {
    if (startScreen(Screen::Menu)) {
        oled.println(deviceId);
        const auto* target = currentTarget();
        oled.print("To: ");
        oled.println(target ? target->id : "Select target");
        for (size_t i = 0; i < menuItemCount; ++i) {
            oled.print(i == selectedItem ? "> " : "  ");
            oled.println(menuItems[i].label);
        }
        oled.display();
    }
}

void showTargets() {
    if (startScreen(Screen::Targets)) {
        oled.println("Change target");
        if (peerCount == 0) {
            oled.println("No other devices");
        } else {
            const size_t pageItem = targetMenuItem == peerCount ? peerCount - 1 : targetMenuItem;
            const size_t first = pageItem - pageItem % targetsPerPage;
            for (size_t i = first; i < peerCount && i < first + targetsPerPage; ++i) {
                oled.print(i == targetMenuItem ? "> " : "  ");
                oled.print(peerAt(i).id);
                oled.println(i == selectedTarget ? " (set)" : "");
            }
        }
        oled.setCursor(0, 6 * oledLineHeight);
        oled.println(targetMenuItem == peerCount ? "> Back" : "  Back");
        oled.println("Tap: next  Hold: OK");
        oled.display();
    }
}

BleState bleState() {
    if (!bleEnabled) {
        return BleState::Off;
    }
    const uint16_t handle = Bluefruit.connHandle();
    ble_gap_conn_sec_t security = {};
    if (handle != BLE_CONN_HANDLE_INVALID && sd_ble_gap_conn_sec_get(handle, &security) == NRF_SUCCESS) {
        if (security.sec_mode.sm == 1 && security.sec_mode.lv >= 3 && bleUart.notifyEnabled(handle)) {
            return BleState::Ready;
        }
        return BleState::Pairing;
    }
    return Bluefruit.Advertising.isRunning() ? BleState::Advertising : BleState::Off;
}

void showBleControl(BleState state, uint32_t passkey) {
    if (!startScreen(Screen::BleControl)) {
        return;
    }
    oled.println("BLE Control");
    switch (state) {
        case BleState::Advertising:
            oled.println("Waiting for phone");
            oled.print(deviceId);
            oled.println(" BLE");
            break;
        case BleState::Pairing:
            oled.println("Open UART to pair");
            if (passkey > 0) {
                char code[7];
                snprintf(code, sizeof(code), "%06lu", (passkey - 1) % 1000000UL);
                oled.print("Code: ");
                oled.println(code);
            }
            break;
        case BleState::Ready:
            oled.println("Phone connected");
            oled.println("Send text or /poll");
            break;
        case BleState::Off:
            oled.println("BLE off");
            break;
    }
    oled.setCursor(0, 4 * oledLineHeight);
    oled.print(bleBackSelected ? "  " : "> ");
    oled.println(state == BleState::Off ? "Start BLE" : "Turn BLE off");
    oled.println(bleBackSelected ? "> Back" : "  Back");
    oled.println("Tap: next  Hold: OK");
    oled.display();
}

void writeBleLine(const char* text) {
    if (bleState() == BleState::Ready) {
        bleUart.println(text);
    }
}

void drawBack() {
    oled.setCursor(0, oled.height() - oledLineHeight);
    oled.print("> Back  Hold: OK");
}

void showStatus(const char* message, const char* detail = nullptr, Screen screen = Screen::Status) {
    if (startScreen(screen)) {
        oled.println(message);
        if (detail) {
            oled.println(detail);
        }
        if (screen == Screen::Result) {
            drawBack();
        }
        oled.display();
    }
}

void reportError(const char* message, const char* detail = nullptr) {
    Serial.println(message);
    writeBleLine(message);
    if (detail) {
        Serial.println(detail);
        writeBleLine(detail);
    }
    showStatus(message, detail, Screen::Result);
}

void reportRadioError(const char* message, int16_t status) {
    char detail[24];
    snprintf(detail, sizeof(detail), "RadioLib status %d", static_cast<int>(status));
    reportError(message, detail);
}

bool showPairingPasskey(uint16_t, const uint8_t passkey[6], bool matchRequest) {
    uint32_t value = 0;
    for (size_t i = 0; i < 6; ++i) {
        value = value * 10 + (passkey[i] - '0');
    }
    displayedPasskey.store(value + 1);
    return !matchRequest; // The OLED provides a passkey for entry, with no numeric confirmation.
}

bool startBle() {
    if (!bleInitialized) {
        if (bleSetupAttempted) {
            return false;
        }
        bleSetupAttempted = true;
        if (!Bluefruit.begin()) {
            return false;
        }
        char name[sizeof("custom-255 BLE")];
        snprintf(name, sizeof(name), "%s BLE", deviceId);
        Bluefruit.setName(name);
        Bluefruit.Security.setIOCaps(true, false, false);
        Bluefruit.Security.setMITM(true);
        Bluefruit.Security.setPairPasskeyCallback(showPairingPasskey);
        bleUart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
        if (bleUart.begin() != ERROR_NONE) {
            return false;
        }
        bleUart.setRxOverflowCallback([](uint16_t, uint16_t) { bleRxOverflow.store(true); });
        Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
        Bluefruit.Advertising.addService(bleUart);
        Bluefruit.ScanResponse.addName();
        Bluefruit.Advertising.restartOnDisconnect(false);
        bleInitialized = true;
    }

    displayedPasskey.store(0);
    bleUart.flush();
    bleRxOverflow.store(false);
    phoneLine.reset();
    bleEnabled = Bluefruit.Advertising.start(bleAdvertisingSeconds);
    return bleEnabled;
}

void stopBle() {
    bleEnabled = false;
    displayedPasskey.store(0);
    if (Bluefruit.Advertising.isRunning()) {
        Bluefruit.Advertising.stop();
    }
    const uint16_t handle = Bluefruit.connHandle();
    if (handle != BLE_CONN_HANDLE_INVALID) {
        sd_ble_gap_disconnect(handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    }
}

void openBleControl() {
    if (bleState() == BleState::Off && !startBle()) {
        reportError("Could not start BLE");
        return;
    }
    bleBackSelected = true;
    showBleControl(bleState(), displayedPasskey.load());
}

bool initializeRadio() {
    showStatus("Starting radio...");

    // The RA-01SH uses a crystal, and its TXEN is wired to the radio's DIO2.
    radio.tcxoVoltage = 0.0f;
    ConfigLoRa_t config;
    config.frequency = 868.1f;
    int16_t status = radio.begin(config);
    if (status == RADIOLIB_ERR_NONE) {
        radio.setRfSwitchPins(radioRxEnable, RADIOLIB_NC);
        status = radio.setDio2AsRfSwitch(true);
    }
    if (status != RADIOLIB_ERR_NONE) {
        reportRadioError("RA-01SH initialization failed.", status);
        return false;
    }

    status = node.beginABP(deviceAddress, nullptr, nullptr, networkSessionKey, applicationSessionKey);
    if (status != RADIOLIB_ERR_NONE || !InternalFS.begin() || !node.restore(sessionPath)) {
        reportError("Could not load the LoRaWAN session.");
        return false;
    }

    status = node.activateABP();
    if (status != RADIOLIB_LORAWAN_NEW_SESSION && status != RADIOLIB_LORAWAN_SESSION_RESTORED) {
        reportRadioError("ABP activation failed.", status);
        return false;
    }

    node.setADR(false);
    if (node.setDatarate(uplinkDataRate) != RADIOLIB_ERR_NONE ||
        (status == RADIOLIB_LORAWAN_NEW_SESSION && node.setTxPower(14) != RADIOLIB_ERR_NONE)) {
        reportError("Could not set the EU868 uplink data rate or power.");
        return false;
    }
    if (!node.initializeReceiveWindows(initialRx1DelaySeconds, initialRx2DataRate)) {
        reportError("Could not configure the EU868 receive windows.");
        return false;
    }
    node.setDutyCycle(true);
    restoreTarget();
    showMenu();
    return true;
}

void printLinkMetrics(Print& output, RadioLibTime_t airtimeMs, const GatewaySession::DownlinkMetrics* ack) {
    output.print("TX air ");
    output.print(airtimeMs);
    output.println(" ms");
    if (!ack) {
        return;
    }

    output.print("RSSI ");
    output.print(ack->rssiDbm, 1);
    output.println(" dBm");
    if (ack->hasSnr) {
        output.print("SNR ");
        output.print(ack->snrDb, 2);
        output.println(" dB");
    }
    output.print("ACK wait ");
    output.print(ack->replyDelayMs);
    output.println(" ms");
}

void sendUplink(int command, const char* phoneMessage = nullptr) {
    const bool poll = command == 'p';
    const bool confirmed = command == 'a';
    const auto* target = currentTarget();
    if (!poll && !target) {
        reportError("Select a target", "Use Change target");
        return;
    }

    const auto wait = node.timeUntilUplink();
    if (wait > 0) {
        Serial.print("Duty cycle wait, milliseconds remaining: ");
        Serial.println(wait);
        char detail[24];
        snprintf(detail, sizeof(detail), "Try again in %lu s", static_cast<unsigned long>((wait + 999) / 1000));
        writeBleLine("Duty cycle wait");
        writeBleLine(detail);
        showStatus("Duty cycle wait", detail, Screen::Result);
        return;
    }

    const uint32_t counter = node.nextCounter();
    char generatedMessage[sizeof("hello[4294967295]")];
    snprintf(generatedMessage, sizeof(generatedMessage), "%s[%lu]", poll ? "poll" : "hello",
             static_cast<unsigned long>(counter + 1));
    const char* message = phoneMessage ? phoneMessage : generatedMessage;

    // A poll opens the receive windows without asking the router to forward anything.
    const uint8_t port = poll ? pollPort : routingPort;
    const size_t tagLength = poll ? 0 : 1;
    uint8_t payload[maxPhoneTextBytes + 1];
    if (!poll) {
        payload[0] = target->tag;
    }
    const size_t messageLength = strlen(message);
    const size_t payloadLength = tagLength + messageLength;
    if (payloadLength > sizeof(payload) || payloadLength > node.getMaxPayloadLen()) {
        reportError("Uplink payload too long.", "Current LoRaWAN rate");
        return;
    }
    memcpy(payload + tagLength, message, messageLength);

    if (!node.reserveCounter(sessionPath)) {
        reportError("Could not save the uplink counter.", "Test stopped.");
        ready = false;
        return;
    }

    Serial.print("Sending ");
    Serial.print(message);
    if (!poll) {
        Serial.print(" to ");
        Serial.print(target->id);
    }
    Serial.print(" on FPort ");
    Serial.print(port);
    Serial.print(" (");
    Serial.print(payloadLength);
    Serial.println(" bytes)");
    showStatus("Sending uplink", phoneMessage ? target->id : message);
    if (poll) {
        writeBleLine("Checking messages...");
    } else {
        char detail[sizeof("Sending to custom-255...")];
        snprintf(detail, sizeof(detail), "Sending to %s...", target->id);
        writeBleLine(detail);
    }

    uint8_t downlink[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE];
    size_t downlinkLength = 0;
    LoRaWANEvent_t uplink = {};
    LoRaWANEvent_t event = {};
    node.resetDownlinkMetrics();
    const int16_t result = node.sendReceive(payload, payloadLength, port, downlink, &downlinkLength,
                                            confirmed, &uplink, &event);

    if (!node.saveAfterUplink(sessionPath, counter)) {
        reportError("Could not save the updated session.", "Test stopped.");
        ready = false;
        return;
    }
    if (result < 0) {
        reportRadioError("LoRaWAN exchange failed.", result);
        return;
    }

    Serial.println("Uplink sent. Check TTN Live data.");
    writeBleLine(poll ? "Poll sent" : "Uplink sent");
    Serial.print("Uplink DR");
    Serial.println(uplink.datarate);
    const auto& metrics = node.lastDownlinkMetrics();
    const auto airtimeMs = metrics.received ? metrics.uplinkAirtimeMs : node.getLastToA();
    const bool ackReceived = confirmed && result > 0 && event.confirming;
    const auto* ackMetrics = ackReceived && metrics.received ? &metrics : nullptr;
    const char* ackMessage = nullptr;
    if (confirmed) {
        ackMessage = ackReceived ? "TTN ACK received." : "TTN ACK was not received.";
        Serial.println(ackMessage);
        writeBleLine(ackMessage);
    }
    const char* pollStatus = nullptr;
    if (poll && downlinkLength == 0) {
        pollStatus = result > 0 ? "Network reply only" : "No downlink received";
        Serial.println(pollStatus);
        writeBleLine(pollStatus);
    }
    printLinkMetrics(Serial, airtimeMs, ackMetrics);
    if (downlinkLength > 0) {
        Serial.print("Downlink on port ");
        Serial.print(event.fPort);
        Serial.print(": ");
        Serial.write(downlink, downlinkLength);
        Serial.println();
        if (bleState() == BleState::Ready) {
            bleUart.print("Received: ");
            bleUart.write(downlink, downlinkLength);
            bleUart.println();
        }
    }

    if (startScreen(Screen::Result)) {
        if (phoneMessage) {
            // Keep a row free for status and space to show any received message.
            oled.write(reinterpret_cast<const uint8_t*>(message), min(messageLength, oled.width() / size_t{6}));
            oled.println();
        } else {
            oled.println(message);
        }
        oled.println(ackMessage ? ackMessage : "Uplink sent");
        if (pollStatus) {
            oled.println(pollStatus);
        }
        printLinkMetrics(oled, airtimeMs, ackMetrics);
        if (downlinkLength > 0) {
            const auto backRow = oled.height() - oledLineHeight;
            // Keep one row for the payload and the bottom row for Back.
            if (oled.getCursorY() + 2 * oledLineHeight <= backRow) {
                oled.print("Downlink port ");
                oled.println(event.fPort);
            }
            const auto remainingRows = (backRow - oled.getCursorY()) / oledLineHeight;
            const size_t charactersLeft = remainingRows > 0 ? (oled.width() / 6) * remainingRows : 0;
            for (size_t i = 0; i < downlinkLength && i < charactersLeft; ++i) {
                const uint8_t byte = downlink[i];
                oled.write(byte >= 32 && byte <= 126 ? byte : '.');
            }
        }
        drawBack();
        oled.display();
    }
}

void updateBle() {
    const auto state = bleState();
    const uint32_t passkey = state == BleState::Pairing ? displayedPasskey.load() : 0;
    if (state != lastBleState || passkey != lastPasskey) {
        if (currentScreen == Screen::BleControl || passkey > 0) {
            showBleControl(state, passkey);
        }
        if (state == BleState::Off) {
            displayedPasskey.store(0);
        } else if (state == BleState::Ready && lastBleState != BleState::Ready) {
            writeBleLine("Ready. Send text or /poll.");
        }
        lastBleState = state;
        lastPasskey = passkey;
    }
    if (state != BleState::Ready) {
        return;
    }

    const auto result = phoneLine.read(bleUart);
    if (bleRxOverflow.exchange(false)) {
        bleUart.flush();
        phoneLine.reset();
        writeBleLine("Input buffer full. Send one message at a time.");
        return;
    }

    if (result == BleLineResult::TooLong) {
        char detail[24];
        snprintf(detail, sizeof(detail), "Use up to %u bytes", static_cast<unsigned>(maxPhoneTextBytes));
        reportError("Message too long", detail);
    } else if (result == BleLineResult::Ready) {
        if (!ready) {
            writeBleLine("Radio unavailable");
        } else if (strcmp(phoneLine.text(), "/poll") == 0) {
            sendUplink('p');
        } else {
            sendUplink('s', phoneLine.text());
        }
    }
}

void leaveResult() {
    if (ready) {
        showMenu();
    } else {
        showHome();
    }
}

void selectTargetMenuItem() {
    if (targetMenuItem < peerCount) {
        const auto& target = peerAt(targetMenuItem);
        if (!saveTarget(target.tag)) {
            reportError("Could not save target");
            return;
        }
        selectedTarget = targetMenuItem;
    }
    showMenu();
}

void updateMenuButton() {
    const auto now = millis();
    const bool pressed = digitalRead(menuButtonPin) == LOW;
    if (pressed != buttonRawPressed) {
        buttonRawPressed = pressed;
        buttonChangedAt = now;
    }
    if (now - buttonChangedAt < buttonDebounceMs) {
        return;
    }

    if (pressed != buttonPressed) {
        buttonPressed = pressed;
        if (pressed) {
            buttonPressedAt = now;
            buttonHoldHandled = false;
        } else if (!buttonHoldHandled) {
            if (currentScreen == Screen::Menu) {
                selectedItem = (selectedItem + 1) % menuItemCount;
                showMenu();
            } else if (currentScreen == Screen::Targets) {
                targetMenuItem = (targetMenuItem + 1) % (peerCount + 1);
                showTargets();
            } else if (currentScreen == Screen::BleControl) {
                bleBackSelected = !bleBackSelected;
                showBleControl(lastBleState, lastPasskey);
            } else if (currentScreen == Screen::Result) {
                leaveResult();
            }
        }
    }

    if (buttonPressed && !buttonHoldHandled && now - buttonPressedAt >= buttonHoldMs) {
        buttonHoldHandled = true;
        if (currentScreen == Screen::Menu) {
            const char command = menuItems[selectedItem].command;
            if (command == 't') {
                targetMenuItem = selectedTarget;
                showTargets();
            } else if (command == 'b') {
                openBleControl();
            } else if (command) {
                sendUplink(command);
            } else {
                showHome();
            }
        } else if (currentScreen == Screen::Targets) {
            selectTargetMenuItem();
        } else if (currentScreen == Screen::BleControl) {
            if (bleBackSelected) {
                showMenu();
            } else if (bleState() == BleState::Off) {
                openBleControl();
            } else {
                stopBle();
                showBleControl(BleState::Off, 0);
            }
        } else if (currentScreen == Screen::Result) {
            leaveResult();
        } else if (currentScreen == Screen::Home && ready) {
            selectedItem = 0;
            showMenu();
        }
    }
}
} // namespace

void setup() {
    Serial.begin(115200);
    pinMode(menuButtonPin, INPUT_PULLUP);

    // The OLED and RA-01SH share the Pro Micro's switched 3.3 V supply.
    pinMode(PIN_3V3_EN, OUTPUT);
    digitalWrite(PIN_3V3_EN, HIGH);
    delay(100);

    oledReady = initializeOled();
    ready = initializeRadio();

    // Wait for a button held during startup to be released before accepting input.
    buttonRawPressed = buttonPressed = digitalRead(menuButtonPin) == LOW;
    buttonHoldHandled = buttonPressed;
}

void loop() {
    const bool serialOpen = Serial;
    if (serialOpen && !serialWasOpen) {
        Serial.println(oledReady ? "OLED ready." : "OLED unavailable.");
        if (!ready) {
            ready = initializeRadio();
        }
        if (ready) {
            Serial.print("Device ");
            Serial.println(deviceId);
            node.printReceiveWindows(Serial);
            if (const auto* target = currentTarget()) {
                Serial.print("Target ");
                Serial.print(target->id);
                Serial.print(" tag 0x");
                if (target->tag < 0x10) {
                    Serial.print('0');
                }
                Serial.println(target->tag, HEX);
            }
            Serial.println("EU868 ready. Tap P1 to scroll and hold to select.");
        }
    }
    serialWasOpen = serialOpen;

    if (oledReady) {
        updateMenuButton();
    }
    if (serialOpen && ready && Serial.available()) {
        const int command = Serial.read();
        if (command == 's' || command == 'a' || command == 'p') {
            sendUplink(command);
        }
    }
    updateBle();
    delay(10);
}
