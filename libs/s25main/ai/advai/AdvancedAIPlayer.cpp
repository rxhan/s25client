// AdvancedAIPlayer â€“ Implementierung. Siehe AdvancedAIPlayer.h und RTTR-KI-KONZEPT.md.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AdvancedAIPlayer.h"
#include "AIParams.h"
#include "ProductionData.h"

#include "GamePlayer.h"
#include "RoadSegment.h"
#include "ai/AIEvents.h"
#include "ai/AIInterface.h"
#include "buildings/noBuilding.h"
#include "buildings/nobBaseMilitary.h"
#include "buildings/nobBaseWarehouse.h"
#include "buildings/nobHQ.h"
#include "buildings/nobHarborBuilding.h"
#include "buildings/nobMilitary.h"
#include "buildings/nobShipYard.h"
#include "buildings/nobUsual.h"
#include "nodeObjs/noFlag.h"
#include "nodeObjs/noSign.h"
#include "gameData/BuildingProperties.h"
#include "gameData/BuildingConsts.h"
#include "gameData/JobConsts.h"
#include "gameData/MilitaryConsts.h"
#include "gameData/ToolConsts.h"
#include "helpers/EnumRange.h"
#include "nodeObjs/noShip.h"
#include "gameTypes/BuildingQuality.h"
#include "gameTypes/Direction.h"
#include "gameTypes/Inventory.h"
#include "buildings/noBuildingSite.h"
#include "figures/nofCarrier.h"
#include "notifications/BuildingNote.h"
#include "notifications/ExpeditionNote.h"
#include "notifications/NotificationManager.h"
#include "notifications/ResourceNote.h"
#include "notifications/RoadNote.h"
#include "notifications/ShipNote.h"
#include "pathfinding/RoadPathFinder.h"
#include "world/GameWorldBase.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <queue>
#include <utility>

namespace advai {

namespace {
    // Takte und weitere Schwellen sind Ã¼ber AIParams (RTTR_AI_PARAMS) tunebar.
    constexpr unsigned kSearchRadius = 14;  // Suchradius um Lager/MilitÃ¤rgebÃ¤ude
    constexpr unsigned kAttackScanRadius = 12;

