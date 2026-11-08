/*
 * Copyright 2026 Cory Lamming
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The shortest decimal digits that read back as a float: the Ryu algorithm (Ulf Adams, "Ryu: Fast Float-to-String
 * Conversion", PLDI 2018) for 32-bit floats. It follows the structure of Ryu's reference implementation, Copyright
 * 2018 Ulf Adams, Apache License 2.0. Whole-number arithmetic only, so no printf, no strtof and no heap; every board
 * gets the same digits.
 */

#include "erb_internal.h"

#include <string.h>

/** @brief A float's stored fields. */
#define FLOAT_SIGNIFICAND_BITS 23
#define FLOAT_EXPONENT_MASK 0xFFu
#define FLOAT_BIAS 127

/** @brief The bits each table entry keeps: 5^-q scaled up, and 5^i scaled down. */
#define POW5_INVERSE_BITS 59
#define POW5_BITS 61

/** @brief Entries the tables need to cover every float's exponent. */
#define POW5_INVERSE_COUNT 31
#define POW5_COUNT 47

/** @brief Digits are counted in this base. */
#define DECIMAL_BASE 10u

/** @brief Below this, a scaled value's power of 5 is checked; above it, it can't be a multiple. */
#define POW5_CHECK_LIMIT 9u

/** @brief Below this, a scaled value's power of 2 is checked. */
#define POW2_CHECK_LIMIT 31u

/** @brief 2^(bits - 1 + POW5_INVERSE_BITS) / 5^q + 1, where bits is the bit length of 5^q. */
static const uint64_t POW5_INVERSE[POW5_INVERSE_COUNT] = {
    0x0800000000000001u, 0x0666666666666667u, 0x051eb851eb851eb9u, 0x04189374bc6a7efau, 0x068db8bac710cb2au,
    0x053e2d6238da3c22u, 0x0431bde82d7b634eu, 0x06b5fca6af2bd216u, 0x055e63b88c230e78u, 0x044b82fa09b5a52du,
    0x06df37f675ef6eaeu, 0x057f5ff85e592558u, 0x0465e6604b7a8447u, 0x0709709a125da071u, 0x05a126e1a84ae6c1u,
    0x0480ebe7b9d58567u, 0x0734aca5f6226f0bu, 0x05c3bd5191b525a3u, 0x049c97747490eae9u, 0x0760f253edb4ab0eu,
    0x05e72843249088d8u, 0x04b8ed0283a6d3e0u, 0x078e480405d7b966u, 0x060b6cd004ac9452u, 0x04d5f0a66a23a9dbu,
    0x07bcb43d769f762bu, 0x063090312bb2c4efu, 0x04f3a68dbc8f03f3u, 0x07ec3daf94180651u, 0x065697bfa9acd1dau,
    0x051212ffbaf0a7e2u,
};

/** @brief 5^i, its top POW5_BITS bits. */
static const uint64_t POW5[POW5_COUNT] = {
    0x1000000000000000u, 0x1400000000000000u, 0x1900000000000000u, 0x1f40000000000000u, 0x1388000000000000u,
    0x186a000000000000u, 0x1e84800000000000u, 0x1312d00000000000u, 0x17d7840000000000u, 0x1dcd650000000000u,
    0x12a05f2000000000u, 0x174876e800000000u, 0x1d1a94a200000000u, 0x12309ce540000000u, 0x16bcc41e90000000u,
    0x1c6bf52634000000u, 0x11c37937e0800000u, 0x16345785d8a00000u, 0x1bc16d674ec80000u, 0x1158e460913d0000u,
    0x15af1d78b58c4000u, 0x1b1ae4d6e2ef5000u, 0x10f0cf064dd59200u, 0x152d02c7e14af680u, 0x1a784379d99db420u,
    0x108b2a2c28029094u, 0x14adf4b7320334b9u, 0x19d971e4fe8401e7u, 0x1027e72f1f128130u, 0x1431e0fae6d7217cu,
    0x193e5939a08ce9dbu, 0x1f8def8808b02452u, 0x13b8b5b5056e16b3u, 0x18a6e32246c99c60u, 0x1ed09bead87c0378u,
    0x13426172c74d822bu, 0x1812f9cf7920e2b6u, 0x1e17b84357691b64u, 0x12ced32a16a1b11eu, 0x178287f49c4a1d66u,
    0x1d6329f1c35ca4bfu, 0x125dfa371a19e6f7u, 0x16f578c4e0a060b5u, 0x1cb2d6f618c878e3u, 0x11efc659cf7d4b8du,
    0x166bb7f0435c9e71u, 0x1c06a5ec5433c60du,
};

