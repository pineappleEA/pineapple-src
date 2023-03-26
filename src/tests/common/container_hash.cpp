// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/container_hash.h"

TEST_CASE("ContainerHash", "[common]") {
    constexpr std::array<u8, 32> U8Values{
        114, 10, 238, 189, 199, 242, 86, 96, 53,  193, 195, 247, 249, 56, 253, 61,
        205, 3,  172, 4,   210, 197, 43, 72, 103, 8,   99,  89,  5,   97, 68,  196,
    };
    constexpr std::array<u16, 32> U16Values{
        61586, 49151, 3313,  11641, 31695, 54795, 46764, 20965, 23287, 14039, 19265,
        49093, 58932, 22518, 27139, 42825, 57417, 54237, 48057, 14586, 42813, 32994,
        33970, 45501, 5619,  15895, 33227, 27509, 25391, 37275, 60218, 17599,
    };
    constexpr std::array<u32, 32> U32Values{
        3838402410, 2029146863, 1730869921, 985528872,  186773874,  2094639868, 3324775932,
        1795512424, 2571165571, 3256934519, 2358691590, 2752682538, 1484336451, 378124520,
        3463015699, 3395942161, 1263211979, 3473632889, 3039822212, 2068707357, 2223837919,
        1823232191, 1583884041, 1264393380, 4087566993, 3188607101, 3933680362, 1464520765,
        1786838406, 1311734848, 2773642241, 3993641692,
    };
    constexpr std::array<u64, 32> U64Values{
        5908025796157537817,  10947547850358315100, 844798943576724669,   7999662937458523703,
        4006550374705895164,  1832550525423503632,  9323088254855830976,  12028890075598379412,
        6021511300787826236,  7864675007938747948,  18099387408859708806, 6438638299316820708,
        9029399285648501543,  18195459433089960253, 17214335092761966083, 5549347964591337833,
        14899526073304962015, 5058883181561464475,  7436311795731206973,  7535129567768649864,
        1287169596809258072,  8237671246353565927,  1715230541978016153,  8443157615068813300,
        6098675262328527839,  704652094100376853,   1303411723202926503,  7808312933946424854,
        6863726670433556594,  9870361541383217495,  9273671094091079488,  17541434976160119010,
    };

    REQUIRE(Common::HashValue(U8Values) == 5867183267093890552);
    REQUIRE(Common::HashValue(U16Values) == 9594135570564347135);
    REQUIRE(Common::HashValue(U32Values) == 13123757214696618460);
    REQUIRE(Common::HashValue(U64Values) == 7296500016546938380);
}
