#pragma once

// SD history uses the dedicated microSD wiring on the HeishaMon PCB.
// Initialization remains non-fatal when no card is inserted.
#ifndef HEISHAMON_SD_HISTORY_ENABLED
#define HEISHAMON_SD_HISTORY_ENABLED 1
#endif

#ifndef HEISHAMON_SD_CS_PIN
#define HEISHAMON_SD_CS_PIN 34
#endif

#ifndef HEISHAMON_SD_SCK_PIN
#define HEISHAMON_SD_SCK_PIN 36
#endif

#ifndef HEISHAMON_SD_MISO_PIN
#define HEISHAMON_SD_MISO_PIN 37
#endif

#ifndef HEISHAMON_SD_MOSI_PIN
#define HEISHAMON_SD_MOSI_PIN 35
#endif

// Native 1-bit SDMMC transfer clock. The card is initialized at the protocol's
// required low speed before switching to this frequency.
#ifndef HEISHAMON_SDMMC_FREQUENCY_KHZ
#define HEISHAMON_SDMMC_FREQUENCY_KHZ 10000
#endif

#ifndef HEISHAMON_HISTORY_DEFAULT_INTERVAL_SECONDS
#define HEISHAMON_HISTORY_DEFAULT_INTERVAL_SECONDS 60U
#endif

#ifndef HEISHAMON_HISTORY_MAX_SAMPLES
#define HEISHAMON_HISTORY_MAX_SAMPLES 1440U
#endif

#ifndef HEISHAMON_HISTORY_MAX_EVENTS
#define HEISHAMON_HISTORY_MAX_EVENTS 100U
#endif

#ifndef HEISHAMON_HISTORY_SD_RETENTION_DAYS
#define HEISHAMON_HISTORY_SD_RETENTION_DAYS 30U
#endif

#ifndef HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX
#define HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX 8U
#endif
