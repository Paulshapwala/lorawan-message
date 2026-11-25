#pragma once

#include "WVariant.h"

#define VARIANT_MCK 64000000UL
#define USE_LFRC

#define PINS_COUNT 48
#define NUM_DIGITAL_PINS PINS_COUNT
#define NUM_ANALOG_INPUTS 1
#define NUM_ANALOG_OUTPUTS 0
#define ADC_RESOLUTION 14

#define PIN_LED1 15
#define LED_BUILTIN PIN_LED1
#define LED_STATE_ON 1
#define PIN_3V3_EN 13

#define PIN_SERIAL1_RX 6
#define PIN_SERIAL1_TX 8

#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA (32 + 4) // P1.04
#define PIN_WIRE_SCL 11       // P0.11

#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO 2
#define PIN_SPI_MOSI (32 + 15)
#define PIN_SPI_SCK (32 + 11)

static const uint8_t SS = 32 + 13;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;
