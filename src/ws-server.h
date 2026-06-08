#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ws_server_start(unsigned short port);
void ws_server_stop(void);
void ws_server_broadcast_text(const char *text);
unsigned short ws_server_port(void);

#ifdef __cplusplus
}
#endif
