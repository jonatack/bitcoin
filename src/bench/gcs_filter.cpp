// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <blockfilter.h>

static const GCSFilter::ElementSet GenerateGCSTestElements()
{
    GCSFilter::ElementSet elements;
    for (int i = 0; i < 10000; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i >> 8);
        elements.insert(std::move(element));
    }

    return elements;
}

static void GCSFilterConstruct(benchmark::Bench& bench)
{
    auto elements = GenerateGCSTestElements();

    uint64_t siphash_k0 = 0;
    bench.batch(elements.size()).unit("elem").run([&] {
        GCSFilter filter({siphash_k0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, elements);

        siphash_k0++;
    });
}

static void GCSFilterMatch(benchmark::Bench& bench)
{
    auto elements = GenerateGCSTestElements();

    GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, elements);

    bench.unit("elem").run([&] {
        filter.Match(GCSFilter::Element());
    });
}

static void GCSFilterDecode(benchmark::Bench& bench)
{
    auto elements = GenerateGCSTestElements();

    GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, elements);
    auto encoded = filter.GetEncoded();

    bench.unit("elem").run([&] {
        GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, encoded, /*filter_checked=*/false);
    });
}
static void BlockFilterGetHash(benchmark::Bench& bench)
{
    auto elements = GenerateGCSTestElements();

    GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, elements);
    BlockFilter block_filter(BlockFilterType::BASIC, {}, filter.GetEncoded(), /*filter_checked=*/false);

    bench.unit("elem").run([&] {
        block_filter.GetHash();
    });
}

static void GCSFilterDecodeChecked(benchmark::Bench& bench)
{
    auto elements = GenerateGCSTestElements();

    GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, elements);
    auto encoded = filter.GetEncoded();

    bench.unit("elem").run([&] {
        GCSFilter filter({0, 0, BASIC_FILTER_P, BASIC_FILTER_M}, encoded, /*filter_checked=*/true);
    });
}
BENCHMARK(BlockFilterGetHash);
BENCHMARK(GCSFilterConstruct);
BENCHMARK(GCSFilterDecode);
BENCHMARK(GCSFilterDecodeChecked);
BENCHMARK(GCSFilterMatch);
