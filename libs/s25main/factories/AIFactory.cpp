// Copyright (C) 2005 - 2021 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AIFactory.h"
#include "ai/DummyAI.h"
#include "ai/advai/AdvancedAIPlayer.h"
#include "ai/aijh/AIPlayerJH.h"
#include "gameTypes/AIInfo.h"
#include <cstdlib>

std::unique_ptr<AIPlayer> AIFactory::Create(const AI::Info& aiInfo, unsigned playerId, const GameWorldBase& world)
{
    switch(aiInfo.type)
    {
        case AI::Type::Dummy: return std::make_unique<DummyAI>(playerId, world, aiInfo.level); break;
        // Pro-Slot wählbar: AdvancedAI gezielt für DIESEN Spieler (z.B. Advanced vs JH).
        case AI::Type::Advanced: return std::make_unique<advai::AdvancedAIPlayer>(playerId, world, aiInfo.level);
        case AI::Type::Default:
        default:
            // Opt-in (global): Mit gesetzter Umgebungsvariable RTTR_USE_ADVAI ersetzt
            // unsere KI ALLE Standard-KIs (AIPlayerJH). Für gemischte Partien lieber
            // den Slot-Typ AI::Type::Advanced verwenden.
            if(std::getenv("RTTR_USE_ADVAI"))
                return std::make_unique<advai::AdvancedAIPlayer>(playerId, world, aiInfo.level);
            return std::make_unique<AIJH::AIPlayerJH>(playerId, world, aiInfo.level);
    }
}
