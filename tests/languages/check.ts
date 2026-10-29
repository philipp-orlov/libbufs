// Copyright (c) 2026 Philipp Orlov

// The TypeScript side of the cross-language test (see run.cmake).
//   node check.mjs WORK_DIR     reads cpp_*.bin, writes ts_*.bin
import * as fs from 'node:fs';
import * as path from 'node:path';
import { Color, Kitchen, KitchenInit, Vec } from './kitchen.bufs';

const work = process.argv[2];
let checks = 0;
let failures = 0;
function check(ok: boolean, what: string): void {
  checks++;
  if (!ok) {
    failures++;
    console.error(`  CHECK FAILED: ${what}`);
  }
}
const same = (a: ArrayLike<unknown>, b: ArrayLike<unknown>): boolean => a.length === b.length && Array.from(a).every((x, i) => x === b[i]);
const read = (name: string): Uint8Array => new Uint8Array(fs.readFileSync(path.join(work, name)));

const vec = (seed: number): Vec => ({ x: 1.5 + seed, y: 2.5, z: -3.5, tag: [1, 2, 3 + seed], flag: true, big: BigInt(-5 - seed) });
const sameVec = (a: Vec, b: Vec): boolean => a.x === b.x && a.y === b.y && a.z === b.z && same(a.tag, b.tag) && a.flag === b.flag && a.big === b.big;

const reference: KitchenInit = {
  tag: 0, b: true, bt: false, i8: 100, u8: 7, i16: 31000, u16: 1, i32: 2000000000, u32: 3000000000, i64: 9007199254740993n,
  u64: 18446744073709551000n, f32: 3.25, f64: 1e300, color: Color.Blue, text: 'héllo ✓ wörld', vec: vec(0),
  inner: { n: 42, s: 'inner' }, bytes: new Uint8Array([0, 1, 254, 255]), i8s: [-128, 127], ints: [-1, 0, 2147483647],
  longs: [-9223372036854775807n, 0n, 9223372036854775807n], doubles: [0.5, -1e-300], bools: [true, false, true],
  names: ['a', '', 'ccc'], colors: [Color.Red, Color.Green, Color.Blue], inners: [{ n: 1, s: 'x' }, { n: 2, s: 'y' }],
  vecs: [vec(1), vec(2)], late: 5,
};

function checkReference(m: Kitchen, who: string): void {
  check(m.tag === 0 && m.b && !m.bt, `${who}: flags`);
  check(m.i8 === 100 && m.u8 === 7 && m.i16 === 31000 && m.u16 === 1, `${who}: small ints`);
  check(m.i32 === 2000000000 && m.u32 === 3000000000 && m.i64 === 9007199254740993n && m.u64 === 18446744073709551000n, `${who}: wide ints`);
  check(m.f32 === 3.25 && m.f64 === 1e300 && m.color === Color.Blue && m.late === 5, `${who}: float, enum`);
  check(m.text === 'héllo ✓ wörld', `${who}: text`);
  check(sameVec(m.vec, vec(0)), `${who}: struct`);
  check(m.inner !== null && m.inner.n === 42 && m.inner.s === 'inner', `${who}: table`);
  check(same(m.bytes, [0, 1, 254, 255]) && same(m.i8s, [-128, 127]) && same(m.ints, [-1, 0, 2147483647]), `${who}: scalar vectors`);
  check(same(m.longs, [-9223372036854775807n, 0n, 9223372036854775807n]) && same(m.doubles, [0.5, -1e-300]), `${who}: wide vectors`);
  check(same(m.bools, [true, false, true]) && same(m.names, ['a', '', 'ccc']), `${who}: bool and string vectors`);
  check(same(m.colors, [Color.Red, Color.Green, Color.Blue]), `${who}: enum vector`);
  check(m.inners.length === 2 && m.inners[0].n === 1 && m.inners[1].s === 'y', `${who}: table vector`);
  check(m.vecs.length === 2 && sameVec(m.vecs[0], vec(1)) && sameVec(m.vecs[1], vec(2)), `${who}: struct vector`);
}

function checkDefaults(m: Kitchen, who: string): void {
  check(!m.b && m.bt && m.i8 === -3 && m.u8 === 200 && m.i16 === -1000 && m.u16 === 65000, `${who}: small defaults`);
  check(m.i32 === -123456 && m.u32 === 4000000000 && m.i64 === -9007199254740993n && m.u64 === 18446744073709551615n, `${who}: wide defaults`);
  check(m.f32 === 1.5 && m.f64 === -2.25 && m.color === Color.Green && m.text === '' && m.late === 99, `${who}: other defaults`);
  check(m.inner === null && m.bytes.length === 0 && m.ints.length === 0 && m.names.length === 0 && m.vecs.length === 0, `${who}: empty`);
}

checkReference(Kitchen.decode(read('cpp_ref.bin')), 'C++ -> TypeScript');
checkDefaults(Kitchen.decode(read('cpp_defaults.bin')), 'C++ -> TypeScript');

const ref = Kitchen.encode(reference);
const defaults = Kitchen.encode({ tag: 1 });
checkReference(Kitchen.decode(ref), 'TypeScript round trip');
checkDefaults(Kitchen.decode(defaults), 'TypeScript round trip');
check(Buffer.compare(ref, read('cpp_ref.bin')) === 0, 'the encoding is byte for byte what C++ writes');
check(Buffer.compare(defaults, read('cpp_defaults.bin')) === 0, 'the defaults encoding is byte for byte what C++ writes');

for (const [bad, what] of [[ref.slice(0, 20), 'truncated'], [new Uint8Array(16), 'zeroes']] as const) {
  try {
    const m = Kitchen.decode(bad);
    void m.text;
    check(false, `${what}: expected an error`);
  } catch {
    /* the reader refused it */
  }
}

fs.writeFileSync(path.join(work, 'ts_ref.bin'), ref);
fs.writeFileSync(path.join(work, 'ts_defaults.bin'), defaults);
console.log(`${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