    // --- Ãœbersetzung Engine-Notes -> AIEvents (gespiegelt von AIPlayerJH) ---
    void handleBuildingNote(AIEventManager& mgr, const BuildingNote& note)
    {
        using namespace AIEvent;
        std::unique_ptr<Base> ev;
        switch(note.type)
        {
            case BuildingNote::Constructed:
                ev = std::make_unique<Building>(EventType::BuildingFinished, note.pos, note.bld);
                break;
            case BuildingNote::Destroyed:
                ev = std::make_unique<Building>(EventType::BuildingDestroyed, note.pos, note.bld);
                break;
            case BuildingNote::Captured:
                ev = std::make_unique<Building>(EventType::BuildingConquered, note.pos, note.bld);
                break;
            case BuildingNote::Lost: ev = std::make_unique<Building>(EventType::BuildingLost, note.pos, note.bld); break;
            case BuildingNote::LostLand:
                ev = std::make_unique<Building>(EventType::LostLand, note.pos, note.bld);
                break;
            case BuildingNote::NoRessources:
                ev = std::make_unique<Building>(EventType::NoMoreResourcesReachable, note.pos, note.bld);
                break;
            default: return; // LuaOrder u.a. ignorieren
        }
        mgr.AddAIEvent(std::move(ev));
    }
    void handleExpeditionNote(AIEventManager& mgr, const ExpeditionNote& note)
    {
        using namespace AIEvent;
        if(note.type == ExpeditionNote::Waiting)
            mgr.AddAIEvent(std::make_unique<Location>(EventType::ExpeditionWaiting, note.pos));
        else if(note.type == ExpeditionNote::ColonyFounded)
            mgr.AddAIEvent(std::make_unique<Location>(EventType::NewColonyFounded, note.pos));
    }
    void handleResourceNote(AIEventManager& mgr, const ResourceNote& note)
    {
        mgr.AddAIEvent(std::make_unique<AIEvent::Resource>(AIEvent::EventType::ResourceFound, note.pos, note.res));
    }
    void handleShipNote(AIEventManager& mgr, const ShipNote& note)
    {
        if(note.type == ShipNote::Constructed)
            mgr.AddAIEvent(std::make_unique<AIEvent::Location>(AIEvent::EventType::ShipBuilt, note.pos));
    }
} // namespace

AdvancedAIPlayer::AdvancedAIPlayer(unsigned char playerId, const GameWorldBase& gwb, AI::Level level)
    : AIPlayer(playerId, gwb, level)
{
    // Reaktive Schicht: Engine-Notifications abonnieren und (gefiltert nach
    // eigenem Spieler) in die AIEvent-Queue Ã¼bersetzen.
    NotificationManager& notes = gwb.GetNotifications();
    subBuilding_ = notes.subscribe<BuildingNote>([this, playerId](const BuildingNote& n) {
        if(n.player == playerId)
            handleBuildingNote(eventManager_, n);
    });
    subExpedition_ = notes.subscribe<ExpeditionNote>([this, playerId](const ExpeditionNote& n) {
        if(n.player == playerId)
            handleExpeditionNote(eventManager_, n);
    });
    subResource_ = notes.subscribe<ResourceNote>([this, playerId](const ResourceNote& n) {
        if(n.player == playerId)
            handleResourceNote(eventManager_, n);
    });
    subShip_ = notes.subscribe<ShipNote>([this, playerId](const ShipNote& n) {
        if(n.player == playerId)
            handleShipNote(eventManager_, n);
    });
    // RoadNote wird derzeit nur passiv verarbeitet (Polling baut Wege neu).
    subRoad_ = notes.subscribe<RoadNote>([](const RoadNote&) {});
}

// ===========================================================================
// Herzschlag
// ===========================================================================
void AdvancedAIPlayer::RunGF(unsigned gf, bool gfisnwf)
{
    currentGF_ = gf;

    if(surrendered_)
        return;

    // Niederlage: Falls keine Basis mehr existiert, aufgeben.
    if(aii.GetHeadquarter() == nullptr && aii.GetStorehouses().empty() && aii.GetMilitaryBuildings().empty())
    {
        if(!surrendered_)
        {
            aii.Surrender();
            surrendered_ = true;
        }
        return;
    }

    if(!initialized_)
    {
        runInit();
        initialized_ = true;
    }

    // --- Reaktive Schicht zuerst: auf Ereignisse seit dem letzten GF reagieren ---
    reactEconomy_ = false;
    reactExpansion_ = false;
    drainEvents();

    // --- Deliberative Schicht: amortisiert getaktete Planer ---
    const AIParams& P = AIParams::get();
    const unsigned off = playerId; // pro Spieler versetzt
    bool ranEconomy = false;
    if((gf + off) % P.econInterval == 0)
    {
        runEconomy();
        ranEconomy = true;
    }
    if((gf + off) % P.expandInterval == 0)
        runExpansion();
    if((gf + off) % P.militaryInterval == 0)
        runMilitary();
    if((gf + off) % P.scoutInterval == 0)
        runScouting();
    if((gf + off) % P.seaInterval == 0)
        runSea();
    if((gf + off) % P.settingsInterval == 0)
        adjustSettings();
    if((gf + off) % P.roadOptInterval == 0)
        runRoadOptimize();

    // --- Ereignis-getriebene SofortmaÃŸnahmen (hÃ¶chstens einmal pro GF) ---
    if(reactExpansion_)
        placeMilitary();
    if(reactEconomy_ && !ranEconomy)
        runEconomy();

    // --- Aufgeschobene Wegenetz-Anbindung: nur am Netzwerk-Frame, damit die
    //     gebauten StraÃŸen vor der nÃ¤chsten Planung wirksam sind (keine sich
    //     Ã¼berschneidenden Wege durch gleichzeitige AuftrÃ¤ge). ---
    if(gfisnwf)
        runConnect();
}

// Verarbeitet alle seit dem letzten GF eingegangenen Ereignisse.
void AdvancedAIPlayer::drainEvents()
{
    while(eventManager_.EventAvailable())
    {
        std::unique_ptr<AIEvent::Base> ev = eventManager_.GetEvent();
        if(ev)
            handleEvent(*ev);
    }
}

void AdvancedAIPlayer::handleEvent(const AIEvent::Base& ev)
{
    using namespace AIEvent;
    switch(ev.GetType())
    {
        case EventType::NoMoreResourcesReachable:
        {
            // ErschÃ¶pfte Mine/Quelle abreiÃŸen -> Bauplatz frei, Ersatz planen.
            const auto& b = static_cast<const Building&>(ev);
            const BuildingType bt = b.GetBuildingType();
            if(!aii.IsObjectTypeOnNode(b.GetPos(), NodalObjectType::Building))
                break;
            if(bt == BuildingType::Woodcutter && !shouldDestroyDepletedWoodcutter(b.GetPos()))
            {
                reactEconomy_ = true;
                break;
            }
            // Leergefischten Ort MERKEN: Fische regenerieren sich nicht -> hier (und
            // in Arbeitsradius-NÃ¤he) keine neue FischerhÃ¼tte mehr bauen.
            if(bt == BuildingType::Fishery)
                depletedFishSpots_.push_back(b.GetPos());
            aii.DestroyBuilding(b.GetPos());
            if(bt == BuildingType::Woodcutter)
                forgetDepletedWoodcutter(b.GetPos());
            reactEconomy_ = true;
            break;
        }
        case EventType::BuildingDestroyed:
        case EventType::BuildingLost:
        case EventType::LostLand:
            if(static_cast<const Building&>(ev).GetBuildingType() == BuildingType::Woodcutter)
                forgetDepletedWoodcutter(static_cast<const Building&>(ev).GetPos());
            // Verlust an der Grenze / zerstÃ¶rte Kette -> nachbauen & verstÃ¤rken.
            reactExpansion_ = true;
            reactEconomy_ = true;
            break;
        case EventType::BuildingConquered:
        case EventType::ResourceFound:
        case EventType::BuildingFinished:
            // Neue MÃ¶glichkeiten -> Wirtschaft erneut prÃ¼fen.
            reactEconomy_ = true;
            break;
        case EventType::ExpeditionWaiting:
        {
            // Expeditionsschiff wartet an einem KÃ¼stenplatz -> Kolonie grÃ¼nden.
            const auto& loc = static_cast<const Location&>(ev);
            foundColonyAt(loc.GetPos());
            break;
        }
        case EventType::ShipBuilt:
            // Neues Schiff -> ggf. sofort Expedition prÃ¼fen.
            considerExpedition();
            break;
        default:
            // NewColonyFounded -> nÃ¤chster See-Tick baut die Kolonie aus
            // RoadConstruction* -> Polling Ã¼bernimmt
            break;
    }
}

bool AdvancedAIPlayer::shouldKeepDepletedWoodcutter(MapPoint pos) const
{
    constexpr unsigned kWoodcutterKeepRadius = 7;

    bool hasNearbyForester = false;
    for(const nobUsual* forester : aii.GetBuildings(BuildingType::Forester))
    {
        if(gwb.CalcDistance(pos, forester->GetPos()) <= kWoodcutterKeepRadius)
        {
            hasNearbyForester = true;
            break;
        }
    }
    if(!hasNearbyForester)
        return false;

    for(MapPoint pt : collectPoints(pos, kWoodcutterKeepRadius))
    {
        if(!aii.IsOwnTerritory(pt))
            continue;
        if(aii.GetSurfaceResource(pt) == AISurfaceResource::Wood
           || aii.GetResourceRating(pt, AIResource::Plantspace) > 0)
            return true;
    }
    return false;
}

bool AdvancedAIPlayer::shouldDestroyDepletedWoodcutter(MapPoint pos)
{
    constexpr unsigned kWoodcutterIdleHysteresis = 2000;

    if(shouldKeepDepletedWoodcutter(pos))
    {
        forgetDepletedWoodcutter(pos);
        return false;
    }

    auto it = std::find_if(depletedWoodcutters_.begin(), depletedWoodcutters_.end(),
                           [pos](const DepletedWoodcutter& entry) { return entry.pos == pos; });
    if(it == depletedWoodcutters_.end())
    {
        depletedWoodcutters_.push_back({pos, currentGF_});
        return false;
    }

    return currentGF_ - it->firstGF >= kWoodcutterIdleHysteresis;
}

void AdvancedAIPlayer::forgetDepletedWoodcutter(MapPoint pos)
{
    depletedWoodcutters_.erase(
      std::remove_if(depletedWoodcutters_.begin(), depletedWoodcutters_.end(),
                     [pos](const DepletedWoodcutter& entry) { return entry.pos == pos; }),
      depletedWoodcutters_.end());
}

void AdvancedAIPlayer::runInit()
{
    // Vor dem ersten Bau: prüfen, welche (Holz-)Ressourcen zur Verfügung stehen.
    surveyWoodResources();
    adjustSettings();
}

// Erfasst die verfügbare Waldfläche im eigenen Territorium und leitet daraus die
// karten-skalierte Holzketten-Größe ab. Bewusst günstig (flaches seen-Array statt
// O(n²)-Dedup), da pro Ökonomie-Takt einmal über die Lager-/Militär-Umkreise läuft.
void AdvancedAIPlayer::surveyWoodResources()
{
    const AIParams& P = AIParams::get();
    const MapExtent mapSize = gwb.GetSize();
    const std::size_t area = static_cast<std::size_t>(mapSize.x) * mapSize.y;
    smallMap_ = area < static_cast<std::size_t>(std::max(0, P.smallMapArea));

    std::vector<char> seen(area, 0);
    auto toIdx = [&](MapPoint p) {
        return static_cast<std::size_t>(p.x) + static_cast<std::size_t>(p.y) * mapSize.x;
    };

    std::vector<MapPoint> centers = warehousePositions();
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        centers.push_back(mb->GetPos());

    int woodSpots = 0;  // Plätze mit fällbaren Bäumen (Holzfäller)
    int plantSpots = 0; // Plätze mit Pflanzfläche (Förster)
    for(MapPoint center : centers)
    {
        for(MapPoint pt : collectPoints(center, kSearchRadius))
        {
            const std::size_t i = toIdx(pt);
            if(seen[i])
                continue;
            seen[i] = 1;
            if(!aii.IsOwnTerritory(pt))
                continue;
            if(!canUseBq(aii.GetBuildingQuality(pt), BuildingQuality::Hut))
                continue;
            if(aii.GetResourceRating(pt, AIResource::Wood) > 0)
                ++woodSpots;
            if(aii.GetResourceRating(pt, AIResource::Plantspace) > 0)
                ++plantSpots;
        }
    }
    woodcutterSpots_ = woodSpots;
    foresterSpots_ = plantSpots;
    // Kapazität aus der PFLANZFLÄCHE: Förster pflanzen Bäume nach, also bestimmt die
    // verfügbare Pflanzfläche (nicht die schon stehenden Bäume – die liegen NIE auf
    // einem baubaren Feld, daher wäre woodSpots am Bauplatz stets 0) nachhaltig, wie
    // viele Holzketten-Einheiten (je 2 Holzfäller / 1 Förster / 1 Säge) der Wald
    // trägt. Skaliert mit dem Wald (Nutzer-Vorgabe), gedeckelt durch woodMaxUnits;
    // sehr kleine Karten bleiben bei genau 1 Einheit.
    constexpr int kPlantSpotsPerUnit = 12;
    const int byCapacity = plantSpots / kPlantSpotsPerUnit;
    woodUnits_ = smallMap_ ? 1 : std::clamp(byCapacity, 1, std::max(1, P.woodMaxUnits));
    if(std::getenv("RTTR_AI_DEBUG"))
    {
        static int dn = 0;
        if(dn < 60)
        {
            ++dn;
            std::cerr << "[survey] p=" << static_cast<int>(playerId) << " area=" << area
                      << " small=" << smallMap_ << " woodSpots=" << woodSpots
                      << " plantSpots=" << plantSpots << " byCap=" << byCapacity
                      << " woodUnits=" << woodUnits_ << "\n";
        }
    }
}

// ===========================================================================
// Schicht: Ã–konomie (Bedarfsgetriebenes Bauen + Deadlock-Schutz)
// ===========================================================================
void AdvancedAIPlayer::runEconomy()
{
    // "Erneut prüfen": verfügbare Holz-Ressourcen vor jeder Bau-Planung neu erfassen
    // (Territorium wächst -> mehr Wald -> die Holzkette darf weiter wachsen).
    surveyWoodResources();

    // PrioritÃ¤tsreihenfolge: Grundversorgung -> Nahrung -> Metall/Werkzeuge ->
    // MilitÃ¤rgÃ¼ter. Der Deadlock-Schutz steckt in den Bedingungen von
    // desiredCount() (Werkzeug/Nahrung/Bretter werden bei Mangel hochgezogen).
    static const BuildingType order[] = {
      BuildingType::Woodcutter,  BuildingType::Forester,   BuildingType::Sawmill,
      BuildingType::Quarry,      BuildingType::Well,        BuildingType::Farm,
      BuildingType::Mill,        BuildingType::Bakery,      BuildingType::Fishery,
      BuildingType::Hunter,      BuildingType::Ironsmelter, BuildingType::Metalworks,
      BuildingType::CoalMine,    BuildingType::IronMine,    BuildingType::Brewery,
      BuildingType::Armory,      BuildingType::GoldMine,    BuildingType::Mint,
      BuildingType::DonkeyBreeder, BuildingType::Storehouse,
    };

    // Bis zu mehreren Bauvorhaben pro Tick (Wirtschaft schneller hochfahren).
    int built = 0;
    for(BuildingType bt : order)
    {
        if(total(bt) < desiredCount(bt) && buildBuilding(bt))
        {
            if(++built >= 5)
                break;
        }
    }
}

helpers::EnumArray<int, Tool> AdvancedAIPlayer::calculateToolDemand() const
{
    helpers::EnumArray<int, Tool> demand{};
    for(const Tool t : helpers::enumRange<Tool>())
    {
        const GoodType good = TOOL_TO_GOOD[t];
        int openWorkplaces = 0;
        int availableWorkers = 0;
        const int availableTools = stock(good);

        for(const Job job : helpers::enumRange<Job>())
        {
            const auto& tool = JOB_CONSTS[job].tool;
            if(!tool || *tool != good)
                continue;

            int jobOpenWorkplaces = 0;
            for(const BuildingType bt : helpers::enumRange<BuildingType>())
            {
                if(BLD_WORK_DESC[bt].job != job)
                    continue;
                for(const noBuildingSite* site : aii.GetBuildingSites())
                    if(site->GetBuildingType() == bt)
                        ++jobOpenWorkplaces;
                for(const nobUsual* bld : aii.GetBuildings(bt))
                    if(!bld->GetWorker())
                        ++jobOpenWorkplaces;
            }

            openWorkplaces += jobOpenWorkplaces;
            availableWorkers += stock(job);
        }

        demand[t] = std::max(0, openWorkplaces - availableWorkers - availableTools);
    }

    if(stock(GoodType::Saw) + stock(Job::Carpenter) < 2)
        demand[Tool::Saw] = std::max(demand[Tool::Saw], 2 - stock(GoodType::Saw) - stock(Job::Carpenter));
    if(stock(GoodType::Axe) + stock(Job::Woodcutter) < 2)
        demand[Tool::Axe] = std::max(demand[Tool::Axe], 2 - stock(GoodType::Axe) - stock(Job::Woodcutter));
    if(stock(GoodType::PickAxe) + stock(Job::Stonemason) < 2)
        demand[Tool::PickAxe] =
          std::max(demand[Tool::PickAxe], 2 - stock(GoodType::PickAxe) - stock(Job::Stonemason));

    const int constructionSites = static_cast<int>(aii.GetBuildingSites().size());
    demand[Tool::Hammer] =
      std::max(demand[Tool::Hammer], constructionSites - stock(Job::Builder) - stock(GoodType::Hammer));
    demand[Tool::Shovel] =
      std::max(demand[Tool::Shovel], constructionSites - stock(Job::Planer) - stock(GoodType::Shovel));

    if(stock(GoodType::Hammer) == 0)
        demand[Tool::Hammer] = std::max(demand[Tool::Hammer], 1);
    if(stock(GoodType::Shovel) == 0)
        demand[Tool::Shovel] = std::max(demand[Tool::Shovel], 1);
    if(stock(GoodType::Tongs) == 0 && anyMine())
        demand[Tool::Tongs] = std::max(demand[Tool::Tongs], 1);
    return demand;
}

int AdvancedAIPlayer::totalToolPressure() const
{
    const auto demand = calculateToolDemand();
    int pressure = 0;
    for(const Tool t : helpers::enumRange<Tool>())
    {
        int queued = 0;
        if(ggs.isEnabled(AddonId::TOOL_ORDERING))
            queued = static_cast<int>(player.GetToolsOrderedVisual(t));
        pressure += std::max(demand[t], queued);
    }
    return pressure;
}

int AdvancedAIPlayer::desiredCount(BuildingType bt) const
{
    const int mil = numMilitary();
    const int wc = total(BuildingType::Woodcutter);
    const bool mines = anyMine();
    const int foodStock = stock(GoodType::Fish) + stock(GoodType::Meat) + stock(GoodType::Bread);
    auto productivePct = [&](BuildingType prodBt) {
        int pct = 0;
        for(const nobUsual* b : aii.GetBuildings(prodBt))
            pct += b->GetProductivity();
        return pct;
    };
    auto mineableSpots = [&](AIResource res) {
        std::vector<MapPoint> seen;
        std::vector<MapPoint> centers = warehousePositions();
        for(const nobMilitary* mb : aii.GetMilitaryBuildings())
            centers.push_back(mb->GetPos());
        int n = 0;
        for(MapPoint center : centers)
        {
            for(MapPoint pt : collectPoints(center, kSearchRadius + 8))
            {
                if(std::find(seen.begin(), seen.end(), pt) != seen.end())
                    continue;
                seen.push_back(pt);
                if(!aii.IsOwnTerritory(pt))
                    continue;
                if(!canUseBq(aii.GetBuildingQuality(pt), BuildingQuality::Mine))
                    continue;
                if(aii.GetResourceRating(pt, res) <= 0)
                    continue;
                ++n;
            }
        }
        return n;
    };
    auto plannedBreweries = [&]() {
        if(total(BuildingType::Armory) == 0)
            return 0;
        const int armoryProductivity = productivePct(BuildingType::Armory);
        return std::max(1, armoryProductivity / 400);
    };
    auto plannedDonkeyBreeders = [&]() {
        int n = total(BuildingType::DonkeyBreeder);
        if(total(BuildingType::Mill) == 0 || total(BuildingType::Bakery) == 0)
            return n;
        if(ggs.isEnabled(AddonId::MANUAL_ROAD_ENLARGEMENT))
            n = std::max(n, mil >= 16 ? 2 : 1);
        else if(stock(GoodType::Grain) >= 25)
            n = std::max(n, 1);
        return n;
    };

    switch(bt)
    {
        // ====== HOLZ-KETTE (Treiber: HolzfÃ¤ller; Brettbedarf wÃ¤chst mit mil) ======
        // VerhÃ¤ltnis FÃ¶rster:HolzfÃ¤ller:SÃ¤ge = 1:1:~Â½ (Wald nachhaltig, SÃ¤ge schnell).
        // Kalibriert an Engine-Arbeitszeiten (JOB_CONSTS): FÃ¶rster-Zyklus ~370,
        // HolzfÃ¤ller ~937 -> 1 FÃ¶rster versorgt ~2 HolzfÃ¤ller. SÃ¤ge (Zimmermann)
        // ~575 -> ~1 SÃ¤ge je 2 HolzfÃ¤ller.
        // Holzfäller = Treiber. Auf normalen Karten zusätzlich KARTEN-SKALIERT: bis
        // woodUnits_ Einheiten (je 2 Holzfäller) hochziehen, begrenzt durch die real
        // verfügbare Waldfläche (surveyWoodResources). Der Militär-Term (2+mil/2)
        // bleibt die Untergrenze (Brettbedarf des Bauens). Sehr kleine Karten bleiben
        // genügsam (Wege-Durchsatz ist dort der Engpass, s. ITERATION-BEFUNDE).
        // woodChainScaling=0 -> altes Verhalten (für A/B-Messung der Karten-Skalierung).
        case BuildingType::Woodcutter:
            if(AIParams::get().woodChainScaling == 0 || smallMap_)
                return 2 + mil / 2;
            return std::max(2 + mil / 2, 2 * woodUnits_);
        // 1 Förster je 2 Holzfäller (mehr als das frühere ~1:3 -> nachhaltigerer Wald,
        // s. Nutzer-Vorgabe). Durch die verfügbare Pflanzfläche begrenzt. Sehr kleine
        // Karten / Skalierung aus: konservatives ~1:3 wie bisher.
        case BuildingType::Forester:
            if(wc <= 0)
                return 0;
            if(AIParams::get().woodChainScaling == 0 || smallMap_)
                return std::max(1, (wc + 2) / 3);
            return std::min(foresterSpots_, std::max(1, (wc + 1) / 2));
        case BuildingType::Sawmill: return std::max(1, (wc + 1) / 2);            // 1 SÃ¤ge : 2 HolzfÃ¤ller

        case BuildingType::Quarry: return 2 + mil / 3; // Steine (Platzierung an Steinvorkommen gebunden)

        // ====== NAHRUNG / GETREIDE (Wurzel: Farm) ======
        case BuildingType::Farm:
            return 1 + mil / 3 + plannedBreweries() + plannedDonkeyBreeders();
        case BuildingType::Mill:
            return std::max(total(BuildingType::Farm) > 0 ? 1 : 0, total(BuildingType::Farm) / 2);
        case BuildingType::Bakery: return total(BuildingType::Mill);                  // 1 BÃ¤cker je MÃ¼hle
        // Brauerei: Bierbedarf wÃ¤chst mit MilitÃ¤r (Soldaten); Getreide-Angebot
        // (HÃ¶fe = 1+mil/3) skaliert ebenfalls mit mil -> konsistent.
        case BuildingType::Brewery:
        {
            // Breweries compete directly with bread for grain. Beer demand comes
            // from soldier production, so cap breweries by actually productive
            // armories: one brewery per 400% armory productivity. The first
            // armory gets one brewery as soon as farm + well exist, avoiding a
            // beer deadlock before the armory can recruit soldiers.
            if(total(BuildingType::Armory) == 0 || total(BuildingType::Farm) == 0 || total(BuildingType::Well) == 0)
                return 0;
            const int farmProductivity = productivePct(BuildingType::Farm);
            const int grainLimited = farmProductivity / 400; // about one brewery per four productive farms
            const int armoryProductivity = productivePct(BuildingType::Armory);
            const int armoryLimited = armoryProductivity / 400;
            return std::max(1, std::min(grainLimited, armoryLimited));
        }
        case BuildingType::Well:
        {
            const int waterConsumers = total(BuildingType::Bakery)
                                       + std::max(total(BuildingType::Brewery), plannedBreweries())
                                       + plannedDonkeyBreeders();
            return (total(BuildingType::Farm) > 0 || mines) ?
                     std::max(1, (waterConsumers + 1) / 2) :
                     0;
        }
        case BuildingType::Fishery: return 1 + mil / 5 + (mines ? 1 : 0) + ((mines && foodStock == 0) ? 1 : 0);
        case BuildingType::Hunter: return 1 + mil / 5; // Basis-Nahrung

        // ====== METALL-KETTE: ALLES an den Zulieferer koppeln, NICHT an mil! ======
        // (FrÃ¼her skalierten Schlosserei/Schmiede mit mil -> 10 Schmieden bei 2
        // Schmelzen. Jetzt: Verbraucher ~ Anzahl Schmelzen = Eisenbarren-Angebot.)
        // Minen skalieren mit dem BEDARF (Territorium/MilitÃ¤r) â€“ begrenzt durch die
        // gefundenen Erzvorkommen (Platzierung + AufklÃ¤rung) und die Nahrung (mehr
        // Nahrung -> mehr Bergarbeiter). Nicht mehr fest bei 2!
        case BuildingType::CoalMine:
        case BuildingType::IronMine:
        case BuildingType::GoldMine:
        {
            if(bt == BuildingType::CoalMine)
                return std::min(2 + mil / 4, mineableSpots(AIResource::Coal));
            if(bt == BuildingType::IronMine)
                // Eisen NICHT Ã¼ber die Kohle hinaus: Eisenerz ist ohne Kohle zum
                // Schmelzen nutzlos, und Kohle versorgt zusÃ¤tzlich die Schmiede.
                return std::min({2 + mil / 4, total(BuildingType::CoalMine), mineableSpots(AIResource::Ironore)});
            return std::min(1 + mil / 12, mineableSpots(AIResource::Gold));
        }
        // Schmelze braucht Eisenerz UND Kohle -> durch das knappere begrenzt
        // (Mind. 1, solange Eisen da ist: lÃ¤uft notfalls auf HQ-Kohlevorrat).
        case BuildingType::Ironsmelter:
            return total(BuildingType::IronMine) > 0 ?
                     std::max(1, std::min(total(BuildingType::IronMine), total(BuildingType::CoalMine))) :
                     0;
        // Eisenbarren-Verbraucher an die SCHMELZEN koppeln (nicht an mil!), aber
        // beide Linien (Werkzeug + Waffen) mit je mind. 1 vertreten, sobald Eisen
        // flieÃŸt. So bleibt es im VerhÃ¤ltnis (~1 Verbraucher je Schmelze) statt
        // 10 Schmieden bei 2 Schmelzen.
        case BuildingType::Metalworks: // Schlosserei (Werkzeug)
        {
            const int pressure = totalToolPressure();
            const int smelters = total(BuildingType::Ironsmelter);
            if(pressure <= 0 || (smelters == 0 && stock(GoodType::Iron) < 8))
                return 0;
            const int ironLimited = smelters > 0 ? std::max(1, (smelters + 1) / 2) : 1;
            const int queueLimited = pressure >= 8 ? 1 + (pressure - 8) / 8 : 1;
            return std::min(ironLimited, queueLimited);
        }
        case BuildingType::Armory: // Waffenschmiede
            return total(BuildingType::Ironsmelter) > 0 ? std::max(1, total(BuildingType::Ironsmelter) / 2) : 0;
        case BuildingType::Mint: return total(BuildingType::GoldMine) > 0 ? 1 : 0;
        // Eselzucht: Esel beschleunigen den Warentransport auf stark genutzten
        // Wegen (EselstraÃŸen). In der Regel genÃ¼gt EINE; bei groÃŸem Reich (viel
        // FlÃ¤che ~ viele MilitÃ¤rgebÃ¤ude) sind ZWEI ok. Erst wenn die Nahrungskette
        // steht (genug Getreide-HÃ¶fe + MÃ¼hle + BÃ¤cker), sonst hungert die Nahrung.
        case BuildingType::DonkeyBreeder:
        {
            // Esel beschleunigen den Transport, VERBRAUCHEN aber Getreide. Erst,
            // wenn die Nahrungskette steht (MÃ¼hle+BÃ¤cker).
            if(total(BuildingType::Mill) == 0 || total(BuildingType::Bakery) == 0)
                return 0;
            // Hat der Spieler das Addon "Wege manuell aufwerten" gewÃ¤hlt, will er die
            // ESEL-STRATEGIE (EselstraÃŸen) -> Eselzucht bauen. 1 reicht meist; bei
            // groÃŸem Reich (viel FlÃ¤che ~ viele MilitÃ¤rgebÃ¤ude) 2.
            if(ggs.isEnabled(AddonId::MANUAL_ROAD_ENLARGEMENT))
                return mil >= 16 ? 2 : 1;
            // Sonst nur bei echtem Getreide-ÃœBERSCHUSS (sonst nimmt der Esel der
            // Nahrung -> Soldaten das Getreide weg; im Self-Play klar schlechter).
            return stock(GoodType::Grain) >= 25 ? 1 : 0;
        }
        // Vorgeschobene LagerhÃ¤user als zusÃ¤tzliche Netz-Knoten: verkÃ¼rzen die
        // Wege zu entfernten BauplÃ¤tzen und entlasten das Ã¼berlastete HQ-Netz
        // (Tipp: "LagerhÃ¤user in bestimmtem Abstand gegen Wege-EngpÃ¤sse").
        case BuildingType::Storehouse:
        {
            const int stores = static_cast<int>(aii.GetStorehouses().size());
            int blds = mil;
            for(const BuildingType bldType : helpers::enumRange<BuildingType>())
                blds += static_cast<int>(aii.GetBuildings(bldType).size());
            if(stores < 2 && ((mil >= 10 && blds >= 26) || mil >= 14))
                return 1;
            if(stores < 3 && ((mil >= 18 && blds >= 42) || mil >= 24))
                return 1;
            if(stores < 4 && ((mil >= 28 && blds >= 62) || mil >= 36))
                return 1;
            return 0;
        }
        default: return 0;
    }
}

// ===========================================================================
// Platzierung & Wegenetz
// ===========================================================================
bool AdvancedAIPlayer::buildBuilding(BuildingType bt)
{
    // Vorgeschobenes Lagerhaus: bewusst WEIT weg vom bestehenden Netz (nahe
    // einem Grenz-MilitÃ¤rgebÃ¤ude) als neuer Knoten. Nur MilitÃ¤rzentren, und der
    // Platz muss deutlich von vorhandenen Lagern entfernt liegen.
    if(bt == BuildingType::Storehouse)
    {
        std::vector<MapPoint> centers;
        for(const nobMilitary* mb : aii.GetMilitaryBuildings())
            centers.push_back(mb->GetPos());
        for(const BuildingType bldType : helpers::enumRange<BuildingType>())
        {
            if(bldType == BuildingType::Headquarters || bldType == BuildingType::Storehouse
               || bldType == BuildingType::HarborBuilding)
                continue;
            for(const nobUsual* bld : aii.GetBuildings(bldType))
                centers.push_back(bld->GetPos());
        }
        for(const noBuildingSite* site : aii.GetBuildingSites())
        {
            const BuildingType siteType = site->GetBuildingType();
            if(siteType != BuildingType::Storehouse && siteType != BuildingType::HarborBuilding)
                centers.push_back(site->GetPos());
        }
        if(centers.empty())
            return false;
        return placeNear(bt, centers, 18);
    }

    // RÃ„UMLICHE NÃ„HE: nachgelagerte GebÃ¤ude bevorzugt NAHE ihrer Vorstufe bauen
    // (SÃ¤ge an HolzfÃ¤ller, Schmelze an Erz-/Kohlemine, Schlosserei/Schmiede an
    // Schmelze, MÃ¼hle an Farm, BÃ¤cker an MÃ¼hle ...). Das hÃ¤lt die Wege der
    // direkt aufeinanderfolgenden Linien kurz.
    std::vector<MapPoint> centers;
    for(BuildingType sup : supplierTypes(bt))
        for(const nobUsual* u : aii.GetBuildings(sup))
            centers.push_back(u->GetPos());

    // Keine Vorstufe vorhanden (oder GebÃ¤ude ohne Vorstufe) -> um Lager und
    // MilitÃ¤rgebÃ¤ude herum suchen (deren Territorium reicht zu Wald/Bergen).
    if(centers.empty())
    {
        centers = warehousePositions();
        for(const nobMilitary* mb : aii.GetMilitaryBuildings())
            centers.push_back(mb->GetPos());
    }
    return placeNear(bt, centers, kSearchRadius);
}

// Zulieferer-Typen (Vorstufe) je ProduktionsgebÃ¤ude â€“ fÃ¼r rÃ¤umliche NÃ¤he.
std::vector<BuildingType> AdvancedAIPlayer::supplierTypes(BuildingType bt) const
{
    using B = BuildingType;
    switch(bt)
    {
        case B::Sawmill: return {B::Woodcutter};                  // Bretter <- Holz
        case B::Forester: return {B::Woodcutter};                 // pflanzt, wo gefÃ¤llt wird
        // HolzfÃ¤ller in NÃ„CHSTE NÃ„HE der FÃ¶rster (nur dort werden BÃ¤ume nachgepflanzt
        // -> nachhaltig). 2-3 HolzfÃ¤ller scharen sich so um jeden FÃ¶rster.
        case B::Woodcutter: return {B::Forester};
        case B::Ironsmelter: return {B::IronMine, B::CoalMine};   // Barren <- Erz + Kohle
        case B::Metalworks: return {B::Ironsmelter};              // Werkzeug <- Barren
        case B::Armory: return {B::Ironsmelter};                  // Waffen  <- Barren
        case B::Mill: return {B::Farm};                           // Mehl   <- Getreide
        case B::Bakery: return {B::Mill};                         // Brot   <- Mehl
        case B::Brewery: return {B::Farm};                        // Bier   <- Getreide
        case B::PigFarm: return {B::Farm};                        // Schwein<- Getreide
        case B::DonkeyBreeder: return {B::Farm};                  // Esel   <- Getreide
        case B::Mint: return {B::GoldMine};                       // MÃ¼nzen <- Gold
        case B::Slaughterhouse: return {B::PigFarm};              // Fleisch<- Schwein
        default: return {};                                      // Quelle/keine Vorstufe
    }
}

// Sucht in 'centers' (jeweils bis 'radius') den besten Bauplatz fÃ¼r 'bt'.
bool AdvancedAIPlayer::placeNear(BuildingType bt, const std::vector<MapPoint>& centers, unsigned radius,
                                 unsigned minDistToWarehouse)
{
    if(!aii.CanBuildBuildingtype(bt))
        return false;

    const ChainInfo& ci = chainOf(bt);

    // Alle geeigneten BauplÃ¤tze sammeln (nicht nur den besten), nach Score
    // sortieren und der Reihe nach versuchen, bis die StraÃŸenanbindung klappt.
    // (Der beste Platz ist oft nicht anbindbar -> sonst scheitert die Platzierung.)
    std::vector<std::pair<int, MapPoint>> cands;
    const bool dedupeCandidates = bt == BuildingType::Storehouse;
    const MapExtent mapSize = gwb.GetSize();
    std::vector<char> seen;
    if(dedupeCandidates)
        seen.assign(static_cast<std::size_t>(mapSize.x) * mapSize.y, 0);
    auto toIdx = [&](MapPoint p) { return static_cast<std::size_t>(p.x) + static_cast<std::size_t>(p.y) * mapSize.x; };
    for(MapPoint center : centers)
    {
        for(MapPoint pt : collectPoints(center, radius))
        {
            if(dedupeCandidates)
            {
                const std::size_t idx = toIdx(pt);
                if(seen[idx])
                    continue;
                seen[idx] = 1;
            }
            if(!aii.IsOwnTerritory(pt))
                continue;
            if(!canUseBq(aii.GetBuildingQuality(pt), ci.size))
                continue;
            if(!placementAllowed(bt, pt))
                continue; // typ-spezifischer Freiraum/Abstand (z.B. Bauernhof)
            if(minDistToWarehouse > 0)
            {
                unsigned dWh = UINT_MAX;
                for(const nobBaseWarehouse* wh : aii.GetStorehouses())
                    dWh = std::min(dWh, gwb.CalcDistance(pt, wh->GetPos()));
                if(dWh < minDistToWarehouse)
                    continue; // zu nah am bestehenden Netz -> kein neuer Knoten
            }
            const int score = scorePlacement(bt, pt, center);
            if(score == INT_MIN)
                continue; // Ressource fehlt (nur Minen)
            cands.emplace_back(score, pt);
        }
    }
    if(std::getenv("RTTR_AI_DEBUG"))
    {
        static int dn = 0;
        if(dn < 100)
        {
            ++dn;
            std::cerr << "[placeNear] bt=" << static_cast<int>(bt) << " centers=" << centers.size()
                      << " cands=" << cands.size() << "\n";
        }
    }
    if(cands.empty())
        return false;
    std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    int tries = 0;
    for(const auto& c : cands)
    {
        if(placeAt(bt, c.second))
            return true;
        if(++tries >= 15) // genÃ¼gend Versuche, aber begrenzt
            break;
    }
    return false;
}

int AdvancedAIPlayer::scorePlacement(BuildingType bt, MapPoint pt, MapPoint center) const
{
    if(bt == BuildingType::Storehouse)
        return scoreStorehousePlacement(pt);

    const ChainInfo& ci = chainOf(bt);
    const int dist = static_cast<int>(gwb.CalcDistance(pt, center));

    const AIParams& P = AIParams::get();
    if(ci.hasResource)
    {
        // OberflÃ¤chen-Rohstoffe (Steine, Fisch) liegen NEBEN dem Bauplatz, nicht
        // darauf -> Umkreis-Summe (CalcResourceValue) statt Wert am Bauplatz.
        // Minen brauchen das Erz GENAU am Platz (senkrecht abbauen) -> Wert am Platz.
        const bool surfaceDeposit = (ci.resource == AIResource::Stones || ci.resource == AIResource::Fish);
        // Plantspace per Umkreis fÃ¼r den BAUERNHOF (misst echte FeldflÃ¤che ringsum).
        const bool areaResource = surfaceDeposit || (ci.resource == AIResource::Plantspace && bt == BuildingType::Farm);
        const int rating =
          areaResource ? aii.CalcResourceValue(pt, ci.resource) : aii.GetResourceRating(pt, ci.resource);
        if((ci.isMine || surfaceDeposit) && rating <= 0)
            return INT_MIN;
        if(bt == BuildingType::Farm && rating < 12)
            return INT_MIN; // zu wenig PflanzflÃ¤che im Arbeitsradius -> unproduktiv
        return rating * P.placeResourceWeight + (P.placeDistanceBase - dist);
    }
    // sonst: mÃ¶glichst nah am Lager (kurze Wege, weniger TrÃ¤ger)
    return P.placeDistanceBase - dist;
}

int AdvancedAIPlayer::countNearbyBuildings(MapPoint pt, unsigned radius) const
{
    int n = 0;
    for(const BuildingType bt : helpers::enumRange<BuildingType>())
    {
        if(bt == BuildingType::Headquarters || bt == BuildingType::Storehouse || bt == BuildingType::HarborBuilding)
            continue;
        for(const nobUsual* bld : aii.GetBuildings(bt))
            if(gwb.CalcDistance(pt, bld->GetPos()) <= radius)
                ++n;
    }
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        if(gwb.CalcDistance(pt, mb->GetPos()) <= radius)
            ++n;
    for(const noBuildingSite* site : aii.GetBuildingSites())
    {
        const BuildingType bt = site->GetBuildingType();
        if(bt == BuildingType::Storehouse || bt == BuildingType::HarborBuilding)
            continue;
        if(gwb.CalcDistance(pt, site->GetPos()) <= radius)
            ++n;
    }
    return n;
}

int AdvancedAIPlayer::countNearbyOwnTerritory(MapPoint pt, unsigned radius) const
{
    int n = 0;
    for(MapPoint p : collectPoints(pt, radius))
        if(aii.IsOwnTerritory(p))
            ++n;
    return n;
}

unsigned AdvancedAIPlayer::estimateWarehouseRoadDistance(MapPoint bldPos) const
{
    const MapPoint bldFlag = gwb.GetNeighbour(bldPos, Direction::SouthEast);
    std::vector<Direction> route;
    MapPoint target = MapPoint::Invalid();
    bool junction = false;
    if(!planConnection(bldFlag, route, &target, &junction) || !target.isValid())
        return std::numeric_limits<unsigned>::max();

    auto nearestWarehouseFlagDistance = [&](const noFlag& fromFlag) {
        unsigned best = std::numeric_limits<unsigned>::max();
        for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        {
            const noFlag* whFlag = gwb.GetSpecObj<noFlag>(gwb.GetNeighbour(wh->GetPos(), Direction::SouthEast));
            if(!whFlag)
                continue;
            unsigned dist = 0;
            if(&fromFlag == whFlag)
                dist = 0;
            else if(!aii.FindPathOnRoads(fromFlag, *whFlag, &dist))
                continue;
            best = std::min(best, dist);
        }
        return best;
    };

    unsigned rest = std::numeric_limits<unsigned>::max();
    if(const noFlag* targetFlag = gwb.GetSpecObj<noFlag>(target))
    {
        if(targetFlag->GetPlayer() == playerId)
            rest = nearestWarehouseFlagDistance(*targetFlag);
    } else if(junction)
    {
        for(MapPoint pt : collectPoints(target, 10))
        {
            const noFlag* flag = gwb.GetSpecObj<noFlag>(pt);
            if(!flag || flag->GetPlayer() != playerId)
                continue;
            const unsigned flagRest = nearestWarehouseFlagDistance(*flag);
            if(flagRest == std::numeric_limits<unsigned>::max())
                continue;
            rest = std::min(rest, flagRest + gwb.CalcDistance(target, pt));
        }
    }

    if(rest == std::numeric_limits<unsigned>::max())
        return rest;
    return static_cast<unsigned>(route.size()) + rest;
}

int AdvancedAIPlayer::scoreStorehousePlacement(MapPoint pt) const
{
    constexpr unsigned kMinWarehouseRoadDistance = 30;
    constexpr unsigned kMaxClusterWarehouseRoadDistance = 55;
    constexpr unsigned kMaxGapWarehouseRoadDistance = 85;
    constexpr int kIdealWarehouseRoadDistance = 42;

    const int nearBuildings = countNearbyBuildings(pt, 10);
    const int widerBuildings = countNearbyBuildings(pt, 16);
    unsigned nearestAir = std::numeric_limits<unsigned>::max();
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        nearestAir = std::min(nearestAir, gwb.CalcDistance(pt, wh->GetPos()));
    if(nearestAir < 20)
        return INT_MIN;

    const unsigned roadDist = estimateWarehouseRoadDistance(pt);
    if(roadDist == std::numeric_limits<unsigned>::max() || roadDist < kMinWarehouseRoadDistance)
        return INT_MIN;

    const int dist = static_cast<int>(roadDist);
    const bool denseCluster = nearBuildings >= 5 && widerBuildings >= 10 && roadDist <= kMaxClusterWarehouseRoadDistance;
    const int territory = countNearbyOwnTerritory(pt, 18);
    const bool serviceGap = nearBuildings >= 2 && widerBuildings >= 4 && territory >= 150 && nearestAir >= 28
                            && roadDist <= kMaxGapWarehouseRoadDistance;
    if(!denseCluster && !serviceGap)
        return INT_MIN;

    if(serviceGap && !denseCluster)
    {
        const int gapScore = std::min(dist, static_cast<int>(kMaxGapWarehouseRoadDistance)) * 5
                             - std::max(0, dist - 65) * 8;
        return gapScore + territory * 3 + nearBuildings * 80 + widerBuildings * 18;
    }

    const int distanceScore = 260 - std::abs(dist - kIdealWarehouseRoadDistance) * 9;
    return distanceScore + nearBuildings * 115 + widerBuildings * 22;
}

int AdvancedAIPlayer::countForesterPlantSpots(MapPoint foresterPos, MapPoint blockedBuildingPos) const
{
    constexpr unsigned kForesterPlantRadius = 7;

    const bool hasBlockedBuilding = blockedBuildingPos.isValid();
    const MapPoint blockedFlag =
      hasBlockedBuilding ? gwb.GetNeighbour(blockedBuildingPos, Direction::SouthEast) : MapPoint::Invalid();

    int spots = 0;
    for(MapPoint pt : collectPoints(foresterPos, kForesterPlantRadius))
    {
        if(!aii.IsOwnTerritory(pt))
            continue;
        if(hasBlockedBuilding && (pt == blockedBuildingPos || pt == blockedFlag))
            continue;
        if(aii.GetResourceRating(pt, AIResource::Plantspace) > 0)
            ++spots;
    }
    return spots;
}

bool AdvancedAIPlayer::preservesForesterPlantReserve(MapPoint pt) const
{
    constexpr unsigned kForesterPlantRadius = 7;
    // Förstern wird mehr Platz zugestanden (Default 14 statt fix 10) -> nachhaltiger
    // Wald, weniger Leerlauf. Über AIParams::foresterPlantReserve tunebar.
    const int kMinForesterPlantSpots = std::max(1, AIParams::get().foresterPlantReserve);

    for(const nobUsual* forester : aii.GetBuildings(BuildingType::Forester))
    {
        if(gwb.CalcDistance(pt, forester->GetPos()) <= kForesterPlantRadius
           && countForesterPlantSpots(forester->GetPos(), pt) < kMinForesterPlantSpots)
            return false;
    }
    for(const noBuildingSite* site : aii.GetBuildingSites())
    {
        if(site->GetBuildingType() == BuildingType::Forester
           && gwb.CalcDistance(pt, site->GetPos()) <= kForesterPlantRadius
           && countForesterPlantSpots(site->GetPos(), pt) < kMinForesterPlantSpots)
            return false;
    }
    return true;
}

bool AdvancedAIPlayer::placementAllowed(BuildingType bt, MapPoint pt) const
{
    // Ist ein GebÃ¤ude vom Typ t (fertig ODER Baustelle) im Radius rad?
    auto nearType = [&](BuildingType t, unsigned rad) {
        for(const nobUsual* u : aii.GetBuildings(t))
            if(gwb.CalcDistance(pt, u->GetPos()) <= rad)
                return true;
        for(const noBuildingSite* bs : aii.GetBuildingSites())
            if(bs->GetBuildingType() == t && gwb.CalcDistance(pt, bs->GetPos()) <= rad)
                return true;
        return false;
    };

    // FELD-SCHUTZ: kein anderes GebÃ¤ude in den Arbeitsradius (2) eines Hofs setzen,
    // sonst werden die Felder zugebaut und der Hof unproduktiv.
    if(bt != BuildingType::Farm && nearType(BuildingType::Farm, 2))
        return false;
    if(!preservesForesterPlantReserve(pt))
        return false;

    switch(bt)
    {
        case BuildingType::Farm:
        {
            // BauernhÃ¶fe bewirtschaften Felder im Radius 2 -> Abstand zu FÃ¶rstern
            // und anderen HÃ¶fen (Felder Ã¼berlappen sonst). Die FELDMENGE bewertet
            // scorePlacement Ã¼ber die Plantspace-Umkreissumme + Mindestschwelle.
            if(nearType(BuildingType::Forester, 6) || nearType(BuildingType::Farm, 5))
                return false;
            // NICHT in der NÃ¤he von HQ/LÃ¤gern: dort herrscht hoher Warenumschlag und
            // es wird Platz fÃ¼r Wege gebraucht; ein groÃŸer Hof wÃ¼rde das zubauen.
            // HÃ¶fe gehÃ¶ren ins freie Hinterland mit dauerhaft viel FeldflÃ¤che.
            for(const nobBaseWarehouse* wh : aii.GetStorehouses())
                if(gwb.CalcDistance(pt, wh->GetPos()) <= 8)
                    return false;
            return true;
        }
        case BuildingType::Forester:
            // FÃ¶rster nicht direkt an BauernhÃ¶fe (Feld-/Baum-Konflikt) und nur mit
            // genug freier PflanzflÃ¤che ringsum (mehr Platz: foresterPlantReserve).
            return !nearType(BuildingType::Farm, 6)
                   && countForesterPlantSpots(pt, pt) >= std::max(1, AIParams::get().foresterPlantReserve);
        case BuildingType::Fishery:
        {
            // Fische regenerieren sich NICHT: keine neue FischerhÃ¼tte in NÃ¤he eines
            // bereits LEERGEFISCHTEN Ortes (bei ErschÃ¶pfung gemerkt). Bei aktivem
            // Addon "unerschÃ¶pfliche Fische" entfÃ¤llt die Sperre.
            // (KEINE generelle Fischerei-Abstandsregel: das wÃ¼rde auf fischreichen
            //  KÃ¼sten zu wenige HÃ¼tten bauen -> Nahrungsmangel; gemessen klar
            //  schlechter. Wo zu wenig Fisch ist, sperrt ohnehin scorePlacement.)
            constexpr unsigned kFisherR = 7; // = nofFarmhand::GetWorkRadius(Fisher)
            if(gwb.GetGGS().isEnabled(AddonId::INEXHAUSTIBLE_FISH))
                return true;
            for(const MapPoint& dp : depletedFishSpots_)
                if(gwb.CalcDistance(pt, dp) <= kFisherR)
                    return false;
            return true;
        }
        default:
            return true;
    }
}

bool AdvancedAIPlayer::placeAt(BuildingType bt, MapPoint pt, bool immediate)
{
    const MapPoint bldFlag = gwb.GetNeighbour(pt, Direction::SouthEast);

    // 1) MACHBARKEIT prÃ¼fen: existiert Ã¼berhaupt eine sinnvolle Anbindung ans
    //    Netz? (Auswahl guter, anbindbarer PlÃ¤tze.)
    std::vector<Direction> probe;
    const bool roadOk = planConnection(bldFlag, probe);
    if(!roadOk)
        return false; // nicht anbindbar -> Aufrufer probiert nÃ¤chsten Kandidaten

    if(!aii.SetBuildingSite(pt, bt))
        return false;

    // 2a) MilitÃ¤r an der Front: StraÃŸe SOFORT (rasche Besetzung). Keine
    //     Zwischenfahnen (kurze FrontstraÃŸen, kein Geisterfahnen-Risiko).
    //     Trotzdem zur Reparatur einreihen, falls die StraÃŸe doch scheitert.
    if(immediate && !probe.empty())
        aii.BuildRoad(bldFlag, false, probe);
    // 2b) Sonst aufgeschoben: StraÃŸe/Fahnen baut runConnect, sobald die Fahne
    //     real existiert und gegen den AKTUELLEN Zustand geplant werden kann
    //     -> keine Geisterfahnen / unverbundenen HÃ¤user / Wege-Konflikte.
    enqueueConnect(bldFlag);
    return true;
}

// Reiht eine (kÃ¼nftige) Fahne zur spÃ¤teren Anbindung ein (Doppelte vermeiden).
void AdvancedAIPlayer::enqueueConnect(MapPoint flagPos, bool allowDestroy)
{
    for(const auto& e : pendingConnect_)
        if(e.flag == flagPos)
            return;
    PendingConnect e;
    e.flag = flagPos;
    e.cooldown = 1; // 1 NWF warten, bis SetBuildingSite ausgefÃ¼hrt ist (Fahne da)
    e.allowDestroy = allowDestroy;
    pendingConnect_.push_back(e);
}

// Ist die Fahne bereits ans Lager-Netz angebunden (Weg Ã¼ber StraÃŸen vorhanden)?
bool AdvancedAIPlayer::isFlagConnected(MapPoint flagPos) const
{
    const noFlag* f = gwb.GetSpecObj<noFlag>(flagPos);
    if(!f)
        return false;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        const noFlag* whF = gwb.GetSpecObj<noFlag>(gwb.GetNeighbour(wh->GetPos(), Direction::SouthEast));
        if(whF && (whF == f || aii.FindPathOnRoads(*f, *whF)))
            return true;
    }
    return false;
}

