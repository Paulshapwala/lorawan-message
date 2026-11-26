/** @file
 * Extends RadioLib with LittleFS session persistence, receive-window
 * configuration, and link measurements for this EU868 ABP end device.
 */
#pragma once

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <RadioLib.h>
#include <stddef.h>

/// Keep the LoRaWAN session and counters across reboots and capture downlink measurements.
class GatewaySession : public LoRaWANNode {
public:
    using LoRaWANNode::LoRaWANNode;

    /// Measurements captured on the first downlink in the current exchange.
    struct DownlinkMetrics {
        bool received = false;
        bool hasSnr = false;
        float rssiDbm = 0;
        float snrDb = 0;
        RadioLibTime_t uplinkAirtimeMs = 0;
        RadioLibTime_t replyDelayMs = 0;
    };

    /// Clear the previous measurements before starting a new exchange.
    void resetDownlinkMetrics() { downlinkMetrics = {}; }

    /// Return the captured measurements. Check received before using their values.
    const DownlinkMetrics& lastDownlinkMetrics() const { return downlinkMetrics; }

    /// Apply the profile's initial RX1 delay and RX2 data rate when needed.
    /// Preserve network-negotiated receive settings for a matching saved session.
    bool initializeReceiveWindows(uint8_t rx1DelaySeconds, uint8_t rx2DataRate) {
        // Open the radio earlier and allow more time for the downlink preamble.
        scanGuard = 60;

        // Record which starting settings were applied. The previous firmware
        // used marker 1 for a 5-second delay and RX2 DR3.
        const uint8_t settingsMarker = (rx1DelaySeconds << 3) | rx2DataRate;
        const bool matchesLegacySettings = receiveSettingsVersion == 1 && rx1DelaySeconds == 5 && rx2DataRate == 3;
        // Keep the network's negotiated settings when restoring a current session.
        if (receiveSettingsVersion == settingsMarker || matchesLegacySettings) {
            receiveSettingsVersion = settingsMarker;
            return true;
        }

        // Seed a new session or correct an older save that overwrote the TTN settings.
        uint8_t timing[] = {rx1DelaySeconds};
        execMacCommand(RADIOLIB_LORAWAN_MAC_RX_TIMING_SETUP, timing, sizeof(timing));

        constexpr uint32_t rx2Frequency = 8695250; // 869.525 MHz, in 100 Hz units
        uint8_t params[] = {
            static_cast<uint8_t>((rx1DrOffset << 4) | rx2DataRate),
            static_cast<uint8_t>(rx2Frequency),
            static_cast<uint8_t>(rx2Frequency >> 8),
            static_cast<uint8_t>(rx2Frequency >> 16),
        };
        uint8_t ack = 0;
        execMacCommand(RADIOLIB_LORAWAN_MAC_RX_PARAM_SETUP, params, sizeof(params), &ack);
        if (ack != 0x07) {
            return false;
        }
        receiveSettingsVersion = settingsMarker;
        return true;
    }

    /// Print the active RX1 delay in seconds and the RX2 data-rate index.
    void printReceiveWindows(Print& output) const {
        output.print("RX1 delay ");
        output.print(rxDelays[RADIOLIB_LORAWAN_RX1] / 1000);
        output.print(" s, RX2 DR");
        output.println(channels[RADIOLIB_LORAWAN_RX2].dr);
    }

    /// Return the frame counter that RadioLib will use for the next uplink.
    uint32_t nextCounter() const { return fCntUp; }

    /// Save the next unused counter in flash before transmitting with the current one.
    /// This lets a reboot skip the attempted counter if transmission was interrupted.
    bool reserveCounter(const char* path) {
        // Save the next unused counter before transmitting the current one.
        if (fCntUp == UINT32_MAX) {
            return false;
        }
        ++fCntUp;
        const bool saved = save(path);
        --fCntUp;
        return saved;
    }

    /// Load and validate a saved session. A missing file allows the board to start a new session.
    bool restore(const char* path) {
        receiveSettingsVersion = 0;
        if (!InternalFS.exists(path)) {
            return true;
        }
        SavedSession data{};
        if (!read(path, data, true) || setBufferNonces(data.nonces) != RADIOLIB_ERR_NONE ||
            setBufferSession(data.session) != RADIOLIB_ERR_NONE) {
            return false;
        }
        receiveSettingsVersion = data.receiveSettingsVersion;
        return true;
    }

    /// Persist the session after the exchange and advance past the attempted uplink counter.
    bool saveAfterUplink(const char* path, uint32_t attemptedCounter) {
        if (fCntUp <= attemptedCounter) {
            fCntUp = attemptedCounter + 1;
        }
        return save(path);
    }

protected:
    /// Delegate reception to RadioLib, then capture the first downlink's timing and radio measurements.
    int16_t receiveDownlink() override {
        const int16_t window = LoRaWANNode::receiveDownlink();
        if (window > 0 && !downlinkMetrics.received) {
            // Read the packet before MAC processing can trigger another radio exchange.
            downlinkMetrics.received = true;
            downlinkMetrics.uplinkAirtimeMs = getLastToA();
            downlinkMetrics.replyDelayMs = tDownlink - tUplinkEnd;
            downlinkMetrics.rssiDbm = phyLayer->getRSSI();
            downlinkMetrics.hasSnr = band->dataRates[channels[window].dr].modem == RADIOLIB_MODEM_LORA;
            if (downlinkMetrics.hasSnr) {
                downlinkMetrics.snrDb = phyLayer->getSNR();
            }
        }
        return window;
    }

private:
    uint8_t receiveSettingsVersion = 0;
    DownlinkMetrics downlinkMetrics;

    /// Preserve the RadioLib buffers and receive-settings marker in their on-flash order.
    struct SavedSession {
        uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
        uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
        uint8_t receiveSettingsVersion;
    };

    /// Read an exact-size session record. When allowed, also accept the legacy layout without the marker.
    static bool read(const char* path, SavedSession& data, bool allowLegacy = false) {
        auto file = InternalFS.open(path, Adafruit_LittleFS_Namespace::FILE_O_READ);
        const auto size = file ? file.size() : 0;
        const bool validSize = size == sizeof(data) ||
                               (allowLegacy && size == offsetof(SavedSession, receiveSettingsVersion));
        const bool valid = validSize && file.read(&data, size) == static_cast<int>(size);
        file.close();
        return valid;
    }

    /// Write and read back a temporary session file before replacing the saved record.
    bool save(const char* path) {
        constexpr char stagingPath[] = "/ttn-session.tmp";
        // An interrupted save can leave this file behind. FILE_O_WRITE opens at
        // the end, and truncating there leaves LittleFS's write position stale.
        if (InternalFS.exists(stagingPath) && !InternalFS.remove(stagingPath)) {
            return false;
        }

        SavedSession data;
        memcpy(data.nonces, getBufferNonces(), sizeof(data.nonces));
        memcpy(data.session, getBufferSession(), sizeof(data.session));
        data.receiveSettingsVersion = receiveSettingsVersion;

        auto file = InternalFS.open(stagingPath, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
        const bool written = file && file.size() == 0 &&
                             file.write(reinterpret_cast<const uint8_t*>(&data), sizeof(data)) == sizeof(data);
        file.close();

        SavedSession check;
        return written && read(stagingPath, check) && memcmp(&data, &check, sizeof(data)) == 0 &&
               InternalFS.rename(stagingPath, path);
    }
};
