#pragma once

#include <Arduino.h>

enum class BleLineResult { Pending, Ready, TooLong };

template <size_t MaxBytes>
class BleTextLine {
public:
    BleLineResult read(Stream& input) {
        for (int byte = input.read(); byte >= 0; byte = input.read()) {
            if (byte == '\r' || byte == '\n') {
                if (length > 0) {
                    return finish();
                }
            } else if (byte != '\0') {
                lastByteAt = millis();
                if (length < MaxBytes) {
                    message[length++] = static_cast<char>(byte);
                } else {
                    tooLong = true;
                }
            }
        }
        // Some phone terminals send text without a line ending.
        return length > 0 && millis() - lastByteAt >= 200 ? finish() : BleLineResult::Pending;
    }

    const char* text() const { return message; }

    void reset() {
        length = 0;
        tooLong = false;
    }

private:
    BleLineResult finish() {
        message[length] = '\0';
        const auto result = tooLong ? BleLineResult::TooLong : BleLineResult::Ready;
        reset();
        return result;
    }

    char message[MaxBytes + 1] = {};
    size_t length = 0;
    bool tooLong = false;
    unsigned long lastByteAt = 0;
};