// Aufgeschobene Anbindung: baut StraÃŸen erst, wenn die Fahne existiert und gegen
// den aktuellen Weltzustand geplant werden kann. Pro Tick nur wenige StraÃŸen
// (verhindert sich Ã¼berschneidende Wege aus gleichzeitiger Platzierung).
void AdvancedAIPlayer::runConnect()
{
    if(pendingConnect_.empty())
        return;
    std::vector<PendingConnect> keep;
    keep.reserve(pendingConnect_.size());
    int issued = 0;
    for(PendingConnect e : pendingConnect_)
    {
        if(e.cooldown > 0)
        {
            --e.cooldown;
            keep.push_back(e);
            continue;
        }
        const noFlag* f = gwb.GetSpecObj<noFlag>(e.flag);
        if(!f)
        {
            // Fahne noch nicht in der Welt (Bau-Befehl noch nicht ausgefÃ¼hrt)
            // oder GebÃ¤ude wurde wieder entfernt -> begrenzt weiter warten.
            if(++e.fails > 12)
                continue; // aufgeben
            e.cooldown = 1;
            keep.push_back(e);
            continue;
        }
        if(isFlagConnected(e.flag))
            continue; // erfolgreich angebunden -> aus der Queue entfernen
        if(e.roadIssued)
        {
            // StraÃŸe war beauftragt, hat aber nicht verbunden -> erneut planen.
            e.roadIssued = false;
            if(++e.fails > 15)
            {
                // dauerhaft nicht anbindbar. Geisterbau (frisch platziert) aufrÃ¤umen;
                // ein BESTEHENDES/erobertes GebÃ¤ude NICHT abreiÃŸen, nur aufgeben (es
                // wird beim nÃ¤chsten Wegenetz-Lauf erneut versucht, falls sich das
                // Territorium Ã¤ndert).
                if(e.allowDestroy)
                    aii.DestroyBuilding(gwb.GetNeighbour(e.flag, Direction::NorthWest));
                continue;
            }
        }
        if(issued >= 16)
        {
            e.cooldown = 1; // diesen NWF nicht mehr bauen (Durchsatz begrenzen)
            keep.push_back(e);
            continue;
        }
        std::vector<Direction> route;
        MapPoint target = MapPoint::Invalid();
        bool junction = false;
        if(planConnection(e.flag, route, &target, &junction) && !route.empty())
        {
            // Kreuzungsfahne (Steiner-Punkt) auf der bestehenden StraÃŸe zuerst
            // setzen, dann die neue StraÃŸe daran anschlieÃŸen.
            if(junction && target.isValid())
                aii.SetFlag(target);
            aii.BuildRoad(e.flag, false, route);
            setFlagsAlongRoad(e.flag, route);
            e.roadIssued = true;
            e.cooldown = 2; // auf AusfÃ¼hrung der StraÃŸe warten, dann prÃ¼fen
            ++issued;
            keep.push_back(e);
        } else
        {
            if(++e.fails > 15)
            {
                // Geisterbau aufrÃ¤umen; bestehendes/erobertes GebÃ¤ude nur aufgeben.
                if(e.allowDestroy)
                    aii.DestroyBuilding(gwb.GetNeighbour(e.flag, Direction::NorthWest));
                continue;
            }
            e.cooldown = 3;
            keep.push_back(e);
        }
    }
    pendingConnect_ = std::move(keep);
}

