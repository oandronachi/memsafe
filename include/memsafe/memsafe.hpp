/**
 * @file memsafe.hpp
 * @brief Single public umbrella header for the header-only memsafe library.
 *
 * @details
 * Work package: CPP_MEMSAFE-0030-FUNC.
 *
 * Purpose:
 * - Provide one include entry point, `<memsafe/memsafe.hpp>`, for the public
 *   memsafe API.
 * - Include configuration plumbing plus the accepted six feature-header
 *   roster: `violation.hpp`, `backend.hpp`, `owner.hpp`, `handle.hpp`,
 *   `scope.hpp`, and `sync.hpp`.
 * - Keep the Slice 0 package purely header-only while allowing later FUNC work
 *   packages to replace forward-slice stubs with full feature implementations.
 *
 * Key invariants:
 * - This header contributes no runtime definitions, storage, or dependencies
 *   beyond what the included headers provide.
 * - `config.hpp` is support plumbing and is not counted as a feature header.
 * - The only feature headers included here are the six accepted Q4 roster
 *   members.
 * - Every roster member is included unconditionally. The CPP_MEMSAFE-0030-FUNC
 *   acceptance criteria require the umbrella to include exactly the six feature
 *   headers plus configuration; conditional probing would leave the roster
 *   dependent on filesystem state rather than the public contract.
 * - `config.hpp` is included before feature headers so configuration defaults
 *   and compatibility probes are visible consistently.
 *
 * Ownership and thread-safety:
 * - The umbrella itself owns no resources and has no runtime state. Including
 *   it is thread-safe by construction.
 *
 * @note The work-package notes permit `owner.hpp`, `handle.hpp`, `scope.hpp`,
 * and `sync.hpp` to be empty Slice 0 stubs, with later FUNC work packages
 * filling them. This umbrella deliberately declares no substitute API for those
 * headers; each feature header remains the ownership boundary for its public
 * symbols.
 */
#ifndef MEMSAFE_MEMSAFE_HPP
#define MEMSAFE_MEMSAFE_HPP

#include <memsafe/config.hpp>

#include <memsafe/violation.hpp>
#include <memsafe/backend.hpp>
#include <memsafe/owner.hpp>
#include <memsafe/handle.hpp>
#include <memsafe/scope.hpp>
#include <memsafe/sync.hpp>

#endif /* MEMSAFE_MEMSAFE_HPP */
