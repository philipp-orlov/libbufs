// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Building and reading the benchmark messages with libbufs. Shared by the
// codec benchmark and the shared-memory transport benchmark.

#include "bench.bufs.hpp"
#include "bench_common.hpp"

namespace bufsbench {

static_assert(sizeof(bench::Box) == sizeof(bd::BoxData), "Box layout");
static_assert(sizeof(bench::Detection) == sizeof(bd::DetectionData), "Detection layout");

inline void fillItem(bench::Item::Builder p, const bd::ItemData& d) {
    p.setLabel(d.label);
    p.setCategory(d.category);
    p.setSubcategory(d.subcategory);
    p.setScore(d.score);
    p.setCategoryScore(d.categoryScore);
    p.setSubcategoryScore(d.subcategoryScore);
    p.setItemScore(d.itemScore);
    p.setExactScore(d.exactScore);
    p.setLogProb(d.logProb);
    p.setGapped(d.gapped);
    p.setProvided(d.provided);
    p.setVerdict(static_cast<bench::Verdict>(d.verdict));
    p.setRows(d.rows);
    p.setFlags(d.flags);
    p.setSplitAt(d.splitAt);
    p.setPartScores(d.partScores.data(), d.partScores.size());
    p.setPartBoxes(reinterpret_cast<const bench::Box*>(d.partBoxes.data()), d.partBoxes.size());
    p.setPartRows(d.partRows.data(), d.partRows.size());
    p.setFeatures(d.features.data(), d.features.size());
    p.setBounds(bench::Box{d.bounds.x, d.bounds.y, d.bounds.w, d.bounds.h});
    if (d.blobSize)
        p.setBlob(d.blob, d.blobSize);
}

// Builds `in` as a new message in `mb`.
inline void build(bufs::MessageBuilder& mb, const bd::Input& in) {
    switch (in.type) {
    case bd::Type::Query: {
        const bd::QueryData& d = *in.query;
        auto r = mb.initRoot<bench::Query>();
        r.setQueryId(d.queryId);
        r.setSource(d.source);
        r.setWidth(d.width);
        r.setHeight(d.height);
        r.setStride(d.stride);
        r.setExact(d.exact);
        r.setThreshold(d.threshold);
        r.setMinScore(d.minScore);
        r.setEncoding(static_cast<bench::Encoding>(d.encoding));
        r.setSlot(d.slot);
        r.setTimestampUs(d.timestampUs);
        r.setClientId(d.clientId);
        r.setFlags(d.flags);
        break;
    }
    case bd::Type::Report: {
        const bd::ReportData& d = *in.report;
        auto r = mb.initRoot<bench::Report>();
        r.setQueryId(d.queryId);
        r.setWidth(d.width);
        r.setHeight(d.height);
        r.setPrepareTime(d.prepareTime);
        r.setProcessTime(d.processTime);
        auto items = r.initItems(d.itemCount);
        for (size_t i = 0; i < d.itemCount; ++i)
            fillItem(items.init(i), d.items[i]);
        break;
    }
    case bd::Type::Frame: {
        const bd::FrameData& d = *in.frame;
        bench::FrameReady& f = mb.initRootStruct<bench::FrameReady>();
        f.queryId = d.queryId;
        f.timestampUs = d.timestampUs;
        f.slot = d.slot;
        f.width = d.width;
        f.height = d.height;
        f.stride = d.stride;
        f.clientId = d.clientId;
        f.encoding = static_cast<bench::Encoding>(d.encoding);
        f.exact = d.exact;
        f.threshold = d.threshold;
        f.minScore = d.minScore;
        break;
    }
    case bd::Type::Detections: {
        const bd::DetectionsData& d = *in.detections;
        auto r = mb.initRoot<bench::DetectionList>();
        r.setQueryId(d.queryId);
        r.setDetections(reinterpret_cast<const bench::Detection*>(d.items.data()), d.count);
        break;
    }
    }
}

inline bool verify(bd::Type type, const uint8_t* data, size_t size) {
    switch (type) {
    case bd::Type::Query: return bufs::verify<bench::Query>(data, size);
    case bd::Type::Report: return bufs::verify<bench::Report>(data, size);
    case bd::Type::Frame: return bufs::verifyStruct<bench::FrameReady>(data, size);
    case bd::Type::Detections: return bufs::verify<bench::DetectionList>(data, size);
    }
    return false;
}

inline uint64_t readString(std::string_view s) { return s.size() + (s.empty() ? 0 : static_cast<uint8_t>(s[0])); }

inline uint64_t readQuery(const uint8_t* data) {
    const auto r = bufs::root<bench::Query>(data);
    uint64_t sum = r.queryId() + readString(r.source());
    sum += static_cast<uint64_t>(r.width()) + static_cast<uint64_t>(r.height()) + static_cast<uint64_t>(r.stride());
    sum += r.exact() + bd::bits(r.threshold()) + bd::bits(r.minScore());
    sum += static_cast<uint64_t>(r.encoding()) + r.slot() + static_cast<uint64_t>(r.timestampUs());
    sum += r.clientId() + r.flags();
    return sum;
}

inline uint64_t readBox(const bench::Box& b) { return bd::bits(b.x) + bd::bits(b.y) + bd::bits(b.w) + bd::bits(b.h); }

inline uint64_t readReport(const uint8_t* data) {
    const auto r = bufs::root<bench::Report>(data);
    uint64_t sum = r.queryId() + static_cast<uint64_t>(r.width()) + static_cast<uint64_t>(r.height());
    sum += bd::bits(r.prepareTime()) + bd::bits(r.processTime());
    for (const bench::Item::Reader p : r.items()) {
        sum += readString(p.label()) + readString(p.category()) + readString(p.subcategory());
        sum += bd::bits(p.score()) + bd::bits(p.categoryScore()) + bd::bits(p.subcategoryScore());
        sum += bd::bits(p.itemScore()) + bd::bits(p.exactScore()) + bd::bits(p.logProb());
        sum += p.gapped() + p.provided() + static_cast<uint64_t>(p.verdict());
        sum += static_cast<uint64_t>(p.rows()) + p.flags() + static_cast<uint64_t>(p.splitAt());
        for (float f : p.partScores())
            sum += bd::bits(f);
        for (const bench::Box& b : p.partBoxes())
            sum += readBox(b);
        for (uint8_t row : p.partRows())
            sum += row;
        for (float f : p.features())
            sum += bd::bits(f);
        sum += readBox(p.bounds());
        // Zero-copy bytes: their place and size, and the ends.
        const auto blob = p.blob();
        sum += blob.size();
        if (!blob.empty())
            sum += blob[0] + blob[blob.size() - 1];
    }
    return sum;
}

inline uint64_t readFrame(const uint8_t* data) {
    const bench::FrameReady& f = bufs::rootStruct<bench::FrameReady>(data);
    return f.queryId + static_cast<uint64_t>(f.timestampUs) + f.slot + static_cast<uint64_t>(f.width) +
           static_cast<uint64_t>(f.height) + static_cast<uint64_t>(f.stride) + f.clientId +
           static_cast<uint64_t>(f.encoding) + f.exact + bd::bits(f.threshold) + bd::bits(f.minScore);
}

inline uint64_t readDetections(const uint8_t* data) {
    const auto r = bufs::root<bench::DetectionList>(data);
    uint64_t sum = r.queryId();
    for (const bench::Detection& d : r.detections())
        sum += bd::bits(d.x1) + bd::bits(d.y1) + bd::bits(d.x2) + bd::bits(d.y2) + bd::bits(d.score) +
               static_cast<uint64_t>(d.classId);
    return sum;
}

inline uint64_t read(bd::Type type, const uint8_t* data) {
    switch (type) {
    case bd::Type::Query: return readQuery(data);
    case bd::Type::Report: return readReport(data);
    case bd::Type::Frame: return readFrame(data);
    case bd::Type::Detections: return readDetections(data);
    }
    return 0;
}

} // namespace bufsbench
