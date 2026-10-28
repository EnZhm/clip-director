#pragma once
/* Clip Director dock (Qt). A no-op when built without the frontend API / Qt. */
#ifdef __cplusplus
extern "C" {
#endif

void clip_director_dock_load(void);

#ifdef __cplusplus
}
#endif
