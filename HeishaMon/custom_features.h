#pragma once

#include <Arduino.h>
#include "src/common/webserver.h"

// Integration boundary for functionality that is specific to this fork.
// The upstream HeishaMon.ino only delegates setup, loop and web requests here.
void customFeaturesBegin();
void customFeaturesLoop();

bool customFeaturesHandleUri(struct webserver_t *client, const char *uri);
bool customFeaturesHandleArgs(struct webserver_t *client, struct arguments_t *args);
bool customFeaturesHandleCommandArgument(struct webserver_t *client,
  struct arguments_t *args);
bool customFeaturesHandleWrite(struct webserver_t *client);
