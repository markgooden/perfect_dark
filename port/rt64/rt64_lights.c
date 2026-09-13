/*
 * The game's own lights, gathered for the path tracer.
 *
 * Perfect Dark's level lighting is baked into vertex colours, and its RSP light path
 * carries almost nothing - measured on an in-level frame, 32 of 5878 vertices had a
 * non-zero light count (docs/TASKLOG.md, 2026-09-09). So the lights a path tracer needs
 * cannot come from the display list. They come from here.
 *
 * struct light (types.h:5308) is a real positioned area light: four bbox corners, a
 * direction, a colour and a brightness, per room, with runtime state - `on`, `sparking`,
 * `healthy` - which is why lights can be shot out in this game.
 *
 * This is C because it reaches game headers. The rt64 side cannot: PR/gbi.h and RT64's
 * headers define overlapping names, so no translation unit may include both
 * (port/rt64/rt64_host.cpp:20). The two halves meet at a POD array and nothing else.
 *
 * Reads game state and never writes it - invariant 2 keeps src/ untouched.
 */

/* The game's own include order, which its .c files all follow (src/game/dlights.c:1).
 * ultra64.h has to come first, and stdbool.h must not appear at all: types.h defines bool
 * as s32 and the two collide. */
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "game/dlights.h"

#include <math.h>

#include "rt64_lights.h"

int32_t pdrt64GatherRoomLights(struct pdrt64Light *out, int32_t maxLights, struct pdrt64LightStats *stats)
{
	s32 written = 0;

	if (stats) {
		stats->nearbyRooms = 0;
		stats->onscreenRooms = 0;
		stats->litRooms = 0;
		stats->totalLights = 0;
		stats->lightsOn = 0;
	}

	if (!g_Rooms || g_Vars.roomcount <= 0) {
		return 0;
	}

	/* Where the camera is, for deciding which lights are worth keeping when there are more
	 * than the tracer can afford. Null before a player exists, which is every menu frame. */
	const struct player *player = g_Vars.currentplayer;
	const f32 camx = player ? player->cam_pos.x : 0.0f;
	const f32 camy = player ? player->cam_pos.y : 0.0f;
	const f32 camz = player ? player->cam_pos.z : 0.0f;

	for (s32 roomnum = 0; roomnum < g_Vars.roomcount; roomnum++) {
		const struct room *room = &g_Rooms[roomnum];
		const s32 onscreen = (room->flags & ROOMFLAG_ONSCREEN) != 0;

		/* Rooms that are visible, and rooms that were visible recently.
		 *
		 * Only on-screen rooms were gathered before, which meant a light never lit anything
		 * outside its own room: stand in a doorway and the room behind you stops
		 * contributing the instant it leaves the frustum, though its light still falls on
		 * the floor in front of you. loaded240 is the game's own answer to "is this room
		 * still relevant" - 1 while visible, ticking down from 120 after - and it is exactly
		 * the set a path tracer wants, because light arrives from places the camera cannot
		 * see. That is most of the point of tracing it. */
		if (room->loaded240 == 0) {
			continue;
		}

		if (stats) {
			stats->nearbyRooms++;
			if (onscreen) {
				stats->onscreenRooms++;
			}
		}

		const s32 numlights = room->numlights;
		if (numlights <= 0) {
			continue;
		}

		if (stats) {
			stats->litRooms++;
			stats->totalLights += numlights;
		}

		for (s32 i = 0; i < numlights; i++) {
			struct light *light = roomGetLight(roomnum, i);
			if (!light || !light->on) {
				continue;
			}

			if (stats) {
				stats->lightsOn++;
			}

			if (!out || written >= maxLights) {
				continue;
			}

			struct pdrt64Light *dst = &out[written++];

			/* Centre and extent of the quad the light occupies. bbox is four corners,
			 * so this is the average rather than a min/max pair: a degenerate corner
			 * would drag a bounding box but not the centroid. */
			f32 cx = 0.0f, cy = 0.0f, cz = 0.0f;
			for (s32 c = 0; c < 4; c++) {
				cx += (f32)light->bbox[c].x;
				cy += (f32)light->bbox[c].y;
				cz += (f32)light->bbox[c].z;
			}

			dst->x = cx * 0.25f;
			dst->y = cy * 0.25f;
			dst->z = cz * 0.25f;

			f32 radius = 0.0f;
			for (s32 c = 0; c < 4; c++) {
				const f32 dx = (f32)light->bbox[c].x - dst->x;
				const f32 dy = (f32)light->bbox[c].y - dst->y;
				const f32 dz = (f32)light->bbox[c].z - dst->z;
				const f32 d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > radius) {
					radius = d2;
				}
			}

			/* Squared while comparing, rooted once. */
			dst->radius = (radius > 0.0f) ? sqrtf(radius) : 0.0f;

			dst->dirx = (f32)light->dirx / 127.0f;
			dst->diry = (f32)light->diry / 127.0f;
			dst->dirz = (f32)light->dirz / 127.0f;

			/* colour is 4/4/4/4. The low nibble is unused by the light itself. */
			const f32 brightness = (f32)light->brightness / 255.0f;
			dst->r = (f32)((light->colour >> 12) & 0xf) / 15.0f * brightness;
			dst->g = (f32)((light->colour >> 8) & 0xf) / 15.0f * brightness;
			dst->b = (f32)((light->colour >> 4) & 0xf) / 15.0f * brightness;

			/* The room's own size. A light is authored to light its room, so the room is
			 * what its reach should be - which beats the multiple of the fitting's extent
			 * that stood here, where a small fitting in a large hall lit a sphere around
			 * itself and nothing else. */
			dst->roomradius = room->radius;

			const f32 lx = dst->x - camx;
			const f32 ly = dst->y - camy;
			const f32 lz = dst->z - camz;
			dst->camdist = sqrtf(lx * lx + ly * ly + lz * lz);

			dst->roomnum = roomnum;
			dst->sparking = light->sparking ? 1 : 0;
		}
	}

	return written;
}
