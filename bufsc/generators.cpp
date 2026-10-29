// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "generators.hpp"

#include <fstream>

namespace bufsc {

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw SchemaError(path + ": cannot write");
    out << text;
}

} // namespace bufsc
