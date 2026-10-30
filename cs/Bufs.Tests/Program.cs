// Copyright (c) 2026 Philipp Orlov

// The C# side of the cross-language test (see tests/languages/run.cmake).
//   dotnet run -- WORK_DIR      reads cpp_*.bin, writes cs_*.bin
using Bufs;
using Test;

int checks = 0, failures = 0;
void Check(bool ok, string what)
{
    checks++;
    if (!ok) { failures++; Console.Error.WriteLine($"  CHECK FAILED: {what}"); }
}

var work = args[0];
byte[] Read(string name) => File.ReadAllBytes(Path.Combine(work, name));

static Vec ReferenceVec(int seed) => new Vec { X = 1.5f + seed, Y = 2.5f, Z = -3.5f, Tag = new byte[] { 1, 2, (byte)(3 + seed) }, Flag = true, Big = -5 - seed };

static KitchenInit Reference() => new KitchenInit
{
    Tag = 0, B = true, Bt = false, I8 = 100, U8 = 7, I16 = 31000, U16 = 1, I32 = 2000000000, U32 = 3000000000, I64 = 9007199254740993,
    U64 = 18446744073709551000UL, F32 = 3.25f, F64 = 1e300, Color = Color.Blue, Text = "héllo ✓ wörld", Vec = ReferenceVec(0),
    Inner = new InnerInit { N = 42, S = "inner" }, Bytes = new byte[] { 0, 1, 254, 255 }, I8s = new sbyte[] { -128, 127 },
    Ints = new[] { -1, 0, 2147483647 }, Longs = new[] { -9223372036854775807, 0, 9223372036854775807 }, Doubles = new[] { 0.5, -1e-300 },
    Bools = new[] { true, false, true }, Names = new[] { "a", "", "ccc" }, Colors = new[] { Color.Red, Color.Green, Color.Blue },
    Inners = new[] { new InnerInit { N = 1, S = "x" }, new InnerInit { N = 2, S = "y" } }, Vecs = new[] { ReferenceVec(1), ReferenceVec(2) }, Late = 5,
};

void CheckReference(Kitchen k, string who)
{
    Check(k.Tag == 0 && k.B && !k.Bt, $"{who}: flags");
    Check(k.I8 == 100 && k.U8 == 7 && k.I16 == 31000 && k.U16 == 1, $"{who}: small ints");
    Check(k.I32 == 2000000000 && k.U32 == 3000000000 && k.I64 == 9007199254740993 && k.U64 == 18446744073709551000UL, $"{who}: wide ints");
    Check(k.F32 == 3.25f && k.F64 == 1e300 && k.Color == Color.Blue && k.Text == "héllo ✓ wörld" && k.Late == 5, $"{who}: float, enum, text");
    Check(k.Vec.X == 1.5f && k.Vec.Tag.SequenceEqual(new byte[] { 1, 2, 3 }) && k.Vec.Flag && k.Vec.Big == -5, $"{who}: struct");
    Check(k.Inner is { N: 42 } inner && inner.S == "inner", $"{who}: table");
    Check(k.Bytes.SequenceEqual(new byte[] { 0, 1, 254, 255 }) && k.I8s.SequenceEqual(new sbyte[] { -128, 127 }) && k.Ints.SequenceEqual(new[] { -1, 0, 2147483647 }), $"{who}: scalar vectors");
    Check(k.Longs.SequenceEqual(new[] { -9223372036854775807, 0, 9223372036854775807 }) && k.Doubles.SequenceEqual(new[] { 0.5, -1e-300 }), $"{who}: wide vectors");
    Check(k.Bools.SequenceEqual(new[] { true, false, true }) && k.Names.SequenceEqual(new[] { "a", "", "ccc" }), $"{who}: bool and string vectors");
    Check(k.Colors.SequenceEqual(new[] { Color.Red, Color.Green, Color.Blue }) && k.Inners.Length == 2 && k.Inners[1].S == "y", $"{who}: enum and table vectors");
    Check(k.Vecs.Length == 2 && k.Vecs[1].Tag[2] == 5 && k.Vecs[1].Big == -7, $"{who}: struct vector");
}

void CheckDefaults(Kitchen k, string who)
{
    Check(!k.B && k.Bt && k.I8 == -3 && k.U8 == 200 && k.I16 == -1000 && k.U16 == 65000, $"{who}: small defaults");
    Check(k.I32 == -123456 && k.U32 == 4000000000 && k.I64 == -9007199254740993 && k.U64 == ulong.MaxValue, $"{who}: wide defaults");
    Check(k.F32 == 1.5f && k.F64 == -2.25 && k.Color == Color.Green && k.Text == "" && k.Late == 99, $"{who}: other defaults");
    Check(k.Inner == null && k.Vec.X == 0 && k.Bytes.Length == 0 && k.Ints.Length == 0 && k.Names.Length == 0 && k.Vecs.Length == 0, $"{who}: empty");
}

CheckReference(Kitchen.Decode(Read("cpp_ref.bin")), "C++ -> C#");
CheckDefaults(Kitchen.Decode(Read("cpp_defaults.bin")), "C++ -> C#");

var refBytes = Kitchen.Encode(Reference());
var defaultBytes = Kitchen.Encode(new KitchenInit { Tag = 1 });
CheckReference(Kitchen.Decode(refBytes), "C# round trip");
CheckDefaults(Kitchen.Decode(defaultBytes), "C# round trip");
Check(refBytes.AsSpan().SequenceEqual(Read("cpp_ref.bin")), "the encoding is byte for byte what C++ writes");
Check(defaultBytes.AsSpan().SequenceEqual(Read("cpp_defaults.bin")), "the defaults encoding is byte for byte what C++ writes");

foreach (var (bad, what) in new[] { (refBytes[..20], "truncated"), (new byte[16], "zeroes") })
{
    try { var m = Kitchen.Decode(bad); _ = m.Text; Check(false, $"{what}: expected an error"); }
    catch (Exception e) when (e is MalformedMessageException or ArgumentException or IndexOutOfRangeException) { }
}

File.WriteAllBytes(Path.Combine(work, "cs_ref.bin"), refBytes);
File.WriteAllBytes(Path.Combine(work, "cs_defaults.bin"), defaultBytes);
Console.WriteLine($"{checks} checks, {failures} failed");
return failures == 0 ? 0 : 1;
