#pragma once

// Custom release versioning:
//   Firmware: X.Y.Z
//   Web UI:   X.Y.Z-web.N
// N is a monotonically increasing Web UI build number. Never reset it: this
// makes every uploaded package uniquely identifiable, even across firmware
// releases. Increment it for every Web UI package we publish.
#define CUSTOM_FIRMWARE_VERSION "0.2.7"
#define CUSTOM_WEBUI_BUILD "21"
#define CUSTOM_WEBUI_VERSION CUSTOM_FIRMWARE_VERSION "-web." CUSTOM_WEBUI_BUILD

// Backwards-compatible name used by existing custom feature code.
#define CUSTOM_FEATURES_VERSION CUSTOM_FIRMWARE_VERSION
#define HEISHAMON_BASE_VERSION "v4.2.1"
