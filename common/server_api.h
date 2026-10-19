#ifndef COMMON_SERVER_API_H
#define COMMON_SERVER_API_H

#include "shared.h"

void SV_Init(void);
void SV_Frame(uint32_t msec);
void SV_PrepareFrame(uint32_t msec);
void SV_StartPreparedFrame(bool threaded);
void SV_EndFrame(void);
void SV_StopThread(void);
void SV_InitGameProgs(void);
void SV_Map(cstring_t mapFilename);
bool SV_GetSaveMap(cstring_t name, string_t map, uint32_t map_size);
bool SV_IsActive(void);
bool SV_NeedsFrame(void);
bool SV_HasExternalControl(void);
void SV_SetPaused(bool paused);

#endif
