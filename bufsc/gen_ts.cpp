// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// <stem>.bufs.ts: for a schema, TypeScript that reads and writes its messages (the format of docs/design.md).
//
//   enum E : ubyte {..}      ->  export enum E
//   struct S {..}            ->  export interface S, readS(), writeS()
//   table T {..}             ->  export class T (reader: getters, T.decode(bytes), T.encode(init)),
//                                export interface TInit (what encode takes), export function writeT()
//
// Readers are lazy views over the received bytes: a getter reads on every call, and nothing is copied until a
// vector or string is asked for. DataView throws RangeError on any read outside the message, so a malformed
// message cannot read memory it does not own (there is no separate verifier). The runtime is ts/src/bufs.ts.

#include "gen_ts.hpp"

namespace bufsc {

std::string generateTypeScript(const Schema& schema, const GenOptions& options) {
    const std::string path = options.outDir + "/" + stemOf(schema.mainFile) + ".bufs.ts";
    writeFile(path, TsGenerator(schema, options).run());
    return path;
}

} // namespace bufsc
