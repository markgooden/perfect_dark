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

/* stdint, not PR/ultratypes.h: this header is compiled by the shim too, which has the
 * game's include path nowhere near it. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One of the game's lights, in world units, already resolved out of struct light. */
struct pdrt64Light {
	float x, y, z;          /* centre of the light's quad */
	float radius;           /* distance from that centre to its furthest corner */
	float dirx, diry, dirz; /* direction, normalised out of the s8 fields */
	float r, g, b;          /* colour times brightness */
	int32_t roomnum;
	int32_t sparking;     /* the light is damaged and flickering */
};

/* What was there to gather, whether or not it fitted. Counting separately from writing is
 * the point: a cap that silently truncates looks identical to a level with few lights. */
struct pdrt64LightStats {
	int32_t onscreenRooms;
	int32_t litRooms;
	int32_t totalLights;
	int32_t lightsOn;
};

/* Fills `out` with the lights of every on-screen room that are switched on, up to
 * `maxLights`, and reports what was found. Either pointer may be null. Returns how many
 * were written. */
int32_t pdrt64GatherRoomLights(struct pdrt64Light *out, int32_t maxLights, struct pdrt64LightStats *stats);

#ifdef __cplusplus
}

namespace pdrt64 {
	/* Hands a frame's lights to the path tracer, replacing the previous set whole. In the
	 * game build this is the DLL loader forwarding across the shim boundary; in dlreplay it
	 * is the direct implementation. Both live beside hostUpdateScreen. */
	void hostSetRoomLights(const ::pdrt64Light *lights, int count);
}
#endif

#endif
