// Headless Self-Play-Suite: AdvancedAIPlayer (Spieler 0) vs. AIPlayerJH (Rest)
// auf MEHREREN echten Karten mit AUTO-erkannter Spielerzahl. Der betrachtete
// Zeitraum (GF) skaliert mit der Distanz ~ sqrt(Flaeche / Spielerzahl).
// Ausgabe je Partie: "RESULT {json}", am Ende "SUMMARY {json}".
// SPDX-License-Identifier: GPL-2.0-or-later

#define BOOST_TEST_MODULE AISelfPlay
#include <rttr/test/Fixture.hpp>
#include <boost/test/unit_test.hpp>

#include "Game.h"
#include "GameCommand.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "PlayerInfo.h"
#include "BuildingRegister.h"
#include "ai/AIPlayer.h"
#include "ai/advai/AdvancedAIPlayer.h"
#include "buildings/noBuilding.h"
#include "buildings/noBuildingSite.h"
#include "buildings/nobHQ.h"
#include "buildings/nobMilitary.h"
#include "buildings/nobUsual.h"
#include "nodeObjs/noFlag.h"
#include "factories/AIFactory.h"
#include "ogl/glAllocator.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "worldFixtures/TestEventManager.h"
#include "gameData/BuildingProperties.h"
#include "gameTypes/AIInfo.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/GoodTypes.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/JobTypes.h"
#include "libsiedler2/Archiv.h"
#include "libsiedler2/ArchivItem_Map.h"
#include "libsiedler2/ArchivItem_Map_Header.h"
#include "libsiedler2/libsiedler2.h"
#include "libsiedler2/prototypen.h"
#include "test/testConfig.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct GlobalFixture : rttr::test::Fixture
{
    GlobalFixture() { libsiedler2::setAllocator(new GlAllocator); }
};
BOOST_GLOBAL_FIXTURE(GlobalFixture);

namespace {
unsigned envU(const char* n, unsigned def)
{
    const char* v = std::getenv(n);
    if(!v)
        return def;
    try
    {
        return static_cast<unsigned>(std::stoul(v));
    } catch(...)
    {
        return def;
    }
}

struct Stats
{
    int defeated = 0;
    unsigned mil = 0, econ = 0, stores = 0, soldiers = 0, iron = 0, coal = 0, tools = 0, boards = 0, stones = 0;
};

Stats gather(const GamePlayer& p)
{
    Stats s;
    s.defeated = p.IsDefeated() ? 1 : 0;
    const BuildingRegister& br = p.GetBuildingRegister();
    s.mil = static_cast<unsigned>(br.GetMilitaryBuildings().size());
    s.stores = static_cast<unsigned>(br.GetStorehouses().size());
    for(unsigned i = 0; i < 40; ++i)
    {
        const BuildingType bt = BuildingType(i);
        if(BuildingProperties::IsMilitary(bt) || BuildingProperties::IsWareHouse(bt))
            continue;
        s.econ += static_cast<unsigned>(br.GetBuildings(bt).size());
    }
    const Inventory& inv = p.GetInventory();
    s.soldiers = static_cast<unsigned>(inv[Job::Private] + inv[Job::PrivateFirstClass] + inv[Job::Sergeant]
                                       + inv[Job::Officer] + inv[Job::General]);
    s.iron = inv[GoodType::Iron];
    s.coal = inv[GoodType::Coal];
    s.tools = inv[GoodType::Hammer];
    s.boards = inv[GoodType::Boards];
    s.stones = inv[GoodType::Stones];
    return s;
}

double strength(const Stats& s)
{
    return s.mil * 3.0 + s.econ * 1.0 + s.soldiers * 0.3 + (s.defeated ? -1000.0 : 0.0);
}

void printStats(const char* key, const Stats& s, bool comma)
{
    std::cout << "\"" << key << "\":{\"defeated\":" << s.defeated << ",\"mil\":" << s.mil << ",\"econ\":" << s.econ
              << ",\"stores\":" << s.stores << ",\"soldiers\":" << s.soldiers << ",\"iron\":" << s.iron
              << ",\"coal\":" << s.coal << ",\"tools\":" << s.tools << ",\"boards\":" << s.boards
              << ",\"stones\":" << s.stones << "}" << (comma ? "," : "");
}

// ===========================================================================
// Umfassende Ausbau-Qualität (mehrdimensional statt reiner Gebäudezahl).
//
// Bewertet die WIRKLICHE Güte des Ausbaus:
//   * Produktive Wirtschaft  – Produktivität JE Gebäude (nicht nur Existenz).
//                              Das erfasst implizit die Ketten-Balance: ein
//                              Sägewerk ohne Holznachschub hat ~0 % und zählt 0.
//   * Selbstversorgung       – produzieren ALLE kritischen Ketten wirklich?
//   * Territorium            – kontrolliertes Land.
//   * Militär-Bereitschaft   – besetzte Gebäude, rang­starke Armee, Gold.
//   * Bau-Kapazität          – Bauarbeiter (kann weiter ausgebaut werden).
//   * Strafen                – Leerlauf, Wege-Ineffizienz, Lager-Stau.
// ===========================================================================
struct Quality
{
    double prodCapacity = 0; // Σ Produktivität/100 über Wirtschaftsgebäude (Kern)
    int idle = 0;            // besetzte Gebäude mit ~0 % (blockiert -> Verschwendung)
    int selfSuff = 0;        // # aktiver kritischer Versorgungsketten (0..7)
    int territory = 0;       // eigene Karten-Knoten (Land)
    int occupiedMil = 0;     // Militärgebäude mit Truppen
    double armyStrength = 0; // rang­gewichtete Soldaten (Gebäude + Lager)
    int coins = 0, gold = 0; // Beförderungskapazität
    int builders = 0;        // Bauarbeiter -> Bau-Kapazität
    unsigned roadLen = 0;
    int buildings = 0;       // fertige Gebäude gesamt (für Wege-Effizienz)
    int congestion = 0;      // # stark überfüllter Güter (Stau/Verschwendung)
    bool defeated = false;

