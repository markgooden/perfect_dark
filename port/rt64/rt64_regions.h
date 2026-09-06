#pragma once

/*
 * Where the port's memory lives, for the two pieces that have to know.
 *
 * Display lists reach four regions, not one (docs/census.md, and the Region
 * enum in rt64_mem.h says what each is for). Capture classifies references so
 * a .pddl records where they came from; the live backend needs the same spans
 * to build a LiveMemReader the translator can read through. Both used to be
 * able to answer "is this address in the executable's image?" only because
 * rt64_capture.cpp worked it out privately, which is exactly the fact that
 * would drift once a second caller wanted it.
 *
 * Two of the spans are port globals and need no help (g_MempHeap, g_RomFile).
 * The image is the one that has to be discovered, from the PE headers rather
 * than guessed, and that discovery lives here.
 *
 * This header deliberately does not include rt64_mem.h's implementation or any
 * port header, so the cost of including it is small.
 */

#include <cstddef>
#include <cstdint>

namespace pdrt64 {

/*
 * Finds the executable's own image span. Idempotent, and safe to call before
 * anything else has been set up. On a platform where it has not been wired up
 * the span stays empty, which classifies those references as unreadable rather
 * than as something to dereference blindly - the safe failure.
 */
void regionsInit();

/* The image span, or {0, 0} if regionsInit has not run or could not find it.
 * Mapped for the process lifetime, so reading inside it is safe. */
uintptr_t regionsImageBase();
size_t regionsImageSize();

/* True if [addr, addr+len) lies entirely inside the executable's image. */
bool regionsInImage(uintptr_t addr, size_t len);

/* True if [addr, addr+len) lies entirely inside [base, base+size). The null
 * and zero-length cases are false rather than a matter of opinion, and the
 * bound is computed once so a caller cannot get the overflow case wrong. */
bool regionsInSpan(uintptr_t addr, size_t len, const void *base, size_t size);

} // namespace pdrt64
