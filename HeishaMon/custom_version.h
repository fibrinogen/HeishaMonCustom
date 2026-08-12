#pragma once

// Custom release versioning:
//   Firmware: X.Y.Z
//   Web UI:   X.Y.Z-web.N
// Keep the shared X.Y.Z in sync for a release. Increment only N for a Web UI
// fix that does not require a firmware flash.
#define CUSTOM_FIRMWARE_VERSION "0.2.3"
#define CUSTOM_WEBUI_REVISION "3"
#define CUSTOM_WEBUI_VERSION CUSTOM_FIRMWARE_VERSION "-web." CUSTOM_WEBUI_REVISION

// Backwards-compatible name used by existing custom feature code.
#define CUSTOM_FEATURES_VERSION CUSTOM_FIRMWARE_VERSION
#define HEISHAMON_BASE_VERSION "v4.2.1"
