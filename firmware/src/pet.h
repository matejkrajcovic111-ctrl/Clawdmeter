#pragma once
#include <stdbool.h>
#include <stdint.h>

// clawdmeter2 pet mode: a tap on the title brings the face up (the director's
// B_PET beat) and it's yours to pet. Boop it (tap), stroke it (rub sideways),
// it watches your finger, shake it (dizzy), tilt it (the eyes roll). It sweats
// above 90 %, watches a thinking dot while Claude works, gets bored, sleepy at
// night, stretches in the morning, sneezes, hiccups and yawns. Mood and gag
// timing are pet_mood.h (pure); this drives the face (face.h) and the props
// (pet_props.h) from them. Neon green while account 2 is shown.

void pet_init(void);                 // after face_init()
void pet_enter(void);                // the face is up and settled (director)
void pet_leave(void);                // props off, eyes back (director, before the drop)
bool pet_active(void);
void pet_tick(uint32_t now);         // every frame while active (director)
bool pet_done(uint32_t now);         // left alone long enough (or the panel slept): back to the meters

void pet_tap(int x, int y);          // a boop (main.cpp, on release)
void pet_touch(bool pressed, int x, int y);   // raw touch while active: eyes follow, strokes
void pet_celebrate(void);            // the 5-hour window reset: confetti
void pet_set_usage(float max_pct);   // the shown account's highest %, <0 unknown
