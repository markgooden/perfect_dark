/*
 * The POD boundary between the game's lights and the path tracer.
 *
 * Nothing in here names a game type or an RT64 type. PR/gbi.h and RT64's headers define
 * overlapping names, so no translation unit may include both (rt64_host.cpp:20); this is
 * what lets the C side read struct light and the C++ side build an RT64 light without
 * either seeing the other's headers.
 */

#ifndef RT64_LIGHTS_H
#define RT64_LIGHTS_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One of the game's lights, in world units, already resolved out of struct light. */
struct pdrt64Light {
	f32 x, y, z;          /* centre of the light's quad */
	f32 radius;           /* distance from that centre to its furthest corner */
	f32 dirx, diry, dirz; /* direction, normalised out of the s8 fields */
	f32 r, g, b;          /* colour times brightness */
	s32 roomnum;
	s32 sparking;         /* the light is damaged and flickering */
};

/* What was there to gather, whether or not it fitted. Counting separately from writing is
 * the point: a cap that silently truncates looks identical to a level with few lights. */
struct pdrt64LightStats {
	s32 onscreenRooms;
	s32 litRooms;
	s32 totalLights;
	s32 lightsOn;
};

/* Fills `out` with the lights of every on-screen room that are switched on, up to
 * `maxLights`, and reports what was found. Either pointer may be null. Returns how many
 * were written. */
s32 pdrt64GatherRoomLights(struct pdrt64Light *out, s32 maxLights, struct pdrt64LightStats *stats);

#ifdef __cplusplus
}
#endif

#endif