// Bindet eigene GebÃ¤ude wieder an, deren Fahne KEINEN Weg mehr zum Lager hat â€“
// typisch nach Eroberung (GebÃ¤ude neu in unserem Besitz, ohne Anschluss) oder nach
// Gebietsverlust (die verbindende StraÃŸe wurde mit dem Land zerstÃ¶rt). Die
// eigentliche StraÃŸe baut runConnect; hier wird nur erkannt + eingereiht. WICHTIG:
// allowDestroy=false -> ein vorÃ¼bergehend unerreichbares GebÃ¤ude wird NICHT
// abgerissen (nÃ¤chster Lauf versucht es erneut, falls sich das Territorium Ã¤ndert).
void AdvancedAIPlayer::reconnectOrphanedBuildings()
{
    auto consider = [&](MapPoint bldPos) {
        const MapPoint flagPos = gwb.GetNeighbour(bldPos, Direction::SouthEast);
        const noFlag* f = gwb.GetSpecObj<noFlag>(flagPos);
        if(!f || f->GetPlayer() != playerId)
            return;
        if(isFlagConnected(flagPos))
            return; // bereits am Netz
        enqueueConnect(flagPos, /*allowDestroy=*/false);
    };
    // MilitÃ¤rgebÃ¤ude NUR im sicheren Inland (Far) wieder anbinden. Ein abgetrenntes
    // FRONT-MilitÃ¤rgebÃ¤ude (z.B. frisch erobert) anzubinden lockt Soldaten in eine
    // exponierte VorwÃ¤rtsposition -> Ãœberdehnung und Verluste (im Self-Play klar
    // schlechter). Es wird ohnehin angebunden, sobald die Front daran vorbeizieht.
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        if(mb->GetFrontierDistance() == FrontierDistance::Far)
            consider(mb->GetPos());
    // WirtschaftsgebÃ¤ude (Produktion) immer â€“ ein abgetrenntes SÃ¤gewerk/Bergwerk
    // soll wieder ans Netz (genau der gemeldete Fall).
    for(unsigned i = 0; i < 40; ++i)
    {
        const BuildingType b = BuildingType(i);
        if(BuildingProperties::IsMilitary(b) || BuildingProperties::IsWareHouse(b))
            continue;
        for(const nobUsual* u : aii.GetBuildings(b))
            consider(u->GetPos());
    }
}

