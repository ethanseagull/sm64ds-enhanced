// interp3ds.h -- 60 fps interpolation on the step 5 record (interp3ds.cpp).
#pragma once

#include <vector>

#include "ntr/gx.h"

// GAME3DS_INTERP=1 (the in-between frames) or GAME3DS_INTERP_STATS=1 (the
// counters alone; on an Old 3DS only =2): card3ds asks gx_take_record for
// each id's factors.
bool interp3ds_wanted();
// Whether the in-between frames are on (GAME3DS_INTERP=1 at first, then the
// options menu's 60 fps switch), and that switch: set between ticks, with the
// present idle (card3ds_set_options). Off also stops GAME3DS_INTERP_STATS.
bool interp3ds_on();
void interp3ds_set_on(bool on);
// Each record card3ds takes with want_raw: pairs its ids with the previous record's and, when this tick gets
// an in-between frame, fills `mid` (one matrix per id, as rec.mtx) and
// answers true. `mid_v` is left empty, or (when a mesh's own vertices move
// from tick to tick) is the in-between's copy of rec.v. rec.raw is taken (a
// swap): it comes back holding an older buffer, not this record's factors.
// `view` is the ROM's view matrix as it stood when the tick was taken
// (interp3ds_capture_view), or null. card3ds calls this on core 2, in the
// present worker, after the game thread is released: everything here is that
// thread's alone.
bool interp3ds_take(ntr::GxRecord &rec, std::vector<ntr::GxRecMatrix> &mid,
                    std::vector<ntr::GxRecVertex> &mid_v, const int32_t *view);
// The view matrix now (data_0209b3ec, 4.12), for a take that runs later:
// false when no game is linked (the card's own test executable).
bool interp3ds_capture_view(int32_t out[12]);
// A tick the card did not take from a record: the next one has no previous.
void interp3ds_reset();
