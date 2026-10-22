// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A JSON baseline as a benchmark codec: messages are built as libhttp
// json::Json trees and dump()ed, then parsed with libhttp's Json::parse (row
// "json") or the minimal JsonLite parser in baseline/ (row "json/jsonlite").
// Item blobs travel as base64 data URIs.

#include <cstdlib>
#include <type_traits>

#include "bench_common.hpp"
#include "http/base64.hpp"
#include "internal/JsonLite.hpp"
#include "json/json.hpp"

namespace {

using bd::Input;
using bd::Type;
using json::Json;
using jsonlite::JsonValue;

const char* const kVerdicts[] = {"accept", "partial", "reject"};
const char* const kEncodings[] = {"raw", "compressed", "packed"};
constexpr std::string_view kDataUri = "data:application/octet-stream;base64,";

Json box(const bd::BoxData& b) { return Json{{"x", b.x}, {"y", b.y}, {"w", b.w}, {"h", b.h}}; }

Json item(const bd::ItemData& d) {
    Json scores = Json::array(), boxes = Json::array(), rows = Json::array(), features = Json::array();
    for (float f : d.partScores)
        scores.push_back(f);
    for (const bd::BoxData& b : d.partBoxes)
        boxes.push_back(box(b));
    for (uint8_t row : d.partRows)
        rows.push_back(static_cast<int>(row));
    for (float f : d.features)
        features.push_back(f);
    Json entry{{"category", d.category},
               {"categoryScore", d.categoryScore},
               {"subcategory", d.subcategory},
               {"subcategoryScore", d.subcategoryScore},
               {"label", d.label},
               {"score", d.score},
               {"partScores", std::move(scores)},
               {"partBoxes", std::move(boxes)},
               {"partRows", std::move(rows)},
               {"itemScore", d.itemScore},
               {"bounds", box(d.bounds)},
               {"gapped", d.gapped},
               {"verdict", kVerdicts[d.verdict % 3]},
               {"exactScore", d.exactScore},
               {"logProb", d.logProb},
               {"flags", d.flags},
               {"rows", d.rows},
               {"features", std::move(features)},
               {"provided", d.provided},
               {"splitAt", d.splitAt}};
    if (d.blobSize) {
        std::string uri(kDataUri);
        uri += http::base64Encode(d.blob, d.blobSize);
        entry["blob"] = std::move(uri);
    }
    return entry;
}

Json tree(const Input& in) {
    switch (in.type) {
    case Type::Query: {
        const bd::QueryData& d = *in.query;
        return Json{{"op", "query"},
                    {"queryId", static_cast<unsigned long long>(d.queryId)},
                    {"source", d.source},
                    {"width", d.width},
                    {"height", d.height},
                    {"stride", d.stride},
                    {"format", "raw"},
                    {"exact", d.exact},
                    {"threshold", d.threshold},
                    {"minScore", d.minScore},
                    {"encoding", kEncodings[d.encoding % 3]},
                    {"slot", d.slot},
                    {"timestampUs", static_cast<long long>(d.timestampUs)},
                    {"clientId", d.clientId},
                    {"flags", d.flags}};
    }
    case Type::Report: {
        const bd::ReportData& d = *in.report;
        Json::Array items;
        for (size_t i = 0; i < d.itemCount; ++i)
            items.push_back(item(d.items[i]));
        return Json{{"op", "report"},
                    {"queryId", static_cast<unsigned long long>(d.queryId)},
                    {"width", d.width},
                    {"height", d.height},
                    {"prepareTime", d.prepareTime},
                    {"processTime", d.processTime},
                    {"items", std::move(items)}};
    }
    case Type::Frame: {
        const bd::FrameData& d = *in.frame;
        return Json{{"op", "frame"},
                    {"queryId", static_cast<unsigned long long>(d.queryId)},
                    {"timestampUs", static_cast<long long>(d.timestampUs)},
                    {"slot", d.slot},
                    {"width", d.width},
                    {"height", d.height},
                    {"stride", d.stride},
                    {"clientId", d.clientId},
                    {"encoding", kEncodings[d.encoding % 3]},
                    {"exact", d.exact},
                    {"threshold", d.threshold},
                    {"minScore", d.minScore}};
    }
    case Type::Detections: {
        const bd::DetectionsData& d = *in.detections;
        Json::Array detections;
        for (size_t i = 0; i < d.count; ++i) {
            const bd::DetectionData& x = d.items[i];
            detections.push_back(Json{{"x1", x.x1}, {"y1", x.y1}, {"x2", x.x2}, {"y2", x.y2},
                                      {"score", x.score}, {"classId", x.classId}});
        }
        return Json{{"op", "result"},
                    {"queryId", static_cast<unsigned long long>(d.queryId)},
                    {"detections", std::move(detections)}};
    }
    }
    return Json();
}

// ------------------------------------------------- reading, for either tree --

// The few calls the readers need, over libhttp's Json or JsonLite's JsonValue.
struct HttpJson {
    using Value = Json;
    static const Value* find(const Value& v, const char* key) { return v.find(key); }
    static bool parse(std::string_view text, Value& out) {
        std::string error;
        return Json::parse(text, out, error);
    }
};

struct LiteJson {
    using Value = JsonValue;
    static const Value* find(const Value& v, const char* key) { return v.find(key); }
    static bool parse(std::string_view text, Value& out) {
        std::string error;
        return JsonValue::parse(text, out, error);
    }
};

template <class J>
uint64_t num(const typename J::Value& v, const char* key) {
    const auto* x = J::find(v, key);
    return x && x->isNumber() ? bd::bits(static_cast<float>(x->asDouble())) : 0;
}

template <class J>
uint64_t integer(const typename J::Value& v, const char* key) {
    const auto* x = J::find(v, key);
    return x && x->isNumber() ? static_cast<uint64_t>(x->asInt()) : 0;
}

template <class J>
uint64_t flag(const typename J::Value& v, const char* key) {
    const auto* x = J::find(v, key);
    return x && x->isBool() && x->asBool();
}

template <class J>
uint64_t text(const typename J::Value& v, const char* key) {
    const auto* x = J::find(v, key);
    if (!x || !x->isString())
        return 0;
    const std::string& s = x->asString();
    return s.size() + (s.empty() ? 0 : static_cast<uint8_t>(s[0]));
}

template <class J>
uint64_t readBox(const typename J::Value& v) {
    return num<J>(v, "x") + num<J>(v, "y") + num<J>(v, "w") + num<J>(v, "h");
}

template <class J>
uint64_t readArray(const typename J::Value& v, const char* key) {
    uint64_t sum = 0;
    if (const auto* a = J::find(v, key); a && a->isArray())
        for (const auto& x : a->asArray())
            sum += bd::bits(static_cast<float>(x.asDouble()));
    return sum;
}

template <class J>
uint64_t readTree(Type type, const typename J::Value& v) {
    uint64_t sum = 0;
    switch (type) {
    case Type::Query:
        sum += integer<J>(v, "queryId") + text<J>(v, "source") + integer<J>(v, "width") + integer<J>(v, "height");
        sum += integer<J>(v, "stride") + text<J>(v, "format") + flag<J>(v, "exact") + num<J>(v, "threshold");
        sum += num<J>(v, "minScore") + text<J>(v, "encoding") + integer<J>(v, "slot");
        sum += integer<J>(v, "timestampUs") + integer<J>(v, "clientId") + integer<J>(v, "flags");
        break;
    case Type::Report: {
        sum += integer<J>(v, "queryId") + integer<J>(v, "width") + integer<J>(v, "height");
        sum += num<J>(v, "prepareTime") + num<J>(v, "processTime");
        const auto* items = J::find(v, "items");
        if (!items || !items->isArray())
            break;
        for (const auto& p : items->asArray()) {
            sum += text<J>(p, "label") + text<J>(p, "category") + text<J>(p, "subcategory");
            sum += num<J>(p, "score") + num<J>(p, "categoryScore") + num<J>(p, "subcategoryScore");
            sum += num<J>(p, "itemScore") + num<J>(p, "exactScore") + num<J>(p, "logProb");
            sum += flag<J>(p, "gapped") + flag<J>(p, "provided") + text<J>(p, "verdict");
            sum += integer<J>(p, "rows") + integer<J>(p, "flags") + integer<J>(p, "splitAt");
            sum += readArray<J>(p, "partScores");
            if (const auto* a = J::find(p, "partBoxes"); a && a->isArray())
                for (const auto& b : a->asArray())
                    sum += readBox<J>(b);
            if (const auto* a = J::find(p, "partRows"); a && a->isArray())
                for (const auto& x : a->asArray())
                    sum += static_cast<uint64_t>(x.asInt());
            sum += readArray<J>(p, "features");
            if (const auto* b = J::find(p, "bounds"))
                sum += readBox<J>(*b);
            // The bytes themselves: a client has to decode them to use them.
            if (const auto* blob = J::find(p, "blob"); blob && blob->isString()) {
                std::string_view uri = blob->asString();
                const size_t comma = uri.find(',');
                const std::string bytes = http::base64Decode(uri.substr(comma + 1));
                sum += bytes.size();
                if (!bytes.empty())
                    sum += static_cast<uint8_t>(bytes.front()) + static_cast<uint8_t>(bytes.back());
            }
        }
        break;
    }
    case Type::Frame:
        sum += integer<J>(v, "queryId") + integer<J>(v, "timestampUs") + integer<J>(v, "slot");
        sum += integer<J>(v, "width") + integer<J>(v, "height") + integer<J>(v, "stride");
        sum += integer<J>(v, "clientId") + text<J>(v, "encoding") + flag<J>(v, "exact");
        sum += num<J>(v, "threshold") + num<J>(v, "minScore");
        break;
    case Type::Detections: {
        sum += integer<J>(v, "queryId");
        if (const auto* a = J::find(v, "detections"); a && a->isArray())
            for (const auto& d : a->asArray())
                sum += num<J>(d, "x1") + num<J>(d, "y1") + num<J>(d, "x2") + num<J>(d, "y2") + num<J>(d, "score") +
                       integer<J>(d, "classId");
        break;
    }
    }
    return sum;
}

template <class J>
class JsonCodec : public bd::Codec {
public:
    explicit JsonCodec(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    bool ownsEncoder() const override { return std::is_same<J, HttpJson>::value; }

    std::vector<uint8_t> encode(const Input& in) override {
        const std::string text = tree(in).dump();
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    // The tree, then dump() into one recycled string.
    uint64_t buildReuse(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            out_.clear();
            tree(*bd::escape(&in)).dump(out_);
            sum += out_.size();
            bd::clobber();
        }
        return sum;
    }

    // The tree, then a new string from dump(), as a server would send it.
    uint64_t buildFresh(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        std::string last;
        for (uint64_t i = 0; i < n; ++i) {
            last = tree(*bd::escape(&in)).dump();
            sum += last.size();
            bd::clobber();
        }
        return sum;
    }

    // "Verify" is parsing: a parse that succeeds is the only check JSON has.
    uint64_t verify(Type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t ok = 0;
        for (uint64_t i = 0; i < n; ++i) {
            typename J::Value value;
            ok += J::parse(std::string_view(reinterpret_cast<const char*>(bd::escape(data)), size), value);
            bd::clobber();
        }
        return ok;
    }

    // Walks a tree parsed once, outside the timed loop.
    uint64_t read(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        typename J::Value value;
        if (!J::parse(std::string_view(reinterpret_cast<const char*>(data), size), value))
            return 0;
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            sum += readTree<J>(type, *bd::escape(&value));
            bd::clobber();
        }
        return sum;
    }

