# Copyright (c) 2026 Philipp Orlov

"""The Python side of the cross-language test (see run.cmake).
   python3 check.py GEN_DIR WORK_DIR     GEN_DIR holds kitchen_bufs.py, WORK_DIR cpp_*.bin; writes py_*.bin"""
import os
import struct
import sys

sys.path.insert(0, sys.argv[1])
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python"))

import kitchen_bufs as k  # noqa: E402
from bufs import MalformedMessage  # noqa: E402

work = sys.argv[2]
checks = failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print(f"  CHECK FAILED: {what}", file=sys.stderr)


def vec(seed):
    return k.Vec(x=1.5 + seed, y=2.5, z=-3.5, tag=[1, 2, 3 + seed], flag=True, big=-5 - seed)


def reference_init():
    return dict(
        tag=0, b=True, bt=False, i8=100, u8=7, i16=31000, u16=1, i32=2000000000, u32=3000000000, i64=9007199254740993,
        u64=18446744073709551000, f32=3.25, f64=1e300, color=k.Color.Blue, text="héllo ✓ wörld", vec=vec(0),
        inner=dict(n=42, s="inner"), bytes=bytes([0, 1, 254, 255]), i8s=[-128, 127], ints=[-1, 0, 2147483647],
        longs=[-9223372036854775807, 0, 9223372036854775807], doubles=[0.5, -1e-300], bools=[True, False, True],
        names=["a", "", "ccc"], colors=[k.Color.Red, k.Color.Green, k.Color.Blue],
        inners=[dict(n=1, s="x"), dict(n=2, s="y")], vecs=[vec(1), vec(2)], late=5)


def check_reference(m, who):
    check(m.tag == 0 and m.b is True and m.bt is False, f"{who}: flags")
    check(m.i8 == 100 and m.u8 == 7 and m.i16 == 31000 and m.u16 == 1, f"{who}: small ints")
    check(m.i32 == 2000000000 and m.u32 == 3000000000 and m.i64 == 9007199254740993 and m.u64 == 18446744073709551000, f"{who}: wide ints")
    check(m.f32 == 3.25 and m.f64 == 1e300 and m.color == k.Color.Blue and m.late == 5, f"{who}: float, enum")
    check(m.text == "héllo ✓ wörld", f"{who}: text")
    check(m.vec == vec(0), f"{who}: struct")
    check(m.inner is not None and m.inner.n == 42 and m.inner.s == "inner", f"{who}: table")
    check(m.bytes == bytes([0, 1, 254, 255]) and m.i8s == [-128, 127] and m.ints == [-1, 0, 2147483647], f"{who}: scalar vectors")
    check(m.longs == [-9223372036854775807, 0, 9223372036854775807] and m.doubles == [0.5, -1e-300], f"{who}: wide vectors")
    check(m.bools == [True, False, True] and m.names == ["a", "", "ccc"], f"{who}: bool and string vectors")
    check(m.colors == [k.Color.Red, k.Color.Green, k.Color.Blue], f"{who}: enum vector")
    check([i.n for i in m.inners] == [1, 2] and m.inners[1].s == "y", f"{who}: table vector")
    check(m.vecs == [vec(1), vec(2)], f"{who}: struct vector")


def check_defaults(m, who):
    check(m.b is False and m.bt is True and m.i8 == -3 and m.u8 == 200 and m.i16 == -1000 and m.u16 == 65000, f"{who}: small defaults")
    check(m.i32 == -123456 and m.u32 == 4000000000 and m.i64 == -9007199254740993 and m.u64 == 18446744073709551615, f"{who}: wide defaults")
    check(m.f32 == 1.5 and m.f64 == -2.25 and m.color == k.Color.Green and m.text == "" and m.late == 99, f"{who}: other defaults")
    check(m.inner is None and m.bytes == b"" and m.ints == [] and m.names == [] and m.vecs == [] and m.vec == k.Vec(), f"{who}: empty")


def read(name):
    with open(os.path.join(work, name), "rb") as f:
        return f.read()


check_reference(k.Kitchen.decode(read("cpp_ref.bin")), "C++ -> Python")
check_defaults(k.Kitchen.decode(read("cpp_defaults.bin")), "C++ -> Python")

ref = k.Kitchen.encode(reference_init())
defaults = k.Kitchen.encode(tag=1)
check_reference(k.Kitchen.decode(ref), "Python round trip")
check_defaults(k.Kitchen.decode(defaults), "Python round trip")
check(ref == read("cpp_ref.bin"), "the encoding is byte for byte what C++ writes")
check(defaults == read("cpp_defaults.bin"), "the defaults encoding is byte for byte what C++ writes")
check(k.Kitchen.decode(ref).to_dict()["inner"] == {"n": 42, "s": "inner"}, "to_dict")

# a malformed message raises instead of reading outside itself
for bad, what in ((ref[:20], "truncated"), (b"\x00" * 16, "zeroes"), (read("cpp_ref.bin")[:-8] + b"\xff" * 8, "garbage tail")):
    try:
        m = k.Kitchen.decode(bad)
        _ = (m.text, m.names, m.vecs, m.inners)
        check(what == "garbage tail", f"{what}: expected an error")
    except (MalformedMessage, struct.error, IndexError, ValueError):
        pass

with open(os.path.join(work, "py_ref.bin"), "wb") as f:
    f.write(ref)
with open(os.path.join(work, "py_defaults.bin"), "wb") as f:
    f.write(defaults)
print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