    double total() const
    {
        double t = 0;
        t += prodCapacity * 3.0;   // produktive Wirtschaft = Kern des Ausbaus
        t -= idle * 1.0;           // Leerlauf bestrafen
        t += selfSuff * 8.0;       // vollständige Versorgung (alle Ketten)
        t += territory * 0.02;     // Landgewinn (Potenzial, nicht überbewerten)
        t += occupiedMil * 2.0;    // gehaltene Front
        t += armyStrength * 0.15;  // echte, rang­starke Armee
        t += (coins + gold) * 0.4; // Gold/Münzen für Beförderung
        t += builders * 0.5;       // Bau-Kapazität
        if(buildings > 0)
        {
            const double per = double(roadLen) / buildings;
            if(per > 4.0)
                t -= (per - 4.0) * buildings * 0.1; // Wege-Ineffizienz
        }
        t -= congestion * 2.0; // Lager-Stau
        if(defeated)
            t -= 1000.0;
        return t;
    }
};

Quality computeQuality(const GameWorldBase& w, const GamePlayer& p)
{
    Quality q;
    q.defeated = p.IsDefeated();
    const BuildingRegister& br = p.GetBuildingRegister();
    const unsigned char pid = static_cast<unsigned char>(p.GetPlayerId());

    auto anyWorking = [&](BuildingType bt) {
        for(const nobUsual* u : br.GetBuildings(bt))
            if(u->GetProductivity() > 0)
                return true;
        return false;
    };

    // --- Produktive Wirtschaft (produktivitätsgewichtet) ---
    int econBld = 0;
    for(unsigned i = 0; i < 40; ++i)
    {
        const BuildingType bt = BuildingType(i);
        if(BuildingProperties::IsMilitary(bt) || BuildingProperties::IsWareHouse(bt))
            continue;
        for(const nobUsual* u : br.GetBuildings(bt))
        {
            ++econBld;
            q.prodCapacity += u->GetProductivity() / 100.0;
            if(u->HasWorker() && u->GetProductivity() < 10)
                ++q.idle;
        }
    }

    // --- Selbstversorgung: kritische Ketten, die WIRKLICH produzieren ---
    if(anyWorking(BuildingType::Sawmill))
        ++q.selfSuff; // Bretter
    if(anyWorking(BuildingType::Quarry))
        ++q.selfSuff; // Steine
    if(anyWorking(BuildingType::Bakery) || anyWorking(BuildingType::Hunter) || anyWorking(BuildingType::Fishery))
        ++q.selfSuff; // Nahrung
    if(anyWorking(BuildingType::Metalworks))
        ++q.selfSuff; // Werkzeug
    if(anyWorking(BuildingType::Armory))
        ++q.selfSuff; // Waffen
    if(anyWorking(BuildingType::Brewery))
        ++q.selfSuff; // Bier
    if(anyWorking(BuildingType::Mint))
        ++q.selfSuff; // Münzen

    // --- Territorium + Wegelänge (ein Karten-Durchlauf) ---
    const auto sz = w.GetSize();
    for(unsigned y = 0; y < static_cast<unsigned>(sz.y); ++y)
        for(unsigned x = 0; x < static_cast<unsigned>(sz.x); ++x)
        {
            const MapPoint pt(x, y);
            if(w.GetNode(pt).owner == pid + 1)
                ++q.territory;
            const noFlag* f = w.GetSpecObj<noFlag>(pt);
            if(f && f->GetPlayer() == pid)
                for(const auto* rseg : f->getRoutes())
                    if(rseg)
                        q.roadLen += rseg->GetLength();
        }
    q.roadLen /= 2; // jedes Segment von beiden Enden gezählt

    // --- Militär-Bereitschaft ---
    for(const nobMilitary* mb : br.GetMilitaryBuildings())
    {
        if(mb->GetNumTroops() > 0)
            ++q.occupiedMil;
        q.armyStrength += mb->GetSoldiersStrength(); // rang­gewichtete Stärke der Besatzung
    }
    const Inventory& inv = p.GetInventory();
    for(unsigned r = 0; r < NUM_SOLDIER_RANKS; ++r)
        q.armyStrength += inv[SOLDIER_JOBS[r]] * (r + 1);
    q.coins = inv[GoodType::Coins];
    q.gold = inv[GoodType::Gold];
    q.builders = inv[Job::Builder];
    q.buildings = static_cast<int>(br.GetMilitaryBuildings().size() + br.GetStorehouses().size()) + econBld;

    // --- Lager-Stau (Verstopfung/Verschwendung) ---
    const GoodType watch[] = {GoodType::Boards, GoodType::Stones, GoodType::Wood,  GoodType::Grain,
                              GoodType::Coal,   GoodType::IronOre, GoodType::Beer, GoodType::Water};
    for(GoodType g : watch)
        if(inv[g] > 200)
            ++q.congestion;

    return q;
}

void printQuality(const char* key, const Quality& q, bool comma)
{
    std::cout << "\"" << key << "\":{\"total\":" << q.total() << ",\"prod\":" << q.prodCapacity << ",\"idle\":" << q.idle
              << ",\"selfSuff\":" << q.selfSuff << ",\"territory\":" << q.territory << ",\"occMil\":" << q.occupiedMil
              << ",\"army\":" << q.armyStrength << ",\"coins\":" << q.coins << ",\"gold\":" << q.gold
              << ",\"builders\":" << q.builders << ",\"roadLen\":" << q.roadLen << ",\"bld\":" << q.buildings
              << ",\"congest\":" << q.congestion << "}" << (comma ? "," : "");
}

std::unique_ptr<AIPlayer> makeAI(bool advanced, unsigned char pid, const GameWorldBase& w)
{
    if(advanced)
        return std::make_unique<advai::AdvancedAIPlayer>(pid, w, AI::Level::Hard);
    return AIFactory::Create(AI::Info(AI::Type::Default, AI::Level::Hard), pid, w);
}

// Liefert die Spielerzahl einer Karte aus dem Header (0 bei Fehler).
unsigned readNumPlayers(const boost::filesystem::path& path)
{
    libsiedler2::Archiv hdr;
    if(libsiedler2::loader::LoadMAP(path, hdr, true) != 0)
        return 0;
    const auto* map = dynamic_cast<const libsiedler2::ArchivItem_Map*>(hdr[0]);
    return map ? map->getHeader().getNumPlayers() : 0;
}

// Header-Scan: gibt Größe/Spielerzahl/Fläche-pro-Spieler aus (keine Partie).
void printMapHeader(const std::string& rel, const boost::filesystem::path& path)
{
    libsiedler2::Archiv hdr;
    if(libsiedler2::loader::LoadMAP(path, hdr, true) != 0)
    {
        std::cout << "MAPHDR {\"map\":\"" << rel << "\",\"error\":1}" << std::endl;
        return;
    }
    const auto* map = dynamic_cast<const libsiedler2::ArchivItem_Map*>(hdr[0]);
    if(!map)
    {
        std::cout << "MAPHDR {\"map\":\"" << rel << "\",\"error\":2}" << std::endl;
        return;
    }
    const auto& h = map->getHeader();
    const unsigned w = h.getWidth(), ht = h.getHeight(), np = h.getNumPlayers();
    const unsigned areaPer = np ? (w * ht / np) : 0;
    std::cout << "MAPHDR {\"map\":\"" << rel << "\",\"w\":" << w << ",\"h\":" << ht << ",\"players\":" << np
              << ",\"areaPerPlayer\":" << areaPer << "}" << std::endl;
}

// Eine Partie: advSide = welcher Spieler die AdvancedAI ist. Rest = AIPlayerJH.
// Gibt die Stärke-Differenz (adv - bester Gegner) zurück; schreibt eine RESULT-Zeile.
double runMatch(const std::string& mapRel, unsigned numPlayers, unsigned advSide, double& outParity)
{
    std::vector<PlayerInfo> players(numPlayers);
    for(auto& p : players)
        p.ps = PlayerState::Occupied;
    GlobalGameSettings ggs;
    // Optional das "Military Control"-Addon aktivieren (0=keins,1=minimal,2=voll),
    // um die per-Gebäude-Truppensteuerung der KI zu testen (Default: keins).
    if(const char* mc = std::getenv("RTTR_MIL_CONTROL"))
        ggs.setSelection(AddonId::MILITARY_CONTROL, static_cast<unsigned>(std::atoi(mc)));
    if(std::getenv("RTTR_TOOL_ORDERING"))
        ggs.setSelection(AddonId::TOOL_ORDERING, 1); // Direkt-Bestellung von Werkzeug
    if(std::getenv("RTTR_ROAD_ENLARGE"))
        ggs.setSelection(AddonId::MANUAL_ROAD_ENLARGEMENT, 1); // manuelle Wege-Aufwertung

    auto game = std::make_shared<Game>(ggs, std::make_unique<TestEventManager>(), players);
    GameWorld& world = game->world_;
    TestEventManager& em = static_cast<TestEventManager&>(*game->em_);

    MapLoader loader(world);
    if(!loader.Load(rttr::test::rttrBaseDir / mapRel))
    {
        std::cout << "SKIP " << mapRel << " (load failed)" << std::endl;
        outParity = 0;
        return 0;
    }
    world.InitAfterLoad();

    // GF-Horizont ~ Distanz: sqrt(Flaeche / Spielerzahl). Override via RTTR_MATCH_MAXGF.
    const auto size = world.GetSize();
    const double dist = std::sqrt(static_cast<double>(size.x) * size.y / std::max(1u, numPlayers));
    unsigned maxGF = envU("RTTR_MATCH_MAXGF", 0);
    if(maxGF == 0)
        maxGF = static_cast<unsigned>(std::min(20000.0, std::max(12000.0, 4000.0 + 110.0 * dist)));

    std::vector<std::unique_ptr<AIPlayer>> ais(numPlayers);
    for(unsigned p = 0; p < numPlayers; ++p)
        ais[p] = makeAI(p == advSide, static_cast<unsigned char>(p), world);

    unsigned endGF = 0;
    // Korrekte Reihenfolge: Welt fortschreiben -> KI denkt -> ihre Befehle werden
    // am Netzwerk-Frame SOFORT ausgeführt. So plant die KI nie gegen einen
    // veralteten Zustand (frühere Reihenfolge führte zu Geisterfahnen/Wegfehlern).
    constexpr unsigned nwfLen = 5;
    for(unsigned gf = 0; gf < maxGF; ++gf)
    {
        em.ExecuteNextGF();
        const bool isNWF = (gf % nwfLen == 0);
        for(unsigned p = 0; p < numPlayers; ++p)
            ais[p]->RunGF(gf, isNWF);
        if(isNWF)
        {
            for(unsigned p = 0; p < numPlayers; ++p)
            {
                auto cmds = ais[p]->FetchGameCommands();
                for(auto& gc : cmds)
                    gc->Execute(world, static_cast<unsigned char>(p));
            }
        }
        endGF = gf;
        if(world.GetPlayer(advSide).IsDefeated())
            break;
    }

    // --- Militär-Besetzungsdiagnose für unseren Spieler ---
    {
        const GamePlayer& me = world.GetPlayer(advSide);
        const BuildingRegister& br = me.GetBuildingRegister();
        unsigned milBld = 0, troops = 0, want = 0, newBuilt = 0, sites = 0;
        for(const nobMilitary* mb : br.GetMilitaryBuildings())
        {
            ++milBld;
            troops += mb->GetNumTroops();
            want += mb->CalcRequiredNumTroops();
            if(mb->IsNewBuilt())
                ++newBuilt;
        }
        unsigned allSites = 0;
        for(const noBuildingSite* bs : br.GetBuildingSites())
        {
            ++allSites;
            if(BuildingProperties::IsMilitary(bs->GetBuildingType()))
                ++sites;
        }
        std::cout << "MILDBG {\"map\":\"" << mapRel << "\",\"side\":" << advSide << ",\"milBld\":" << milBld
                  << ",\"troops\":" << troops << ",\"want\":" << want << ",\"newBuilt\":" << newBuilt
                  << ",\"milSites\":" << sites << ",\"allSites\":" << allSites
                  << ",\"invSoldiers\":" << gather(me).soldiers << "}" << std::endl;

        // --- Wegenetz-Diagnose: Gebäude/​Baustellen OHNE jegliche Straße an der
        //     Fahne (= "Waisen": nicht angebunden). Sollte ~0 sein. ---
        auto flagHasNoRoad = [&](MapPoint bldPos) {
            const noFlag* f = world.GetSpecObj<noFlag>(world.GetNeighbour(bldPos, Direction::SouthEast));
            if(!f)
                return true;
            for(const auto* r : f->getRoutes())
                if(r)
                    return false;
            return true;
        };
        unsigned orphanBld = 0, orphanSites = 0;
        for(unsigned i = 0; i < 40; ++i)
        {
            const BuildingType bt = BuildingType(i);
            if(BuildingProperties::IsMilitary(bt) || BuildingProperties::IsWareHouse(bt))
                continue;
            for(const nobUsual* u : br.GetBuildings(bt))
                if(flagHasNoRoad(u->GetPos()))
                    ++orphanBld;
        }
        for(const nobMilitary* mb : br.GetMilitaryBuildings())
            if(flagHasNoRoad(mb->GetPos()))
                ++orphanBld;
        for(const noBuildingSite* bs : br.GetBuildingSites())
            if(flagHasNoRoad(bs->GetPos()))
                ++orphanSites;
        // Echte WAISENFLAGGEN: eigene Fahnen OHNE jegliche Straße (kein Nutzen),
        // die KEINEM Gebäude gehören (Gebäudefahnen oben separat gezählt).
        unsigned orphanFlags = 0;
        {
            const auto sz = world.GetSize();
            for(unsigned y = 0; y < static_cast<unsigned>(sz.y); ++y)
                for(unsigned x = 0; x < static_cast<unsigned>(sz.x); ++x)
                {
                    const MapPoint fp(x, y);
                    const noFlag* f = world.GetSpecObj<noFlag>(fp);
                    if(!f || f->GetPlayer() != static_cast<unsigned char>(advSide))
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
                    // Fahne ohne Straße. Gehört sie zu einem Gebäude (NW)? Dann ist
                    // es eine (separat gezählte) Gebäudewaise, sonst eine reine
                    // nutzlose Fahne.
                    const MapPoint nw = world.GetNeighbour(fp, Direction::NorthWest);
                    if(!world.GetSpecObj<noBuilding>(nw) && !world.GetSpecObj<noBuildingSite>(nw))
                        ++orphanFlags;
                }
        }
        std::cout << "ORPHANS {\"map\":\"" << mapRel << "\",\"side\":" << advSide << ",\"orphanBld\":" << orphanBld
                  << ",\"orphanSites\":" << orphanSites << ",\"orphanFlags\":" << orphanFlags << "}" << std::endl;

        // --- Minen-Diagnose (skaliert die Minenzahl mit Nahrung/Vorkommen?) ---
        std::cout << "MINES {\"map\":\"" << mapRel << "\",\"side\":" << advSide
                  << ",\"coal\":" << br.GetBuildings(BuildingType::CoalMine).size()
                  << ",\"iron\":" << br.GetBuildings(BuildingType::IronMine).size()
                  << ",\"gold\":" << br.GetBuildings(BuildingType::GoldMine).size()
                  << ",\"granite\":" << br.GetBuildings(BuildingType::GraniteMine).size()
                  << ",\"mint\":" << br.GetBuildings(BuildingType::Mint).size()
                  << ",\"donkey\":" << br.GetBuildings(BuildingType::DonkeyBreeder).size()
                  << ",\"wood\":" << br.GetBuildings(BuildingType::Woodcutter).size()
                  << ",\"forester\":" << br.GetBuildings(BuildingType::Forester).size()
                  << ",\"farm\":" << br.GetBuildings(BuildingType::Farm).size()
                  << ",\"mintProd\":" << (br.GetBuildings(BuildingType::Mint).empty() ? -1 :
                                          br.GetBuildings(BuildingType::Mint).front()->GetProductivity())
                  << "}" << std::endl;

        // --- Richtungs-Verteilung der Militärgebäude (8 Sektoren um HQ) ---
        // Deckt Expansions-Bias auf (z.B. nur links/oben).
        {
            const auto sz = world.GetSize();
            // Sektor 0=W,1=NW,2=N,3=NE,4=E,5=SE,6=S,7=SW (atan2 + pi, 8 Bins).
            auto sectorsFor = [&](const char* who, unsigned pid) {
                const GamePlayer& pl = world.GetPlayer(pid);
                const MapPoint hq = pl.GetHQPos();
                int sec[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for(const nobMilitary* mb : pl.GetBuildingRegister().GetMilitaryBuildings())
                {
                    int dx = int(mb->GetPos().x) - int(hq.x), dy = int(mb->GetPos().y) - int(hq.y);
                    if(dx > sz.x / 2)
                        dx -= sz.x;
                    else if(dx < -sz.x / 2)
                        dx += sz.x;
                    if(dy > sz.y / 2)
                        dy -= sz.y;
                    else if(dy < -sz.y / 2)
                        dy += sz.y;
                    if(dx == 0 && dy == 0)
                        continue;
                    double ang = std::atan2(double(dy), double(dx)) + 3.14159265358979;
                    int s = int(ang / (2 * 3.14159265358979) * 8.0);
                    sec[s < 0 ? 0 : (s > 7 ? 7 : s)]++;
                }
                std::cout << "MILSECTORS {\"map\":\"" << mapRel << "\",\"who\":\"" << who << "\",\"W\":" << sec[0]
                          << ",\"NW\":" << sec[1] << ",\"N\":" << sec[2] << ",\"NE\":" << sec[3] << ",\"E\":" << sec[4]
                          << ",\"SE\":" << sec[5] << ",\"S\":" << sec[6] << ",\"SW\":" << sec[7] << "}" << std::endl;
            };
            sectorsFor("adv", advSide);
            sectorsFor("jh", advSide == 0 ? 1 : 0);
        }

        // --- Wegenetz-Effizienz: Gesamt-Straßenlänge je Spieler (Vergleich mit
        //     JH). Niedriger bei gleicher Gebäudezahl = effizienteres Netz. ---
        auto totalRoadLen = [&](unsigned char pid) {
            unsigned len = 0;
            const auto sz = world.GetSize();
            for(unsigned y = 0; y < static_cast<unsigned>(sz.y); ++y)
                for(unsigned x = 0; x < static_cast<unsigned>(sz.x); ++x)
                {
                    const noFlag* f = world.GetSpecObj<noFlag>(MapPoint(x, y));
                    if(!f || f->GetPlayer() != pid)
                        continue;
                    for(const auto* r : f->getRoutes())
                        if(r)
                            len += r->GetLength();
                }
            return len / 2; // jedes Segment von beiden Enden gezählt
        };
        const unsigned advRoad = totalRoadLen(static_cast<unsigned char>(advSide));
        const Stats advS = gather(world.GetPlayer(advSide));
        const unsigned advBld = advS.mil + advS.econ + advS.stores;
        const unsigned char oppId = advSide == 0 ? 1 : 0;
        const unsigned oppRoad = totalRoadLen(oppId);
        const Stats oppS = gather(world.GetPlayer(oppId));
        const unsigned oppBld = oppS.mil + oppS.econ + oppS.stores;
        std::cout << "ROADLEN {\"map\":\"" << mapRel << "\",\"side\":" << advSide << ",\"advRoad\":" << advRoad
                  << ",\"advBld\":" << advBld << ",\"advPerBld\":" << (advBld ? double(advRoad) / advBld : 0)
                  << ",\"oppRoad\":" << oppRoad << ",\"oppBld\":" << oppBld
                  << ",\"oppPerBld\":" << (oppBld ? double(oppRoad) / oppBld : 0) << "}" << std::endl;
    }

    const Stats adv = gather(world.GetPlayer(advSide));
    const Quality advQ = computeQuality(world, world.GetPlayer(advSide));

    // Bester Gegner – nach AUSBAU-QUALITÄT (nicht mehr nur Gebäudezahl).
    Stats bestOpp;
    Quality bestOppQ;
    double bestOppTotal = -1e9;
    for(unsigned p = 0; p < numPlayers; ++p)
    {
        if(p == advSide)
            continue;
        const Quality oq = computeQuality(world, world.GetPlayer(p));
        if(oq.total() > bestOppTotal)
        {
            bestOppTotal = oq.total();
            bestOppQ = oq;
            bestOpp = gather(world.GetPlayer(p));
        }
    }

    // Rohstatistik (Referenz) ...
    std::cout << "RESULT {\"map\":\"" << mapRel << "\",\"players\":" << numPlayers << ",\"side\":" << advSide
              << ",\"endgf\":" << endGF << ",";
    printStats("adv", adv, true);
    printStats("opp", bestOpp, false);
    std::cout << "}" << std::endl;

    // ... und die umfassende Ausbau-Qualität (Basis der Bewertung).
    std::cout << "QUALITY {\"map\":\"" << mapRel << "\",\"side\":" << advSide << ",";
    printQuality("adv", advQ, true);
    printQuality("opp", bestOppQ, false);
    std::cout << "}" << std::endl;

    outParity = advQ.total() - bestOppTotal;
    return outParity;
}
} // namespace

BOOST_AUTO_TEST_SUITE(AdvAI)

BOOST_AUTO_TEST_CASE(SelfPlaySuite)
{
    // Karten-Suite (verschiedene Größen/Spielerzahlen). Override via RTTR_MATCH_MAPS
    // (semikolon-getrennt). Spielerzahl wird automatisch erkannt.
    std::vector<std::string> maps = {
      "data/RTTR/MAPS/NEW/TueranTuer.swd",
      "data/RTTR/MAPS/OTHER/Bergschlumpf.swd",
      "data/RTTR/MAPS/NEW/DoppeltBedroht.swd",
      "data/RTTR/MAPS/NEW/dreamland.swd",
    };
    if(const char* env = std::getenv("RTTR_MATCH_MAPS"))
    {
        maps.clear();
        std::string s(env), cur;
        for(char c : s)
        {
            if(c == ';')
            {
                if(!cur.empty())
                    maps.push_back(cur);
                cur.clear();
            } else
                cur += c;
        }
        if(!cur.empty())
            maps.push_back(cur);
    }

    // Header-Scan-Modus: nur Kartengrößen ausgeben, keine Partien laufen lassen.
    if(std::getenv("RTTR_MAP_SCAN"))
    {
        for(const std::string& m : maps)
            printMapHeader(m, rttr::test::rttrBaseDir / m);
        BOOST_TEST(true);
        return;
    }

    double paritySum = 0;
    int n = 0;
    for(const std::string& m : maps)
    {
        const unsigned np = readNumPlayers(rttr::test::rttrBaseDir / m);
        if(np < 2)
        {
            std::cout << "SKIP " << m << " (numPlayers=" << np << ")" << std::endl;
            continue;
        }
        // 2 Spieler: beide Seiten spielen (Bias entfernen); sonst nur P0.
        const unsigned sides = (np == 2) ? 2u : 1u;
        for(unsigned side = 0; side < sides; ++side)
        {
            double parity = 0;
            runMatch(m, np, side, parity);
            paritySum += parity;
            ++n;
        }
    }
    const double avgParity = n ? paritySum / n : 0;
    std::cout << "SUMMARY {\"matches\":" << n << ",\"avg_parity\":" << avgParity << "}" << std::endl;
    // Parität: >=0 bedeutet im Schnitt mindestens so stark wie der beste JH.
    BOOST_TEST_MESSAGE("avg_parity=" << avgParity);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()
