#pragma once

#include <Arduino.h>
#include "src/common/webserver.h"

// SD-hosted web content for custom pages. The original HeishaMon recovery,
// firmware, settings and home pages remain embedded in the firmware.
void sdWebUiBegin();
void sdWebUiLoop();
bool sdWebUiHandleUri(struct webserver_t *client, const char *uri);
bool sdWebUiHandleArgs(struct webserver_t *client, struct arguments_t *args);
bool sdWebUiHandleWrite(struct webserver_t *client);
bool sdWebUiHandleHeader(struct webserver_t *client, struct header_t *header);
bool sdWebUiHandleClose(struct webserver_t *client);