    uint64_t decode(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            typename J::Value value;
            if (J::parse(std::string_view(reinterpret_cast<const char*>(bd::escape(data)), size), value))
                sum += readTree<J>(type, value);
            bd::clobber();
        }
        return sum;
    }

    bd::LongRunResult longRun(const bd::LongRunConfig& config) override {
        return bd::runLongRun<std::string>(
            config, [](const Input& in) { return tree(in).dump(); }, consume);
    }

    bd::MtResult multiThread(const bd::MtConfig& config) override {
        return bd::runMultiThread<std::string>(
            config, [](const Input& in) { return tree(in).dump(); }, consume);
    }

private:
    static uint64_t consume(Type type, std::string& m) {
        typename J::Value value;
        if (!J::parse(m, value)) {
            std::fprintf(stderr, "json: a message failed to parse\n");
            std::abort();
        }
        const uint64_t sum = readTree<J>(type, value);
        std::string().swap(m);
        return sum;
    }

    std::string name_;
    std::string out_;
};

} // namespace

std::unique_ptr<bd::Codec> bd::makeJsonCodec() { return std::make_unique<JsonCodec<HttpJson>>("json (libhttp)"); }

std::unique_ptr<bd::Codec> bd::makeJsonLiteCodec() {
    return std::make_unique<JsonCodec<LiteJson>>("json/jsonlite");
}
