// AdvancedAIPlayer – ein KI-Gegenspieler für Return to the Roots,
// umgesetzt nach RTTR-KI-KONZEPT.md (hierarchische, pollende Architektur).
//
// Schichten:
//   - Wahrnehmung:  direkte Abfragen über AIInterface (Ground Truth jeden Tick)
//   - Ökonomie:     Produktionsketten-Planer mit Bedarfslogik + Deadlock-Schutz
//   - Expansion:    militärische Landnahme zur Grenze hin
//   - Militär:      opportunistische Angriffe, Verteidigung über Einstellungen
//   - Ausführung:   GameCommands via AIInterface (GameCommandFactory)
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ai/AIEventManager.h"
#include "ai/AIPlayer.h"
#include "notifications/Subscription.h"
#include "gameTypes/AIInfo.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/ChatDestination.h"
#include "gameTypes/Direction.h"
#include "gameTypes/GoodTypes.h"
#include "gameTypes/JobTypes.h"
#include "gameTypes/MapCoordinates.h"
#include "helpers/EnumArray.h"
#include <string>
#include <vector>

class GameWorldBase;
class noFlag;
namespace AIEvent {
class Base;
}

namespace advai {

class AdvancedAIPlayer final : public AIPlayer
{
public:
    AdvancedAIPlayer(unsigned char playerId, const GameWorldBase& gwb, AI::Level level);

    /// Herzschlag – wird von der Engine für jeden Game-Frame aufgerufen.
    void RunGF(unsigned gf, bool gfisnwf) override;
    void OnChatMessage(unsigned /*sendPlayerId*/, ChatDestination, const std::string& /*msg*/) override {}

private:
    // ---- Schicht-Einstiegspunkte (amortisiert getaktet) ----
    void runInit();
    void runEconomy();
    void runExpansion();
    void runMilitary();
    void runScouting();
    void adjustSettings();
    /// Bedarfsgerechte Werkzeugproduktion: produziert/bestellt nur Werkzeuge, für
    /// die offene Arbeitsplätze fehlen; sonst 0 (Produktion aus -> Eisen frei für
    /// die Schmiede). Nutzt Direkt-Bestellung, wenn das Addon TOOL_ORDERING aktiv ist.
    void adjustToolProduction();
    void manageRecruitWarehouse();

    // ---- Reaktive Schicht (AIEvents -> sofortige Reaktion) ----
    void drainEvents();
    void handleEvent(const AIEvent::Base& ev);
    bool shouldKeepDepletedWoodcutter(MapPoint pos) const;
    bool shouldDestroyDepletedWoodcutter(MapPoint pos);
    void forgetDepletedWoodcutter(MapPoint pos);

    // ---- Ökonomie ----
    int desiredCount(BuildingType bt) const;
    bool buildBuilding(BuildingType bt);
    helpers::EnumArray<int, Tool> calculateToolDemand() const;
    int totalToolPressure() const;
    /// Zulieferer-Gebäudetypen einer Produktionsstufe (für räumliche Nähe):
    /// das Gebäude wird bevorzugt NAHE seiner Vorstufe platziert (kurze Wege).
    std::vector<BuildingType> supplierTypes(BuildingType bt) const;

    // ---- Platzierung & Wege ----
    // immediate=true: Straße sofort bauen (geringe Latenz, für Militär an der
    // Front wichtig). Sonst aufgeschoben über runConnect (Wirtschaft).
    bool placeAt(BuildingType bt, MapPoint pt, bool immediate = false);
    bool connectToNetwork(MapPoint bldPos);
    MapPoint nearestWarehouseFlag(MapPoint from) const;
    /// JH-artige Wegenetz-Anbindung: plant die beste Straße von bldFlag ins Netz
    /// (minimiert 2*Neubaulänge + Restweg zum Lager + Nicht-Fahnbarkeit). true +
    /// Route, wenn eine gültige Anbindung existiert.
    /// outTarget/outJunction (optional): Zielpunkt der Straße und ob dort eine
    /// neue Kreuzungsfahne (auf einer bestehenden Straße) gesetzt werden muss.
    bool planConnection(MapPoint bldFlag, std::vector<Direction>& outRoute, MapPoint* outTarget = nullptr,
                        bool* outJunction = nullptr) const;
    /// Setzt Zwischenfahnen entlang einer frisch gebauten Straße (Abstand ~2).
    void setFlagsAlongRoad(MapPoint startFlag, const std::vector<Direction>& route);
    int scorePlacement(BuildingType bt, MapPoint pt, MapPoint center) const;
    /// Typ-spezifische Platz-Eignung (Abstand/Freiraum), v.a. für Bauernhöfe
    /// (brauchen offene Felder) und Förster (nicht an Höfe).
    bool placementAllowed(BuildingType bt, MapPoint pt) const;
    int countForesterPlantSpots(MapPoint foresterPos, MapPoint blockedBuildingPos = MapPoint::Invalid()) const;
    bool preservesForesterPlantReserve(MapPoint pt) const;

