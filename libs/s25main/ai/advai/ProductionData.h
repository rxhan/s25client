// Produktionsketten-Datenbasis der KI.
// Werte 1:1 abgeleitet aus libs/s25main/gameData/BuildingConsts.cpp
// (BLD_WORK_DESC) und BUILDING_SIZE des RTTR-Repositories.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ai/AIResource.h"
#include "gameTypes/BuildingQuality.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/GoodTypes.h"
#include <vector>

namespace advai {

/// Beschreibt für jeden Gebäudetyp die Produktionskette und Bauanforderung.
struct ChainInfo
{
    bool producesGood = false;                 ///< stellt eine handelbare Ware her?
    GoodType output = GoodType::Nothing;       ///< erzeugte Ware (falls producesGood)
    std::vector<GoodType> inputs;              ///< benötigte Eingangswaren
    BuildingQuality size = BuildingQuality::Nothing; ///< benötigte Bauqualität
    bool isMine = false;                       ///< Mine? (braucht Bergplatz + Nahrung)
    bool hasResource = false;                  ///< platzabhängig von einer AIResource?
    AIResource resource = AIResource::Wood;    ///< zugehörige Ressource (falls hasResource)
};

/// Liefert die (statisch initialisierte) Ketteninfo für einen Gebäudetyp.
const ChainInfo& chainOf(BuildingType bt);

} // namespace advai
