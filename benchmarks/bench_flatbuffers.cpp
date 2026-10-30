// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// FlatBuffers as a benchmark codec, from flatc's code for the same bench.bufs.

#include <array>
#include <cstdlib>

#include "bench_common.hpp"
#include "bench_generated.h"

namespace {

using bd::Input;
using bd::Type;

static_assert(sizeof(bench::Box) == sizeof(bd::BoxData), "Box layout");
static_assert(sizeof(bench::Detection) == sizeof(bd::DetectionData), "Detection layout");

::flatbuffers::Offset<bench::Item> buildItem(::flatbuffers::FlatBufferBuilder& fbb, const bd::ItemData& d) {
    const auto label = fbb.CreateString(d.label);
    const auto category = fbb.CreateString(d.category);
    const auto subcategory = fbb.CreateString(d.subcategory);
    const auto scores = fbb.CreateVector(d.partScores.data(), d.partScores.size());
    const auto boxes =
        fbb.CreateVectorOfStructs(reinterpret_cast<const bench::Box*>(d.partBoxes.data()), d.partBoxes.size());
    const auto rows = fbb.CreateVector(d.partRows.data(), d.partRows.size());
    const auto features = fbb.CreateVector(d.features.data(), d.features.size());
    ::flatbuffers::Offset<::flatbuffers::Vector<uint8_t>> blob;
    if (d.blobSize)
        blob = fbb.CreateVector(d.blob, d.blobSize);
    const bench::Box bounds(d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h);
    return bench::CreateItem(fbb, label, category, subcategory, d.score, d.categoryScore, d.subcategoryScore,
                             d.itemScore, d.exactScore, d.logProb, d.gapped, d.provided,
                             static_cast<bench::Verdict>(d.verdict), d.rows, d.flags, d.splitAt, scores, boxes, rows,
                             features, &bounds, blob);
}

void build(::flatbuffers::FlatBufferBuilder& fbb, const Input& in) {
    switch (in.type) {
    case Type::Query: {
        const bd::QueryData& d = *in.query;
        const auto source = fbb.CreateString(d.source);
        fbb.Finish(bench::CreateQuery(fbb, d.queryId, source, d.width, d.height, d.stride, d.exact, d.threshold,
                                      d.minScore, static_cast<bench::Encoding>(d.encoding), d.slot, d.timestampUs,
                                      d.clientId, d.flags));
        break;
    }
    case Type::Report: {
        const bd::ReportData& d = *in.report;
        std::array<::flatbuffers::Offset<bench::Item>, 16> items;
        if (d.itemCount > items.size())
            std::abort();
        for (size_t i = 0; i < d.itemCount; ++i)
            items[i] = buildItem(fbb, d.items[i]);
        const auto vector = fbb.CreateVector(items.data(), d.itemCount);
        fbb.Finish(bench::CreateReport(fbb, d.queryId, d.width, d.height, d.prepareTime, d.processTime, vector));
        break;
    }
    case Type::Frame: {
        const bd::FrameData& d = *in.frame;
        const bench::FrameReady frame(d.queryId, d.timestampUs, d.slot, d.width, d.height, d.stride, d.clientId,
                                      static_cast<bench::Encoding>(d.encoding), d.exact, d.threshold, d.minScore);
        fbb.Finish(bench::CreateFrameReadyMessage(fbb, &frame));
        break;
    }
    case Type::Detections: {
        const bd::DetectionsData& d = *in.detections;
        const auto items =
            fbb.CreateVectorOfStructs(reinterpret_cast<const bench::Detection*>(d.items.data()), d.count);
        fbb.Finish(bench::CreateDetectionList(fbb, d.queryId, items));
        break;
    }
    }
}

bool verifyMessage(Type type, const uint8_t* data, size_t size) {
    ::flatbuffers::Verifier v(data, size);
    switch (type) {
    case Type::Query: return v.VerifyBuffer<bench::Query>(nullptr);
    case Type::Report: return v.VerifyBuffer<bench::Report>(nullptr);
    case Type::Frame: return v.VerifyBuffer<bench::FrameReadyMessage>(nullptr);
    case Type::Detections: return v.VerifyBuffer<bench::DetectionList>(nullptr);
    }
    return false;
}

uint64_t readString(const ::flatbuffers::String* s) {
    return s ? s->size() + (s->size() ? static_cast<uint8_t>(s->c_str()[0]) : 0) : 0;
}

uint64_t readBox(const bench::Box* b) {
    return b ? bd::bits(b->x()) + bd::bits(b->y()) + bd::bits(b->w()) + bd::bits(b->h()) : 0;
}

uint64_t readMessage(Type type, const uint8_t* data) {
    uint64_t sum = 0;
    switch (type) {
    case Type::Query: {
        const auto* r = ::flatbuffers::GetRoot<bench::Query>(data);
        sum += r->query_id() + readString(r->source());
        sum += static_cast<uint64_t>(r->width()) + static_cast<uint64_t>(r->height()) +
               static_cast<uint64_t>(r->stride());
        sum += r->exact() + bd::bits(r->threshold()) + bd::bits(r->min_score());
        sum += static_cast<uint64_t>(r->encoding()) + r->slot() + static_cast<uint64_t>(r->timestamp_us());
        sum += r->client_id() + r->flags();
        break;
    }
    case Type::Report: {
        const auto* r = ::flatbuffers::GetRoot<bench::Report>(data);
        sum += r->query_id() + static_cast<uint64_t>(r->width()) + static_cast<uint64_t>(r->height());
        sum += bd::bits(r->prepare_time()) + bd::bits(r->process_time());
        if (const auto* items = r->items()) {
            for (const bench::Item* p : *items) {
                sum += readString(p->label()) + readString(p->category()) + readString(p->subcategory());
                sum += bd::bits(p->score()) + bd::bits(p->category_score()) + bd::bits(p->subcategory_score());
                sum += bd::bits(p->item_score()) + bd::bits(p->exact_score()) + bd::bits(p->log_prob());
                sum += p->gapped() + p->provided() + static_cast<uint64_t>(p->verdict());
                sum += static_cast<uint64_t>(p->rows()) + p->flags() + static_cast<uint64_t>(p->split_at());
                if (const auto* v = p->part_scores())
                    for (float f : *v)
                        sum += bd::bits(f);
                if (const auto* v = p->part_boxes())
                    for (const bench::Box* b : *v)
                        sum += readBox(b);
                if (const auto* v = p->part_rows())
                    for (uint8_t row : *v)
                        sum += row;
                if (const auto* v = p->features())
                    for (float f : *v)
                        sum += bd::bits(f);
                sum += readBox(p->bounds());
                if (const auto* blob = p->blob()) {
                    sum += blob->size();
                    if (blob->size())
                        sum += blob->Get(0) + blob->Get(blob->size() - 1);
                }
            }
        }
        break;
    }
    case Type::Frame: {
        const auto* m = ::flatbuffers::GetRoot<bench::FrameReadyMessage>(data);
        if (const bench::FrameReady* f = m->frame())
            sum += f->query_id() + static_cast<uint64_t>(f->timestamp_us()) + f->slot() +
                   static_cast<uint64_t>(f->width()) + static_cast<uint64_t>(f->height()) +
                   static_cast<uint64_t>(f->stride()) + f->client_id() + static_cast<uint64_t>(f->encoding()) +
                   f->exact() + bd::bits(f->threshold()) + bd::bits(f->min_score());
        break;
    }
    case Type::Detections: {
        const auto* r = ::flatbuffers::GetRoot<bench::DetectionList>(data);
        sum += r->query_id();
        if (const auto* v = r->detections())
            for (const bench::Detection* d : *v)
                sum += bd::bits(d->x1()) + bd::bits(d->y1()) + bd::bits(d->x2()) + bd::bits(d->y2()) +
                       bd::bits(d->score()) + static_cast<uint64_t>(d->class_id());
        break;
    }
    }
    return sum;
}

class FlatBuffersCodec : public bd::Codec {
public:
    std::string name() const override { return "flatbuffers"; }

