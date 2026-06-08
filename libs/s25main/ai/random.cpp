// Copyright (C) 2005 - 2025 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ai/random.h"
#include <cstdlib>
#include <string>

namespace AI {

std::minstd_rand& getRandomGenerator()
{
    // Saat: normalerweise zufällig (std::random_device). Für reproduzierbare
    // Self-Play-Messungen kann über die Umgebungsvariable RTTR_AI_SEED eine
    // feste Saat erzwungen werden – sonst variiert das Verhalten der
    // (Gegner-)KI von Lauf zu Lauf und macht jede Bewertung unbrauchbar.
    static std::minstd_rand rng([] {
        if(const char* s = std::getenv("RTTR_AI_SEED"))
        {
            try
            {
                return std::minstd_rand(static_cast<std::minstd_rand::result_type>(std::stoul(s)));
            } catch(...)
            {}
        }
        return std::minstd_rand(std::random_device{}());
    }());
    return rng;
}

} // namespace AI
