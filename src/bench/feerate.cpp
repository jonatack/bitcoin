// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <amount.h>
#include <policy/feerate.h>
#include <random.h>

static constexpr uint32_t num_bytes{314};
static constexpr size_t size{314};

static bool PassAmountByReferenceToConst(const CAmount& a, const CAmount& b, const CAmount& c)
{
    return a > b && b > c;
}

static bool PassAmountByValue(CAmount a, CAmount b, CAmount c)
{
    return a > b && b > c;
}

static bool PassFeeRateByReferenceToConst(const CFeeRate& a, const CFeeRate& b, const CFeeRate& c)
{
    return a > b && b > c;
}

/* Will be optimized away in a non-debug build. */
static void PassFeeRateByReferenceToConstDoNothing(const CFeeRate& a, const CFeeRate& b, const CFeeRate& c)
{
    return;
}

static bool PassFeeRateByValue(CFeeRate a, CFeeRate b, CFeeRate c)
{
    return a > b && b > c;
}

/* Will be optimized away in a non-debug build. */
static void PassFeeRateByValueDoNothing(CFeeRate a, CFeeRate b, CFeeRate c)
{
    return;
}

static bool PassFeeRateByReferenceToConstGetFee(const CFeeRate& a, const CFeeRate& b, const CFeeRate& c)
{
    return a.GetFee(size) > b.GetFee(size) && b.GetFee(size) > c.GetFee(size);
}

static CAmount PassFeeRateByValueGetFee(CFeeRate a, CFeeRate b, CFeeRate c)
{
    return a.GetFee(size) > b.GetFee(size) && b.GetFee(size) > c.GetFee(size);
}

/* Benchmarks */

static void AmountByReferenceToConst(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    bench.run([&] {
        CAmount a = rand.randrange(100000);
        CAmount b = rand.randrange(100000);
        CAmount c = rand.randrange(100000);
        PassAmountByReferenceToConst(a, b, c);
    });
}

static void AmountByValue(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    bench.run([&] {
        CAmount a = rand.randrange(100000);
        CAmount b = rand.randrange(100000);
        CAmount c = rand.randrange(100000);
        PassAmountByValue(a, b, c);
    });
}

static void FeeRateByReferenceToConst(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    bench.run([&] {
        CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
        CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
        CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);
        PassFeeRateByReferenceToConst(a, b, c);
    });
}

static void FeeRateByValue(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    bench.run([&] {
        CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
        CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
        CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);
        PassFeeRateByValue(a, b, c);
    });
}

static void FeeRateByReferenceToConstDoNothing(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);

    bench.run([&] {
        PassFeeRateByReferenceToConstDoNothing(a, b, c);
    });
}

static void FeeRateByValueDoNothing(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);

    bench.run([&] {
        PassFeeRateByValueDoNothing(a, b, c);
    });
}
static void FeeRateGetFeeByReferenceToConst(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);

    bench.run([&] { PassFeeRateByReferenceToConstGetFee(a, b, c); });
}

static void FeeRateGetFeeByValue(benchmark::Bench& bench)
{
    FastRandomContext rand{true};
    CFeeRate a = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate b = CFeeRate(rand.randrange(10000), num_bytes);
    CFeeRate c = CFeeRate(rand.randrange(10000), num_bytes);

    bench.run([&] { PassFeeRateByValueGetFee(a, b, c); });
}

BENCHMARK(AmountByReferenceToConst);
BENCHMARK(AmountByValue);

BENCHMARK(FeeRateByReferenceToConst);
BENCHMARK(FeeRateByReferenceToConstDoNothing);

BENCHMARK(FeeRateByValue);
BENCHMARK(FeeRateByValueDoNothing);

BENCHMARK(FeeRateGetFeeByReferenceToConst);
BENCHMARK(FeeRateGetFeeByValue);
