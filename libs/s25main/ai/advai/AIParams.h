// AIParams – extern konfigurierbare Tuning-Parameter der KI.
//
// Werden einmalig aus der Datei im Umgebungs­variablen-Pfad RTTR_AI_PARAMS
// geladen (Format: key=value je Zeile, '#'=Kommentar). Fehlt die Datei, gelten
// die Defaults. So kann der Self-Play-Optimierer (siehe harness/) die KI
// tunen, ohne neu zu kompilieren.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace advai {

struct AIParams
{
    // Takte der deliberativen Planer (Game-Frames)
    // HINWEIS: Diese Defaults stammen aus der automatischen Self-Play-Optimierung
    // (harness/optimize_selfplay.py), BALANCIERTER Lauf (Kampf + Wirtschaft).
    // Ergebnis ggü. Ausgangsstand (~-7 Kampf / -64 ALASKA40k): Kampf ~-3,5 UND
    // ALASKA40k ~-22,6 (großer Wirtschaftsgewinn bei kleinem Kampf-Verlust).
    // Ein rein kampf-optimierter Satz liegt in harness/best.aiparams (Kampf ~0,
    // aber Wirtschaft schwach) – via RTTR_AI_PARAMS als "Rush"-Profil nutzbar.
    unsigned econInterval = 93;
    unsigned expandInterval = 98;
    unsigned militaryInterval = 422;
    unsigned scoutInterval = 669;
    unsigned seaInterval = 600;       // (nicht optimiert)
    unsigned settingsInterval = 855;
    unsigned roadOptInterval = 672;

    // Expansion / Militär
    int expandPerTick = 4;       // Militärbauten je Expansions-Takt (optimiert)
    unsigned milSpacing = 5;     // optimiert
    int attackMinSoldiers = 8;   // optimiert: höhere Angriffsschwelle (weniger riskant)
    int attackFractionPct = 33;  // optimiert: kleinere Angriffswellen

    // Platzierungs-Scoring
    int placeResourceWeight = 15; // optimiert
    int placeDistanceBase = 253;  // optimiert

    // Militär-Schieberegler (8 Werte). Maxima lt. MILITARY_SETTINGS_SCALE:
    // {10,5,5,5,8,8,8,8}. Alle 8 optimiert; [4]=Inland klein (Auslagerung an Front),
    // [7]=Grenze voll. [0]Rekrut [1]VerteidStärke [2]aktiveVert [3]Angriff
    // [4]Inland [5]Mittel [6]Hafen [7]Grenze
    std::array<int, 8> milSettings = {{8, 4, 3, 8, 5, 3, 7, 8}};

    /// Einmalig (lazy) geladene Instanz.
    static const AIParams& get()
    {
        static const AIParams inst = load();
        return inst;
    }

private:
    static AIParams load()
    {
        AIParams p;
        const char* path = std::getenv("RTTR_AI_PARAMS");
        if(!path)
            return p;
        std::ifstream f(path);
        if(!f)
            return p;

        std::unordered_map<std::string, std::string> kv;
        std::string line;
        auto trim = [](std::string& s) {
            while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.erase(s.begin());
            while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.pop_back();
        };
        while(std::getline(f, line))
        {
            if(line.empty() || line[0] == '#')
                continue;
            const auto pos = line.find('=');
            if(pos == std::string::npos)
                continue;
            std::string k = line.substr(0, pos);
            std::string v = line.substr(pos + 1);
            trim(k);
            trim(v);
            if(!k.empty())
                kv[k] = v;
        }

        auto getU = [&](const char* key, unsigned& dst) {
            auto it = kv.find(key);
            if(it == kv.end())
                return;
            try
            {
                const unsigned long val = std::stoul(it->second);
                if(val > 0)
                    dst = static_cast<unsigned>(val);
            } catch(...)
            {}
        };
        auto getI = [&](const char* key, int& dst) {
            auto it = kv.find(key);
            if(it == kv.end())
                return;
            try
            {
                dst = std::stoi(it->second);
            } catch(...)
            {}
        };

        getU("econInterval", p.econInterval);
        getU("expandInterval", p.expandInterval);
        getU("militaryInterval", p.militaryInterval);
        getU("scoutInterval", p.scoutInterval);
        getU("seaInterval", p.seaInterval);
        getU("settingsInterval", p.settingsInterval);
        getU("roadOptInterval", p.roadOptInterval);
        getU("milSpacing", p.milSpacing);
        getI("expandPerTick", p.expandPerTick);
        getI("attackMinSoldiers", p.attackMinSoldiers);
        getI("attackFractionPct", p.attackFractionPct);
        getI("placeResourceWeight", p.placeResourceWeight);
        getI("placeDistanceBase", p.placeDistanceBase);
        for(int i = 0; i < 8; ++i)
            getI(("mil" + std::to_string(i)).c_str(), p.milSettings[i]);
        return p;
    }
};

} // namespace advai
