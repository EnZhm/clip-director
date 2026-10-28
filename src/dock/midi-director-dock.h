#pragma once
/* MIDI Director dock (W4, Qt). A no-op when built without the frontend API / Qt. */
#ifdef __cplusplus
extern "C" {
#endif

void clip_director_midi_dock_load(void);
void clip_director_midi_dock_unload(void);

#ifdef __cplusplus
}
#endif
