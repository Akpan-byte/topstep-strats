#pragma once
#include "ig91_common.hpp"
#include "ig91_part1.hpp"
#include "ig91_part2.hpp"
#include "ig91_part3.hpp"

namespace s546 {

inline void register_ig91(std::vector<s546::RegisteredStrategy>& out) {
    ig91::register_ig91_part1(out);
    ig91::register_ig91_part2(out);
    ig91::register_ig91_part3(out);
}

} // namespace s546
