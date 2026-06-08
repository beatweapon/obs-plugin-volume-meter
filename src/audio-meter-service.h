#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_meter_service_start(void);
void audio_meter_service_stop(void);

#ifdef __cplusplus
}
#endif
