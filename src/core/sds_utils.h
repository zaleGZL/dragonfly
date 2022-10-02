// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <cstdint>

namespace dfly {

char* AllocSdsWithSpace(uint32_t strlen, uint32_t space);

}  // namespace dfly
