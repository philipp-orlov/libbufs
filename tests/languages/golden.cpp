// The C++ side of the cross-language test (see run.cmake): writes the reference messages of kitchen.bufs, and checks the
// ones the other languages wrote.
//   golden write DIR      DIR/cpp_ref.bin, DIR/cpp_defaults.bin
//   golden verify DIR     checks DIR/<lang>_ref.bin and DIR/<lang>_defaults.bin for ts, cs and py
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "bufs/bufs.hpp"
#include "kitchen.bufs.hpp"

namespace test {

inline Vec referenceVec(int seed) {
    Vec v{};
    v.x = 1.5f + static_cast<float>(seed);
    v.y = 2.5f;
    v.z = -3.5f;
    v.tag = {1, 2, static_cast<uint8_t>(3 + seed)};
    v.flag = 1;
    v.big = -5 - seed;
    return v;
}

inline void fillReference(Kitchen::Builder& k) {
    k.setTag(0);
    k.setB(true);
    k.setBt(false);
    k.setI8(100);
    k.setU8(7);
    k.setI16(31000);
    k.setU16(1);
    k.setI32(2000000000);
    k.setU32(3000000000u);
    k.setI64(9007199254740993ll);
    k.setU64(18446744073709551000ull);
    k.setF32(3.25f);
    k.setF64(1e300);
    k.setColor(Color::Blue);
    k.setText("h\xc3\xa9llo \xe2\x9c\x93 w\xc3\xb6rld");
    k.setVec(referenceVec(0));
    auto inner = k.initInner();
    inner.setN(42);
    inner.setS("inner");
    const uint8_t bytes[] = {0, 1, 254, 255};
    k.setBytes(bytes, 4);
    const int8_t i8s[] = {-128, 127};
    k.setI8s(i8s, 2);
    const int32_t ints[] = {-1, 0, 2147483647};
    k.setInts(ints, 3);
    const int64_t longs[] = {-9223372036854775807ll, 0, 9223372036854775807ll};
    k.setLongs(longs, 3);
    const double doubles[] = {0.5, -1e-300};
    k.setDoubles(doubles, 2);
    auto bools = k.initBools(3);
    bools.set(0, true);
    bools.set(1, false);
    bools.set(2, true);
    auto names = k.initNames(3);
    names.set(0, "a");
    names.set(1, "");
    names.set(2, "ccc");
    const Color colors[] = {Color::Red, Color::Green, Color::Blue};
    k.setColors(colors, 3);
    auto inners = k.initInners(2);
    auto first = inners.init(0);
    first.setN(1);
    first.setS("x");
    auto second = inners.init(1);
    second.setN(2);
    second.setS("y");
    const Vec vecs[] = {referenceVec(1), referenceVec(2)};
    k.setVecs(vecs, 2);
    k.setLate(5);
}

#define KITCHEN_EXPECT(cond)                                                          \
    do {                                                                              \
        if (!(cond)) {                                                                \
            out << #cond << "; ";                                                     \
        }                                                                             \
    } while (0)

inline bool sameVec(const Vec& a, const Vec& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.tag == b.tag && a.flag == b.flag && a.big == b.big;
}

/// "" when `k` holds the reference values (tag 0) or the defaults (tag 1); otherwise what differs.
inline std::string checkKitchen(const Kitchen::Reader& k) {
    std::ostringstream out;
    if (k.tag() == 0) {
        KITCHEN_EXPECT(k.b());
        KITCHEN_EXPECT(!k.bt());
        KITCHEN_EXPECT(k.i8() == 100);
        KITCHEN_EXPECT(k.u8() == 7);
        KITCHEN_EXPECT(k.i16() == 31000);
        KITCHEN_EXPECT(k.u16() == 1);
        KITCHEN_EXPECT(k.i32() == 2000000000);
        KITCHEN_EXPECT(k.u32() == 3000000000u);
        KITCHEN_EXPECT(k.i64() == 9007199254740993ll);
        KITCHEN_EXPECT(k.u64() == 18446744073709551000ull);
        KITCHEN_EXPECT(k.f32() == 3.25f);
        KITCHEN_EXPECT(k.f64() == 1e300);
        KITCHEN_EXPECT(k.color() == Color::Blue);
        KITCHEN_EXPECT(k.text() == "h\xc3\xa9llo \xe2\x9c\x93 w\xc3\xb6rld");
        KITCHEN_EXPECT(sameVec(k.vec(), referenceVec(0)));
        KITCHEN_EXPECT(k.hasInner() && k.inner().n() == 42 && k.inner().s() == "inner");
        KITCHEN_EXPECT(k.bytes().size() == 4 && k.bytes()[0] == 0 && k.bytes()[2] == 254 && k.bytes()[3] == 255);
        KITCHEN_EXPECT(k.i8s().size() == 2 && k.i8s()[0] == -128 && k.i8s()[1] == 127);
        KITCHEN_EXPECT(k.ints().size() == 3 && k.ints()[0] == -1 && k.ints()[2] == 2147483647);
        KITCHEN_EXPECT(k.longs().size() == 3 && k.longs()[0] == -9223372036854775807ll && k.longs()[2] == 9223372036854775807ll);
        KITCHEN_EXPECT(k.doubles().size() == 2 && k.doubles()[0] == 0.5 && k.doubles()[1] == -1e-300);
        KITCHEN_EXPECT(k.bools().size() == 3 && k.bools()[0] && !k.bools()[1] && k.bools()[2]);
        KITCHEN_EXPECT(k.names().size() == 3 && k.names()[0] == "a" && k.names()[1].empty() && k.names()[2] == "ccc");
        KITCHEN_EXPECT(k.colors().size() == 3 && k.colors()[0] == Color::Red && k.colors()[2] == Color::Blue);
        KITCHEN_EXPECT(k.inners().size() == 2 && k.inners()[0].n() == 1 && k.inners()[1].s() == "y");
        KITCHEN_EXPECT(k.vecs().size() == 2 && sameVec(k.vecs()[0], referenceVec(1)) && sameVec(k.vecs()[1], referenceVec(2)));
        KITCHEN_EXPECT(k.late() == 5);
    } else {
        KITCHEN_EXPECT(!k.b());
        KITCHEN_EXPECT(k.bt());
        KITCHEN_EXPECT(k.i8() == -3);
        KITCHEN_EXPECT(k.u8() == 200);
        KITCHEN_EXPECT(k.i16() == -1000);
        KITCHEN_EXPECT(k.u16() == 65000);
        KITCHEN_EXPECT(k.i32() == -123456);
        KITCHEN_EXPECT(k.u32() == 4000000000u);
        KITCHEN_EXPECT(k.i64() == -9007199254740993ll);
        KITCHEN_EXPECT(k.u64() == 18446744073709551615ull);
        KITCHEN_EXPECT(k.f32() == 1.5f);
        KITCHEN_EXPECT(k.f64() == -2.25);
        KITCHEN_EXPECT(k.color() == Color::Green);
        KITCHEN_EXPECT(k.text().empty());
        KITCHEN_EXPECT(!k.hasInner());
        KITCHEN_EXPECT(k.bytes().size() == 0 && k.ints().size() == 0 && k.names().size() == 0 && k.vecs().size() == 0);
        KITCHEN_EXPECT(k.late() == 99);
    }
    return out.str();
}

} // namespace test

