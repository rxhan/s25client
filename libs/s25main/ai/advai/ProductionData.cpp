// Produktionsketten-Tabelle der KI – siehe ProductionData.h.
// Werte aus libs/s25main/gameData/BuildingConsts.cpp (BLD_WORK_DESC, BUILDING_SIZE).
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ProductionData.h"
#include <array>
#include <cstddef>

namespace advai {

namespace {
    constexpr std::size_t kNumBld = 40; // = NumEnumValues<BuildingType>

    std::size_t idx(BuildingType bt) { return static_cast<std::size_t>(bt); }

    std::array<ChainInfo, kNumBld> buildTable()
    {
        std::array<ChainInfo, kNumBld> t{};

        using B = BuildingType;
        using G = GoodType;
        using Q = BuildingQuality;

        // nur Größe
        auto set = [&](B bt, Q bq) -> ChainInfo& {
            ChainInfo& c = t[idx(bt)];
            c.size = bq;
            return c;
        };
        // Produktionsgebäude
        auto prod = [&](B bt, Q bq, G out, std::vector<G> in) -> ChainInfo& {
            ChainInfo& c = set(bt, bq);
            c.producesGood = true;
            c.output = out;
            c.inputs = std::move(in);
            return c;
        };
        // Mine (Bergplatz, Nahrung gesondert behandelt)
        auto mine = [&](B bt, G out) {
            ChainInfo& c = set(bt, Q::Mine);
            c.producesGood = true;
            c.output = out;
            c.isMine = true;
        };
        // ressourcenabhängiger Bauplatz
        auto res = [&](B bt, AIResource r) {
            t[idx(bt)].hasResource = true;
            t[idx(bt)].resource = r;
        };

        // --- Lager / Sonder ---
        set(B::Headquarters, Q::Castle);
        set(B::Storehouse, Q::House);
        set(B::HarborBuilding, Q::Harbor);
        set(B::LookoutTower, Q::Hut);

        // --- Militär ---
        set(B::Barracks, Q::Hut);
        set(B::Guardhouse, Q::Hut);
        set(B::Watchtower, Q::House);
        set(B::Fortress, Q::Castle);
        set(B::Catapult, Q::House);

        // --- Holz & Bau ---
        prod(B::Woodcutter, Q::Hut, G::Wood, {});  res(B::Woodcutter, AIResource::Wood);
        set(B::Forester, Q::Hut);                  res(B::Forester, AIResource::Plantspace);
        prod(B::Sawmill, Q::House, G::Boards, {G::Wood});
        prod(B::Quarry, Q::Hut, G::Stones, {});    res(B::Quarry, AIResource::Stones);
        prod(B::Well, Q::Hut, G::Water, {});

        // --- Nahrung ---
        prod(B::Farm, Q::Castle, G::Grain, {});    res(B::Farm, AIResource::Plantspace);
        prod(B::Mill, Q::House, G::Flour, {G::Grain});
        prod(B::Bakery, Q::House, G::Bread, {G::Flour, G::Water});
        prod(B::Fishery, Q::Hut, G::Fish, {});     res(B::Fishery, AIResource::Fish);
        prod(B::Hunter, Q::Hut, G::Meat, {});
        prod(B::PigFarm, Q::Castle, G::Ham, {G::Grain, G::Water});
        prod(B::Slaughterhouse, Q::House, G::Meat, {G::Ham});
        prod(B::Brewery, Q::House, G::Beer, {G::Grain, G::Water});
        set(B::DonkeyBreeder, Q::Castle).inputs = {G::Grain, G::Water};

        // --- Bergbau ---
        mine(B::GraniteMine, G::Stones); res(B::GraniteMine, AIResource::Granite);
        mine(B::CoalMine, G::Coal);      res(B::CoalMine, AIResource::Coal);
        mine(B::IronMine, G::IronOre);   res(B::IronMine, AIResource::Ironore);
        mine(B::GoldMine, G::Gold);      res(B::GoldMine, AIResource::Gold);

        // --- Metall / Gold ---
        prod(B::Ironsmelter, Q::House, G::Iron, {G::IronOre, G::Coal});
        prod(B::Metalworks, Q::House, G::Tongs, {G::Iron, G::Boards});
        prod(B::Armory, Q::House, G::Sword, {G::Iron, G::Coal});
        prod(B::Mint, Q::House, G::Coins, {G::Gold, G::Coal});
        prod(B::Charburner, Q::Castle, G::Coal, {G::Wood, G::Grain});

        // --- See ---
        prod(B::Shipyard, Q::House, G::Boat, {G::Boards});

        // --- Leder-Kette ---
        prod(B::Skinner, Q::Hut, G::Skins, {G::Ham});
        prod(B::Tannery, Q::House, G::Leather, {G::Skins, G::Boards});
        prod(B::LeatherWorks, Q::House, G::Armor, {G::Leather});

        // --- Wein / Tempel ---
        prod(B::Vineyard, Q::Castle, G::Grapes, {G::Wood, G::Water});
        prod(B::Winery, Q::House, G::Wine, {G::Grapes});
        prod(B::Temple, Q::Castle, G::Gold, {G::Wine, G::Meat, G::Bread});

        return t;
    }
} // namespace

const ChainInfo& chainOf(BuildingType bt)
{
    static const std::array<ChainInfo, kNumBld> table = buildTable();
    return table[idx(bt)];
}

} // namespace advai