    // ---- Aufgeschobene Anbindung (korrekte Reihenfolge!) ----
    // Gebäude werden zuerst platziert; die Straße wird ERST gebaut, wenn die
    // Fahne real in der Welt existiert und gegen den AKTUELLEN Zustand verbunden
    // werden kann. Das verhindert Geisterfahnen, nicht angebundene Häuser und
    // sich überschneidende/unsinnige Straßen aus Stapel-Platzierung.
    void enqueueConnect(MapPoint flagPos, bool allowDestroy = true);
    /// Bindet eigene Gebäude, die (durch Eroberung oder Gebietsverlust) vom
    /// Wegenetz getrennt sind, wieder an (ohne sie bei Misserfolg abzureißen).
    void reconnectOrphanedBuildings();
    void runConnect();
    bool isFlagConnected(MapPoint flagPos) const;

    /// Periodische Wegenetz-Optimierung: erkennt Staus (volle Fahnen) und baut
    /// Abkürzungen/Querverbindungen zwischen Zweigen, wenn der bestehende
    /// On-Road-Umweg deutlich länger ist als eine neue direkte Straße.
    void runRoadOptimize();
    bool pruneDeadRoadBranch(const noFlag& startFlag, Direction excludeDir, bool hasExclude);
    bool pruneLongUnusedRoadDetour();
    /// Versucht von 'fromFlag' eine lohnende Abkürzung zu einer nahen,
    /// angebundenen Fahne zu bauen (Umweg >> Neubaulänge). true bei Bau.
    bool buildShortcutFrom(MapPoint fromFlag);

    // ---- Militär ----
    bool placeMilitary();
    void retireSafeInlandMilitary();
    bool tryAttack();
    /// Gold-Kette schließen: ein INLAND-Wachturm/-Burg nahe der Münzprägerei wird
    /// zum dedizierten "Beförderungs-Gebäude" – Gold dorthin (sonst aus), und per
    /// Truppenlimit werden Generäle (max. Rang) ausgelagert, damit ständig frische
    /// Rekruten nachrücken und mit Gold befördert werden. Baut bei Bedarf einen
    /// Wachturm in Münznähe, falls kein geeignetes Inland-Gebäude existiert.
    void manageMilitaryGold();
    bool ensureUpgradeBuilding();

    // ---- See & Expeditionen ----
    void runSea();
    bool buildOnHarborSpot();         ///< Hafengebäude auf freiem Hafenplatz errichten
    bool buildShipyardNearHarbor();   ///< Werft in Hafennähe errichten
    void setShipyardsToShips();       ///< alle Werften auf Schiffsbau stellen
    void considerExpedition();        ///< Expedition starten, wenn Schiff verfügbar
    void foundColonyAt(MapPoint pos); ///< wartendes Expeditionsschiff gründet Kolonie
    bool placeNear(BuildingType bt, const std::vector<MapPoint>& centers, unsigned radius,
                   unsigned minDistToWarehouse = 0);

    // ---- Helfer / Wahrnehmung ----
    int countBuildings(BuildingType bt) const;
    int countSites(BuildingType bt) const;
    int total(BuildingType bt) const { return countBuildings(bt) + countSites(bt); }
    int numMilitary() const;
    int stock(GoodType g) const;
    int stock(Job j) const;
    int soldiersAvailable() const;
    bool anyMine() const;
    std::vector<MapPoint> warehousePositions() const;
    std::vector<MapPoint> collectPoints(MapPoint center, unsigned radius) const;

    // ---- Reaktive Schicht: Ereignis-Queue + Notification-Abos ----
    AIEventManager eventManager_;
    Subscription subBuilding_, subExpedition_, subResource_, subRoad_, subShip_;
    // Pro-Tick gesetzte Reaktionswünsche (durch Ereignisse ausgelöst).
    bool reactEconomy_ = false;
    bool reactExpansion_ = false;

    // ---- Aufgeschobene Anbindungs-Aufträge ----
    struct PendingConnect
    {
        MapPoint flag;
        int cooldown = 0;    // Ticks bis zur nächsten Bearbeitung
        int fails = 0;       // Fehlversuche (-> irgendwann aufgeben/abreißen)
        bool roadIssued = false; // Straße bereits beauftragt (auf Ausführung warten)
        bool allowDestroy = true; // bei Dauer-Misserfolg Gebäude abreißen? (Geisterbau
                                  // ja; WIEDER-Anbindung bestehender/eroberter Bauten nein)
    };
    std::vector<PendingConnect> pendingConnect_;

    // ---- Zustand ----
    bool initialized_ = false;
    bool surrendered_ = false;
    unsigned currentGF_ = 0;
    /// Aktuelles Beförderungs-Gebäude (Gold-Upgrade). Invalid = keines.
    MapPoint upgradeBldPos_ = MapPoint::Invalid();
    /// Orte erschöpfter Fischgründe (Fische regenerieren sich nicht!). Hier bzw. in
    /// Arbeitsradius-Nähe wird KEINE neue Fischerhütte mehr gebaut.
    std::vector<MapPoint> depletedFishSpots_;
    struct DepletedWoodcutter
    {
        MapPoint pos;
        unsigned firstGF = 0;
    };
    std::vector<DepletedWoodcutter> depletedWoodcutters_;
    /// Sperre (in settingsInterval-Takten) nach dem Bau eines Upgrade-Turms, bis er
    /// fertig ist und das Territorium ihn zum Inland macht – kein Doppelbau.
    int upgradeBuildCd_ = 0;
};

} // namespace advai
