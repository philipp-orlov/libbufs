// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// <stem>_bufs.py: for a schema, Python that reads and writes its messages (the format of docs/design.md).
//
//   enum E : ubyte {..}      ->  class E(IntEnum)
//   struct S {..}            ->  class S(Struct): fields as attributes, S._read(buf, at), s._write(buf, at)
//   table T {..}             ->  class T: a reader (properties over the received bytes), T.decode(data),
//                                T.encode(dict or keywords), t.to_dict()
//
// Readers are views over the received bytes (a memoryview): a property reads on every call and nothing is copied until
// a vector or string is asked for. Out-of-range offsets raise (struct.error / MalformedMessage), so a malformed message
// cannot read memory it does not own. The runtime is python/bufs.

#include "gen_py.hpp"

namespace bufsc {

std::string generatePython(const Schema& schema, const GenOptions& options) {
    const std::string path = options.outDir + "/" + stemOf(schema.mainFile) + "_bufs.py";
    writeFile(path, PyGenerator(schema, options).run());
    return path;
}

} // namespace bufsc
