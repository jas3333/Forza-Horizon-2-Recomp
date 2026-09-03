/**
 * @file        system/memory_provenance.h
 * @brief       Filtered guest-memory write provenance for crash diagnostics
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>

struct PPCContext;

namespace rex::runtime {

/** Copy the process-wide memory-watch configuration into a new guest context. */
void ConfigureMemoryProvenanceContext(PPCContext& context) noexcept;

/** Record one guest store that overlaps the selected memory range. */
void RecordMemoryProvenanceStore(const PPCContext& context, uint32_t address,
                                 uint32_t size, uint64_t value) noexcept;

/** Write the retained memory-store records to AVDIAG. */
void LogMemoryProvenance() noexcept;

}  // namespace rex::runtime
