/*
 * Debug readback of the traced image.
 *
 * Deliberately not part of rt64_host.h. That header is the authoritative spec for the
 * backend (CLAUDE.md), and this is a measurement hook, not something the game asks the
 * backend to do.
 *
 * It exists because the traced image never reaches RDRAM. dlreplay hashes an RDRAM range,
 * the tracer composites into a target State::fullSync never copies back, and the two were
 * measured to be disjoint (docs/TASKLOG.md, 2026-09-09). Without this, an RT change that
 * alters only pixels has no automated check at all.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace pdrt64 {
    /* Fills `rgba` with the last traced frame, four bytes per pixel, rows top down and
     * tightly packed. Returns false when there is nothing to read - raytracing off, no
     * scene traced yet, or a target format this does not handle - which is a normal
     * answer and not an error. */
    bool rtDebugReadbackImage(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height);
}