    std::vector<uint8_t> encode(const Input& in) override {
        ::flatbuffers::FlatBufferBuilder fbb;
        build(fbb, in);
        return std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    }

    // One FlatBufferBuilder, Clear()ed between messages.
    uint64_t buildReuse(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            builder_.Clear();
            build(builder_, *bd::escape(&in));
            sum += builder_.GetSize();
            bd::clobber();
        }
        return sum;
    }

    // A new FlatBufferBuilder (default allocator, 1 KiB start) per message,
    // its buffer Release()d as the finished message.
    uint64_t buildFresh(const Input& in, uint64_t n) override {
        uint64_t sum = 0;
        ::flatbuffers::DetachedBuffer last;
        for (uint64_t i = 0; i < n; ++i) {
            ::flatbuffers::FlatBufferBuilder fbb;
            build(fbb, *bd::escape(&in));
            last = fbb.Release();
            sum += last.size();
            bd::clobber();
        }
        return sum;
    }

    uint64_t verify(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t ok = 0;
        for (uint64_t i = 0; i < n; ++i) {
            ok += verifyMessage(type, bd::escape(data), size);
            bd::clobber();
        }
        return ok;
    }

    uint64_t read(Type type, const uint8_t* data, size_t, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            sum += readMessage(type, bd::escape(data));
            bd::clobber();
        }
        return sum;
    }

    uint64_t decode(Type type, const uint8_t* data, size_t size, uint64_t n) override {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* p = bd::escape(data);
            if (verifyMessage(type, p, size))
                sum += readMessage(type, p);
            bd::clobber();
        }
        return sum;
    }

    bd::LongRunResult longRun(const bd::LongRunConfig& config) override {
        return bd::runLongRun<::flatbuffers::DetachedBuffer>(
            config,
            [](const Input& in) {
                ::flatbuffers::FlatBufferBuilder fbb;
                build(fbb, in);
                return fbb.Release();
            },
            consume);
    }

    bd::MtResult multiThread(const bd::MtConfig& config) override {
        return bd::runMultiThread<::flatbuffers::DetachedBuffer>(
            config,
            [](const Input& in) {
                ::flatbuffers::FlatBufferBuilder fbb;
                build(fbb, in);
                return fbb.Release();
            },
            consume);
    }

private:
    static uint64_t consume(Type type, ::flatbuffers::DetachedBuffer& m) {
        if (!verifyMessage(type, m.data(), m.size())) {
            std::fprintf(stderr, "flatbuffers: a message failed to verify\n");
            std::abort();
        }
        const uint64_t sum = readMessage(type, m.data());
        m = ::flatbuffers::DetachedBuffer();
        return sum;
    }

    ::flatbuffers::FlatBufferBuilder builder_;
};

} // namespace

std::unique_ptr<bd::Codec> bd::makeFlatBuffersCodec() { return std::make_unique<FlatBuffersCodec>(); }

#define BENCH_STRINGIFY2(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY2(x)
const char* flatbuffersVersion() {
    return BENCH_STRINGIFY(FLATBUFFERS_VERSION_MAJOR) "." BENCH_STRINGIFY(FLATBUFFERS_VERSION_MINOR) "." BENCH_STRINGIFY(
        FLATBUFFERS_VERSION_REVISION);
}