// Periodische Wegenetz-Optimierung: Stau-Erkennung + Querverbindungen.
// Das Netz wÃ¤chst sonst als BAUM zum Lager -> Waren zwischen zwei Zweigen mÃ¼ssen
// einen langen Umweg Ã¼ber die Wurzel nehmen (Umweg + Stau). Hier werden volle
// Fahnen (Stau) priorisiert und lohnende AbkÃ¼rzungen zwischen Zweigen gebaut.
void AdvancedAIPlayer::runRoadOptimize()
{
    // 0) NUTZLOSE FAHNEN aufrÃ¤umen: eigene Fahnen OHNE jede StraÃŸe und OHNE
    // GebÃ¤ude (NW) bringen nichts (entstehen z.B., wenn ein StraÃŸenbefehl bei
    // AusfÃ¼hrung scheitert, die Zwischenfahne aber gesetzt wurde). Da StraÃŸen
    // binnen weniger NWF gebaut werden, ist eine Fahne ohne StraÃŸe zum Zeitpunkt
    // dieses (seltenen) Laufs eine echte Waise -> abreiÃŸen.
    {
        const MapExtent sz = gwb.GetSize();
        for(unsigned y = 0; y < static_cast<unsigned>(sz.y); ++y)
            for(unsigned x = 0; x < static_cast<unsigned>(sz.x); ++x)
            {
                const MapPoint fp(x, y);
                const noFlag* f = gwb.GetSpecObj<noFlag>(fp);
                if(!f || f->GetPlayer() != playerId)
                    continue;
                bool hasRoad = false;
                for(const auto* r : f->getRoutes())
                    if(r)
                    {
                        hasRoad = true;
                        break;
                    }
                if(hasRoad)
                    continue;
                // Keine StraÃŸe. HÃ¤ngt ein GebÃ¤ude (NW) dran? Dann ist es eine
                // GebÃ¤udefahne (Ã¼bernimmt runConnect) -> nicht anfassen.
                const MapPoint nw = gwb.GetNeighbour(fp, Direction::NorthWest);
                if(gwb.GetSpecObj<noBuilding>(nw) || gwb.GetSpecObj<noBuildingSite>(nw))
                    continue;
                aii.DestroyFlag(f); // reine Waisenflagge -> entfernen
            }
    }

    // 0b) ABGETRENNTE eigene GebÃ¤ude wieder anbinden (erobert oder durch
    // Gebietsverlust vom Netz getrennt).
    {
        bool pruned = false;
        const MapExtent sz = gwb.GetSize();
        for(unsigned y = 0; y < static_cast<unsigned>(sz.y) && !pruned; ++y)
            for(unsigned x = 0; x < static_cast<unsigned>(sz.x) && !pruned; ++x)
            {
                const noFlag* f = gwb.GetSpecObj<noFlag>(MapPoint(x, y));
                if(!f || f->GetPlayer() != playerId)
                    continue;
                pruned = pruneDeadRoadBranch(*f, Direction::NorthWest, false);
            }
    }

    reconnectOrphanedBuildings();

    // Alle eigenen GebÃ¤ude-Fahnen einsammeln (Netzknoten).
    std::vector<const noFlag*> flags;
    auto addFlag = [&](MapPoint bldPos) {
        if(const noFlag* f = gwb.GetSpecObj<noFlag>(gwb.GetNeighbour(bldPos, Direction::SouthEast)))
            flags.push_back(f);
    };
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        addFlag(wh->GetPos());
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        addFlag(mb->GetPos());
    for(unsigned i = 0; i < 40; ++i)
    {
        const BuildingType b = BuildingType(i);
        if(BuildingProperties::IsMilitary(b) || BuildingProperties::IsWareHouse(b))
            continue;
        for(const nobUsual* u : aii.GetBuildings(b))
            addFlag(u->GetPos());
    }
    if(flags.empty())
        return;

    // Nach STAU (wartende Waren an der Fahne) absteigend sortieren -> zuerst die
    // EngpÃ¤sse entlasten.
    std::sort(flags.begin(), flags.end(),
              [](const noFlag* a, const noFlag* b) { return a->GetNumWares() > b->GetNumWares(); });

    // Nur die obersten Kandidaten betrachten, wenige AbkÃ¼rzungen pro Lauf bauen
    // (Konflikte/TrÃ¤ger-Flut vermeiden).
    int built = 0;
    const std::size_t limit = std::min<std::size_t>(flags.size(), 14);
    for(std::size_t i = 0; i < limit && built < 3; ++i)
        if(buildShortcutFrom(flags[i]->GetPos()))
            ++built;
    if(built > 0 && std::getenv("RTTR_AI_DEBUG"))
        std::cerr << "[roadOpt] shortcuts=" << built << " topWares=" << flags.front()->GetNumWares() << "\n";
    if(built == 0)
        pruneLongUnusedRoadDetour();

    // --- Stark frequentierte, langsame Wege zu ESELSTRASSEN aufwerten ---
    // Nur mit Addon MANUAL_ROAD_ENLARGEMENT (sonst werten sich Wege automatisch auf)
    // und nur, wenn eine Eselzucht existiert (sonst kein Esel fÃ¼r die StraÃŸe). Ein
    // verstopfter NORMALER Wegabschnitt (viele dort wartende Waren) IST der langsame,
    // hoch frequentierte Weg -> genau der lohnt die Aufwertung.
    if(ggs.isEnabled(AddonId::MANUAL_ROAD_ENLARGEMENT) && total(BuildingType::DonkeyBreeder) > 0)
    {
        int upgraded = 0;
        for(std::size_t i = 0; i < limit && upgraded < 3; ++i)
        {
            const noFlag* f = flags[i];
            for(const Direction d : helpers::enumRange<Direction>())
            {
                const RoadSegment* rs = f->GetRoute(d);
                if(!rs || rs->GetRoadType() != RoadType::Normal)
                    continue; // kein Weg, oder schon Esel-/WasserstraÃŸe
                if(f->GetNumWaresForRoad(d) < 4)
                    continue; // nicht hoch frequentiert genug
                aii.UpgradeRoad(f->GetPos(), d);
                if(++upgraded >= 3)
                    break;
            }
        }
    }
}

// Baut von 'fromFlag' eine AbkÃ¼rzung zu einer nahen, ans Lager angebundenen
// Fahne, falls der bestehende On-Road-Weg deutlich lÃ¤nger ist als die neue
// StraÃŸe (Faktor 4). Route muss fahnbar sein (<=2 nicht-fahnbare Felder).
bool AdvancedAIPlayer::pruneDeadRoadBranch(const noFlag& startFlag, Direction excludeDir, bool hasExclude)
{
    if(startFlag.GetPlayer() != playerId)
        return false;
    if(startFlag.GetNumWares() > 0)
        return false;

    const MapPoint attached = gwb.GetNeighbour(startFlag.GetPos(), Direction::NorthWest);
    if(gwb.GetSpecObj<noBuilding>(attached) || gwb.GetSpecObj<noBuildingSite>(attached))
        return false;

    const RoadSegment* foundRoad = nullptr;
    unsigned roads = 0;
    for(const Direction dir : helpers::enumRange<Direction>())
    {
        if(hasExclude && dir == excludeDir)
            continue;
        const RoadSegment* rs = startFlag.GetRoute(dir);
        if(!rs)
            continue;
        if(rs->GetRoadType() == RoadType::Water)
            return false;
        if(++roads > 1)
            return false;
        foundRoad = rs;
    }

    if(!foundRoad)
        return false;

    const noRoadNode* otherNode = foundRoad->GetF1() == &startFlag ? foundRoad->GetF2() : foundRoad->GetF1();
    const noFlag* otherFlag = dynamic_cast<const noFlag*>(otherNode);
    if(!otherFlag)
        return false;
    const Direction reverseDir = foundRoad->GetOtherFlagDir(startFlag) + 3u;
    aii.DestroyFlag(&startFlag);
    pruneDeadRoadBranch(*otherFlag, reverseDir, true);
    return true;
}

bool AdvancedAIPlayer::pruneLongUnusedRoadDetour()
{
    auto carrierBusy = [](const nofCarrier* carrier) {
        if(!carrier)
            return false;
        switch(carrier->GetCarrierState())
        {
            case CarrierState::WaitForWare:
            case CarrierState::GotoMiddleOfRoad: return false;
            default: return true;
        }
    };

    const MapExtent sz = gwb.GetSize();
    for(unsigned y = 0; y < static_cast<unsigned>(sz.y); ++y)
        for(unsigned x = 0; x < static_cast<unsigned>(sz.x); ++x)
        {
            const noFlag* start = gwb.GetSpecObj<noFlag>(MapPoint(x, y));
            if(!start || start->GetPlayer() != playerId)
                continue;

            for(const Direction dir : helpers::enumRange<Direction>())
            {
                const RoadSegment* rs = start->GetRoute(dir);
                if(!rs || rs->GetRoadType() != RoadType::Normal)
                    continue;
                if(rs->GetLength() < 8)
                    continue;

                const noFlag* f1 = dynamic_cast<const noFlag*>(rs->GetF1());
                const noFlag* f2 = dynamic_cast<const noFlag*>(rs->GetF2());
                if(!f1 || !f2 || f1->GetPlayer() != playerId || f2->GetPlayer() != playerId)
                    continue;

                const Direction f1Dir = rs->GetRoute(0);
                const Direction f2Dir = rs->GetRoute(rs->GetLength() - 1) + 3u;
                if(f1->GetNumWaresForRoad(f1Dir) > 0 || f2->GetNumWaresForRoad(f2Dir) > 0)
                    continue;
                if(carrierBusy(rs->getCarrier(0)) || carrierBusy(rs->getCarrier(1)))
                    continue;

                unsigned altLen = 0;
                if(!gwb.GetRoadPathFinder().FindPath(*f1, *f2, false, std::numeric_limits<unsigned>::max(), rs,
                                                      &altLen))
                    continue;
                if(altLen + 4 >= rs->GetLength())
                    continue;
                if(altLen * 3 > rs->GetLength() * 2)
                    continue;

                aii.DestroyRoad(start->GetPos(), dir);
                if(std::getenv("RTTR_AI_DEBUG"))
                    std::cerr << "[roadOpt] pruneDetour len=" << rs->GetLength() << " alt=" << altLen << "\n";
                return true;
            }
        }
    return false;
}

