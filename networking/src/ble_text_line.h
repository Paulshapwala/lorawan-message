/** @file
 * Converts bytes from a BLE UART stream into bounded text lines for the
 * gateway firmware and the bench sketch.
 */
#pragma once

#include <Arduino.h>

/// Indicates whether a complete line is available or was rejected for its length.
enum class BleLineResult { Pending, Ready, TooLong };

/// Buffers at most MaxBytes per line and skips empty CR or LF separators.
template <size_t MaxBytes>
class BleTextLine {
public:
    /// Consume available bytes until one line ends or the stream runs out of data.
    /// Treat 200 ms without another byte as the end when the terminal omits a line ending.
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

    /// Return the null-terminated line after a Ready result. A later read can overwrite it.
    const char* text() const { return message; }

    /// Discard partial input and clear the length error before reading another line.
    void reset() {
        length = 0;
        tooLong = false;
    }

private:
    /// Terminate the buffer, report whether the line fit, and reset the parser for the next line.
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