/** @brief The bit length of 5^@p power, for a power from 0 to 3528. */
static int32_t pow5_bits(int32_t power)
{
    return (int32_t)(((uint32_t)power * 1217359u) >> 19) + 1;
}

/** @brief floor(log10(2^@p power)), for a power from 0 to 1650. */
static uint32_t log10_pow2(int32_t power)
{
    return ((uint32_t)power * 78913u) >> 18;
}

/** @brief floor(log10(5^@p power)), for a power from 0 to 2620. */
static uint32_t log10_pow5(int32_t power)
{
    return ((uint32_t)power * 732923u) >> 20;
}

/** @brief Whether @p value, which isn't 0, is a multiple of 5^@p power. */
static bool multiple_of_pow5(uint32_t value, uint32_t power)
{
    uint32_t factors = 0;
    while (value % 5u == 0u)
    {
        value /= 5u;
        factors++;
    }
    return factors >= power;
}

/** @brief Whether @p value is a multiple of 2^@p power. */
static bool multiple_of_pow2(uint32_t value, uint32_t power)
{
    return (value & ((1u << power) - 1u)) == 0u;
}

/** @brief (@p value * @p factor) >> @p shift, for a shift of at least 32. */
static uint32_t multiply_shift(uint32_t value, uint64_t factor, int32_t shift)
{
    const uint64_t low = (uint64_t)value * (uint32_t)factor;
    const uint64_t high = (uint64_t)value * (uint32_t)(factor >> 32);
    return (uint32_t)(((low >> 32) + high) >> (shift - 32));
}