bool AdvancedAIPlayer::buildShortcutFrom(MapPoint fromFlag)
{
    const noFlag* from = gwb.GetSpecObj<noFlag>(fromFlag);
    if(!from)
        return false;
    constexpr unsigned R = 10;
    for(MapPoint pt : collectPoints(fromFlag, R))
    {
        if(pt == fromFlag)
            continue;
        const noFlag* cand = gwb.GetSpecObj<noFlag>(pt);
        if(!cand || cand->GetPlayer() != playerId)
            continue;
        // Keine MilitÃ¤rgebÃ¤ude-Fahne als Ziel.
        if(gwb.IsMilitaryBuildingOnNode(gwb.GetNeighbour(pt, Direction::NorthWest), true))
            continue;
        std::vector<Direction> route;
        unsigned newLen = 0;
        if(!aii.FindFreePathForNewRoad(fromFlag, pt, &route, &newLen))
            continue;
        if(newLen < 2)
            continue; // schon (quasi) benachbart
        // Fahnbarkeit der neuen StraÃŸe prÃ¼fen (max. 2 nicht-fahnbare in Folge).
        unsigned maxNon = 0, cur = 0;
        MapPoint tp = fromFlag;
        for(Direction d : route)
        {
            tp = gwb.GetNeighbour(tp, d);
            if(aii.GetBuildingQuality(tp) == BuildingQuality::Nothing)
            {
                ++cur;
                maxNon = std::max(maxNon, cur);
            } else
                cur = 0;
        }
        if(maxNon > 2)
            continue;
        // Bestehender Weg Ã¼ber das Netz?
        unsigned oldLen = 0;
        if(!aii.FindPathOnRoads(*from, *cand, &oldLen))
            continue; // nicht verbunden -> macht runConnect, nicht hier
        // Lohnt sich die AbkÃ¼rzung? (Umweg >= 4x neue LÃ¤nge)
        if(newLen * 4u >= oldLen)
            continue;
        aii.BuildRoad(fromFlag, false, route);
        setFlagsAlongRoad(fromFlag, route);
        return true;
    }
    return false;
}

// Beste Anbindung der (kÃ¼nftigen) Fahne bldFlag ans Wegenetz.
//
// MATHEMATISCHE SICHT: Wir verbinden bldFlag mit dem nÃ¤chstgelegenen Punkt des
// Wegenetz-GRAPHEN. Der Graph besteht nicht nur aus Fahnen (Knoten), sondern
// auch aus StraÃŸen (Kanten). Daher sind Ziele:
//   (a) bestehende, ans Lager angebundene Fahnen, ODER
//   (b) ein Punkt MITTEN auf einer eigenen StraÃŸe -> dort wird eine
//       Kreuzungsfahne gesetzt (Steiner-Punkt). Das verkÃ¼rzt die neue StraÃŸe
//       drastisch, wenn der nÃ¤chste Netzpunkt keine Fahne, sondern eine StraÃŸe
//       ist (hÃ¤ufigste Ursache "unnÃ¶tig langer" Wege).
// Kostenfunktion (MST-artig): minimiere primÃ¤r die NEUE StraÃŸenlÃ¤nge; ist man
// erst am Netz, routet das Netz selbst zum Lager.
//   kosten = 10*neue_LÃ¤nge + Restweg_zum_Lager/4 + 10*nicht_fahnbare_Folge
// Routen mit >2 nicht-fahnbaren Feldern werden verworfen (nicht unterteilbar).
bool AdvancedAIPlayer::planConnection(MapPoint bldFlag, std::vector<Direction>& outRoute, MapPoint* outTarget,
                                      bool* outJunction) const
{
    const MapPoint whFlagPos = nearestWarehouseFlag(bldFlag);
    const noFlag* whFlag = gwb.GetSpecObj<noFlag>(whFlagPos);
    if(!whFlag)
        return false;

    constexpr unsigned R = 11;
    unsigned bestCost = UINT_MAX;
    std::vector<Direction> route;

    // PrÃ¼ft Route auf Fahnbarkeit (max. 2 nicht-fahnbare Felder in Folge).
    auto routeFlaggable = [&](const std::vector<Direction>& r) {
        unsigned maxNon = 0, cur = 0;
        MapPoint tp = bldFlag;
        for(Direction d : r)
        {
            tp = gwb.GetNeighbour(tp, d);
            if(aii.GetBuildingQuality(tp) == BuildingQuality::Nothing)
            {
                ++cur;
                maxNon = std::max(maxNon, cur);
            } else
                cur = 0;
        }
        return maxNon;
    };
    // Ist pt ein Punkt auf einer eigenen StraÃŸe (fÃ¼r eine Kreuzungsfahne)?
    auto isOwnRoadPoint = [&](MapPoint pt) {
        if(!aii.IsOwnTerritory(pt))
            return false;
        for(unsigned d = 0; d < 6; ++d)
            if(gwb.GetPointRoad(pt, Direction(d)) != PointRoad::None)
                return true;
        return false;
    };

    for(MapPoint pt : collectPoints(bldFlag, R))
    {
        if(pt == bldFlag)
            continue;

        bool junction = false;
        unsigned distance = 0;
        const noFlag* cand = gwb.GetSpecObj<noFlag>(pt);
        if(cand)
        {
            if(cand->GetPlayer() != playerId)
                continue;
            // Keine Fahne direkt an einem MilitÃ¤rgebÃ¤ude als Ziel verwenden.
            if(gwb.IsMilitaryBuildingOnNode(gwb.GetNeighbour(pt, Direction::NorthWest), true))
                continue;
            // Fahne muss selbst ans Lager angebunden sein (sonst nutzlos).
            if(cand != whFlag && !aii.FindPathOnRoads(*cand, *whFlag, &distance))
                continue;
        } else if(isOwnRoadPoint(pt) && !gwb.IsFlagAround(pt))
        {
            // (b) Kreuzungsfahne mitten auf einer StraÃŸe -> Steiner-Punkt.
            // Die StraÃŸe ist Teil des Netzes (Restweg unbekannt -> 0, was kurze
            // AnschlÃ¼sse zusÃ¤tzlich begÃ¼nstigt).
            junction = true;
        } else
            continue;

        route.clear();
        unsigned length = 0;
        if(!aii.FindFreePathForNewRoad(bldFlag, pt, &route, &length))
            continue;
        const unsigned maxNon = routeFlaggable(route);
        if(maxNon > 2)
            continue;
        const unsigned cost = 10u * length + distance / 4u + 10u * maxNon;
        if(cost < bestCost)
        {
            bestCost = cost;
            outRoute = route;
            if(outTarget)
                *outTarget = pt;
            if(outJunction)
                *outJunction = junction;
        }
    }
    return bestCost != UINT_MAX;
}

void AdvancedAIPlayer::setFlagsAlongRoad(MapPoint startFlag, const std::vector<Direction>& route)
{
    // Fahnen entlang der StraÃŸe setzen (erste Ã¼berspringen, letzte 2 auslassen).
    // Die Engine lehnt zu nahe Fahnen ab -> es bleiben ~2er-AbstÃ¤nde.
    if(route.size() < 3)
        return;
    MapPoint cur = gwb.GetNeighbour(startFlag, route[0]); // erste Ã¼berspringen
    for(unsigned i = 1; i + 2 < route.size(); ++i)
    {
        cur = gwb.GetNeighbour(cur, route[i]);
        aii.SetFlag(cur);
    }
}

bool AdvancedAIPlayer::connectToNetwork(MapPoint bldPos)
{
    const MapPoint bldFlag = gwb.GetNeighbour(bldPos, Direction::SouthEast);
    const MapPoint target = nearestWarehouseFlag(bldFlag);
    std::vector<Direction> route;
    unsigned len = 0;
    if(target.isValid() && aii.FindFreePathForNewRoad(bldFlag, target, &route, &len) && !route.empty())
        return aii.BuildRoad(bldFlag, false, route);
    return false;
}

MapPoint AdvancedAIPlayer::nearestWarehouseFlag(MapPoint from) const
{
    MapPoint best = MapPoint::Invalid();
    unsigned bestDist = UINT_MAX;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        const MapPoint flag = gwb.GetNeighbour(wh->GetPos(), Direction::SouthEast);
        const unsigned d = gwb.CalcDistance(from, flag);
        if(d < bestDist)
        {
            bestDist = d;
            best = flag;
        }
    }
    return best;
}

// ===========================================================================
// Schicht: Expansion (militÃ¤rische Landnahme)
// ===========================================================================
void AdvancedAIPlayer::runExpansion()
{
    retireSafeInlandMilitary();

    // Bis zu N MilitÃ¤rgebÃ¤ude pro Tick (Territorium schneller ausdehnen) â€“ tunebar.
    const int per = AIParams::get().expandPerTick;
    for(int i = 0; i < per; ++i)
        if(!placeMilitary())
            break;
}

void AdvancedAIPlayer::retireSafeInlandMilitary()
{
    if(numMilitary() < 8)
        return;

    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
    {
        if(upgradeBldPos_.isValid() && mb->GetPos() == upgradeBldPos_)
            continue;
        if(!mb->IsUseless())
            continue;
        if(!mb->IsDemolitionAllowed())
            continue;

        aii.DestroyBuilding(mb->GetPos());
        return;
    }
}

