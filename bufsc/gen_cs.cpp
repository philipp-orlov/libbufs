// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// <stem>.bufs.cs: for a schema, C# that reads and writes its messages (the format of docs/design.md).
//
//   enum E : ubyte {..}      ->  public enum E : byte
//   struct S {..}            ->  public struct S (fields, Read/Write at an offset)
//   table T {..}             ->  public readonly struct T (reader: properties over the received bytes,
//                                T.Decode(bytes), T.Encode(TInit)) and public sealed class TInit (what Encode takes)
//
// Readers are views over the received bytes: a property reads on every call and nothing is copied until a vector or
// string is asked for (byte and other scalar vectors are ReadOnlySpan<T> over the message). Out-of-range offsets
// throw (spans are bounds-checked), so a malformed message cannot read memory it does not own. The runtime is cs/Bufs.

#include "gen_cs.hpp"

namespace bufsc {

std::string generateCSharp(const Schema& schema, const GenOptions& options) {
    const std::string path = options.outDir + "/" + stemOf(schema.mainFile) + ".bufs.cs";
    writeFile(path, CsGenerator(schema, options).run());
    return path;
}

} // namespace bufsc
