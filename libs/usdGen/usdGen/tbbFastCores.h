// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

// tbbFastCores.h — shared big.LITTLE placement for TBB arenas.
//
// TBB workers are born spread over every allowed core and stick where born,
// so on big.LITTLE silicon some workers run 2-3x slower on little cores
// while the span waits for them (short bursts never look busy enough for
// the scheduler to promote). Observing an arena pins its workers to
// max-frequency cores. Everything fails closed: unreadable topology,
// homogeneous frequencies, too few fast cores for the arena, or
// USDGEN_NO_FAST_CORE_PIN=1 all leave placement to the OS.

#pragma once

#include <tbb/task_arena.h>
#include <tbb/task_scheduler_observer.h>

#include <memory>

namespace usdGen {

// Pins `arena`'s workers to max-frequency cores where the topology
// qualifies. Returns the observer, which must stay alive while the arena
// runs (declare it after the arena so it detaches first), or nullptr when
// the topology is homogeneous, undetectable, or disabled. `workers` is the
// arena's concurrency; fewer fast cores than workers declines rather than
// oversubscribing.
std::unique_ptr<tbb::task_scheduler_observer> ObserveFastCores(
    tbb::task_arena& arena, int workers);

} // namespace usdGen
