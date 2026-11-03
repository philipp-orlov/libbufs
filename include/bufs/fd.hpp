// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// An owned file descriptor, closed when destroyed. Move-only. Lives in libcore's libsys now.

#include "sys/fd.hpp"

namespace bufs {
using Fd = sys::Fd;
} // namespace bufs