uint32_t erb_float_digits(float value, int32_t* exponent)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint32_t stored_significand = bits & ((1u << FLOAT_SIGNIFICAND_BITS) - 1u);
    const uint32_t stored_exponent = (bits >> FLOAT_SIGNIFICAND_BITS) & FLOAT_EXPONENT_MASK;

    /* The value is significand * 2^binary_exponent, both scaled by 4 so the halfway points to its neighbours are
       whole numbers. */
    int32_t binary_exponent;
    uint32_t significand;
    if (stored_exponent == 0u)
    {
        binary_exponent = 1 - FLOAT_BIAS - FLOAT_SIGNIFICAND_BITS - 2;
        significand = stored_significand;
    }
    else
    {
        binary_exponent = (int32_t)stored_exponent - FLOAT_BIAS - FLOAT_SIGNIFICAND_BITS - 2;
        significand = (1u << FLOAT_SIGNIFICAND_BITS) | stored_significand;
    }
    const bool bounds_included = (significand & 1u) == 0u;

    /* The value, and the halfway points above and below it: any decimal between them reads back as the value. The
       gap below is half as wide at a power of 2. */
    const uint32_t middle = 4u * significand;
    const uint32_t upper = middle + 2u;
    const uint32_t lower_shift = stored_significand != 0u || stored_exponent <= 1u;
    const uint32_t lower = middle - 1u - lower_shift;

    /* The three in decimal, scaled by a power of 10, and whether the scaling dropped only zeros. */
    uint32_t decimal_middle;
    uint32_t decimal_upper;
    uint32_t decimal_lower;
    int32_t decimal_exponent;
    bool lower_ends_in_zeros = false;
    bool middle_ends_in_zeros = false;
    uint32_t last_removed_digit = 0;
    if (binary_exponent >= 0)
    {
        const uint32_t power = log10_pow2(binary_exponent);
        decimal_exponent = (int32_t)power;
        const int32_t shift = -binary_exponent + (int32_t)power + POW5_INVERSE_BITS + pow5_bits((int32_t)power) - 1;
        decimal_middle = multiply_shift(middle, POW5_INVERSE[power], shift);
        decimal_upper = multiply_shift(upper, POW5_INVERSE[power], shift);
        decimal_lower = multiply_shift(lower, POW5_INVERSE[power], shift);
        if (power != 0u && (decimal_upper - 1u) / DECIMAL_BASE <= decimal_lower / DECIMAL_BASE)
        {
            /* The loop below removes no digit, but rounding needs the last one the scaling removed. */
            const int32_t previous_shift =
                -binary_exponent + (int32_t)power - 1 + POW5_INVERSE_BITS + pow5_bits((int32_t)power - 1) - 1;
            last_removed_digit = multiply_shift(middle, POW5_INVERSE[power - 1u], previous_shift) % DECIMAL_BASE;
        }
        if (power <= POW5_CHECK_LIMIT)
        {
            /* At most one of the three is a multiple of 5. */
            if (middle % 5u == 0u)
            {
                middle_ends_in_zeros = multiple_of_pow5(middle, power);
            }
            else if (bounds_included)
            {
                lower_ends_in_zeros = multiple_of_pow5(lower, power);
            }
            else
            {
                decimal_upper -= multiple_of_pow5(upper, power);
            }
        }
    }
    else
    {
        const uint32_t power = log10_pow5(-binary_exponent);
        decimal_exponent = (int32_t)power + binary_exponent;
        const int32_t index = -binary_exponent - (int32_t)power;
        int32_t shift = (int32_t)power - (pow5_bits(index) - POW5_BITS);
        decimal_middle = multiply_shift(middle, POW5[index], shift);
        decimal_upper = multiply_shift(upper, POW5[index], shift);
        decimal_lower = multiply_shift(lower, POW5[index], shift);
        if (power != 0u && (decimal_upper - 1u) / DECIMAL_BASE <= decimal_lower / DECIMAL_BASE)
        {
            shift = (int32_t)power - 1 - (pow5_bits(index + 1) - POW5_BITS);
            last_removed_digit = multiply_shift(middle, POW5[index + 1], shift) % DECIMAL_BASE;
        }
        if (power <= 1u)
        {
            /* middle is 4 * significand, so it ends in two zero bits; lower ends in one only when lower_shift is 1. */
            middle_ends_in_zeros = true;
            if (bounds_included)
            {
                lower_ends_in_zeros = lower_shift == 1u;
            }
            else
            {
                decimal_upper--;
            }
        }
        else if (power < POW2_CHECK_LIMIT)
        {
            middle_ends_in_zeros = multiple_of_pow2(middle, power - 1u);
        }
    }

    /* Drop digits while the upper and lower points still differ, then round the middle. */
    int32_t removed = 0;
    uint32_t digits;
    if (lower_ends_in_zeros || middle_ends_in_zeros)
    {
        while (decimal_upper / DECIMAL_BASE > decimal_lower / DECIMAL_BASE)
        {
            lower_ends_in_zeros &= decimal_lower % DECIMAL_BASE == 0u;
            middle_ends_in_zeros &= last_removed_digit == 0u;
            last_removed_digit = decimal_middle % DECIMAL_BASE;
            decimal_middle /= DECIMAL_BASE;
            decimal_upper /= DECIMAL_BASE;
            decimal_lower /= DECIMAL_BASE;
            removed++;
        }
        if (lower_ends_in_zeros)
        {
            while (decimal_lower % DECIMAL_BASE == 0u)
            {
                middle_ends_in_zeros &= last_removed_digit == 0u;
                last_removed_digit = decimal_middle % DECIMAL_BASE;
                decimal_middle /= DECIMAL_BASE;
                decimal_upper /= DECIMAL_BASE;
                decimal_lower /= DECIMAL_BASE;
                removed++;
            }
        }
        if (middle_ends_in_zeros && last_removed_digit == 5u && decimal_middle % 2u == 0u)
        {
            /* Exactly halfway: round to even. */
            last_removed_digit = 4u;
        }
        digits = decimal_middle
                 + ((decimal_middle == decimal_lower && (!bounds_included || !lower_ends_in_zeros))
                    || last_removed_digit >= 5u);
    }
    else
    {
        while (decimal_upper / DECIMAL_BASE > decimal_lower / DECIMAL_BASE)
        {
            last_removed_digit = decimal_middle % DECIMAL_BASE;
            decimal_middle /= DECIMAL_BASE;
            decimal_upper /= DECIMAL_BASE;
            decimal_lower /= DECIMAL_BASE;
            removed++;
        }
        digits = decimal_middle + (decimal_middle == decimal_lower || last_removed_digit >= 5u);
    }

    *exponent = decimal_exponent + removed;
    return digits;
}