namespace {

std::vector<char> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::string mode = argv[1], dir = argv[2];
    if (mode == "write") {
        {
            bufs::MessageBuilder mb;
            auto k = mb.initRoot<test::Kitchen>();
            test::fillReference(k);
            const bufs::Message message = mb.release();
            spit(dir + "/cpp_ref.bin", message.data(), message.size());
        }
        {
            bufs::MessageBuilder mb;
            mb.initRoot<test::Kitchen>().setTag(1);
            const bufs::Message message = mb.release();
            spit(dir + "/cpp_defaults.bin", message.data(), message.size());
        }
        return 0;
    }
    int failures = 0;
    for (const char* lang : {"ts", "cs", "py"}) {
        for (const char* what : {"ref", "defaults"}) {
            const std::string path = dir + "/" + lang + "_" + what + ".bin";
            const std::vector<char> bytes = slurp(path);
            std::string problems;
            if (bytes.empty()) {
                problems = "missing";
            } else {
                if (!bufs::verify<test::Kitchen>(bytes.data(), bytes.size()))
                    problems = "does not verify";
                else
                    problems = test::checkKitchen(bufs::root<test::Kitchen>(bytes.data()));
            }
            if (!problems.empty()) {
                std::fprintf(stderr, "%s: %s\n", path.c_str(), problems.c_str());
                ++failures;
            } else {
                std::printf("ok - %s_%s read by C++\n", lang, what);
            }
        }
    }
    return failures ? 1 : 0;
}
