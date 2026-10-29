/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RUNNER_LOAD_H
#define DOA2U_RUNNER_LOAD_H

#include "xbe_parser.h"

/* fileAuthenticated: the caller has pinned the whole file's SHA-256, so a
   stale section digest in it is a known property of that file, not damage.
   Such sections are loaded and named on stderr. */
bool recomp_load_xbe(const doaxbv::XbeParseResult& parsed,
    std::vector<std::uint8_t>& memory, std::string& error,
    bool fileAuthenticated = false);

#endif