bool AdvancedAIPlayer::placeMilitary()
{
    // GebÃ¤udewahl nach verfÃ¼gbarem Stein: stark bauen, aber auf kleinere
    // Frontposten zurÃ¼ckfallen. Sonst blockiert ein fehlender Haus-/Turmplatz die
    // gesamte Expansion, obwohl eine Baracke oder Wachstube noch Land nehmen kann.
    std::vector<BuildingType> buildTypes;
    const bool wantBarracks = total(BuildingType::Barracks) == 0
                              || total(BuildingType::Barracks) * 4 < std::max(4, numMilitary());
    if(wantBarracks)
        buildTypes = {BuildingType::Barracks, BuildingType::Guardhouse, BuildingType::Watchtower,
                      BuildingType::Fortress};
    else if(stock(GoodType::Stones) >= 10 && numMilitary() >= 6)
        buildTypes = {BuildingType::Fortress, BuildingType::Watchtower, BuildingType::Guardhouse,
                      BuildingType::Barracks};
    else if(stock(GoodType::Stones) >= 7 && numMilitary() >= 4)
        buildTypes = {BuildingType::Watchtower, BuildingType::Guardhouse, BuildingType::Barracks};
    else if(stock(GoodType::Stones) >= 3)
        buildTypes = {BuildingType::Guardhouse, BuildingType::Barracks};
    else
        buildTypes = {BuildingType::Barracks};

    // Bestehende MilitÃ¤rpositionen (fÃ¼r AbstandsprÃ¼fung) sammeln.
    std::vector<MapPoint> milPos;
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        milPos.push_back(mb->GetPos());
    MapPoint hqPos = MapPoint::Invalid();
    if(const nobHQ* hq = aii.GetHeadquarter())
    {
        hqPos = hq->GetPos();
        milPos.push_back(hqPos);
    }

    // Frontier-Bewertung: alle Felder im kleinen Hex-Radius betrachten. Die alte
    // Strahlenprobe in nur 6 Richtungen uebersah Grenzfelder zwischen den Achsen
    // und liess dadurch gute Frontplaetze auf manchen Karten ungenutzt.
    auto frontierScore = [&](MapPoint p) {
        int cnt = 0;
        for(MapPoint n : collectPoints(p, 2))
            if(!aii.IsOwnTerritory(n))
                ++cnt;
        return cnt;
    };
    // AKZEPTANZ (getrennt vom Score!): Platz gilt als Front, wenn fremdes Land in
    // MilitÃ¤rradius-NÃ¤he (4 Schritte) liegt â€“ also auch EIN PAAR FELDER INNERHALB
    // der Grenze. Wichtig, weil GebÃ¤ude an der Ost-/SÃ¼dkante ihre Fahne (SO) oft
    // auÃŸerhalb hÃ¤tten (-> unbaubar); ein Platz knapp innen ist baubar und sein
    // MilitÃ¤rradius schiebt die Grenze trotzdem nach auÃŸen. Der Score nutzt
    // weiterhin die enge 2-Schritt-Front (kein Ãœber-Compounding).
    auto frontierDistance = [&](MapPoint p) {
        for(unsigned radius = 1; radius <= 4; ++radius)
            for(MapPoint n : collectPoints(p, radius))
                if(!aii.IsOwnTerritory(n))
                    return radius;
        return 0u;
    };
    // Militärradius je Bautyp (= gameData MILITARY_RADIUS {8,9,10,11}). Bestimmt,
    // wie weit ein Gebäude Territorium beansprucht -> Reichweite des Landgewinn-Gates.
    auto milClaimRadius = [](BuildingType bt) -> unsigned {
        switch(bt)
        {
            case BuildingType::Barracks: return 8;
            case BuildingType::Guardhouse: return 9;
            case BuildingType::Watchtower: return 10;
            case BuildingType::Fortress: return 11;
            default: return 9;
        }
    };
    // Zählt im Radius die NUTZBAREN, noch nicht eigenen Landfelder: nicht eigenes
    // Territorium UND bebaubares Terrain (GetBuildingQualityAnyOwner != Nothing ->
    // schließt Wasser und unbebaubare Felsspitzen aus; Minenberge/Feindland zählen
    // als nutzbarer Gewinn). So wird "echter Landgewinn" gemessen statt nur "Grenze".
    auto usableLandGain = [&](MapPoint p, unsigned radius) {
        int gain = 0;
        for(MapPoint n : collectPoints(p, radius))
            if(!aii.IsOwnTerritory(n) && aii.GetBuildingQualityAnyOwner(n) != BuildingQuality::Nothing)
                ++gain;
        return gain;
    };

    // RICHTUNGS-BALANCE: Ohne GegenmaÃŸnahme verstÃ¤rkt sich eine zufÃ¤llige
    // Anfangsrichtung (Expansion folgt bestehenden MilitÃ¤rgebÃ¤uden) -> Wachstum
    // nur in EINE Richtung (z.B. links/oben). Wir teilen die Umgebung des HQ in
    // 8 Sektoren und bevorzugen Sektoren mit WENIGEN eigenen MilitÃ¤rgebÃ¤uden.
    const MapExtent msize = gwb.GetSize();
    auto sectorOf = [&](MapPoint p) {
        int dx = static_cast<int>(p.x) - static_cast<int>(hqPos.x);
        int dy = static_cast<int>(p.y) - static_cast<int>(hqPos.y);
        const int w = static_cast<int>(msize.x), h = static_cast<int>(msize.y);
        if(dx > w / 2)
            dx -= w;
        else if(dx < -w / 2)
            dx += w;
        if(dy > h / 2)
            dy -= h;
        else if(dy < -h / 2)
            dy += h;
        if(dx == 0 && dy == 0)
            return 0;
        constexpr double kPi = 3.14159265358979323846;
        const double ang = std::atan2(static_cast<double>(dy), static_cast<double>(dx)) + kPi; // 0..2pi
        int s = static_cast<int>(ang / (2.0 * kPi) * 8.0);
        return s < 0 ? 0 : (s > 7 ? 7 : s);
    };
    int sectorMil[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if(hqPos.isValid())
        for(const nobMilitary* mb : aii.GetMilitaryBuildings())
            ++sectorMil[sectorOf(mb->GetPos())];

    bool placed = false;
    std::size_t debugCands = 0;

    for(BuildingType bt : buildTypes)
    {
        if(!aii.CanBuildBuildingtype(bt))
            continue;
        const ChainInfo& ci = chainOf(bt);
        const unsigned claimR = milClaimRadius(bt);
        const int minLandGain = AIParams::get().minExpansionLandGain;

        // Kandidaten sammeln, nach Score sortieren, der Reihe nach versuchen, bis
        // die Anbindung klappt (analog placeNear).
        std::vector<std::pair<int, MapPoint>> cands;
        for(MapPoint center : milPos)
        {
            for(MapPoint pt : collectPoints(center, kSearchRadius))
            {
                if(!aii.IsOwnTerritory(pt))
                    continue;
                if(!canUseBq(aii.GetBuildingQuality(pt), ci.size))
                    continue;
                const unsigned frontDist = frontierDistance(pt);
                if(frontDist == 0)
                    continue;
                const unsigned wantedSpacing =
                  frontDist <= 2 ? AIParams::get().milSpacing : AIParams::get().milSpacing + 3;
                unsigned minD = UINT_MAX;
                for(MapPoint mp : milPos)
                    minD = std::min(minD, gwb.CalcDistance(pt, mp));
                if(minD < wantedSpacing)
                    continue;
                // LANDGEWINN-Gate: kein Expansions-Militärgebäude, wenn sein Radius
                // kaum NUTZBARES neues Land abdeckt (z.B. am Wasser-/Gebirgsrand,
                // wo nur Wasser/Felsspitzen "erobert" würden). minLandGain=0 -> aus.
                if(minLandGain > 0 && usableLandGain(pt, claimR) < minLandGain)
                    continue;
                const int frontier = frontierScore(pt);
                const int dist = hqPos.isValid() ? static_cast<int>(gwb.CalcDistance(pt, hqPos)) : 0;
                // Sektoren mit weniger eigenem MilitÃ¤r bevorzugen (gleichmÃ¤ÃŸig in
                // ALLE Richtungen expandieren statt sich in eine zu verbeiÃŸen).
                const int sectorPenalty = hqPos.isValid() ? sectorMil[sectorOf(pt)] * 15 : 0;
                const int indirectPenalty = frontDist > 2 ? 18 : 0;
                cands.emplace_back(frontier * 8 - dist - sectorPenalty - indirectPenalty, pt);
            }
        }
        debugCands += cands.size();
        std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int tries = 0;
        for(const auto& c : cands)
        {
            if(placeAt(bt, c.second)) // aufgeschobene Anbindung (korrekt, ohne Konflikte)
            {
                placed = true;
                break;
            }
            if(++tries >= 15)
                break;
        }
        if(placed)
            break;
    }

    if(std::getenv("RTTR_AI_DEBUG"))
    {
        // Stall-Diagnose: nur Mittel-/SpÃ¤tspiel (genug GebÃ¤ude), gestichprobt.
        static int dbgm = 0;
        if(milPos.size() >= 6 && dbgm < 120)
        {
            ++dbgm;
            std::cerr << "[placeMil] milPos=" << milPos.size() << " cands=" << debugCands
                      << " placed=" << placed << "\n";
        }
    }
    return placed;
}

// ===========================================================================
// Schicht: MilitÃ¤r (opportunistische Angriffe)
// ===========================================================================
void AdvancedAIPlayer::runMilitary()
{
    tryAttack();
}

bool AdvancedAIPlayer::tryAttack()
{
    const AIParams& P = AIParams::get();
    if(soldiersAvailable() < P.attackMinSoldiers)
        return false;

    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
    {
        for(MapPoint pt : collectPoints(mb->GetPos(), kAttackScanRadius))
        {
            if(!aii.IsVisible(pt))
                continue;
            const nobMilitary* enemy = gwb.GetSpecObj<nobMilitary>(pt);
            if(!enemy)
                continue;
            const unsigned char owner = enemy->GetPlayer();
            if(owner == playerId || !aii.IsPlayerAttackable(owner))
                continue;

            const int frac = std::max(1, soldiersAvailable() * P.attackFractionPct / 100);
            const unsigned n = static_cast<unsigned>(frac);
            if(n == 0)
                return false;
            // Mit starken Soldaten angreifen.
            if(aii.Attack(pt, n, true))
                return true;
        }
    }
    return false;
}

// ===========================================================================
// Schicht: AufklÃ¤rung (Geologen zur Erzsuche)
// ===========================================================================
void AdvancedAIPlayer::runScouting()
{
    // Weiter prospektieren, solange wir MEHR Minen wollen, als wir haben (skaliert
    // mit der Nahrung), und Geologen verfÃ¼gbar sind. FrÃ¼her war hier hart bei 2
    // Minen Schluss -> es wurde nie nach weiterem Erz gesucht, also nie mehr als
    // ~2 Minen gebaut, selbst bei reichlich Nahrung/Bergfeldern.
    const int wantMines = desiredCount(BuildingType::CoalMine) + desiredCount(BuildingType::IronMine)
                          + desiredCount(BuildingType::GoldMine);
    const int haveMines = total(BuildingType::CoalMine) + total(BuildingType::IronMine)
                          + total(BuildingType::GoldMine);
    if(haveMines >= wantMines || stock(Job::Geologist) == 0)
        return;

    // Geologen von Lager- UND MilitÃ¤rgebÃ¤ude-Flaggen losschicken (MilitÃ¤rgebÃ¤ude
    // liegen am Territoriumsrand, nÃ¤her an Bergen). NUR, wenn ein UNERFORSCHTES
    // Bergwerksfeld in Reichweite ist: ein Bergplatz (Mine-BQ) OHNE Geologen-Schild.
    // Schon vermessene Berge (mit Schild) erneut anzusteuern ist sinnlos (der Geologe
    // meidet beschilderte Felder ohnehin und liefe nur herum) -> das war das
    // "unsinnig oft". FrÃ¼he/schnelle Erzsuche bleibt aber wichtig (Eisen fÃ¼r Waffen),
    // daher KEINE kÃ¼nstliche Bremse: sind noch unerforschte Berge da, wird gesucht;
    // sind alle vermessen, hÃ¶rt es von selbst auf.
    std::vector<MapPoint> origins;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        origins.push_back(wh->GetPos());
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
        origins.push_back(mb->GetPos());

    int dispatched = 0;
    for(MapPoint o : origins)
    {
        bool unsurveyedMountain = false;
        for(MapPoint pt : collectPoints(o, kSearchRadius + 6))
        {
            if(canUseBq(aii.GetBuildingQuality(pt), BuildingQuality::Mine)
               && !gwb.GetSpecObj<noSign>(pt)) // Mine-BQ ohne Schild = noch nicht vermessen
            {
                unsurveyedMountain = true;
                break;
            }
        }
        if(!unsurveyedMountain)
            continue;
        aii.CallSpecialist(gwb.GetNeighbour(o, Direction::SouthEast), Job::Geologist);
        if(++dispatched >= 4)
            break;
    }
}

// ===========================================================================
// Schicht: See & Expeditionen
// ===========================================================================
void AdvancedAIPlayer::runSea()
{
    // Nur aktiv, wenn die Karte Seefahrt erlaubt (eigene HÃ¤fen oder nutzbare
    // HafenplÃ¤tze auf einem Meer mit â‰¥2 PlÃ¤tzen).
    const bool seaRelevant = !aii.GetHarbors().empty() || !aii.getUsableHarbors().empty();
    if(!seaRelevant)
        return;

    // 1. HafengebÃ¤ude auf freiem Hafenplatz errichten (sofern landseitig erreichbar).
    buildOnHarborSpot();

    if(!aii.GetHarbors().empty())
    {
        // 2. Werft in HafennÃ¤he + auf Schiffsbau stellen.
        buildShipyardNearHarbor();
        setShipyardsToShips();
        // 3. Expedition starten, falls ein Schiff bereitsteht.
        considerExpedition();
    }
}

bool AdvancedAIPlayer::buildOnHarborSpot()
{
    std::vector<MapPoint> centers = warehousePositions();
    for(const nobHarborBuilding* hb : aii.GetHarbors())
        centers.push_back(hb->GetPos());
    return placeNear(BuildingType::HarborBuilding, centers, 20);
}

bool AdvancedAIPlayer::buildShipyardNearHarbor()
{
    if(total(BuildingType::Shipyard) > 0)
        return false;
    std::vector<MapPoint> centers;
    for(const nobHarborBuilding* hb : aii.GetHarbors())
        centers.push_back(hb->GetPos());
    return placeNear(BuildingType::Shipyard, centers, 6);
}

void AdvancedAIPlayer::setShipyardsToShips()
{
    for(const nobUsual* b : aii.GetBuildings(BuildingType::Shipyard))
        aii.SetShipYardMode(static_cast<const nobShipYard*>(b), /*buildShips=*/true);
}

void AdvancedAIPlayer::considerExpedition()
{
    if(aii.GetNumShips() == 0)
        return;
    // Eine Expedition je Aufruf starten; die Engine prÃ¼ft die Machbarkeit und
    // meldet spÃ¤ter per ExpeditionWaiting, wenn das Schiff einen Platz erreicht.
    for(const nobHarborBuilding* hb : aii.GetHarbors())
    {
        if(aii.StartStopExpedition(hb, true))
            return;
    }
}

void AdvancedAIPlayer::foundColonyAt(MapPoint pos)
{
    for(noShip* ship : aii.GetShips())
    {
        if(ship && ship->GetPos() == pos)
        {
            aii.FoundColony(*ship);
            return;
        }
    }
}

// ===========================================================================
// Schicht: Einstellungen (MilitÃ¤r- und Werkzeugregler)
// ===========================================================================
void AdvancedAIPlayer::adjustSettings()
{
    // --- MilitÃ¤reinstellungen (8 Schieberegler, vgl. SettingsTypes.h) ---
    // Tendenz: Grenze voll besetzen, Inland leeren, moderat rekrutieren.
    // Hinweis: Die Skala ist enginedefiniert; die Engine begrenzt die Werte.
    // Werte aus AIParams (tunebar): 0=Rekrut, 1=VerteidigerstÃ¤rke, 2=aktive
    // Verteidiger, 3=AngriffsstÃ¤rke, 4..7=Besatzung Inland/Mittel/Hafen/Grenze.
    const AIParams& P = AIParams::get();
    MilitarySettings ms{};
    for(unsigned i = 0; i < 8; ++i)
        ms[i] = static_cast<uint8_t>(P.milSettings[i]);
    // Besatzung nach Frontentfernung (mil4=Inland .. mil7=Grenze) kommt jetzt aus
    // AIParams und ist damit OPTIMIERBAR. Default {â€¦,4,6,8,8}: Inland klein, Front
    // voll -> die Engine schiebt Ã¼berzÃ¤hlige Inland-Soldaten fortlaufend an die
    // Grenze (sicher, da die FrontnÃ¤he jeden Tick neu bewertet wird; ein per-
    // GebÃ¤ude-Limit hÃ¤tte eine ~1000-GF-LÃ¼cke -> GebÃ¤udeverluste). Das +1-
    // Grundlevel hÃ¤lt Inland-Bauten besetzt, das BefÃ¶rderungs-GebÃ¤ude behÃ¤lt genug.
    aii.ChangeMilitary(ms);
    // Hinweis: Warenverteilung NICHT Ã¼berschreiben â€“ die Engine-Defaults geben der
    // MÃ¼nzprÃ¤gerei bereits die hÃ¶chste Kohle-PrioritÃ¤t (Coal->Mint=10 vs Armory=8,
    // Ironsmelter=7). Ein eigenes ChangeDistribution verschlechterte alles.

    // --- Werkzeugproduktion: BEDARFSGERECHT (nicht mehr fix) ---
    adjustToolProduction();
    manageRecruitWarehouse();

    // --- Gold-Kette: BefÃ¶rderung von Soldaten sicherstellen ---
    ensureUpgradeBuilding();
    manageMilitaryGold();
}

void AdvancedAIPlayer::manageRecruitWarehouse()
{
    const auto& warehouses = aii.GetStorehouses();
    if(warehouses.size() < 2)
        return;
    if(aii.GetBuildings(BuildingType::Brewery).empty() || aii.GetBuildings(BuildingType::Armory).empty())
        return;

    auto nearestProductionDistance = [&](MapPoint whPos, BuildingType bt) {
        unsigned best = UINT_MAX;
        for(const nobUsual* bld : aii.GetBuildings(bt))
            best = std::min(best, gwb.CalcDistance(whPos, bld->GetPos()));
        return best;
    };

    const nobBaseWarehouse* target = nullptr;
    unsigned bestScore = UINT_MAX;
    for(const nobBaseWarehouse* wh : warehouses)
    {
        const unsigned beerDist = nearestProductionDistance(wh->GetPos(), BuildingType::Brewery);
        const unsigned armoryDist = nearestProductionDistance(wh->GetPos(), BuildingType::Armory);
        if(beerDist == UINT_MAX || armoryDist == UINT_MAX)
            continue;
        const unsigned score = beerDist + armoryDist;
        if(score < bestScore)
        {
            bestScore = score;
            target = wh;
        }
    }
    if(!target)
        return;

    const GoodType recruitGoods[] = {GoodType::Beer, GoodType::Sword, GoodType::ShieldRomans};
    for(const nobBaseWarehouse* wh : warehouses)
    {
        const bool isTarget = wh->GetPos() == target->GetPos();
        const InventorySetting desired(isTarget ? EInventorySetting::Collect : EInventorySetting::Send);
        for(const GoodType good : recruitGoods)
            if(wh->GetInventorySetting(good) != desired)
                aii.SetInventorySetting(wh->GetPos(), good, desired);
    }
}

void AdvancedAIPlayer::adjustToolProduction()
{
    // Die WIRTSCHAFTLICH ESSENZIELLEN Werkzeuge (fÃ¼r Holz/Bretter/Stein/Nahrung/Bau).
    // Nur diese â€“ die seltenen wÃ¼rden den EINEN Schlosser verzetteln.
    ToolSettings ts{};
    helpers::EnumArray<int8_t, Tool> orderDelta{};
    bool anyOrder = false;

    const auto demand = calculateToolDemand();
    int toolPressure = 0;
    int basicToolPressure = 0;
    for(const Tool t : helpers::enumRange<Tool>())
    {
        int queued = 0;
        if(ggs.isEnabled(AddonId::TOOL_ORDERING))
            queued = static_cast<int>(player.GetToolsOrderedVisual(t));
        const int pressure = std::max(demand[t], queued);
        toolPressure += pressure;
        if(t == Tool::Saw || t == Tool::Axe || t == Tool::PickAxe || t == Tool::Hammer || t == Tool::Shovel
           || t == Tool::Tongs)
            basicToolPressure += pressure;
    }

    const bool canProduceToolsSoon = total(BuildingType::Metalworks) > 0 || desiredCount(BuildingType::Metalworks) > 0;
    const bool pauseArmoriesForTools =
      canProduceToolsSoon && toolPressure >= 8 && basicToolPressure >= 4 && stock(GoodType::Iron) < 8;
    for(const nobUsual* armory : aii.GetBuildings(BuildingType::Armory))
    {
        const bool disabled = armory->IsProductionDisabledVirtual();
        if(disabled == pauseArmoriesForTools)
            continue;
        aii.SetProductionEnabled(armory->GetPos(), !pauseArmoriesForTools);
    }

    if(ggs.isEnabled(AddonId::TOOL_ORDERING))
    {
        // MIT Addon: BEDARFSGERECHTE DIREKT-BESTELLUNG. Es wird nur der Fehlbedarf
        // eines kleinen Puffers bestellt; die laufende Produktion (Slider) bleibt
        // AUS. Ist nichts offen, steht die Schlosserei -> kein Eisenverbrauch, das
        // Eisen steht der Schmiede (Waffen) zur VerfÃ¼gung â€“ genau das gewÃ¼nschte
        // Verhalten (gezielter Werkzeug-Nachschub statt Dauerproduktion).
        for(const Tool t : helpers::enumRange<Tool>())
        {
            int d = demand[t] - static_cast<int>(player.GetToolsOrderedVisual(t));
            d = std::max(-100, std::min(100, d));
            if(d != 0)
            {
                orderDelta[t] = static_cast<int8_t>(d);
                anyOrder = true;
            }
        }
        // ts bleibt 0 -> reine Bestellsteuerung.
    } else
    {
        // OHNE Addon: bewÃ¤hrte STETIGE Grundwerkzeug-Produktion. Wichtiger Befund
        // aus ausfÃ¼hrlichem Self-Play: das Drosseln/Stoppen der Werkzeugproduktion
        // (um Eisen zu sparen) verschlechtert die Wirtschaft DEUTLICH (weniger
        // Werkzeug -> langsamere Arbeiterstellung -> Einbruch; der Eisen-Vorteil fÃ¼r
        // die Schmiede wiegt das nicht auf, ~-15 parity auf ALASKA). Daher hier die
        // robuste Dauerproduktion; das Spar-Verhalten gibt es bewusst nur mit Addon.
        for(const Tool t : helpers::enumRange<Tool>())
            ts[t] = static_cast<uint8_t>(std::min(10, demand[t] > 0 ? 2 + demand[t] * 2 : 0));
    }

    // Nur senden, wenn sich Einstellungen ODER Bestellungen Ã¤ndern (kein GC-Spam).
    bool settingsChanged = false;
    for(const Tool t : helpers::enumRange<Tool>())
        if(ts[t] != player.GetToolPriority(t))
        {
            settingsChanged = true;
            break;
        }
    if(settingsChanged || anyOrder)
        aii.ChangeTools(ts, anyOrder ? orderDelta.data() : nullptr);
}

// Sorgt dafÃ¼r, dass es ein Inland-Wachturm/-Burg als BefÃ¶rderungs-GebÃ¤ude in
// MÃ¼nznÃ¤he gibt. Nur relevant, wenn Ã¼berhaupt MÃ¼nzen produziert werden (Mint).
bool AdvancedAIPlayer::ensureUpgradeBuilding()
{
    if(aii.GetBuildings(BuildingType::Mint).empty())
        return false; // keine MÃ¼nzproduktion -> kein Bedarf

    // Existiert bereits ein geeignetes Inland-Wachturm/-Burg-GebÃ¤ude?
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
    {
        const BuildingType bt = mb->GetBuildingType();
        if((bt == BuildingType::Watchtower || bt == BuildingType::Fortress)
           && mb->GetFrontierDistance() == FrontierDistance::Far)
            return false; // schon vorhanden
    }

    // Nach einem Bau erst abwarten (Bau + Territoriumswachstum), bevor erneut.
    if(upgradeBuildCd_ > 0)
    {
        --upgradeBuildCd_;
        return false;
    }

    // Bauzentren: zuerst die MÃ¼nzprÃ¤gerei (kurze Goldwege), dann die Lager/HQ
    // (dort ist sicher Platz fÃ¼r einen Turm, ebenfalls Inland & geschÃ¼tzt).
    std::vector<MapPoint> centers;
    for(const nobUsual* m : aii.GetBuildings(BuildingType::Mint))
        centers.push_back(m->GetPos());
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        centers.push_back(wh->GetPos());
    if(centers.empty())
        return false;

    // Steht schon ein groÃŸer MilitÃ¤rbau nahe einem Zentrum? Dann abwarten, bis er
    // zum Inland reift, statt einen zweiten danebenzusetzen.
    for(const nobMilitary* mb : aii.GetMilitaryBuildings())
    {
        const BuildingType bt = mb->GetBuildingType();
        if(bt != BuildingType::Watchtower && bt != BuildingType::Fortress)
            continue;
        for(MapPoint c : centers)
            if(gwb.CalcDistance(mb->GetPos(), c) <= 10)
                return false;
    }

    // Turm bauen â€“ Wachturm bevorzugt, sonst Wachstube (kleinere BQ, leichter
    // platzierbar). Beide in/nahe dem geschÃ¼tzten Inland.
    for(BuildingType want : {BuildingType::Watchtower, BuildingType::Guardhouse})
    {
        if(!aii.CanBuildBuildingtype(want))
            continue;
        if(placeNear(want, centers, 12))
        {
            upgradeBuildCd_ = 15; // ~15 Settings-Takte warten
            return true;
        }
    }
    return false;
}

void AdvancedAIPlayer::manageMilitaryGold()
{
    const auto& milBlds = aii.GetMilitaryBuildings();
    if(milBlds.empty())
        return;

    const unsigned maxRank = gwb.GetGGS().GetMaxMilitaryRank();

    // LÃ¤uft die Gold-Kette (MÃ¼nzen im Bestand ODER produzierende MÃ¼nzprÃ¤gerei)?
    // Nur dann lohnt sich ein dediziertes BefÃ¶rderungs-GebÃ¤ude.
    MapPoint mintPos = MapPoint::Invalid();
    bool goldChainActive = stock(GoodType::Coins) > 0;
    for(const nobUsual* m : aii.GetBuildings(BuildingType::Mint))
    {
        if(!mintPos.isValid())
            mintPos = m->GetPos();
        if(m->GetProductivity() > 0)
            goldChainActive = true;
    }

    // BefÃ¶rderungs-GebÃ¤ude wÃ¤hlen (nur bei aktiver Gold-Kette): INLAND (Far),
    // angebundener Wachturm/Burg, mÃ¶glichst nah an der MÃ¼nzprÃ¤gerei.
    const nobMilitary* upgrade = nullptr;
    if(goldChainActive)
    {
        unsigned bestFar = std::numeric_limits<unsigned>::max();
        for(const nobMilitary* mb : milBlds)
        {
            const BuildingType bt = mb->GetBuildingType();
            if(bt != BuildingType::Watchtower && bt != BuildingType::Fortress)
                continue;
            if(mb->GetFrontierDistance() != FrontierDistance::Far)
                continue; // sicher im Inland
            if(!isFlagConnected(mb->GetFlag()->GetPos()))
                continue; // ohne Anbindung kommt kein Gold an
            const unsigned d = mintPos.isValid() ? gwb.CalcDistance(mb->GetPos(), mintPos) : 0;
            if(d < bestFar)
            {
                bestFar = d;
                upgrade = mb;
            }
        }
    }
    // Wechselt/verschwindet das BefÃ¶rderungs-GebÃ¤ude, das alte auf volle Besatzung
    // zurÃ¼cksetzen (sonst bliebe es mit GenerÃ¤le=0 dauerhaft unterbesetzt).
    if(upgradeBldPos_.isValid() && (!upgrade || upgrade->GetPos() != upgradeBldPos_))
    {
        for(const nobMilitary* mb : milBlds)
            if(mb->GetPos() == upgradeBldPos_)
            {
                for(unsigned r = 0; r <= maxRank; ++r)
                    aii.SetTroopLimit(mb->GetPos(), r, mb->GetMaxTroopsCt());
                break;
            }
    }
    upgradeBldPos_ = upgrade ? upgrade->GetPos() : MapPoint::Invalid();

    if(!upgrade)
        return; // keine aktive Gold-Kette / kein Inland-Bau -> nur die Regler wirken

    // Inlandâ†’Front-Verschiebung macht der Schieberegler (oben). HIER nur die
    // Gold-Konzentration + das BefÃ¶rderungs-GebÃ¤ude:
    for(const nobMilitary* mb : milBlds)
    {
        if(mb == upgrade)
        {
            if(mb->IsGoldDisabled())
                aii.SetCoinsAllowed(mb, true); // Gold AN
            // Viele Rekruten (Rang 0), je 1 Mittelrang, KEINE GenerÃ¤le (max.Rang=0)
            // -> GenerÃ¤le werden ausgelagert (an die Front), Rekruten rÃ¼cken nach
            // und werden mit dem konzentrierten Gold befÃ¶rdert.
            aii.SetTroopLimit(mb->GetPos(), 0, mb->GetMaxTroopsCt());
            for(unsigned r = 1; r < maxRank; ++r)
                aii.SetTroopLimit(mb->GetPos(), r, 1);
            aii.SetTroopLimit(mb->GetPos(), maxRank, 0);
        } else if(!mb->IsGoldDisabled())
        {
            aii.SetCoinsAllowed(mb, false); // Gold woanders aus -> konzentrieren
        }
    }
}

// ===========================================================================
// Helfer / Wahrnehmung
// ===========================================================================
int AdvancedAIPlayer::countBuildings(BuildingType bt) const
{
    // MilitÃ¤rgebÃ¤ude liegen in einer eigenen Liste.
    switch(bt)
    {
        case BuildingType::Barracks:
        case BuildingType::Guardhouse:
        case BuildingType::Watchtower:
        case BuildingType::Fortress:
        {
            int n = 0;
            for(const nobMilitary* mb : aii.GetMilitaryBuildings())
                if(mb->GetBuildingType() == bt)
                    ++n;
            return n;
        }
        // Diese sind keine nobUsual-GebÃ¤ude -> nicht Ã¼ber GetBuildings() abfragbar.
        // Fertige Exemplare werden andernorts (GetStorehouses/GetHeadquarter) erfasst.
        case BuildingType::Headquarters:
        case BuildingType::Storehouse:
        case BuildingType::HarborBuilding:
        case BuildingType::Catapult: return 0;
        default: return static_cast<int>(aii.GetBuildings(bt).size());
    }
}

int AdvancedAIPlayer::countSites(BuildingType bt) const
{
    int n = 0;
    for(const noBuildingSite* bs : aii.GetBuildingSites())
        if(bs->GetBuildingType() == bt)
            ++n;
    return n;
}

int AdvancedAIPlayer::numMilitary() const
{
    return static_cast<int>(aii.GetMilitaryBuildings().size());
}

int AdvancedAIPlayer::stock(GoodType g) const
{
    return static_cast<int>(aii.GetInventory()[g]);
}

int AdvancedAIPlayer::stock(Job j) const
{
    return static_cast<int>(aii.GetInventory()[j]);
}

int AdvancedAIPlayer::soldiersAvailable() const
{
    const Inventory& inv = aii.GetInventory();
    return static_cast<int>(inv[Job::Private] + inv[Job::PrivateFirstClass] + inv[Job::Sergeant] + inv[Job::Officer]
                            + inv[Job::General]);
}

bool AdvancedAIPlayer::anyMine() const
{
    return total(BuildingType::CoalMine) + total(BuildingType::IronMine) + total(BuildingType::GoldMine)
             + total(BuildingType::GraniteMine)
           > 0;
}

std::vector<MapPoint> AdvancedAIPlayer::warehousePositions() const
{
    std::vector<MapPoint> result;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        result.push_back(wh->GetPos());
    return result;
}

// Sammelt alle Knotenpunkte bis 'radius' um 'center' (BFS Ã¼ber die 6 Hexrichtungen).
std::vector<MapPoint> AdvancedAIPlayer::collectPoints(MapPoint center, unsigned radius) const
{
    const MapExtent size = gwb.GetSize();
    std::vector<char> visited(static_cast<std::size_t>(size.x) * size.y, 0);
    std::vector<MapPoint> result;

    auto toIdx = [&](MapPoint p) { return static_cast<std::size_t>(p.x) + static_cast<std::size_t>(p.y) * size.x; };

    std::queue<std::pair<MapPoint, unsigned>> q;
    visited[toIdx(center)] = 1;
    result.push_back(center);
    q.push({center, 0});

    while(!q.empty())
    {
        const MapPoint p = q.front().first;
        const unsigned d = q.front().second;
        q.pop();
        if(d >= radius)
            continue;
        for(unsigned di = 0; di < 6; ++di)
        {
            const MapPoint n = gwb.GetNeighbour(p, Direction(di));
            const std::size_t ni = toIdx(n);
            if(!visited[ni])
            {
                visited[ni] = 1;
                result.push_back(n);
                q.push({n, d + 1});
            }
        }
    }
    return result;
}

} // namespace advai
