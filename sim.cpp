#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <deque>
#include <string>
#include <limits>
#include <unordered_map>
#include <array>

using namespace sf;
using namespace std;

// R45-F1a Far Reaches: the map grew from 800 to 1200 px a side. Every count tuned on the old map scales with area so
// density (and so walk distance between jobs) holds; the window stays 800 px and shows the world 1:1 at zoom 1.
constexpr float baseWorldSize = 800.0f;     // map side the density tunables were tuned on
constexpr float worldSize = 2400.0f;        // map side in world px (R47-F1: was 1200)
constexpr float worldAreaScale = (worldSize / baseWorldSize) * (worldSize / baseWorldSize); // 9
constexpr float windowSize = 800.0f;        // window side in px; at zoom 1 the camera spans this many world px
constexpr float mapEdgeInset = 20.0f;       // edge arrivals (trader, deer) keep this far from the corners
constexpr int treeSpawnOneIn = (int)(100000 / worldAreaScale); // 1-in-N per tick for a new tree: same rate per area as before
// R47-F1 The Wilds: three bands, looked up by zoneAt(pos). The safe core is a disc round villageCenter (it wins where it
// overlaps the rim), the sparse rocky edge is a rim zoneEdgeBandPx wide along the map border, and the wild ring is the
// rest. Startup trees/rocks are bucketed per band (densities per 800x800 of map, so they read like the old tunables);
// new trees pick a band by its share of the startup trees, and the core stops taking new trees at zoneCoreTreeCap.
enum zoneId { zoneCore, zoneWild, zoneEdge, zoneCount };
const char* const zoneNames[zoneCount] = {"core", "wild", "edge"};
const float zoneCoreRadius = 420.0f;        // safe core: px from villageCenter (quarters reach ~313, so they sit inside)
const float zoneEdgeBandPx = 320.0f;        // rocky edge: px in from each map border
const float zoneTreeDensity[zoneCount] = {1500.0f, 2200.0f, 400.0f};   // trees per 800x800 (old map: 2000 everywhere)
const float zoneRockDensity[zoneCount] = {100.0f, 60.0f, 200.0f};      // rocks per 800x800 (old map: 100 everywhere)
const int zoneCoreTreeCap = 1400;           // core never holds more than this (startup is clamped to it too)
const float zoneAreaSamplePx = 8.0f;        // band areas are measured once at startup on this grid
const int zoneRollTries = 40;               // position rolls per tree/rock before giving up on that one
// Ground tint: one translucent quad grid, vertex colours blended across each boundary so there are no hard lines, plus a
// +-1 alpha hash jitter per vertex (dither) so the ~6% wash has no visible banding
const float zoneTintTilePx = 16.0f;         // tint grid step (world px); only the rows/columns in view are drawn
const float zoneTintFeatherPx = 72.0f;      // blend half-width either side of a band boundary
const Color zoneTintColors[zoneCount] = {
    Color(196, 180, 120, 14),               // core: warm straw, a touch of settled ground
    Color(40, 92, 44, 16),                  // wild: deeper moss green, lusher than the base grass
    Color(150, 146, 136, 16),               // edge: pale stone grey for the rocky rim
};
const int zoneTintJitter = 1;               // +- alpha per vertex
// Tree spatial grid: work-spot buckets so the per-tick tree searches only look at nearby cells
const float treeGridCellPx = 64.0f;
const int treeGridN = (int)(worldSize / treeGridCellPx) + 1;   // +1: work spots sit up to 15px past a tree near the far edge
// Wolves walk in from (and leave by) the wild band's outer edge, at least this far from the core
const float wolfCoreMarginPx = 80.0f;
const int wolfSpawnTries = 12;
constexpr float deerEntryDepthPx = 80.0f;   // arriving animals walk this far in past mapEdgeInset from their side edge
constexpr float deerOffMapPx = 20.0f;       // arriving animals start this far outside the side edge
// Gold Veins (R46-F1a): a few rocks are gold-flecked; mining one also adds goldPerRock to the global gold count.
// The flag lives in rockGold, parallel to rocks/octs. Draw: warmer diamond plus one pale-gold fleck pixel.
const float goldRockChance = 0.08f;         // per spawned rock
const int goldPerRock = 1;                  // nuggets per gold rock, on top of its normal stone
const Color goldRockColor(136, 128, 108);   // rock diamond (128,126,118) nudged warm, still clearly a rock
const Color goldFleckColor(212, 188, 118);  // muted pale gold, under the gentry sash (224,194,96) in saturation
const Vector2f goldFleckOffset(5.0f, 4.0f); // 1px fleck on the upper-left (lit) face of the 12px diamond
const Color hudGoldColor(212, 188, 118);    // HUD "gold n" text, same pale gold as the fleck
// Gilded Keep (R46-F1b): once a race's keep stands and the global gold reaches keepGildCost, it is spent once to gild
// that keep's gate: a 1px gilt frame (jambs, shoulders, keystone) round the dark arched door. One ticker line per gilding.
const int keepGildCost = 6;                 // nuggets per keep (~6 gold rocks)
const Color keepGildColor(228, 196, 104);   // a touch brighter than the fleck so the frame reads on the slate wall

const char* const hudFontPath = "assets/DejaVuSans.ttf";
const unsigned int hudFontSize = 14;
// Icon HUD strip: full-width top bar of 8x8 pixel icons (drawn at 2x) + numbers; hovering the bar shows the full text
const float hudBarHeight = 24.0f;           // includes the 1px rim along the bottom
const float hudRimPx = 1.0f;
const int hudIconGrid = 8;                  // icon patterns are 8x8 cells
const float hudIconPx = 2.0f;               // screen px per icon cell
const float hudIconSize = hudIconGrid * hudIconPx; // 16px icon, ~4px margin inside the bar
const float hudPadX = 8.0f;                 // left margin before the first icon
const float hudIconGap = 4.0f;              // icon -> number
const float hudCellGap = 14.0f;             // number -> next icon
const char* const hudMinNumber = "000";     // each number reserves at least this width so cells don't jump as values grow
const int maxTicksPerFrame = 20;
const float minZoom = 1.0f;
const float maxZoom = 4.0f;
const float zoomStep = 1.1f;
const int ticksPerMinute = 3600;            // one real minute at 1x (60 ticks/s)
const int realMinutesPerGameHour = 10;      // the one day-length knob: real minutes (at 1x) per in-game hour (4 real hours per day)
const int ticksPerGameHour = realMinutesPerGameHour * ticksPerMinute; // clockk() advances game_time by one hour after this many ticks
const float clearRadiusSq = 50.0f * 50.0f;  // trees/rocks within 50px of the village centre (villageCenter) are cleared
const Color stumpColor(115, 30, 2);         // every tree stump, startup and respawned

const Color barbIdleColor(255, 255, 255);
const Color barbChopColor(120, 220, 90);
const Color barbMineColor(255, 165, 0);
const Color barbBuildColor(80, 170, 255);
const Color barbHuntColor(255, 70, 70);

// Barb sprite (drawn over the 2x2 hitbox square; feet line up with the square's bottom edge)
const Vector2f barbBodySize(2.0f, 4.0f);    // plain white rectangle, no head/hair (same 2x4 footprint as before)
const Color barbBodyColor(226, 224, 214);   // soft off-white: bright on dark green without glaring
const float barbStripeTop = 1.0f;           // task stripe row, measured from the top of the body
const float barbStripeH = 1.0f;             // 2x1 task-coloured band; idle barbs have none
const Vector2f barbToolSize(1.0f, 1.0f);    // tool tick / arm pixel beside the body, tinted with the task colour
const int barbBobFrames = 5;                // walking bobs 1px up every this many frames
const float barbBobPx = 1.0f;
const int barbToolFrames = 6;               // chop/mine/build swing: tool raised then struck
const int barbPickFrames = 10;              // picking: arm dips to the bush on alternate periods
const float barbHungryDroopPx = 1.0f;       // hungry barbs slouch: body 1px shorter
// Founders' line (R24-F1): founders (homeHouse < 0) are Gentry, everyone born in a house is a Commoner
const float gentrySashTop = 2.0f;           // sash row from the top of the body, one below the task stripe
const float gentrySashH = 1.0f;             // 2x1 band, body-wide
const Color gentrySashColor(224, 194, 96);  // pale gold: not hungry tan, not mine orange, never hunger-tinted
const int gentryVoteWeight = 2;             // a Gentry ballot counts this many times at the dawn council
const int gentryFoodReserve = 1;            // while any Commoner is hungry, Gentry eating first leave this much food for them
const Color barbSkinColor(196, 158, 122);   // muted tan (HUD people icon)

// Kin lines: every barb rolls a race at birth. Body colour stays shared so task stripes read the same;
// races differ by body size (which is also the hitbox) and a hat mark drawn above the head.
const int rockMineTicks = 480;              // base work ticks to mine one rock
enum barbRace { raceHuman, raceDwarf, raceElf, raceCount };
// Kin clans (R28-F1): the colony starts with an even clan of founders per race
const int foundersPerRace = 5;
const int numbarbs = foundersPerRace * raceCount;   // founder i is race i / foundersPerRace
struct raceDef {
    const char* name;      // capitalised, for the tooltip
    const char* birthName; // with article, for the birth log line ("An elf is born")
    Vector2f bodySize;     // drawn body and hitbox (width x height)
    Vector2f hatSize;      // 0x0 = no hat; sits on top of the body
    Vector2f hatTipSize;   // optional point on top of the hat, left-aligned
    Color hatColor;
    float mineSpeed;       // rock-mining rate multiplier (dwarf edge: 480 -> 320 ticks per rock)
};
constexpr raceDef raceDefs[raceCount] = {
    {"Human", "A human", {2.0f, 4.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, Color(0, 0, 0),       1.0f},
    {"Dwarf", "A dwarf", {3.0f, 3.0f}, {3.0f, 1.0f}, {0.0f, 0.0f}, Color(150, 92, 58),   1.5f}, // stocky, rust-brown cap
    {"Elf",   "An elf",  {2.0f, 5.0f}, {2.0f, 1.0f}, {1.0f, 1.0f}, Color(170, 196, 150), 1.0f}, // tall, pale sage pointed hat
};

// Race looks (R35-F1): fantasy silhouettes as 1px marks around the body; draw-only, batched into one VertexArray.
// Marks sit beside/above the body (or on the face row) so the task stripe stays the dominant body tint.
// Offsets: x from the body's left edge (or right edge when fromRight), y from bodyTop; toFeet stretches h to the feet.
const int raceLookMaxMarks = 4;
constexpr Color dwarfHornColor(196, 186, 156);      // muted bone, horns on the helm
constexpr Color dwarfBeardColor(132, 92, 60);       // braided auburn beard on the face row
constexpr Color dwarfGlintColor(222, 228, 236);     // brief steel glint above a raised tool
const int dwarfGlintEvery = 4;                  // glint on one raised swing in this many
constexpr Color elfEarColor(196, 158, 122);     // pointed ears, same tan as barbSkinColor
constexpr Color elfCloakColor(96, 124, 78);         // muted leaf green cloak down the back (left side)
constexpr Color elfCloakRainColor(66, 88, 56);      // darker, soaked while it rains
struct lookMark {
    float x, y, w, h;
    bool fromRight;   // x measured from the body's right edge
    bool toFeet;      // h is ignored; the mark runs from y to the bottom of the body
    bool rainDarkens; // swap to the race's rain colour while raining
    Color color;
};
struct raceLook {
    int markCount;
    lookMark marks[raceLookMaxMarks];
    Color rainColor;
    bool axeGlint;    // steel glint above a raised tool while working
};
constexpr raceLook raceLooks[raceCount] = {
    // Human: today's look, no extra marks
    {0, {}, Color(0, 0, 0), false},
    // Dwarf: horned helm (horns flank the cap, clear of the crown), 1px beard on the face row, axe glint
    {3, {{-1.0f, -2.0f, 1.0f, 1.0f, false, false, false, dwarfHornColor},
         { 0.0f, -2.0f, 1.0f, 1.0f, true,  false, false, dwarfHornColor},
         { 1.0f,  0.0f, 1.0f, 1.0f, false, false, false, dwarfBeardColor}},
     Color(0, 0, 0), true},
    // Elf: pointed ears on the head row, leaf cloak down the back below the trait dot
    {3, {{-1.0f,  0.0f, 1.0f, 1.0f, false, false, false, elfEarColor},
         { 0.0f,  0.0f, 1.0f, 1.0f, true,  false, false, elfEarColor},
         {-1.0f,  2.0f, 1.0f, 0.0f, false, true,  true,  elfCloakColor}},
     elfCloakRainColor, false},
};

// Names and traits (R25-F1): rolled once in makeBarb(). Names are unique until the table runs out.
const char* const barbNames[] = {
    "Mika", "Tove", "Arno", "Brin", "Cora", "Dain", "Edda", "Finn", "Gala", "Hale",
    "Ilse", "Joss", "Kira", "Lars", "Mara", "Nils", "Oona", "Pell", "Quin", "Runa",
    "Sven", "Tilda", "Ulf", "Vera", "Wren", "Yara", "Bram", "Esme", "Gorm", "Liv",
    "Odo", "Rhea", "Sigi", "Tam", "Ada", "Bo", "Eir", "Hugo", "Ivo", "Nell",
};
const int barbNameCount = (int)(sizeof(barbNames) / sizeof(barbNames[0]));
bool barbNameUsed[barbNameCount] = {};

enum barbTrait { traitSwift, traitSturdy, traitGlutton, traitForager, traitCount };
struct traitDef {
    const char* name;      // capitalised, for the tooltip and the birth line ("Mika the Swift")
    Color dotColor;        // 1px badge left of the body; kept clear of every task stripe colour
};
constexpr traitDef traitDefs[traitCount] = {
    {"Swift",   Color(215, 245, 245)},   // cyan-white: far paler than fishing cyan (90,225,225)
    {"Sturdy",  Color(160, 72, 52)},     // brick: darker and browner than hunt red (255,70,70)
    {"Glutton", Color(190, 150, 30)},    // mustard: darker than harvest gold and the gentry sash
    {"Forager", Color(200, 170, 225)},   // lilac: paler than the trader (190,140,230), unlike pick pink
};
const float swiftStepSpeed = 1.3f;       // Swift barbs step this many times faster (step interval / 1.3, min 1)
const float sturdyHungerScale = 1.5f;    // Sturdy barbs go 1.5x longer between meals
const float gluttonHungerScale = 0.7f;   // Gluttons get hungry sooner...
const int gluttonMealFood = 2;           // ...and eat this much per meal when the larder allows (never below 0 / the reserve)
const int foragerFoodBonus = 1;          // extra food when a Forager picks berries or reaps the field
const float traitDotMinZoom = 2.0f;      // hidden below this zoom: a 1px dot is noise at 1x

// Elections (R28-F2): at dawn a race with enough members and no leader crowns one; >50% approval or retry next dawn
const int electionThreshold = 10;        // a race needs at least this many living members to hold an election
const char* const racePlurals[raceCount] = {"Humans", "Dwarves", "Elves"};   // ticker subject, indexed by barbRace
// Chance a voter of this race approves a candidate with this trait; the candidate always approves itself
const float raceTraitLike[raceCount][traitCount] = {
    // Swift Sturdy Glutton Forager
    {0.50f, 0.55f, 0.30f, 0.75f},   // humans prize foragers
    {0.35f, 0.75f, 0.55f, 0.45f},   // dwarves prize the sturdy
    {0.75f, 0.40f, 0.25f, 0.60f},   // elves prize the swift
};
const float crownMinZoom = 1.0f;          // the crown shows at every zoom (minZoom is 1)
const Vector2f crownSize(2.0f, 1.0f);     // 2x1 band sitting on the head (or on top of the hat)
const Color crownColor(250, 208, 64);     // bright gold: off the body, so it never meets a task stripe or the sash
// Leader shortage weights (R30-F2): at dawn a race's leader names its top shortage; that race's matching job weight tilts
enum leaderNeed { needWood, needStone, needFood, needCount, needNone = needCount };
const char* const leaderNeedNames[needCount] = {"wood", "stone", "food"};
const float leaderNeedMult = 1.25f;       // matching weight multiplier, re-clamped to [needWeightMin, needWeightMax]
// Leader orders (INBOX-F2b): at dawn the leader hands its need to idle same-race barbs as individual orders
const float orderRaceShare = 0.6f;        // at most this share of the race (leader included in the count) holds an order
const int orderDays = 1;                  // an order expires after this many in-game days (counted in ticks)
const float orderPipMinZoom = 3.0f;       // pips show for every ordered barb at/above this zoom; below it, only on hover
const Vector2f orderPipSize(2.0f, 1.0f);  // 2x1 tick centred under the feet (below the health bar when that shows)

// Sasuke the Shinobi (R25-F2): the first founder; fast, and blinks a few px when idle
const char* const shinobiName = "Sasuke";      // not in barbNames, so nobody else can roll it
bool sasukeTaken = false;                      // set once in makeBarb() when the first founder takes the name
const float shinobiStepSpeed = 2.0f;           // steps this many times faster (interval / 2, min 1), on top of Swift
const Color shinobiBodyColor(150, 196, 236);   // light blue: paler than build blue (80,170,255), bright on dark green
const Color shinobiOutlineColor(225, 238, 250, 110); // pale, translucent 1px rim so the 2px body reads without glaring
const Color shinobiBandColor(40, 52, 96);      // dark navy headband across the top body row
const int shinobiBlinkOneIn = 600;             // 1-in-N per idle tick (~10s of idling between blinks)
const int shinobiBlinkMinPx = 3;               // blink hop per axis: up to max, total at least min
const int shinobiBlinkMaxPx = 8;
const int shinobiBlinkTries = 8;               // random spots tried per blink; no free spot = no blink
const int shinobiPuffFrames = 20;              // puff left at the old spot fades out over this many frames
const Vector2f shinobiPuffSize(3.0f, 3.0f);
const Color shinobiPuffColor(170, 172, 176);   // soft grey
const uint8_t shinobiPuffMaxAlpha = 90;        // subtle: faint at the start, linear fade to 0

// Race stockpiles (RACE-F1): every race keeps its own wood/stone/food; deposits, meals, births and builds use the barb's
// race. Only the campfire draws on one explicit shared pool (campfireWood), fed by a tithe on wood deposits.
struct raceStock { int wood = 0, stone = 0, food = 0; };
constexpr raceStock startStock{8, 2, 4};       // seed and startup floor per race (stone = the shortage threshold, so no
                                               // shortage lines fire at launch); spend() never takes a stock below 0
raceStock stock[raceCount];
constexpr int campfireStartWood = 0;           // shared pool at startup (the fire is already lit with campfireStartFuel)
constexpr int campfireTitheEvery = 10;         // every Nth wood deposit (any race) goes to the campfire pool...
constexpr int campfireWoodCap = 4;             // ...while the pool holds fewer than this, so the fire never hoards
int campfireWood = campfireStartWood;
int campfireTitheCount = 0;
// Shortage cue: a race whose wood, stone or food is below this shows red on its HUD row; the same test fires one ticker
// line per shortage (stockAnnounced), re-armed once that stock is back at or above the threshold.
constexpr int stockShortThreshold = 2;
bool stockAnnounced[raceCount][needCount] = {};
int game_time = 12;
// Day/night hours (R26-F1): night runs nightStartHour..nightEndHour-1, the day tint switches at the other two
const int nightStartHour = 21;             // dusk: night overlay, campfire glow and lit windows begin
const int nightEndHour = 6;                // dawn: night ends (the council also votes at this hour)
const int middayStartHour = 12;            // morning tint -> midday tint
const int eveningStartHour = 18;           // midday tint -> evening tint
const Color duskEventColor(140, 160, 215); // muted moonlight blue for the dusk ticker line; readable on the black strip
bool isNightHour(int h) { return h >= nightStartHour || h < nightEndHour; }
// The Turning Year (R51-F3): the season is a pure function of game_day (counted up by clockk at midnight).
// Day-based timer audit for the 8-day year: the only other day timer is orderDays, counted in ticks (no day modulo),
// so it is unaffected by the year length.
enum season { seasonSpring, seasonSummer, seasonAutumn, seasonWinter, seasonCount };
const int daysPerSeason = 2;
const int yearDays = daysPerSeason * seasonCount;   // 8-day year
const int seasonBlendHours = 12;                    // ground tint cross-fades toward the next season over its last 12h
const float winterHungerMult = 1.15f;               // hunger grows this much faster in winter (hungerTick only)
// The Lean Season (R52-F2): in winter bare bushes do not regrow, reaped/new plots lie fallow until spring, and ripe
// unpicked berries shrivel at the first frost. Survival: hunting, fishing, autumn crops still ripening and the larder
// carry the colony; as the guarantee, winter starvation never takes a barb below leanStarveFloorHp (no winter
// starvation deaths), and spring (bushes ~2 min to ripen, fields resown) lands with 40hp = ~15 min of starving to spare.
const int leanStarveFloorHp = 40;                   // winter starvation damage stops at this health
const int larderRefreshTicks = 60;                  // larder bar values are recomputed and cached once a second at 1x
const float larderBarH = 2.0f;                      // 2px bar under each race row of the stock panel
const float larderBarGap = 1.0f;                    // row -> bar
const Color larderTrackColor(56, 62, 54);           // dim slate-green track: the bar's full width reads on the backing
const Color larderOkColor(118, 156, 96);            // muted green: food covers the days to spring
const Color larderShortColor(196, 150, 76);         // muted amber: it does not
struct seasonDef {
    const char* name;
    const char* arrivalLine;   // ticker line on the first midnight of the season
    Color ground;              // window clear colour (base grass was 4,20,14); zone tint and everything else sit on top
    Color ticker;              // ticker line tint, readable on the black strip
    Color iconKey[4];          // HUD leaf icon: a = leaf, b = vein/stem
};
const seasonDef seasonDefs[seasonCount] = {
    {"Spring", "Spring arrives - the year turns", Color(5, 22, 14), Color(150, 205, 120),
     {Color(130, 190, 90), Color(80, 130, 55), Color::Transparent, Color::Transparent}},
    {"Summer", "Summer comes - long warm days",   Color(6, 21, 12), Color(215, 195, 100),
     {Color(175, 180, 70), Color(115, 120, 45), Color::Transparent, Color::Transparent}},
    {"Autumn", "Autumn turns the leaves",         Color(12, 18, 10), Color(215, 150, 80),
     {Color(205, 120, 50), Color(140, 75, 35), Color::Transparent, Color::Transparent}},
    {"Winter", "Winter sets in - hunger bites harder", Color(11, 18, 22), Color(180, 200, 225),
     {Color(195, 210, 230), Color(130, 150, 180), Color::Transparent, Color::Transparent}},
};
const char* const frostLine = "Frost is coming";   // dawn of autumn's last day
int game_day = 0;   // day 0 = spring, day 1 of year 1
int seasonOf(int day) { return (day % yearDays) / daysPerSeason; }
int dayInSeason(int day) { return day % daysPerSeason; }
bool isWinter() { return seasonOf(game_day) == seasonWinter; }
// R52-F2: whole game hours left in winter (to spring's midnight); 0 outside winter
int leanHoursLeft() { return isWinter() ? (daysPerSeason - 1 - dayInSeason(game_day)) * 24 + (24 - game_time) : 0; }
// R52-F2: the larder bar's target in days: the rest of this winter, or the whole coming winter before it starts
float leanDaysToSpring() { return isWinter() ? leanHoursLeft() / 24.0f : (float)daysPerSeason; }
float larderFill[raceCount] = {};      // R52-F2: cached by leanSeasonTick: days of food / days to spring, clamped 0..1
bool larderCovered[raceCount] = {};    // R52-F2: days of food >= days to spring
// Ground (clear) colour: the season's own, smoothstepped toward the next one over the season's last seasonBlendHours,
// reaching it exactly at the midnight the season turns
Color seasonGroundColor(int day, int hour) {
    int s = seasonOf(day), next = (s + 1) % seasonCount;
    int h = dayInSeason(day) * 24 + hour;
    float t = clamp((float)(h - (daysPerSeason * 24 - seasonBlendHours) + 1) / seasonBlendHours, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    Color a = seasonDefs[s].ground, b = seasonDefs[next].ground;
    auto mix = [t](uint8_t x, uint8_t y) { return (uint8_t)round(x + (y - x) * t); };
    return Color(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

const int stepInterval = 10;
const int rainStepInterval = 16;          // slower barb stepping in rain (~0.6x speed)
const int clearWeatherTicks = 30 * ticksPerMinute; // 30 real min of clear weather between rains
const int rainWeatherTicks = 10 * ticksPerMinute;  // 10 real min per rain
const Color rainOverlayColor(70, 90, 120, 70);
// Falling rain streaks: draw-only, positions hashed from (frame cycle, index) so there is no stored state
const int rainStreakCount = (int)(150 * worldAreaScale); // sparse: 150 per 800x800 of map (337 at 1200)
const int rainStreakPixels = 3;            // 1x3 streak, bottom pixel stepped 1px left for the wind slant
const int rainStreakLifeFrames = 40;       // frames a streak falls before it respawns at a new hashed spot
const float rainStreakFallPx = 4.0f;       // px fallen per frame (~240px/s)
const float rainStreakDriftPx = 1.0f;      // px drifted left per frame, matches the slant
const Color rainStreakColor(170, 195, 225, 64); // pale blue, ~25% alpha
bool raining = false;

const int traderIntervalTicks = ticksPerGameHour; // one in-game hour between trader visits
const int traderStepInterval = 6;         // ticks per 1px step while walking
const int traderTradeTicks = 300;         // ticks spent at the village center
const int tradeStoneCost = 2;             // stone paid per lot
const int tradeWoodGain = 1;              // wood received per lot
const int tradeMaxLots = 5;               // max lots per visit (caps at 5 wood/hour)
const int tradeMsgTicks = 600;            // ticks the last-trade line stays on the HUD
const Vector2f traderSize(3.0f, 3.0f);
const Color traderColor(190, 140, 230);
// R47-F2 Heartland Founding: the campfire sits at the middle of the map, so the safe core is ringed by the wilds
const Vector2f villageCenter(worldSize / 2.0f, worldSize / 2.0f);
const char* const foundingTickerLine = "Founded at the heart of the land";   // logged once, right after the founders
// POL-F4: the trader serves the eligible race with the least wood, at its storehouse (campfire while it has none)
const int traderRaceCooldownVisits = 1;   // a served race sits out this many following visits (1 = no back-to-back)
const int tradeFoodCost = 3;              // food paid per lot when no race can pay in stone
const int tradeFoodReserve = startStock.food; // food fallback never takes a race below this
// R44-F3 Frontier Groves: new trees lean outward. A rolled spot at distance d from villageCenter is kept with
// probability clamp((d - groveRampStart) / groveRamp, groveMinAccept, 1); after groveMaxRetries rerolls the last
// roll is kept, so spawn rate (and tree count) is unchanged and the centre still gets some trees.
// Follow-up (not built yet): deer spawns and bush clumps are meant to share this group.
const float groveRampStart = 60.0f;       // px from villageCenter where the outward bias starts easing off
const float groveRamp = 300.0f;           // px over which acceptance rises from groveMinAccept to 1
const float groveMinAccept = 0.1f;        // floor acceptance near the centre, keeps the clearing wooded
const int groveMaxRetries = 10;           // rerolls before the last roll is accepted anyway

const int hungerThreshold = 15 * ticksPerMinute; // 15 real min between meals; deer are the only food for now
const int hungerWorkSlowdown = 2;         // hungry barbs with no food work and walk this many times slower
const Color hungryTintColor(140, 100, 60);
const float hungryTintAmount = 0.45f;     // 0 = pure task colour, 1 = pure tint
// R30-F5: health and starvation. Unfed from the last meal: 15 min to hungry, +5 min grace, then 1hp per 15s (25 min to 0)
// = ~45 real min (Sturdy ~52, Glutton ~41). Any meal heals and resets the grace; Gentry eat first, so a long famine
// takes the hungriest Commoners one by one instead of the whole race at once.
const int barbMaxHealth = 100;
const int starveGraceTicks = 5 * ticksPerMinute;  // hungry this long (no meal) before damage starts
const int starveDamageTicks = 15 * 60;            // then -1hp every this many ticks
const int eatHealHp = 25;                         // hp restored per meal (capped at barbMaxHealth)
const float healthBarMinZoom = 2.0f;              // hidden below this zoom; also only drawn when hurt
const float healthBarW = 4.0f;                    // 1px tall bar, centred under the feet (2px human/elf, 3px dwarf)
const float healthBarGap = 1.0f;                  // px of grass between the feet and the bar
const Color healthBarFillColor(196, 72, 58);      // muted brick red, same family as the hunt stripe but dimmer
const Color healthBarBackColor(70, 60, 56, 200);  // dull grey-brown for the missing part, readable on dark green
// R52-F4 The Winter Cough, phase 1: a barb's `sick` timer counts down to a natural recovery (all races), then a brief
// immunity. Spread is rolled inside house buckets (kin of one house; founders bucket by race), rebuilt every tick by
// sickTick(); winter multiplies every roll. A new outbreak seeds only while nobody is sick, so one ticker line each.
const int sickSeedOneIn = 4 * 60 * ticksPerMinute;     // outbreak seed: 1-in-N per tick while nobody is sick (~4 real h)
const float sickWinterMult = 4.0f;                     // seed and spread chances x this in winter (seed ~1 real h)
const float sickSpreadPerKin = 1.0f / (8.0f * ticksPerMinute); // per tick per sick housemate: ~8 real min of shared roof
const int sickCrowdSize = 6;                           // a bucket this big counts as a crowded house...
const float sickCrowdMult = 1.5f;                      // ...and spreads this much faster
const int sickMinTicks = 12 * ticksPerMinute;          // illness length, rolled per case
const int sickMaxTicks = 20 * ticksPerMinute;
const int sickImmuneTicks = 15 * ticksPerMinute;       // brief immunity after recovering
const int sickDamageTicks = 15 * 60;                   // -1hp every this many sick ticks (~-60hp over a long case)
const float sickSpeedMult = 0.7f;                      // walking speed multiplier while sick (folds into stepLimit())
const int sickDeathCap = 2;                            // at most this many cough deaths...
const int sickDeathWindowTicks = 30 * ticksPerMinute;  // ...per this rolling window
const int sickColonyFloor = 12;                        // no cough death while the colony is at or under this many
const int sickRaceFloor = 4;                           // ...or the barb's race is at or under this many
static_assert(sickColonyFloor > 0 && sickRaceFloor > 0, "the cough must never wipe out the colony or a race");
const Color sickDotColor(172, 184, 88);                // sallow olive-yellow face pixel: greener than the sash/fleck golds,
                                                       // duller than chop green, far from the red raid pennant
// R47-F9 Ancestor Stones: every death (removeBarb) leaves a 3x4 headstone where the barb stood. The graves vector is
// its own list (no barb index or id kept), capped at graveMax with the oldest evicted first.
const int graveMax = 12;                          // stones on the map at once; a new one evicts the oldest
const Vector2f graveSize(3.0f, 4.0f);             // 1px cap on a 3x3 slab (barbs are 2-3px wide, trees ~16px)
const Color graveStoneColor(128, 126, 118);       // weathered grey, mixed with the race tint
const float graveRaceTint = 0.3f;                 // share of raceEventColors in the stone: muted, still readable by race
const float graveBaseShade = 0.7f;                // bottom row darker, so the stone reads as set in the ground
const int graveFadeStartTicks = 5 * ticksPerMinute;   // fresh stones hold full alpha this long...
const int graveFadeTicks = 20 * ticksPerMinute;       // ...then fade over this long down to graveOldAlpha
const uint8_t graveOldAlpha = 102;                // 40% of 255: old graves stay faintly visible
const float graveNoticePx = 8.0f;                 // a same-race barb within this many px of the stone centre brightens it
const int graveGlowTicks = 90;                    // brighten fades out over this many ticks once the barb has moved on
const Color graveGlowColor(232, 228, 212);        // pale stone the colour lifts toward while lit
const float graveGlowMix = 0.45f;                 // at full glow
const float graveSpreadPx = 4.0f;                 // a stone landing on another is nudged right by this much (a few tries)
// Morale: one term per race per tick, graveMoralePerStone per own stone up to graveMoraleMax. The race's hunger grows
// that much slower (a rest tick every 1/term ticks), so 4 stones = 4% longer between meals at most.
const float graveMoralePerStone = 0.01f;
const float graveMoraleMax = 0.04f;
// R50-F1a The Veil (phase 1, hidden, no visuals): a coarse ether grid per race over the whole map. Every death
// (removeBarb) deposits a soul where the barb fell; every veilStepTicks the grids diffuse to their 4 neighbours (no-flux
// edges, double-buffered) and decay a little. Births read the souls of every race around the birth spot: a mild rate
// factor between veilBirthFloor and 1, never a full gate. While the whole veil is empty the granary rule alone decides.
constexpr float veilCellPx = 40.0f;               // coarse: 60x60 cells on the 2400px map
constexpr int veilGridN = (int)(worldSize / veilCellPx);
static_assert(veilGridN * veilCellPx >= worldSize, "veil grid must cover the map");
const float veilSoulPerDeath = 1.0f;              // ether one death leaves in its cell
const int veilStepTicks = 10;                     // diffusion/decay runs once per this many sim ticks
const float veilDiffusion = 0.02f;                // share of the gap to the neighbour mean closed per step
const float veilDecay = 0.0001f;                  // per step: half-life ~6900 steps = ~19 min at 1x
const float veilZeroBelow = 1e-5f;                // a cell this faint is cleared (keeps the grid free of denormals)
const float veilEmptyEpsilon = 0.01f;             // total ether below this = pools empty: births use the granary rule only
const float veilFullLocal = 0.5f;                 // ether in the 3x3 cells round a birth spot for the full (x1) rate
constexpr float veilBirthFloor = 0.7f;               // the slowest birth rate the veil allows (x0.7): mild, never a gate
static_assert(veilBirthFloor > 0.0f && veilBirthFloor <= 1.0f, "the veil must never fully gate births");
const float veilBirthDraw = 0.25f;                // ether a birth draws from the 3x3 cells round its spot
// R50-F1b The Veil, phase 2 (omens): faint shimmer motes rise over recent death sites while their souls linger, a birth
// that drew a soul tints its ring toward that race, and a rare ticker omen (shared cooldown) marks veil moments.
const int veilSiteMax = 8;                        // death sites remembered for motes; a new one evicts the oldest
const float veilSiteQuietBelow = 0.15f;           // own-race ether in the site's 3x3 cells below this: site forgotten
const int veilMoteMax = 10;                       // motes alive at once over the whole map (hard cap)
const int veilMoteOneIn = 40;                     // per site per veil step at full ether: ~1 mote per 400 ticks (~7s)
const int veilMoteTicks = 180;                    // ~3s life: fades in, shimmers, fades out
const float veilMoteRisePx = 6.0f;                // drifts up this far over its life
const float veilMoteSwayPx = 0.8f;                // gentle side-to-side drift
const float veilMoteScatterPx = 3.0f;             // spawn offset round the site centre
const Vector2f veilMoteSize(1.0f, 1.0f);          // 1px: smaller than a 2px barb
const Color veilMoteColor(214, 222, 226);         // pale silver, mixed a little toward the race tint
const float veilMoteRaceTint = 0.3f;
const uint8_t veilMoteMaxAlpha = 80;              // low alpha: a hint on the dark green, never a marker
const float veilRingTint = 0.35f;                 // share of raceEventColors in a soul-drawn birth ring: subtle, still cream
const int veilOmenCooldownTicks = 4 * ticksPerMinute;   // at most one omen line per 4 min at 1x
const int veilOmenOneIn = 3;                      // an eligible omen fires only 1 in this many times
const Color veilOmenColor(176, 196, 204);         // pale ash-blue: greyer than dusk blue, apart from prowler/race tints
// R51-F2 Faith of the Veil, phase 1: per-race faith, derived only from veil.pool[r] (never written back into it) and
// smoothed in veilTick. Shown as a small tick on the race's stock-panel row; motes rise over the race's shrine at a
// rate that grows with faith; ticker lines when a race reaches high faith or falls back to low.
const float faithFullPool = 3.0f;                 // a race's pool at this much ether (about 3 recent deaths) = faith 1
const float faithSmoothing = 0.004f;              // share of the gap to the target closed per veil step (~40s lag at 1x)
const float faithHigh = 0.6f;                     // reaching this: "feel the Veil close" line
const float faithLow = 0.2f;                      // falling under this (after rising above it): "faith grows thin" line
const float faithTierHysteresis = 0.05f;          // a tier is left only this far past its threshold, so lines never chatter
const float faithSeenAbove = 0.02f;               // the HUD faith tick column appears once any race passes this
const float faithMoteChanceMax = 1.0f / 30.0f;    // per tick per race at faith 1 (~2 motes/s); scales with faith^2,
                                                  // so faith 0.2 is ~1 mote per 12s and faith 0.6 ~1 per 1.4s
const int faithMoteMax = 18;                      // shrine motes alive at once (6 per race at full faith)
const int faithMoteTicks = 200;                   // a little longer than a death-site mote
const float faithMoteRisePx = 10.0f;              // rises higher than a death-site mote: a thin column over the cap
const float faithMoteScatterPx = 2.0f;            // spawn offset across the 4px slab
const uint8_t faithMoteMaxAlpha = 110;            // a touch brighter than death-site motes; still a hint, not a marker
const float faithTickW = 2.0f;                    // HUD tick: 2px wide, 2..8px tall (bottom-aligned with the stock icons)
const float faithTickMinH = 2.0f;
const Color faithTickLowColor(92, 100, 104);      // dim slate: just visible on the panel backing at faith 0
const Color faithTickHighColor(214, 226, 232);    // pale silver at faith 1 (the mote colour, a little lifted)
// R52-F3 The Rite Decree, phase 1: a barb of a high-faith race with a shrine may vote Rite at the dawn council. On a Rite
// day, from dusk to riteEndHour, idle barbs walk to a slot on a ring round their race's shrine (veil.faithShrineId, found
// by id every tick) and kneel there (activity actKneel). Any task assignment, removeBarb and dawn clear it.
// The rite's end logs one ticker line with the kneelers counted and leaves a stub omen per race (no roll in phase 1).
const float riteBallotLean = 1.4f;                // ballot score x faith, only for a high-faith race with a shrine:
                                                  // beats the race lean (1.0) from faith ~0.72 up; a full need (1.5) still wins
const int riteEndHour = 23;                       // the rite runs nightStartHour..riteEndHour-1 on a Rite day
static_assert(riteEndHour > nightStartHour && riteEndHour <= 24, "the rite window must sit between dusk and midnight");
const int riteSlots = 8;                          // kneeling places per shrine ring (8px apart on the 10px ring)
const float riteRingPx = 10.0f;                   // kneel ring radius round the shrine slab's centre (shrines sit >= 10px apart)
const Color riteRingColor(214, 192, 118, 70);     // shrine-cap gold, faint; under barbs and the night overlay
const float riteFaithTickDim = 0.45f;             // HUD faith tick alpha on a Rite day (the faith is "spent" at the shrine)
const int riteOmenDays = 2;                       // an omen lasts this many full game days after its rite night
const Color riteTickerTint(214, 200, 150);        // rite-end ticker line: pale shrine gold, readable on the black strip
// R53-F3 Rite Omens, phase 2: at the rite's end each race that knelt rolls a blessing or a curse (more kneelers, better odds)
// on one effect, births or the cough. Everything reads it through omenMult(race, kind), 1.0 with no omen. Mild by design:
// a curse is never rolled for a small race, its births stay above the veil floor's range, and cough deaths keep their floors.
constexpr float omenBlessBase = 0.45f;            // blessing chance with one kneeler...
constexpr float omenBlessPerKneeler = 0.06f;      // ...plus this per extra kneeler...
constexpr float omenBlessMin = 0.45f;             // ...clamped to this range (a full ring of 8 reaches the cap)
constexpr float omenBlessMax = 0.85f;
constexpr float omenBlessBirthMult = 1.25f;       // blessing: births x this (a second spawn roll, see barbspawner)
constexpr float omenCurseBirthMult = 0.8f;        // curse: births x this
constexpr float omenBlessCoughMult = 0.6f;        // blessing: chance to catch the cough x this
constexpr float omenCurseCoughMult = 1.3f;        // curse: chance to catch the cough x this
constexpr float omenMultMin = 0.5f;               // omenMult() clamps every value to this range
constexpr float omenMultMax = 1.5f;
static_assert(omenBlessMin >= 0.0f && omenBlessMax <= 1.0f && omenBlessMin <= omenBlessMax, "blessing chance is a probability");
static_assert(omenCurseBirthMult >= omenMultMin && omenBlessBirthMult <= omenMultMax && omenBlessBirthMult < 2.0f,
              "births: mild multipliers; the extra roll in barbspawner needs a blessing below x2");
static_assert(omenBlessCoughMult >= omenMultMin && omenCurseCoughMult <= omenMultMax, "cough: mild multipliers");
const int omenCurseFloor = 6;                     // a race at or under this many at the rite's end is always blessed
const float omenAuraPx = 13.0f;                   // aura ring radius round the shrine slab's centre (outside the 10px kneel ring)
const Color omenAuraBlessColor(226, 210, 140, 46);  // pale shrine gold, very faint
const Color omenAuraCurseColor(150, 112, 104, 52);  // ashen rose-brown: dimmer than the brick-red birth/shortage lines
enum omenKind { omenBirths, omenCough, omenKindCount };
float omenMult(int race, int kind);               // defined with riteOmens[]; read by sickTick() and barbspawner()
// R53-F4 Activity Enum: the exclusive owned states (shelter, expedition, raid, forge tasks, drill, watch, kneel) are one
// barb.activity, changed only by beginActivity()/endActivity(). Build with -DSIM_DEBUG for the debug checks: starting a
// second activity over a live one aborts, and kDebugActivityKey toggles the activity name in the hover tooltip (off at start).
#ifdef SIM_DEBUG
const bool kDebugBuild = true;
#else
const bool kDebugBuild = false;
#endif
const Keyboard::Key kDebugActivityKey = Keyboard::Key::F9;
bool debugActivityTip = false;   // toggled by kDebugActivityKey in a debug build only; never on in a release build

const int eventMaxLines = 5;              // ticker shows (and stores) at most this many events
const int eventFadeTicks = 300;           // frames an event stays on screen (~5s at 60fps)
const int eventHoldTicks = 120;           // frames at full opacity before fading starts
const int eventHungryCooldownTicks = 3600; // min sim ticks between "no food" events
const unsigned int eventFontSize = 12;
const Vector2f eventMargin(6.0f, 6.0f);   // gap from the window's bottom-left corner
const Vector2f eventPad(6.0f, 4.0f);
const float eventStripMinWidth = 150.0f;
const Color eventTextColor(205, 200, 180);
const uint8_t eventTextAlpha = 220;
const uint8_t eventStripAlpha = 90;
// POL-F2: lines about one race's barbs are tinted by race. Brighter than lensRaceColors so 12px text reads on the
// translucent black strip; hues kept apart from ticker cream, dusk blue and the brick-red shortage/birth lines.
const Color raceEventColors[raceCount] = {
    Color(236, 190, 104),                     // human: warm gold
    Color(206, 168, 236),                     // dwarf: pale lavender (redder than dusk blue)
    Color(146, 224, 140),                     // elf: bright mint
};
const int raceEventFadeTicks = 60;        // race lines hold at full opacity, then fade over the last ~1s of eventFadeTicks

const float tooltipPickRadius = 7.0f;     // screen px around the cursor that counts as a hover hit
const float tooltipOutlinePx = 1.0f;      // highlight outline width in screen px (scaled by zoom)
const Color tooltipOutlineColor(240, 210, 90);
const unsigned int tooltipFontSize = 12;
const Vector2f tooltipPad(5.0f, 3.0f);
const Vector2f tooltipOffset(12.0f, 20.0f); // label offset below-right of the cursor (clears the arrow)
const float tooltipFlipGap = 8.0f;        // gap from the cursor when flipped left/above
const float tooltipEdgeMargin = 2.0f;
const Color tooltipPanelColor(0, 0, 0, 170);
const Color tooltipTextColor(225, 220, 200);
// Storehouse hover ledger (POL-F1): hovering a race's crate (or the campfire) shows one stock line; text and the 1px
// outline use the race's lens colour (lensRaceColors), the shared campfire uses ledgerPipColor
const float ledgerOutlinePx = 1.0f;       // screen px (scaled by zoom), hovered building only
const Color ledgerPipColor(226, 220, 204); // neutral pale stone for the shared campfire: belongs to no race

const float trailCellSize = 8.0f;          // world px per trail grid cell
const int trailGridN = (int)(worldSize / trailCellSize); // cells per side (2400 / 8 = 300)
const int trailGridMaxN = 320;             // R47-F1: bound on cells per side; raise trailCellSize before a bigger map passes it
static_assert(trailGridN <= trailGridMaxN, "trail grid too large: raise trailCellSize for this worldSize");
const int trailDecayIntervalTicks = 120;   // every 2s each cell loses 1 wear, so stray steps fade out
const int rainErodeMult = 2;               // trails decay this many times faster while raining (4 needed a barb per cell every 4s to hold a road through a shower)
const int trailMaxWear = 240;             // cap so abandoned roads fade within ~8 min
const int trailPathThreshold = 48;         // wear for the faint "path" tint (~6 recent crossings)
const int trailRoadThreshold = 144;        // wear for "road": darker tint and the speed bonus
const int trailSpeedBonus[3] = {0, 1, 2};  // R39-F1: ticks shaved off the step limit by tier (none/path/road), ~10%/20% faster at 1x
const int trailWearPerCell = 8;            // R39-F1: wear added once per cell entered (about what a per-pixel crossing of 8px used to add)
// R39-F1 trail routes: a non-wander walk plans a bounded A* over the trail grid once per goal and follows its cells.
// Each cell costs (1 - routeWearDiscount * min(wear, pathWearClamp) / pathWearClamp) per cell of length, so worn
// cells are cheaper, but no route may be longer than routeDetourCap x the straight (octile) distance.
const int pathWearClamp = trailRoadThreshold; // wear beyond road tier adds no extra pull
constexpr float routeWearDiscount = 0.4f;     // a full road costs 0.6 of bare ground
const float routeDetourCap = 1.3f;         // max route length / straight distance; keeps routes from collapsing onto a few highways
const int routeExpandCap = 400;            // A* node expansions per plan; past it the barb walks straight
const int routeMaxCells = 560;             // waypoint buffer per barb (1.3 x the 424-cell diagonal of the 2400 map fits)
const int routeLookahead = 3;              // a barb that cuts a corner into a later waypoint cell skips ahead
const float routeStrayPx = 24.0f;          // farther than this from the next waypoint (pushed/teleported): replan
static_assert(routeWearDiscount < 1.0f, "a cell must never cost zero or less");
const Color trailPathColor(150, 125, 85, 26);
const Color trailRoadColor(150, 125, 85, 52);   // kept as-is for the underlay of paved cells
// R38-F1: muted tan-brown trails whose alpha follows wear (in 8-wear buckets); road a shade darker than path
const Color trailPathTint(150, 126, 88);
const Color trailRoadTint(136, 113, 78);
const int trailPathAlphaMin = 40, trailPathAlphaMax = 70;    // path tier: wear trailPathThreshold..trailRoadThreshold
const int trailRoadAlphaMin = 70, trailRoadAlphaMax = 110;   // road tier: wear trailRoadThreshold..trailMaxWear
const int trailTierHysteresis = 8;   // a cell drops a tier only once wear falls this far below the threshold
const int trailWearBucket = 8;       // drawTrails is rebuilt only when a cell's wear crosses one of these buckets
// R43-F1: a crate/house placement seeds its quarter trail at trailPathThreshold + trailSeedBoost (= 80): faint path tier
// (alpha ~50, under any worn path's top end), drops out of the path tint after ~80s unused (~40s in rain), gone in ~160s
const int trailSeedBoost = 32;

// Cobbled roads (R20-F2): from the Hamlet era an idle barb paves road-tier trail cells near the campfire; paved cells never decay
const int paveScanTicks = 600;             // look for a new cell to pave this often (only while nobody is paving)
const int paveStoneCost = 1;               // per cell, paid on completion; houseStoneCost is kept back for the next house
const int paveMaxCells = 24;               // total paved cells ever
const float paveRadius = 48.0f;            // cell centre within this of the campfire (inside the 50px tree clearing)
const int paveTicks = 240;                 // ~4s at 1x of work on the cell (barb.timer, shows the build bar)
const Color paveColorA(78, 84, 94, 140);   // slate cobbles: cool blue-grey, darker than rocks (125) and clear of the brown trails;
const Color paveColorB(66, 72, 82, 140);   // the two tones differ by 12 so the 4px checker stays low-contrast
// Stone Roads to the Keep (R33-F3): once a race's keep stands, a causeway is paved cell by cell along the grid line
// from the keep centre out to its crate, paid from stone above keepStoneReserve. Same trailPaved grid (road speed-up).
const int causewayIntervalTicks = ticksPerMinute;   // one cell per minute at 1x
const int causewayStoneCost = 1;                    // per cell, only from stone above keepStoneReserve
const uint8_t causewayPaved = 2;                    // trailPaved value for causeway cells (campfire cobbles are 1)
const Color causewayColor(128, 126, 116, 200);      // pale dressed stone: warmer and lighter than the slate cobbles, not brown like trails
const Color causewayJointColor(110, 108, 100, 200); // fine 1px mortar joints, only 18 darker so the bond stays subtle
const float causewayCoursePx = 4.0f;                // 4px stone courses (3px stone + 1px joint), running bond, tiles across cells

const int birdFlockSize = 8;               // birds per flock (cosmetic only, no gameplay effect)
const float birdFleeRadius = 30.0f;        // birds scatter from barbs closer than this (world px)
const float birdCruiseSpeed = 0.4f;        // px per tick (~24px/s, crosses the map in ~35s)
const float birdMaxSpeed = 1.0f;           // cap while fleeing
const float birdMinSpeed = 0.2f;
const float birdCohesion = 0.002f;         // pull toward flock centre (regroup)
const float birdAlign = 0.02f;             // pull toward the flock's travel heading
const float birdSeparationDist = 5.0f;     // birds closer than this push apart
const float birdSeparation = 0.03f;
const float birdFleeForce = 0.08f;
const float birdSpawnSpread = 12.0f;       // initial cluster half-size
const float birdExitMargin = 40.0f;        // flock is gone once every bird is this far off-map
const int birdGapTicks = 7200;             // ~2 min of quiet sky between flocks
const int birdFlapFrames = 8;              // frames per wing-flap half cycle
const Vector2f birdWingsOut(3.0f, 2.0f);
const Vector2f birdWingsIn(2.0f, 2.0f);
const Color birdColor(205, 215, 230);      // pale blue-white: readable on dark green, distinct from white idle barbs

const int maxWolves = 3;                   // R47-F1: was 2; the wild ring is bigger and wolfier
const int wolfMinDeer = 4;                 // wolves only hunt while more deer than this remain, else they leave
const int wolfSpawnChance = 5000;          // 1-in-N per tick while below the cap and deer are plentiful (~83s)
const int wolfStepInterval = 2;            // ticks per 1px step (deer step every 3, so wolves gain slowly)
const Vector2f wolfSize(5.0f, 3.0f);
const Color wolfColor(150, 150, 160);      // cool mid grey: lighter than rocks, darker than birds, far from deer tan

// R48-F1 Night Prowlers, phase 1: shadow creatures come in from the map edge during the night, chase the nearest barb or
// house outside the lit campfire's safe zone and bite barbs; they dissolve at dawn or on entering the safe zone.
const int prowlerCapByEra[3] = {1, 2, 3};   // prowlers per night (total spawned, not concurrent): Camp / Hamlet / Town
const int prowlerSpawnChance = 2400;        // 1-in-N per night tick while below tonight's cap (~40s at 1x)
const float prowlerEdgeInsetPx = 4.0f;      // spawn this far in from the map border
const int prowlerStepInterval = 4;          // ticks per 1px step (barbs step every 10, wolves every 2)
const int prowlerSearchTicks = 60;          // nearest-target search throttle (~1s at 1x); targets re-validated every tick
const float prowlerSafeRadius = 90.0f;      // px from the campfire: no targets inside, a prowler entering dissolves (fire lit)
const float prowlerBiteRangePx = 3.0f;      // centre distance to bite a barb
const int prowlerBiteDamage = 4;            // hp per bite (barbMaxHealth 100)
const int prowlerBiteCooldownTicks = 300;   // ticks between bites (~5s at 1x)
const int prowlerFadeTicks = 120;           // dissolve fade (dawn and campfire share it), then removed
const Vector2f prowlerSize(5.0f, 4.0f);     // a touch bulkier than a 5x3 wolf
const Color prowlerColor(172, 156, 208);    // pale lavender: reads through the night overlay, apart from the 190,140,230 trader
const Color prowlerOutlineColor(222, 214, 240, 150); // pale 1px rim
const Color prowlerEyeColor(235, 50, 45);   // two 1px red eyes, drawn after the night overlay
const Color prowlerSafeRingColor(235, 165, 85, 60);  // faint warm 1px ring marking the safe zone at night (fire lit)
const Color prowlerEventColor(185, 160, 225);        // ticker tint for the first prowler of a night
// R51-F1 The Night Watch, phase 1: at night, armed and drilled barbs (a weapon and kDrillPipCount drills) take the watch
// (activity actWatch, owned by watchTick(), cleared only by clearWatch()) and walk a ring of watchRingPx round their race's
// crate with a 1px torch. A prowler within watchEngagePx (coarse squared distance, watchmen only) is chased and struck
// with the barb's weapon; at 0 hp it dissolves through the shared dissolveProwler() fade. Hp-bar polish and bite-back
// tuning are follow-ups.
const int prowlerMaxHp = 45;                 // 3 hits at any tier (spear 15 -> 21 at tier 3)
const int prowlerFlashFrames = 2;            // white hit flash, counted in drawn frames
const Color prowlerFlashColor(255, 255, 255);
const float watchRingPx = 70.0f;             // patrol ring radius round the race's crate centre (house disc 56)
const int watchRingStops = 12;               // patrol waypoints round the ring (~37px apart)
const int watchMaxPerRace = 3;               // watchmen per race at once
const int watchMinHealth = 40;               // a barb below this hp is not sent out on watch
const int watchEnlistTicks = 60;             // eligibility scan throttle while the night lasts (~1s at 1x)
const float watchEngagePx = 48.0f;           // coarse engage range, watchman centre to prowler centre
const float watchLeashPx = 72.0f;            // a chased prowler farther than this is let go
const float watchStrikeRangePx = 4.0f;       // centre distance to strike
const int watchStrikeCooldownTicks = 90;     // ticks between strikes (~1.5s at 1x)
const Color watchTorchColor(255, 156, 56);   // orange torch pixel, drawn above the night overlay
const Color watchTorchFlickerColor(232, 116, 40);
const Color watchTorchGlowColor(255, 150, 60, 40);   // faint 3x3 halo behind the pixel, night only
const int watchTorchFlickerFrames = 7;
const Color watchRingColor(235, 160, 80, 55);        // faint 1px patrol ring, only while a watchman is hovered/selected
const int watchRingPoints = 72;
// R52-F1 Spear Marches, phase 1: a cold pair (relCold), once its per-pair cooldown has run out, sends one 3-barb column of
// armed barbs (1px red pennant on the spear) to the rival crate. A pulsing red marker sits on the target crate, the rival
// bell rings through bellTick() (raidThreat[]), and raidTick() owns every raider's and defender's movement and runs its own
// fight resolver. Damage never takes a barb below kRaidHealthFloor, so a raid costs health only. The win/loss ticker line
// is the only outcome: no loot, no grudge, no relation change (R53-F1 adds the plunder below). raidParty holds stable
// barb ids, re-resolved every tick.
const int kRaidPartySize = 3;                 // raiders in the column
const int kRaidMinMembers = 2;                // fewer live (or unbeaten, in the fight) raiders than this ends the raid
const int kRaidDefenderMax = 3;               // defenders called out at once
const int kRaidPairCooldownTicks = 24 * ticksPerGameHour;   // one game day per pair between raids (also before the first)
const int kRaidCheckTicks = 600;              // launch scan throttle (~10s at 1x)
const float kRaidRecruitRadius = 140.0f;      // raiders come from within this of their own crate centre
const int kRaidMinHealth = 60;                // a raider must be at least this hale (and not hungry)
const int kRaidDefendMinHealth = 40;          // a defender must be at least this hale (beaten ones are never recalled)
const float kRaidAlarmPx = 90.0f;             // a raider within this of the rival crate centre rings the bell and starts the fight
const float kRaidDefendRadius = 140.0f;       // defenders come from within this of their crate centre
const int kRaidDefendScanTicks = 60;          // defender call-up throttle during the fight
const float kRaidEngagePx = 40.0f;            // a raider turns on a defender within this (centre to centre)
const float kRaidStrikeRangePx = 4.0f;        // centre distance to strike
const int kRaidStrikeCooldownTicks = 90;      // ticks between strikes (~1.5s at 1x)
const int kRaidHealthFloor = 30;              // raid blows stop here: the barb is beaten and leaves the fight, never killed
const int kRaidHoldTicks = 180;               // an unopposed raider must hold the crate this long (~3s) to win
const int kRaidFightMaxTicks = 45 * 60;       // the fight gives out after ~45s: the raiders are driven off
const int kRaidMaxTicks = 10 * ticksPerMinute;   // backstop on the whole raid (march, fight and walk home)
const float kRaidColumnGapPx = 5.0f;          // follower stands this far behind the barb ahead in the column
const float kRaidColumnStretchPx = 16.0f;     // the column head waits while its tail is farther back than this
const int kRaidFlashFrames = 2;               // white hit flash on the struck barb, counted in drawn frames
const Color kRaidFlashColor(255, 255, 255);   // body and a 1px rim: reads over the off-white body
const Color kRaidPennantColor(226, 50, 44);   // 1px pennant beside the spear tip, raiders only
const Vector2f kRaidMarkerSize(3.0f, 3.0f);   // above the target crate's top-left corner (the bell marker sits top-right)
const Vector2f kRaidMarkerOffset(0.0f, -kRaidMarkerSize.y - 1.0f);
const Color kRaidMarkerColor(222, 54, 46);    // alpha pulses between kRaidMarkerAlphaMin and 255
const int kRaidMarkerAlphaMin = 90;
const float kRaidMarkerPulseRate = 0.12f;     // radians per drawn frame (~0.9s period at 60fps)
const Color kRaidTickerTint(222, 120, 104);   // launch and outcome ticker lines
// R53-F1 Plunder, raid phase 2: on a won raid each unbeaten raider lifts one capped pack from the rival race's stock,
// deducted at pickup, and carries it home as a 2px bundle at 0.8x walk. settleLoot() is the one way a pack ends: deposited
// at the home crate, or dropped (abort, release away from home, death), in which case it goes back to the robbed race.
// The pair's relation drops once per plundering raid. Envoys are phase 3.
const int kLootPackFood = 3;                  // pack size per kind: a raider lifts one kind, the one the rival can spare most
const int kLootPackWood = 3;
const int kLootPackStone = 2;
const int kLootRaidCap = 8;                   // units lifted by the whole raid at most
const float kLootStockFrac = 0.3f;            // and never more than this share of any one rival stock...
const raceStock kLootRivalFloor{6, 3, 6};     // ...nor below this floor (wood, stone, food), so a raid can't cripple a rival
const float kLootSpeedMult = 0.8f;            // laden walk speed multiplier (folds into stepLimit())
const float kLootRelationDelta = 0.15f;       // relation drop for the pair when anything is plundered
const Vector2f kLootBundleSize(2.0f, 1.0f);   // carried low beside the body, one column out from the spear's foot
const Color kLootBundleColor(176, 120, 190);  // muted violet sack: apart from every task stripe, the red pennant and the relic gold
// R54-F1 Caravans and Coin, phase 1: a race with a spare stock sends one two-barb caravan (held by barb id, owned by
// caravanTick()) along the route planner to a rival crate that is not cold. There the pair swaps one lot each of a
// surplus kind, the relation warms, and a pair at or above kRelFriendlyMinRelation becomes "friendly" for
// kRelFriendlyDays (a fresh visit pushes the timer back). Friendly pairs never raid. Without trade the timer runs out;
// it also ends at once if the pair slips to uneasy. Every abort hands the cart's goods back to their owner.
const int kTradeMembers = 2;                  // barbs in a caravan: the first pulls the cart
const int kTradeCheckTicks = 900;             // launch scan throttle (~15s at 1x)
const int kTradePairCooldownTicks = 12 * ticksPerGameHour;   // per pair between caravans (also before the first)
const int kTradeLot = 3;                      // units each side hands over in a swap
const raceStock kTradeKeep{10, 6, 12};        // (wood, stone, food) a race only trades what sits a full lot above this
const float kTradeRecruitRadius = 140.0f;     // caravan barbs come from within this of their own crate centre
const int kTradeMinHealth = 60;               // a caravan barb must be at least this hale (and not hungry)
const int kTradeStayTicks = 240;              // ticks the caravan stands at the rival crate before the swap (~4s)
const int kTradeMaxTicks = 12 * ticksPerMinute;   // backstop on the whole trip
const float kTradeRelationDelta = 0.2f;       // relation warmth per completed swap
const Color kTradeTickerTint(172, 208, 178);  // soft jade: apart from the raid reds, the rite gold and the race tints
const Vector2f kTradeCartSize(2.0f, 2.0f);    // cart on the ground behind the lead barb's arm column
const float kTradeCartGapPx = 2.0f;           // cart's left edge this far right of the body (clear of the arm/spear foot)
const Color kTradeCartColor(128, 112, 90);    // weathered grey-brown wood, the same for every race
const Color kTradeCoinColor(206, 190, 136);   // 1px pale brass pip on the cart while it carries goods
const float kRelFriendlyMinRelation = 0.0f;   // a swap leaving the pair at or above this makes it friendly
const int kRelFriendlyDays = 2;               // friendly timer per swap, in game days
const int kRelFriendlyTicks = kRelFriendlyDays * 24 * ticksPerGameHour;

// Skittish deer: bolt away from a nearby barb for a short, capped time, then a cooldown so hunts can still land
const int deerStepInterval = 3;            // ticks per 1px step while wandering (deer row of speciesDefs)
const int deerFleeStepInterval = 1;        // ticks per 1px step while fleeing (3x wander speed; barbs step every 10)
const float deerFleeRadius = 20.0f;        // barb this close starts a flee (inside the 25px hunt pickup range)
const float deerFleeRadiusSq = deerFleeRadius * deerFleeRadius;
const int deerFleeCheckInterval = 10;      // proximity scan runs once per this many ticks
const int deerFleeTicks = 40;              // hard cap on one flee (~40px at flee speed)
const float deerFleeDistance = 40.0f;      // how far the flee target is set from the deer
const int deerFleeCooldown = 600;          // ticks after a flee before the deer can flee again (barb gains ~60px)
const float deerFleeEdgeMargin = 4.0f;     // flee targets stay this far inside the map and below the HUD strip

const float barbWorkSearchRadius = 25.0f;  // barbs only notice trees/rocks within this many px (findNearest)
const float barbWorkFallbackRadius = 160.0f; // R45-F1a: nothing in reach and no shore to fish: one wider look before wandering
const float noSearchRadius = numeric_limits<float>::infinity(); // findNearest with no distance limit
const int memSpotCount = 3;                // INBOX-F2: work spots a barb remembers (fixed array, [0] = most recent)
const float memSpotRadius = 25.0f;         // a remembered spot is live while a free tree/rock/ripe bush of its kind stands within this
const float memMergeRadius = 12.0f;        // a new spot this close to a remembered one of the same kind refreshes it instead
const float memShareRadius = 40.0f;        // a fresh spot is shared only with a same-race barb within this many px...
const int memShareMax = 1;                 // ...and with at most this many of them, once per barb per trip
const Vector2f treeWorkOffset(8.0f, 15.0f); // where a barb stands to chop a tree, relative to its shape
const Vector2f rockWorkOffset(6.0f, 10.0f); // where a barb stands to mine a rock, relative to its shape
const float treeNeedBoost = 1.0f;          // tree weight rises up to 1 + this as wood falls from houseWoodCost to 0
const float needWeightMin = 0.5f;          // job weights are clamped to [min, max] so no resource is ever starved
const float needWeightMax = 2.0f;          // 2x weight = a tree up to 2x farther still wins (distance / weight)

const int numBushClumps = (int)(4 * worldAreaScale);   // R45-F1a: 4 per 800x800 of map (9 at 1200)
const float bushClumpEdgeMargin = 60.0f;   // clump centres stay this far inside the map edges
const int bushesPerClump = 5;
const float bushClumpSpread = 14.0f;       // max px from the clump centre on each axis
const float bushClumpMinCenterDist = 120.0f; // clumps stay clear of the village center (and its tree clearing)
const float bushTreeClearance = 3.0f;      // startup trees overlapping a bush (plus this margin) are removed
const float bushRadius = 3.0f;             // 6px bush: bigger than a barb, well under a 16px tree
const Vector2f bushWorkOffset(3.0f, 7.0f); // where a barb stands to pick a bush (just below it)
const int bushPickTicks = 180;             // ~3s to pick, +1 food
const int bushRegrowTicks = 7200;          // ~2 min for a bare bush to ripen again
const int rainRegrowMult = 2;              // bushes ripen this many times faster while raining
const Color bushRipeColor(150, 55, 95);    // muted berry red-violet, darker than every barb task colour
const Color bushBareColor(62, 78, 46);     // dull olive: readable on the dark ground, distinct from tree green
const Color barbPickColor(255, 140, 190);  // pink: distinct from the other task colours and the trader

const int birthRingTicks = 40;             // ~0.7s lifetime
const float birthRingStartRadius = 1.5f;   // starts just around the 2px barb
const float birthRingEndRadius = 8.0f;     // ends about half a tree wide
const float birthRingThickness = 1.0f;
const Color birthRingColor(225, 220, 190); // pale warm cream, matches ticker text
const uint8_t birthRingMaxAlpha = 70;      // faint at birth, fades linearly to 0

// Granary-gated births: past the floor, a birth needs at least one food in the larder per living barb
const int popPerHouse = 2;                 // pop cap = houses * this
const int birthFoodCost = 1;               // food paid per birth (food never goes below 0)
const int birthFreeFloor = 4;              // below this many barbs births ignore the larder
const int birthDeferCooldownTicks = 3600;  // min sim ticks between "Birth deferred" ticker lines
const int birthFlashFrames = 30;           // ~0.5s tint on the birth ticker line when food was paid
const Color birthFlashColor(200, 110, 90); // muted brick red, eased back to the normal ticker colour

// Households (hover-only): cream marks, distinct from the task colours and the yellow hover outline
const Color householdColor(238, 226, 190);    // pale cream
const Vector2f householdPipSize(2.0f, 2.0f);  // one pip per resident, about a barb wide
const float householdPipGap = 4.0f;           // pip bottom this far above the hitbox top: clears the 2px elf hat, 1px bob and 1px air

// Settlement eras: a rank needs both thresholds (houses are capped at one per barb, so both grow together)
enum settlementEra { eraCamp, eraHamlet, eraTown, eraCount };
const char* const eraNames[eraCount] = {"Camp", "Hamlet", "Town"};
const int eraHamletBarbs = 4;              // a few births in, first few houses up
const int eraHamletHouses = 3;
const int eraTownBarbs = 12;               // a well-established village, long after startup
const int eraTownHouses = 10;
const int eraCheckIntervalTicks = 60;      // recompute about once per second at 1x

// Dawn council: each barb casts one ballot at dawn; the winning decree tilts job weights until the next dawn
enum councilDecree { decreeTimber, decreeStone, decreeBerries, decreeRite, decreeCount, decreeNone = decreeCount };   // R52-F3: Rite
const char* const decreeNames[decreeCount] = {"Timber", "Stone", "Berries", "Rite"};
const int councilDawnHour = 6;             // vote when game_time ticks over to this hour (night ends at 6)
const float councilDecreeMult = 1.3f;      // matching job weight is multiplied by this (after the need clamp, so it always bites)
const float councilRaceLean = 1.0f;        // ballot score for a barb's racial preference
const float councilNeedLean = 1.5f;        // ballot score for a full colony need; outweighs race so needs swing the vote
const councilDecree raceLeans[raceCount] = {decreeBerries, decreeStone, decreeTimber}; // human, dwarf, elf
// Open ballot: right after the dawn vote each barb shows its ballot as a pip overhead (shapes only, no text)
const int ballotShowFrames = 480;          // ~8s on screen (frames, so it reads the same at any sim speed); frozen while paused
const int ballotFadeFrames = 120;          // last ~2s fade out
const float ballotMinZoom = 2.0f;          // hidden below this zoom: a 2x2 pip is only 2 screen px at 1x
const Vector2f ballotPipSize(2.0f, 2.0f);  // same footprint as the household pips, same spot above the head
const Vector2f gentryBallotPipSize(2.0f, 4.0f); // R24-F1: Gentry pip is twice as tall (double vote), grows upward
const Color ballotPipColors[decreeCount] = {
    Color(70, 190, 170),   // Timber: teal, clear of chop green and build blue
    Color(170, 176, 186),  // Stone: pale slate, darker than the off-white body and idle white
    Color(190, 80, 170),   // Berries: violet, darker than pick pink, brighter than ripe bushes
    Color(214, 192, 118),  // R52-F3 Rite: shrine-cap gold, warmer than the slate Stone pip
};
extern councilDecree currentDecree;   // R52-F3: defined with castBallot(); the HUD faith tick reads it to dim on a Rite day

const int houseWoodCost = 30;
const int houseStoneCost = 2;              // small; with less stone a plain house is built instead
const Vector2f houseSlabSize(10.0f, 10.0f); // centred under the 4px house (6px with outline): 2px rim shows
const Color houseSlabColor(50, 56, 52);    // dim grey, low contrast on the dark ground, darker than rocks
// House sprite, offsets relative to the 4x4 logic shape's top-left (hitbox unchanged). 8x7 overall, inside the 10x10 slab.
const int houseRoofRows = 3;               // stepped roof: 2, 6, 8 px wide
const float houseRoofLeft[houseRoofRows]  = {1.0f, -1.0f, -2.0f};
const float houseRoofWidth[houseRoofRows] = {2.0f, 6.0f, 8.0f};
const float houseRoofTop = -2.0f;          // first roof row; walls start right under the last one
const Vector2f houseWallPos(-1.0f, 1.0f);
const Vector2f houseWallSize(6.0f, 4.0f);
const Vector2f houseDoorPos(0.0f, 3.0f);   // 2x2 door at the bottom of the walls
const Vector2f houseDoorSize(2.0f, 2.0f);
const Vector2f houseWindowPos(3.0f, 2.0f); // 1x1 window right of the door
const Vector2f houseWindowSize(1.0f, 1.0f);
const Color houseWallColor(197, 149, 82);  // same timber tan the houses always had
const Color houseRoofColor(120, 64, 48);   // muted brick
const Color houseDoorColor(70, 46, 26);
const Color houseWindowDayColor(52, 54, 50);   // dark glass by day
const Color houseWindowNightColor(240, 190, 95); // warm lamp, drawn after the night overlay
const float houseWindowGlowPx = 1.0f;      // faint additive halo this far around a lit window
const Color houseWindowGlowColor(90, 60, 20, 255); // additive, so this is a gentle warm lift
// Kin quarters: each house is drawn in its builder's race style. Walls, door and window stay shared;
// only the roof changes (rows end on y = 0, directly above the walls at houseWallPos.y = 1).
const Color houseRoofDwarfColor(108, 106, 98);   // muted stone grey
const Color houseChimneyDwarfColor(70, 68, 64);  // darker stone stack
const Color houseRoofElfColor(146, 172, 128);    // pale sage green, a shade darker than the elf hat
// R41-F1: race walls, doors and windows (same 6x4 wall footprint; offsets from the 4x4 house shape like the above).
// Human: timber tan walls (houseWallColor), 2x2 door split into two 1px planks, 1x1 window (houseWindowPos/Size).
const Color houseDoorPlankColor(92, 62, 36);     // right plank, a step lighter than houseDoorColor
// Dwarf: cool ashlar grey, lighter and bluer than rocks (128,126,118) and the dwarf roof (108), with a darker footing row
const Color houseWallDwarfColor(152, 152, 160);
const Color houseFootingDwarfColor(104, 104, 112); // 1px course along the wall base
const Vector2f houseDoorDwarfPos(0.0f, 2.0f);    // dark 2x3 arched door: top row is the lighter arch shadow
const Vector2f houseDoorDwarfSize(2.0f, 3.0f);
const Color houseDoorDwarfColor(30, 28, 30);
const Color houseArchDwarfColor(66, 62, 60);
const Vector2f houseWindowDwarfPos(3.0f, 2.0f);  // 1x2 slit window, 1px in from the right wall edge
const Vector2f houseWindowDwarfSize(1.0f, 2.0f);
// Elf: pale birch walls with two dark bark knots, a rounded lantern-green door (3x2 plus a 1px crown), 2x1 window
const Color houseWallElfColor(214, 208, 186);
const Color houseKnotElfColor(96, 92, 80);
const Vector2f houseKnotElfPos[2] = {{-1.0f, 2.0f}, {4.0f, 4.0f}};
const Vector2f houseDoorElfPos(0.0f, 3.0f);
const Vector2f houseDoorElfSize(3.0f, 2.0f);
const Vector2f houseDoorElfCrownPos(1.0f, 2.0f); // 1x1 on top of the door's middle column: the round top
const Color houseDoorElfColor(84, 134, 80);      // lantern green: darker and more saturated than the sage roof
const Vector2f houseWindowElfPos(3.0f, 2.0f);
const Vector2f houseWindowElfSize(2.0f, 1.0f);
// Night lamp colour and additive halo per race (drawHouseLights), glowing only around each style's window rect
const Color houseWindowNightDwarfColor(236, 146, 72);  // forge orange
const Color houseWindowGlowDwarfColor(90, 44, 16, 255);
const Color houseWindowNightElfColor(196, 232, 150);   // pale lantern green
const Color houseWindowGlowElfColor(52, 80, 36, 255);
// Ranges into buildingParts for each race's walls/door/window (checked by the static_assert after the table)
const int houseBodyHumanFirst = 22, houseBodyHumanCount = 4;
const int houseBodyDwarfFirst = houseBodyHumanFirst + houseBodyHumanCount, houseBodyDwarfCount = 5;
const int houseBodyElfFirst = houseBodyDwarfFirst + houseBodyDwarfCount, houseBodyElfCount = 6;
const int houseStyleMaxRows = 4;
struct houseStyle {
    int rows;
    float left[houseStyleMaxRows];   // x offset of each roof row, top row first
    float width[houseStyleMaxRows];
    float top;                       // y of the first roof row
    Color roof;
    Vector2f chimneyPos, chimneySize; // 0x0 = no chimney
    int bodyFirst, bodyCount;        // R41-F1: this race's walls/door/window in buildingParts
    Vector2f windowPos, windowSize;  // must match the window part in that range
    Color windowNight, windowGlow;
};
const houseStyle houseStyles[raceCount] = {
    // Human: the original 3-row stepped brick roof
    {houseRoofRows, {houseRoofLeft[0], houseRoofLeft[1], houseRoofLeft[2]}, {houseRoofWidth[0], houseRoofWidth[1], houseRoofWidth[2]},
     houseRoofTop, houseRoofColor, {0.0f, 0.0f}, {0.0f, 0.0f},
     houseBodyHumanFirst, houseBodyHumanCount, houseWindowPos, houseWindowSize, houseWindowNightColor, houseWindowGlowColor},
    // Dwarf: squat 2-row flat roof with a 1x2 chimney on the right
    {2, {-1.0f, -2.0f}, {6.0f, 8.0f}, -1.0f, houseRoofDwarfColor, {3.0f, -3.0f}, {1.0f, 2.0f},
     houseBodyDwarfFirst, houseBodyDwarfCount, houseWindowDwarfPos, houseWindowDwarfSize, houseWindowNightDwarfColor, houseWindowGlowDwarfColor},
    // Elf: tall 4-row pointed roof
    {4, {1.0f, 0.0f, -1.0f, -2.0f}, {2.0f, 4.0f, 6.0f, 8.0f}, -3.0f, houseRoofElfColor, {0.0f, 0.0f}, {0.0f, 0.0f},
     houseBodyElfFirst, houseBodyElfCount, houseWindowElfPos, houseWindowElfSize, houseWindowNightElfColor, houseWindowGlowElfColor},
};

// Shrine stones: build and placement only (no prayer yet). Houses always get first call on stone.
const int maxShrines = 3;                  // hard cap on standing stones in the village
const int shrineStoneCost = 10;            // small: ~10 rocks mined; only spent with houseStoneCost left over
const int shrineHousesPerShrine = 3;       // one shrine per this many houses, so shrines trail the housing
const int shrineBuildTicks = 600;          // same work as a house
const float shrineMinRadius = 18.0f;       // placed in a ring around the campfire, inside the 50px clearing
const float shrineMaxRadius = 40.0f;
const float shrineMinSpacing = 10.0f;      // keep stones from overlapping each other
const int shrinePlaceTries = 8;
const Vector2f shrineSlabSize(4.0f, 8.0f); // standing slab, about half a tree tall
const float shrineCapH = 2.0f;             // gold cap sits on top of the slab (4x2)
const Vector2f shrineStandOffset(3.0f, -6.0f); // slab top-left relative to the builder's spot: barb stands at its left foot
const Color shrineSlabColor(138, 136, 128); // warm stone grey: lighter than rocks (125), clear of wolf blue-grey
const Color shrineCapColor(214, 192, 118); // pale gold: not mine orange, lamp amber, hover gold or hungry tan

// Smokehouse (INBOX2-F1): one per race, beside its crate, built by a barb of that race once the crate stands; once
// standing, that race's own kills of a deer or rabbit yield extra food (other races' kills don't)
const int smokehouseWoodCost = 20;
const int smokehouseStoneCost = 4;
const int smokehouseFoodBonus = 1;         // extra food per own-race deer/rabbit kill (wolf kills still give none)
const int smokehouseMinHouses = 1;         // the race's first home comes before its smokehouse
const int smokehouseBuildTicks = 600;      // same work as a house
// Hut top-left relative to the crate top-left: above the crate's left half, 4px of grass between hut and lid, clear of
// the bell marker (top-right) and the crate stand spot (left side). Set per race in placeQuarters().
const Vector2f smokehouseCrateOffset(-2.0f, -9.0f);
const float smokehouseRoadPadPx = 1.0f;    // quarter roads keep this far from the hut footprint (incl. chimney)
const Vector2f smokehouseSize(6.0f, 5.0f);
const float smokehouseRoofH = 2.0f;        // top 2 rows are roof, bottom 3 are wall
const Vector2f smokehouseDoorPos(1.0f, 3.0f);  // 1x2 door, relative to hut top-left
const Vector2f smokehouseDoorSize(1.0f, 2.0f);
const Vector2f smokehouseChimneyPos(4.0f, -2.0f); // 1x2 chimney poking up from the right of the roof
const Vector2f smokehouseChimneySize(1.0f, 2.0f);
const Vector2f smokehouseStandOffset(-3.0f, 1.0f); // builder's spot relative to the hut: stands at its left wall
const float smokehouseClearPx = 4.0f;      // houses, shrines, plots and blinks keep this far from every hut (and its chimney)
const Color smokehouseWallColor(88, 70, 54);   // dark smoked timber: darker than house tan, still clear of the ground
const Color smokehouseRoofColor(58, 44, 36);
const Color smokehouseDoorColor(30, 22, 16);
const Color smokehouseChimneyColor(110, 106, 100);
const Color smokeColor(176, 174, 168);     // pale grey puffs
// INBOX2-F1: no idle smoke; a race's hut puffs only right after one of its own smoked kills
const uint8_t smokeAlpha = 110;            // faint starting alpha; each puff fades to 0 as it rises
const int smokePuffLifeFrames = 90;        // ~1.5s per puff (drawn frames, frozen while paused)
const int smokePuffCount = 2;              // short: two puffs per smoked kill
const int smokePuffStaggerFrames = 30;     // second puff leaves the chimney 0.5s after the first
const int smokePuffFrames = smokePuffLifeFrames + (smokePuffCount - 1) * smokePuffStaggerFrames; // whole burst
const float smokeRisePx = 12.0f;
const float smokeDriftPx = 3.0f;           // leans east as it rises
const float smokePuffPx = 2.0f;            // 2x2 puff, shrinks to 1x1 in the upper half

// Market stall: at most one, free (just an idle barb's labour); once standing, traders take this many extra lots per visit
const int stallExtraLots = 3;
const int stallBuildTicks = 600;           // same work as a house
const Vector2f stallPos(407.0f, 392.0f);   // stall top-left: just north-east of the campfire, inside the shrine ring (r < 18)
const Vector2f stallSize(7.0f, 6.0f);      // 2 awning rows, 2 post rows, 2 counter rows
const float stallAwningH = 2.0f;
const float stallCounterH = 2.0f;
const Vector2f stallStandOffset(-3.0f, 3.0f); // builder's spot relative to the stall: stands at its left side
const Color stallAwningColorA(196, 176, 134);  // muted cream / brick stripes, one column each
const Color stallAwningColorB(150, 74, 60);
const Color stallPostColor(80, 56, 36);
const Color stallCounterColor(128, 92, 58);
const Color stallGoodsWoodColor(214, 160, 90); // 1px crates on the counter while a bonus trade is under way
const Color stallGoodsStoneColor(170, 170, 160);

// Each race's smokehouse roof takes its race's muted colour (INBOX2-F1)
const Color smokehouseRoofRaceColors[] = {   // human, dwarf, elf (indexed by barbRace)
    smokehouseRoofColor,
    Color(72, 70, 66),                         // dark stone grey
    Color(66, 82, 62),                         // dark sage
};
static_assert(size(smokehouseRoofRaceColors) == raceCount, "one smokehouse roof colour per race");

// Dwarf forge: at most one, built by an idle dwarf once dwarves have built enough houses
const int forgeWoodCost = 10;
const int forgeStoneCost = 8;              // the forge is where stone goes; houses keep houseStoneCost in reserve
const int forgeMinDwarfHouses = 2;
const int forgeBuildTicks = 600;           // same work as a house
const int houseBuildTicks = 600;
const Vector2f forgePos = villageCenter + Vector2f(-25.0f, -7.0f);   // forge top-left: west of the campfire (R47-F7: was the literal (375, 393) of the 800 map)
const Vector2f forgeStandOffset(-3.0f, 1.0f); // builder's spot relative to the forge: stands at its left wall
const float forgeClearPx = 3.0f;           // houses and shrines keep this far from the forge footprint
struct forgePart {Vector2f pos, size; Color color;};
const int forgePartCount = 4;
const forgePart forgeParts[forgePartCount] = {   // drawn in order, offsets from forgePos
    {{0.0f, 0.0f}, {7.0f, 5.0f}, Color(84, 90, 98)},   // slate body
    {{0.0f, 0.0f}, {7.0f, 1.0f}, Color(58, 62, 70)},   // darker slate roof row
    {{1.0f, 2.0f}, {2.0f, 2.0f}, Color(28, 28, 32)},   // hearth mouth
    {{4.0f, 3.0f}, {2.0f, 2.0f}, Color(44, 46, 52)},   // 2x2 iron anvil
};
const Vector2f forgeEmberPos(2.0f, 3.0f);  // 1px ember at the back of the hearth mouth
const Color forgeEmberDim(96, 46, 30);     // breathing range: low contrast against the dark mouth
const Color forgeEmberLit(132, 64, 36);
const Color forgeEmberHot(176, 90, 44);    // brief glow after a dwarf mines a rock
const int forgeEmberBreathFrames = 150;    // one slow breath every ~2.5s
const int forgeEmberBoostFrames = 120;     // ~2s brighter after a dwarf mines (frames, so it reads the same at any speed)
// POL-F5b: forge phase 2. While forgeActive, each dwarf stone load deposits +forgeStoneBonus and pops a few sparks.
const int forgeStoneBonus = 1;             // modest: one extra stone per dwarf rock
const int forgeSparkMin = 3, forgeSparkMax = 4; // sparks per bonus deposit
const int forgeSparkFrames = 20;           // ~1/3s life, counted per drawn frame
const int forgeSparkPoolMax = 8;           // capped pool: at most two bursts in flight; extra sparks are dropped
const Vector2f forgeSparkOrigin(2.0f, 2.0f); // top of the hearth mouth, offset from forgePos
const float forgeSparkRiseMin = 0.15f, forgeSparkRiseMax = 0.3f; // px per frame upward (3-6px over a life)
const float forgeSparkDriftMax = 0.12f;    // px per frame sideways, either way
const Color forgeSparkColor(236, 146, 64); // warm orange, a step above forgeEmberHot
const uint8_t forgeSparkMaxAlpha = 220;    // linear fade to 0
// R41-F2: forge phase 2b. A dwarf finishing a stone haul while forgeActive, with no charges left, pays kTemperStoneCost and
// tempers its pick at forgePos + forgeStandOffset for kTemperTicks; its next kTemperCharges rock mines run kTemperMineSpeed
// times faster. Shaft trips neither use nor spend charges: a shaft mine is kShaftTickMult x as long, so the same speedup
// there would save more than the stone it cost.
constexpr int kTemperStoneCost = 1;
constexpr int kTemperTicks = 2;               // work ticks at the forge, in barb.temperTimer (never barb.timer)
constexpr int kTemperCharges = 3;
constexpr float kTemperMineSpeed = 1.25f;     // 25% faster mining rate while charged
// a rock is 1 stone per mineTicks, so the time saved over all charges, in rocks, must stay below the stone spent
static_assert(kTemperCharges * (1.0f - 1.0f / kTemperMineSpeed) < (float)kTemperStoneCost, "tempering must not pay for itself");
const Vector2f temperPipSize(2.0f, 2.0f);     // beside the left foot of a charged dwarf
const Color temperPipColor(170, 196, 222);    // pale tempered steel blue: reads on the dark green, apart from cap brown and horns
// R47-F7: blacksmith phase 1. The forge is shared: once built, any race's barb that ends a stone haul with stone to spare
// walks to forgePos + smeltStandOffset and smelts kSmeltStoneCost stone into kSmeltIngots of its race's ingots (one smelter
// at the forge at a time). A race's ingot count picks its tool tier from toolTierIngots; a chop or mine started at tier t
// runs toolTierSpeed[t] times faster. The tier is read once when the work starts (barb.jobTier). No ingots = tier 0, no stall.
constexpr int kSmeltStoneCost = 3;
constexpr int kSmeltStoneReserve = houseStoneCost;   // only stone above this is smelted, so the next house never waits on it
constexpr int kSmeltIngots = 1;
constexpr int kSmeltTicks = 240;                     // work ticks at the forge (half a rock), in barb.smeltTimer (never barb.timer)
const Vector2f smeltStandOffset(8.0f, 1.0f);         // smelter hitbox top-left relative to the forge: right of the anvil, clear of the temper spot
constexpr int toolTierCount = 4;
constexpr int toolTierIngots[toolTierCount] = {0, 2, 5, 9};          // ingots a race needs for each tier
constexpr float toolTierSpeed[toolTierCount] = {1.0f, 1.1f, 1.2f, 1.3f}; // work-rate multiplier per tier
constexpr int kIngotCap = toolTierIngots[toolTierCount - 1];         // ingots for the top tier (R49-F1: smelting stops at kIngotStockMax)
constexpr float kToolTierMaxStep = 0.15f;                            // at most +15% per tier
constexpr bool toolTiersOk() {
    if (toolTierIngots[0] != 0 || toolTierSpeed[0] != 1.0f) return false;   // empty stock = tier 0 = base speed
    for (int t = 1; t < toolTierCount; t++)
        if (toolTierIngots[t] <= toolTierIngots[t - 1] || toolTierSpeed[t] <= toolTierSpeed[t - 1]
            || toolTierSpeed[t] - toolTierSpeed[t - 1] > kToolTierMaxStep + 1e-4f) return false;
    return true;
}
static_assert(toolTiersOk(), "tool tiers: start at 0 ingots / 1x, rise strictly, at most +15% per tier");
const Color toolTierMarkColor(204, 212, 222);        // 1px steel edge above the raised tool: a step paler than the Hand Axe head
const int smeltWispPuffs = 3;                        // smoke wisp over the hearth while a smelt is under way
const int smeltWispFrames = 90;                      // one puff's rise, ~1.5s (frames, frozen while paused)
const float smeltWispRisePx = 7.0f;                  // how far a puff climbs above the roof
const Vector2f smeltWispOrigin(2.0f, -1.0f);         // just above the roof row over the hearth mouth, offset from forgePos
const Color smeltWispColor(150, 150, 146);           // muted ash grey
const uint8_t smeltWispMaxAlpha = 120;               // translucent, fades out as it rises
// R49-F1: blacksmith phase 2, tool wear. Every finished chop or mine (tree, rock, shaft) by a race holding ingots adds one
// to raceWear; at kWearJobs one ingot is spent (raceIngots floored at 0). Smelting may now stock up to kIngotStockMax, a
// buffer above kIngotCap (the top-tier threshold), so a race that keeps smelting holds the top tier through wear.
// Tuning: kWearJobs jobs mine roughly kWearJobs/2 stone, and one ingot back costs kSmeltStoneCost, so a working forge
// out-smelts the wear with stone to spare; a neglected race slips a tier every few dozen jobs.
constexpr int kWearJobs = 20;                        // finished chop/mine jobs per ingot worn away
constexpr int kWearIngots = 1;                       // ingots spent each time the wear count reaches kWearJobs
constexpr int kIngotStockMax = kIngotCap + 3;        // smelt gate and clamp: three ingots of wear buffer above the top tier
static_assert(kIngotStockMax > kIngotCap, "ingot stock must allow a buffer above the top-tier threshold");
const Color toolWearColor(228, 146, 62);             // amber: the tier-drop ticker line and the lost HUD tier bar
const int tierDropDimFrames = 180;                   // ~3s: HUD tier icon and on-barb steel mark dimmed after a real tier drop
const float tierDropDimMix = 0.6f;                   // how far the dimmed icon/mark eases toward the panel/ground
const Color toolTierMarkDimColor(110, 116, 122);     // dimmed 1px steel mark during tierDropDimFrames
const int kWearSparkCooldownFrames = 45;             // per-race spark rate limit (drawn frames, ~0.75s)
const int wearSparkFrames = 18;                      // one spark's life, ~0.3s
const float wearSparkRisePx = 3.0f;                  // climbs this far over its life
const Vector2f wearSparkOffset(2.0f, -3.0f);         // from the barb's top-left: just above its tool hand
const Color wearSparkColor(168, 168, 162);           // muted grey 1px chip, apart from the orange forge sparks
const uint8_t wearSparkMaxAlpha = 210;               // linear fade to 0
const float tierIconBarW = 2.0f, tierIconBarGap = 1.0f; // HUD stock panel tier icon: toolTierCount-1 rising bars, 8px wide
const Color tierIconEmptyColor(58, 64, 58);          // an unearned tier bar: dark, just visible on the panel backing

// Race storehouses (R29-F1): one per race at a fixed spot, built by a barb of that race. Chopped wood and mined stone
// are carried to the hauler's own race's storehouse (the campfire while it isn't standing) and only counted on arrival.
const int storehouseWoodCost = 10;
const int storehouseMinRaceHouses = 1;     // a race builds its storehouse once it has a house of its own
const int storehouseBuildTicks = 600;      // same work as a house
const Vector2f storehouseSize(10.0f, 10.0f);
Vector2f storehousePos[raceCount];         // crate top-left, indexed by barbRace; set once at startup by placeQuarters() (R30-F1)
Vector2f smokehousePos[raceCount];         // INBOX2-F1: hut top-left = storehousePos + smokehouseCrateOffset, set with the crates
const Vector2f storehouseStandOffset(-4.0f, 3.0f); // builder/hauler hitbox top-left relative to the crate: at its left side
const Vector2f traderStoreOffset(storehouseSize.x + 1.0f, 3.0f); // POL-F4: trader top-left relative to the crate: its right side
const float storehouseClearPx = 3.0f;      // houses, shrines, the field and blinks keep this far from crate + stand spot
const Color storehouseColors[raceCount] = {   // R36-F1: the crate's runtime tint (drawParts), lid and body ramp off it
    Color(142, 106, 66),                   // human: muted timber brown
    Color(116, 112, 102),                  // dwarf: stone grey
    Color(108, 134, 94),                   // elf: sage
};
const Color storehouseRopeColor(170, 148, 104); // R36-F1: pale hemp band round the crate body
const int storehouseFlashFrames = 18;      // deposit flash, ~0.3s (frames, frozen while paused)
const float storehouseFlashMix = 0.4f;     // peak blend toward white: subtle
const Vector2f campfireDepotOffset(-10.0f, -2.0f); // fallback drop spot (hitbox top-left) just west of the stone ring

// Town bell and garrison (INBOX-F1): a wolf near a standing crate rings that race's bell; the race's barbs near the crate
// walk to the doorstep of their nearest own-race house and shelter inside (hidden) until the bell stops.
const float bellWolfRadius = 70.0f;           // wolf centre within this of the crate centre rings the bell
const float bellGatherRadius = 100.0f;        // that race's barbs within this of the crate centre take shelter
const int bellQuietTicks = 600;               // bell stops ~10s at 1x after the last wolf left bellWolfRadius...
const int bellMaxTicks = 2 * ticksPerMinute;  // ...or after this long at most (timeout), wolf or not
const int bellCooldownTicks = 30 * 60;        // after a bell ends, that race can't ring again for ~30s
const Vector2f bellMarkerSize(3.0f, 3.0f);    // static marker, no pulse
const Vector2f bellMarkerOffset(storehouseSize.x - bellMarkerSize.x, -bellMarkerSize.y - 1.0f); // above the crate's top-right corner, 1px gap
const Color bellMarkerColor(196, 166, 92);    // muted gold: dimmer than the crown and shrine cap
const Vector2f garrisonDoorOffset(0.0f, 8.0f); // doorstep hitbox top-left relative to the 4x4 house: just below the slab, under the door
const Color garrisonRoofTint(196, 166, 92);   // occupied house roofs ease toward the bell gold...
const float garrisonRoofTintAmount = 0.3f;    // ...by this much: subtle
const int garrisonPipMax = 3;                 // 1px pips above the roof, one per sheltering barb, capped
const float garrisonPipGap = 1.0f;            // px between pips, and between the pip row and the top of the roof/chimney

// Leader's Keep (R31-F1): in the Town era each race with a standing crate raises one slate-grey keep just below it,
// built by a barb of that race (buildOneOff, jobKeep). The spot is reserved from the start (forgeClear), like the huts.
const int keepStoneCost = 2 * shrineStoneCost;   // ~2x a shrine stone
const int keepStoneReserve = houseStoneCost + tradeStoneCost; // only paid from stone above this: paving's house reserve + one trade lot
const int keepBuildTicks = 2 * houseBuildTicks;  // a bigger job than a house
const Vector2f keepSize(14.0f, 12.0f);           // whole silhouette, battlements included
const Vector2f keepCrateOffset(-2.0f, 19.0f);    // keep top-left relative to the crate: below it; the banner stays clear of the crate's clear zone
const float keepMerlonH = 2.0f;                  // top 2 rows are the battlements (R36-F1: 3 notched merlons, see buildingParts)
const Vector2f keepGatePos(5.0f, 7.0f);          // dark 4x5 arched door, centred on the 14px front, on the ground line
const Vector2f keepGateSize(4.0f, 5.0f);         // top row is the 2px arch, the 4px door below it
const float keepPoleH = 5.0f;                    // banner pole rises this far above the top-right merlon
const Vector2f keepPolePos(keepSize.x - 1.0f, -keepPoleH); // 1px pole on the right column of the last merlon
const Vector2f keepFlagSize(3.0f, 2.0f);         // flies right from the pole top
const Vector2f keepLeaderPipPos(6.0f, 0.0f);     // R36-F1: fills the centre merlon's 2px notch: inside the silhouette, never above a roof
const Vector2f keepLeaderPipSize(2.0f, 1.0f);
const Vector2f keepStandOffset(6.0f, keepSize.y); // builder hitbox top-left relative to the keep: just below the gate
const float keepStandRoomPx = 6.0f;              // reserved below the keep for the stand spot: tallest barb body (5) + 1
const float keepClearPx = 3.0f;                  // houses, shrines, plots and blinks keep this far from the keep zone
const float keepRoadPadPx = 1.0f;                // quarter roads keep this far from the keep zone
const Color keepStoneColor(104, 110, 122);       // opaque cool slate: lighter than the translucent paving, cooler than rocks/dwarf crate/mill
const Color keepPlinthColor(56, 60, 70);         // 1px darker ground row along the bottom
const Color keepGateColor(20, 20, 26);
const Color keepPoleColor(112, 96, 74);          // weathered wood
const Color keepBannerColors[raceCount] = {      // saturated, unlike the muted crates (indexed by barbRace)
    Color(206, 58, 44),                          // human: red
    Color(60, 102, 220),                         // dwarf: blue
    Color(46, 176, 96),                          // elf: green
};
const Color keepLeaderPipColor = crownColor;     // bright crown gold, not the dimmer bell gold of the garrison pips
// Keep as Bell Refuge (R31-F3): while a race's bell rings, its barbs prefer their built keep (up to keepSlotMax, by barb
// id in keepSlots) over a house. Sheltering barbs light 2x1 slit-window pips on the wall face; the banner flies while
// anyone shelters and droops while the keep is empty.
const int keepSlotMax = 8;
const int keepGarrisonId = -2;                   // garrisonHouseId sentinel: sheltering in the own race's keep (house ids are >= 0)
const Vector2f keepPipSize(2.0f, 1.0f);          // 2px wide: larger than the 1px house garrison pips
const float keepPipCols[4] = {1.0f, 4.0f, 8.0f, 11.0f}; // symmetric on the 14px wall, centre gap over the gate; no merlon/gate overlap
const float keepPipRows[2] = {3.0f, 5.0f};       // wall rows 3 and 5: a clear row under the merlons (0-1), between, and above the gate (7)
const Color keepPipColor(206, 220, 238);         // pale blue-white, distinct from the bell/crown golds
const Vector2f keepDroopSize(2.0f, 3.0f);        // limp banner hanging beside the pole when the keep is empty
const Vector2f keepDroopOffset(1.0f, 1.0f);      // relative to the pole top: just right of the pole, a px below the top
static_assert(keepSlotMax <= 4 * 2, "one pip per slot: 4 columns x 2 rows");

// Building Signatures (R36-F1, phase 1): each building is a list of 1px-grid parts in one shared table, drawn by
// drawParts() into a VertexArray (one draw per building kind). Each part's colour is multiplied by a runtime tint
// (sf::Color modulate): "tinted" ranges are authored on the grey ramp below, so they take the tint's hue at 3 muted
// steps; "fixed" ranges use real colours and are drawn with a white tint. Draw-only: hitboxes are unchanged.
struct partDef {Vector2f pos, size; Color color;};
struct partRange {int first, count;};
const Color partRampLight(255, 255, 255);        // exactly the tint
const Color partRampMid(222, 222, 222);          // ~13% darker
const Color partRampDark(176, 176, 176);         // ~30% darker: undersides, shaded sides
const Color buildingShadowColor(12, 18, 10, 120); // translucent 1px ground shadow, cast 1px right of the footprint
const partDef buildingParts[] = {
    // [0] storehouse, fixed: ground shadow under the 10x10 crate
    {{1.0f, 10.0f}, {10.0f, 1.0f}, buildingShadowColor},
    // [1..4] storehouse, tinted (race colour): 10px lid over an 8px body inset 1px each side, so the lid lips out
    {{0.0f, 0.0f}, {10.0f, 1.0f}, partRampLight},  // lid top
    {{0.0f, 1.0f}, {10.0f, 1.0f}, partRampDark},   // lid edge
    {{1.0f, 2.0f}, {8.0f, 8.0f}, partRampMid},     // body
    {{8.0f, 2.0f}, {1.0f, 8.0f}, partRampDark},    // shaded right side
    // [5] storehouse, fixed: 1px rope band round the body
    {{1.0f, 6.0f}, {8.0f, 1.0f}, storehouseRopeColor},
    // [6] keep, fixed: ground shadow under the 14x12 keep
    {{1.0f, 12.0f}, {14.0f, 1.0f}, buildingShadowColor},
    // [7..17] keep, tinted (keepStoneColor): wall, a dark wall-walk row, then 3 notched merlons (3, 4, 3 px wide at
    // x 0, 5, 11; 2px crenels between). Each merlon's top row is split by a notch: 1px on the sides, 2px in the centre.
    {{0.0f, keepMerlonH}, {keepSize.x, keepSize.y - keepMerlonH}, partRampLight},
    {{0.0f, keepMerlonH}, {keepSize.x, 1.0f}, partRampDark},
    {{0.0f, 1.0f}, {3.0f, 1.0f}, partRampLight},
    {{0.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    {{2.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    {{5.0f, 1.0f}, {4.0f, 1.0f}, partRampLight},
    {{5.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    {{8.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    {{11.0f, 1.0f}, {3.0f, 1.0f}, partRampLight},
    {{11.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    {{13.0f, 0.0f}, {1.0f, 1.0f}, partRampLight},
    // [18..20] keep, fixed: plinth row, then the dark arched door through it
    {{0.0f, keepSize.y - 1.0f}, {keepSize.x, 1.0f}, keepPlinthColor},
    {keepGatePos + Vector2f(1.0f, 0.0f), {keepGateSize.x - 2.0f, 1.0f}, keepGateColor},
    {keepGatePos + Vector2f(0.0f, 1.0f), {keepGateSize.x, keepGateSize.y - 1.0f}, keepGateColor},
    // [21] house, fixed: ground shadow under the 6px walls (offsets from the 4x4 house shape)
    {houseWallPos + Vector2f(1.0f, houseWallSize.y), {houseWallSize.x, 1.0f}, buildingShadowColor},
    // R41-F1: per-race walls, door, window (drawn after the race roof; drawHouseLights lights the window from houseStyle)
    // [22..25] human house, fixed: timber walls, two-plank door, 1x1 window
    {houseWallPos, houseWallSize, houseWallColor},
    {houseDoorPos, {1.0f, houseDoorSize.y}, houseDoorColor},
    {houseDoorPos + Vector2f(1.0f, 0.0f), {1.0f, houseDoorSize.y}, houseDoorPlankColor},
    {houseWindowPos, houseWindowSize, houseWindowDayColor},
    // [26..30] dwarf house, fixed: ashlar walls, footing course, arched 2x3 door (arch row + dark door), slit window
    {houseWallPos, houseWallSize, houseWallDwarfColor},
    {houseWallPos + Vector2f(0.0f, houseWallSize.y - 1.0f), {houseWallSize.x, 1.0f}, houseFootingDwarfColor},
    {houseDoorDwarfPos, {houseDoorDwarfSize.x, 1.0f}, houseArchDwarfColor},
    {houseDoorDwarfPos + Vector2f(0.0f, 1.0f), houseDoorDwarfSize - Vector2f(0.0f, 1.0f), houseDoorDwarfColor},
    {houseWindowDwarfPos, houseWindowDwarfSize, houseWindowDayColor},
    // [31..36] elf house, fixed: birch walls, two bark knots, rounded door (body + crown), 2x1 window
    {houseWallPos, houseWallSize, houseWallElfColor},
    {houseKnotElfPos[0], {1.0f, 1.0f}, houseKnotElfColor},
    {houseKnotElfPos[1], {1.0f, 1.0f}, houseKnotElfColor},
    {houseDoorElfPos, houseDoorElfSize, houseDoorElfColor},
    {houseDoorElfCrownPos, {1.0f, 1.0f}, houseDoorElfColor},
    {houseWindowElfPos, houseWindowElfSize, houseWindowDayColor},
};
const partRange storeShadowParts{0, 1}, storeBodyParts{1, 4}, storeTrimParts{5, 1};
const partRange keepShadowParts{6, 1}, keepStoneParts{7, 11}, keepTrimParts{18, 3};
const partRange houseShadowParts{21, 1};
static_assert(houseBodyElfFirst + houseBodyElfCount == 37, "house body ranges must end the buildingParts table");
static_assert(size(buildingParts) == 37, "partRange indices above must match the buildingParts table");
// Curtain Wall and Gate (R31-F2): cosmetic only (no collision or pathing). Once a race's keep stands, a ring of
// wallSlots arc slots centred on the keep gets one 2px-thick stone segment every wallRiseTicks, growing out from both
// sides of a wallGateSlots-wide gate gap that faces the campfire. A slot whose arc would touch a building, plot, rock,
// bush, lake, paved cell, the map edge or another quarter is skipped for good; trees on an accepted arc are felled once.
const float wallRingRadius = 44.0f;              // around the keep centre (20px below the crate centre): clears the hut's 4px zone above the
                                                 // crate by 3px; the far arc reaches about the quarter clearing edge (63)
const int wallSlots = 15;
const int wallGateSlots = 3;                     // the gap, centred on the campfire bearing
const int wallSegMax = 12;
static_assert(wallSegMax + wallGateSlots == wallSlots, "every non-gate slot holds one segment");
const float wallThickPx = 2.0f;                  // same as a barb
const float wallSeamPx = 2.0f;                   // arc length left open between neighbouring segments
const int wallRiseTicks = ticksPerMinute / 2;    // one segment per 30 s at 1x: a full wall in ~6 min
const Color wallStoneColor(64, 70, 68);          // opaque, low-contrast grey-green stone: darker than the keep and paving
const Color wallRainColor(50, 56, 58);           // wet stone while raining (global flag, no per-segment state)
const Color wallFlashColor(118, 122, 114);       // the newest segment starts toward this...
const float wallFlashStrength = 0.35f;           // ...by this much, fading linearly to nothing
const int wallFlashTicks = 40;                   // ~0.7 s at 1x, timed from the race's last placement tick
// Gatehouse Towers (R33-F2): when a race's curtain wall completes, two 12x16 stone towers are placed on the ring at the
// gate-gap slots either side of the gate slot (slot 0), leaving slot 0 open as the passage. Each is raised in turn by a
// barb of that race through the keep's builder path (jobKeep, keepQueued/keepBuilderId, keep labour), paying stone only
// from above keepStoneReserve, so houses and trade keep first call. A blocked spot is dropped for good.
const int towerCount = 2;
const int towerStoneCost = shrineStoneCost / 2;  // per tower: 10 stone for the pair, half a keep
const Vector2f towerSize(12.0f, 16.0f);
const float towerMerlonH = 2.0f;                 // top 2 rows: 4 merlons with 3 2px notches
const float towerMerlonX[4] = {0.0f, 4.0f, 7.0f, 10.0f};
const float towerMerlonW[4] = {2.0f, 1.0f, 1.0f, 2.0f};   // solid 2px corners, 1px inner merlons: 2+2+1+2+1+2+2 = 12
const Vector2f towerSlitPos(5.0f, 6.0f);         // dark 2x3 arrow slit, centred on the 12px face
const Vector2f towerSlitSize(2.0f, 3.0f);
const float towerArcNudgePx = 3.0f;              // tower centre sits this far (arc px) past its slot centre, away from the
                                                 // gate: keeps any rotation of the 12x16 box off slot 0's arc
const float towerPoleH = 3.0f;                   // pennant pole rises this far above the right corner merlon
const Vector2f towerPolePos(towerSize.x - 1.0f, -towerPoleH);
const Vector2f towerFlagSize(2.0f, 1.0f);        // tiny, flies right from the pole top
const Vector2f towerStandOffset(5.0f, towerSize.y); // builder hitbox top-left relative to the tower: just below it
const Color towerPlinthColor(40, 44, 44);        // 1px darker ground row, same family as the wall stone
const Color towerPennantColors[raceCount] = {    // muted and darker than every barb task colour and the keep banners
    Color(168, 58, 52),                          // human: brick red
    Color(92, 80, 160),                          // dwarf: indigo
    Color(40, 120, 84),                          // elf: dark jade
};
// Deep Mine Shaft (R34-F1, phase 1): each race with a standing crate builds one timber-framed shaft just right of it
// (buildOneOff, jobShaft). Miners score it like a rock; one miner at a time works it for kShaftTickMult x mineTicks and
// hauls kShaftYield stone per trip until the finite seam (kShaftCapacity) runs dry, after which the shaft is inert.
const int kShaftCapacity = 40;                   // stone in the seam; 0 = exhausted for good
const int kShaftYield = 2;                       // stone per trip (a rock gives 1), clamped to what is left
const int kShaftTickMult = 2;                    // work time = this x mineTicks(b)
const float kShaftSearchRadius = 60.0f;          // own-race miners notice the shaft from about the quarter clearing (rocks: 25)
const int shaftWoodCost = 8;                     // timber frame, from the race's own wood
const int shaftBuildTicks = houseBuildTicks;
const Vector2f shaftSize(12.0f, 12.0f);
const Vector2f shaftCrateOffset(16.0f, -2.0f);   // shaft top-left relative to the crate: right of it, past the trader's spot
const Vector2f shaftStandOffset(5.0f, shaftSize.y); // miner/builder hitbox top-left relative to the shaft: just below the pit
const float shaftLintelH = 2.0f;                 // beam across the top
const float shaftPostW = 2.0f;                   // side posts; 1px sill at the bottom
const float shaftSeamGap = 1.0f;                 // seam bar sits this far above the lintel...
const float shaftSeamH = 1.0f;                   // ...1px tall, shaftSize.x wide when full
const float shaftClearPx = 3.0f;                 // houses, plots, mills and towers keep this far from the shaft zone
const float shaftTintRadius = 8.0f;              // a chopping miner of that race this close to the stand spot tints the pit
const float shaftTintMix = 0.3f;                 // faint: pit eases this far toward amber
const Color shaftFrameColor(88, 66, 42);         // dark timber
const Color shaftLintelColor(132, 102, 66);      // lighter beam, close to the spear ash
const Color shaftPitColor(20, 16, 12);           // idle pit: near black, warm
const Color shaftAmberColor(214, 150, 64);       // worked pit tint target
const Color shaftDeadFrameColor(58, 50, 42);     // exhausted: greyed timber...
const Color shaftDeadLintelColor(76, 68, 60);    // ...dimmed lintel...
const Color shaftDeadPitColor(6, 6, 6);          // ...and a flat black pit
const Color shaftSeamColor(150, 150, 146);       // thin grey seam bar
const Color shaftSeamTrackColor(36, 38, 36);     // its empty track
// R40-F1 (phase 2): the working miner drops into the pit (barb.inMine, cleared only by leaveMine()) and is hidden while
// below; faint dust puffs rise off the lintel, and a short lintel flash marks the climb-out with a load.
const int kShaftPuffEveryTicks = 20;             // one puff per this many ticks while a miner is underground
const int kShaftPuffLifeTicks = 36;              // a puff fades out over this long
const int kShaftPuffCap = 3;                     // live puffs per shaft (ring buffer: the oldest is overwritten)
const float kShaftPuffSize = 2.0f;               // 2x2 px
const float kShaftPuffRisePx = 5.0f;             // total rise above the lintel over its life
const uint8_t kShaftPuffAlpha = 60;              // starting alpha, fading to 0: low so stacked puffs don't haze
const Color kShaftPuffColor(140, 124, 104);      // grey-brown dust
const int kShaftFlashTicks = 12;                 // lintel flash countdown when the miner climbs out with stone
const float kShaftFlashMix = 0.55f;              // peak blend of the lintel toward the flash colour
const Color kShaftFlashColor(222, 198, 150);     // pale sandy highlight
// Barracks and Drill Yard (R47-F6, phase 1): once a race's keep stands, a barb of that race builds one 10x8 grey-roofed
// hall left of its crate (buildOneOff, jobBarracks) with a straw dummy in front. Idle barbs of that race nearby walk to
// one of kDrillSlots slots beside the dummy and drill (activity actDrill, owned by drillTick(), cleared only by
// clearTraining()); every hit adds to the barb's drill counter and shakes the dummy. No combat bonus or expedition
// preference yet. The spot is reserved from the start (forgeClear), like the shaft.
const int barracksWoodCost = 16;                 // from the race's own stock, with a house's wood and stone kept back
const int barracksStoneCost = 6;
const int barracksBuildTicks = houseBuildTicks;
const Vector2f barracksSize(10.0f, 8.0f);
const Vector2f barracksCrateOffset(-20.0f, -1.0f); // hall top-left relative to the crate: left of it, past the hauler's stand spot
const float barracksRoofH = 3.0f;                // top 3 rows: grey roof, the top row inset 1px each side, the 3rd an eave
const Vector2f barracksDoorPos(4.0f, 5.0f);      // dark 2x3 door centred on the 10px front, on the ground line
const Vector2f barracksDoorSize(2.0f, 3.0f);
const Vector2f barracksWindowPos[2] = {{1.0f, 4.0f}, {8.0f, 4.0f}};   // 1x1 dark windows either side of the door
const Vector2f barracksStandOffset(4.0f, 9.0f);  // builder hitbox top-left relative to the hall: just below the door
const Vector2f dummyOffset(4.0f, 10.0f);         // dummy top-left relative to the hall: in front, 1px row of grass between
const Vector2f dummySize(3.0f, 5.0f);            // 1px head, 3px arms, 1px torso, 2px post (centre column)
const int kDrillSlots = 2;                       // drillers per race at once: left and right of the dummy
const Vector2f drillSlotOffset[kDrillSlots] = {{1.0f, 11.0f}, {8.0f, 11.0f}};   // barb hitbox top-left relative to the hall:
                                                 // 1px of grass between body and arms, feet about level with the post foot
const Vector2f barracksZoneSize(11.0f, 17.0f);   // hall + shadow + dummy + both slots (dwarf width, elf height)
const float barracksClearPx = 3.0f;              // houses, plots, mills, walls and towers keep this far from the zone
const float barracksRoadPadPx = 1.0f;            // quarter roads keep this far from the zone
const float kDrillJoinRadius = 90.0f;            // idle barbs within this of their hall may join the drill...
const int kDrillJoinOneIn = 240;                 // ...each idle tick with chance 1 in this (~4 s at 1x), so not all at once
const int kDrillHitTicks = 50;                   // work ticks per hit on the dummy
const int kDrillSessionHits = 12;                // hits per session (~10 s), then the barb goes back to looking for work
const int kDrillJabTicks = 10;                   // the jab pixel shows this long after each hit
const int kDummyShakeTicks = 12;                 // dummy top sways +-1px this long after a hit...
const int kDummyShakeStepTicks = 3;              // ...flipping side every this many ticks
const int kDrillPipCount = 24;                   // drill counter at which the barb wears the pip (two sessions)
const float drillPipMinZoom = 2.0f;              // 1px pip (bottom-right body pixel, under stripe and sash) hidden below this zoom
const Color barracksWallColor(102, 84, 64);      // weathered timber: lighter than the smokehouse, warmer than the keep
const Color barracksRoofColor(118, 120, 124);    // flat mid grey, a touch warmer and lighter than the keep slate
const Color barracksEaveColor(86, 88, 92);       // eave row, one step darker
const Color barracksDoorColor(30, 24, 20);
const Color barracksWindowColor(44, 38, 34);
const Color dummyStrawColor(186, 158, 92);       // muted straw, dimmer than the harvest gold stripe
const Color dummyHeadColor(206, 182, 118);       // sack head, a little lighter
const Color dummyPostColor(88, 66, 42);          // dark timber, same as the shaft frame
const Color drillJabColor(222, 214, 196);        // pale fist/stick pixel toward the dummy
const Color drillPipColor(132, 30, 36);          // dark red on the off-white body: darker than the health bar brick and the hunt stripe
// Herb Hut and Summer Patch (R53-F2, cough phase 2a): once the elf crate stands and the elves have kHerbHutMinHouses
// houses, an elf builds one thatched herb hut above-right of the crate (buildOneOff, jobHerbHut) with a 3x2 herb bed
// beside it. The plants grow outside winter (fastest in summer), bloom on harvest day (the last day of summer) and brown
// at the first winter midnight. An idle elf near the hut tends ripe plants (activity actHerb, owned by herbTendTick(),
// ended through clearTraining() like the drill) into the elves' herb stock (raceHerbs), which slowly spoils in winter
// (faster in rain). No cure yet (phase 2b). The spot is reserved from the start (forgeClear), like the barracks.
const int kHerbRace = raceElf;                   // only this race builds a herb hut
const int kHerbHutMinHouses = 2;                 // own houses before the hut is wanted
const int herbHutWoodCost = 12;                  // from the elves' own stock, with a house's wood kept back
const int herbHutBuildTicks = houseBuildTicks;
const Vector2f herbHutCrateOffset(10.0f, -20.0f); // hut top-left relative to the crate: above-right, past the smokehouse, over the shaft
const Vector2f herbHutSize(7.0f, 6.0f);          // 3 roof rows (top inset 1px, 3rd an eave) over 3 wall rows
const float herbHutRoofH = 3.0f;
const Vector2f herbHutDoorPos(3.0f, 4.0f);       // dark 1x2 door centred on the front, on the ground line
const Vector2f herbHutDoorSize(1.0f, 2.0f);
const Vector2f herbHutBundlePos[2] = {{1.0f, 3.0f}, {5.0f, 3.0f}};   // 1px drying bundles hung under the eave
const Vector2f herbHutStandOffset(2.0f, 8.0f);   // builder hitbox top-left relative to the hut: below the shadow, left of the door
const Vector2f herbBedOffset(9.0f, 0.0f);        // bed top-left relative to the hut: a 2px grass gap to its right
const Vector2f herbBedSize(11.0f, 7.0f);         // tilled soil; bottom row a shade darker
const int kHerbCols = 3, kHerbRows = 2;
const int kHerbPlants = kHerbCols * kHerbRows;
const Vector2f herbPlantPitch(4.0f, 3.0f);       // 2x2 plants at (1 + 4c, 1 + 3r) in the bed: 2px soil between them
const Vector2f herbTendOffset(4.0f, 8.0f);       // tender hitbox top-left relative to the bed: under the middle column
const Vector2f herbZoneSize(20.0f, 13.0f);       // hut + gap + bed, with both stand spots below (elf height)
const float herbClearPx = 3.0f;                  // houses, plots, mills, walls and towers keep this far from the zone
const float herbRoadPadPx = 1.0f;                // quarter roads keep this far from the zone
const int kHerbRipenTicks = 8 * ticksPerGameHour; // a summer plant ripens from bare in 8 game hours
const float herbSeasonGrowth[seasonCount] = {0.6f, 1.0f, 0.5f, 0.0f};   // growth rate by season: none in winter
const int kHerbYield = 1;                        // herbs per ripe plant tended...
const int kHerbBloomYield = 2;                   // ...or per plant still in bloom from harvest day
const int kHerbStockMax = 24;                    // stock cap: a well-tended year fills it by autumn
const int kHerbSpoilTicks = 3 * ticksPerGameHour; // winter: one herb spoils per 3 game hours (16 over a dry winter)...
const int kHerbRainSpoilMult = 2;                // ...twice as fast in rain, so a wet, poorly stocked winter runs dry
const float kHerbJoinRadius = 70.0f;             // idle elves within this of the bed may come to tend...
const int kHerbJoinOneIn = 180;                  // ...each idle tick with chance 1 in this (~3 s at 1x)
const int kHerbTendTicks = 240;                  // work ticks at the bed (~4 s) to pick every ripe plant
const Color herbHutWallColor(112, 92, 66);       // pale weathered timber, lighter than the barracks
const Color herbHutRoofColor(128, 134, 92);      // sage thatch, near the elf hat
const Color herbHutEaveColor(98, 104, 70);       // eave row, one step darker
const Color herbHutDoorColor(30, 24, 20);
const Color herbBundleColor(150, 122, 160);      // dried lavender: dull, so the live flowers stay the bright purple
const Color herbBedColor(58, 42, 28);            // tilled soil, warm brown on the dark green
const Color herbBedEdgeColor(42, 30, 20);        // bottom row of the bed
const Color herbLeafColor(84, 136, 72);          // live leaves: muted green, lighter than the grass
const Color herbBudColor(150, 168, 122);         // half-grown: one pale bud pixel
const Color herbFlowerColor(194, 164, 222);      // ripe: light purple, two pixels
const Color herbBloomColor(228, 204, 246);       // harvest-day bloom: paler and brighter, with a third pixel on top
const Color herbBrownColor(108, 80, 50);         // winter: browned leaves, no flower
const Color herbStockColor(196, 168, 222);       // HUD herb count tint (the flower purple, lifted for the text)

// UI red (UIO-F1/F2/F3): one muted brick red for every "starving" mark in the UI (minimap dots, inspect panel, hunger lens)
const Color uiStarvingRed = healthBarFillColor;
// Minimap (UIO-F1): bottom-right 120px map of the whole worldSize x worldSize world, drawn after the HUD in the default (UI) view.
// No trees: only barbs, houses and storehouses, plus the camera's viewport. Left-click recentres the camera.
const float MM_SIZE = 120.0f;                 // outer size in screen px, border included
const float MM_MARGIN = 8.0f;                 // gap from the window's bottom and right edges
const float MM_BORDER_PX = 1.0f;              // drawn inward; the world maps onto the 118px inside it
const Color MM_BG(10, 14, 12, 170);           // near-black like the HUD strip, translucent so the world shows through
const Color MM_BORDER(70, 80, 66, 200);       // same dim green-grey as the HUD rim
const float MM_BARB_PX = 1.0f;
const float MM_HOUSE_PX = 2.0f;
const float MM_STORE_PX = 3.0f;
const Color MM_BARB_COLORS[raceCount] = {     // muted but lighter than the crate colours so 1px dots read on the backing
    Color(214, 180, 136),                     // human: warm tan
    Color(178, 174, 164),                     // dwarf: pale stone
    Color(158, 198, 138),                     // elf: light sage
};
const Color MM_STORE_COLORS[raceCount] = {   // crates: the crate colours lifted ~30% toward white so they stand out from houses
    Color(176, 151, 123),
    Color(158, 155, 148),
    Color(152, 170, 142),
};
const Color MM_VIEW_COLOR(235, 235, 225, 200); // thin near-white camera rect
// Inspect panel (UIO-F2): left-click a barb to select it; compact panel top-right under the HUD strip, ring on the barb.
const float inspectMargin = 8.0f;             // gap from the right edge and below the HUD strip (same as MM_MARGIN)
const Vector2f inspectPad(7.0f, 5.0f);
const float inspectMinWidth = 120.0f;         // panel never narrower than this, so short names don't make a sliver
const float inspectRowGap = 3.0f;             // vertical gap between rows
const float inspectLabelGap = 6.0f;           // "Hunger"/"Health" label -> bar
const unsigned int inspectFontSize = 12;      // same size as the tooltips
const Color inspectPanelColor(10, 14, 12, 170);   // same translucent near-black backing as the minimap
const Color inspectBorderColor(70, 80, 66, 200);  // same dim green-grey rim as the minimap
const Color inspectNameColor(240, 232, 205);      // name a touch brighter than the body lines
const Color inspectTextColor(205, 200, 180);      // ticker cream
const Color inspectHungerFill(204, 168, 92);      // muted ochre while fed; uiStarvingRed once hungry
const Color inspectHealthFill(118, 172, 96);      // muted leaf green, distinct from the red
const float selectRingRadius = 4.5f;          // world px around the barb's centre: clears the 2x4 body and 2px hats
const float selectRingPx = 1.0f;              // screen px (scaled by zoom), thin like the hover outline
const Color selectRingColor(240, 210, 90, 230);   // tooltip-outline yellow
// R44-F1: job target line. Hovered or selected barb -> faint dotted 1px line from its centre to its work target, plus a
// small pulsing ring on the target. Dash/gap/line length are screen px (scaled by zoom like the outlines).
const Color targetLineColor(236, 226, 170);   // pale cream-yellow; alpha comes from targetLineAlpha
const uint8_t targetLineAlpha = 90;           // faint: the world shows through, reads on the dark green and at night
const float targetDashPx = 3.0f;              // dash length, screen px
const float targetGapPx = 3.0f;               // gap between dashes, screen px
const int targetMaxDashes = 160;              // long lines at far zoom stretch the dashes instead of adding more
const float targetLineMinDist = 8.0f;         // world px: closer than this (working at the spot) draws no line, ring only
const Color targetRingColor(222, 210, 140);   // muted pale yellow, a step softer than the select ring
const float targetRingRadius = 3.0f;          // world px at rest, smaller than the 4.5px select ring
const float targetRingPulse = 1.0f;           // world px added at the pulse peak
const float targetRingPulseSpeed = 0.08f;     // radians per frame (~1.3s per pulse at 60fps)
const uint8_t targetRingAlphaLo = 110, targetRingAlphaHi = 190;   // alpha swing over one pulse
// R44-F2: idle and ready beacons. Holding H brackets ripe bushes, ripe wheat and idle barbs with four small corner
// marks, culled to the camera, drawn above the day/night overlay. Arm/pad are world px; line width is screen px.
const Color beaconColor(240, 240, 232);       // soft white, reads on the dark green and through the night tint
const uint8_t beaconAlphaLo = 120, beaconAlphaHi = 210;   // gentle pulse range
const float beaconPulseSpeed = 0.06f;         // radians per frame (~1.7s per pulse at 60fps)
const float beaconPadPx = 1.5f;               // world px gap between the target's box and the bracket
const float beaconArmPx = 2.0f;               // world px corner arm length (clamped to half the box side)
const float beaconLinePx = 1.0f;              // screen px (scaled by zoom), a hairline like the outlines
const int beaconMaxCount = 120;               // cap per frame: idle barbs first, then wheat, then bushes
const float beaconFadeZoom = 2.0f;            // below this zoom the beacons dim...
const float beaconFarAlphaMult = 0.55f;       // ...to this share of their alpha at minZoom (linear in between)
// Overlay lenses (UIO-F3): L cycles Off -> Hunger -> Race. Soft discs on the ground layer, under barbs and buildings;
// only barbs, houses and storehouses are looped (never trees), culled to the camera. Legend panel top-left under the HUD.
enum class Lens { Off, Hunger, Race };
const char* const lensNames[] = {"Off", "Hunger", "Race"};
const uint8_t lensCentreAlpha = 60;           // low-alpha tint at the disc centre...
const uint8_t lensRimAlpha = 18;              // ...fading toward the rim so the ground still reads
const int lensDiscSegments = 16;
const float lensBarbRadius = 5.0f;            // world px: just past the 2-3px barb bodies and hats
const float lensHouseRadius = 9.0f;           // around the 4px house plus its roof
const float lensStoreRadius = 12.0f;          // around the 10px crate
const Color lensFedColor(118, 186, 96);       // muted leaf green: just ate
const Color lensHungryColor(214, 170, 84);    // muted ochre: at the hunger threshold
// starving end of the gradient is uiStarvingRed (same red as the minimap and inspect panel)
const Color lensRaceColors[raceCount] = {     // muted but distinct hues so quarters separate at a glance
    Color(222, 150, 84),                      // human: amber
    Color(140, 156, 214),                     // dwarf: steel blue
    Color(120, 214, 196),                     // elf: pale teal (not grass green, so it reads on the background)
};
const float lensMargin = 8.0f;                // legend gap from the left edge and below the HUD strip
const Vector2f lensPad(7.0f, 5.0f);
const float lensRowGap = 3.0f;
const float lensSwatchPx = 8.0f;              // legend colour square
const float lensSwatchGap = 5.0f;             // swatch -> label
const unsigned int lensFontSize = 12;         // same as the inspect panel and tooltips
const Color lensTitleColor(240, 232, 205);    // inspect-name cream
const Color lensTextColor(205, 200, 180);     // ticker cream
const Color lensSwatchRim(20, 24, 20, 200);   // dark 1px rim so pale swatches don't bleed into the backing
// Race stock panel (RACE-F1): top-left under the HUD strip (the lens legend stacks below it). One row per race:
// race swatch + name, then wood/stone/food as 8px icons + numbers. Numbers below stockShortThreshold turn stockShortColor.
const float stockMargin = lensMargin;         // same gaps and backing as the lens legend
const Vector2f stockPad = lensPad;
const float stockRowGap = lensRowGap;
const float stockSwatchPx = lensSwatchPx;     // 8px race square, same as the legend swatches
const float stockSwatchGap = lensSwatchGap;   // swatch -> race name
const float stockIconPx = 1.0f;               // icon cell size: the strip's 8x8 icons at 1x (8px), about one text line
const float stockIconGap = 3.0f;              // icon -> number
const float stockCellGap = 9.0f;              // name -> first icon, number -> next icon
const char* const stockMinNumber = "00";      // each number column reserves at least this width
const unsigned int stockFontSize = 12;        // same as the lens legend and inspect panel
const Color stockSwatchColors[raceCount] = {MM_STORE_COLORS[0], MM_STORE_COLORS[1], MM_STORE_COLORS[2]}; // lifted crate colours: read on the dark backing
const Color stockShortColor(222, 108, 88);    // muted brick red, a shade lighter than uiStarvingRed so 12px digits read on the backing
const Color stockEmptyRaceColor(120, 118, 108); // a race with no living members: its whole row is dimmed, never red

// Race quarters (R30-F1): each race's crate sits in its own quarter around the village, at least raceMinSeparation from
// the others; that race's houses go within quarterHouseRadius of it. Trees/rocks are kept off each quarter like the
// village clearing, lakes and bush clumps avoid it, and each quarter is topped up at startup if it is short of wood/stone.
// INBOX-F3 wider quarters, R44-F4b pushed further out (R47-F2: the village is at the map centre, worldSize/2): with 3
// races the 120 deg split gives crates >= 210*sqrt(3) ~ 364 apart (> raceMinSeparation), and at any rotation the quarter
// clearing (63) reaches at most 250+63 = 313 from the campfire, well inside the edges and HUD (1200 / 1176 away), and
// stays >= 210-63 = 147 clear of the village clearing (50). quarterLayoutSweepOk() re-checks this at startup over a full
// turn of rotations at both radii, so quarterLayoutOk() always passes and the fallback should never fire.
// Lakes are placed after the quarters and keep quarterClearRadiusFor() (step 0) + lakeMinGap off each one.
const float raceMinSeparation = 260.0f;      // min distance between two races' crates (startup check; R47-F3: was 185)
const bool quarterForceFallback = false;     // R47-F3 test switch: true skips the random layout so the fixed fallback runs
// R47-F3 Frontier Spacing: each race's frontier is the disc of frontierRadius round its crate centre. Crates are at least
// raceMinSeparation apart, so frontiers never touch and the land between them is a gap the trader walks through. The wide
// work look (barbWorkFallbackRadius) skips targets inside another race's frontier. Shown only as a faint dotted circle
// on the minimap; nothing is drawn in the main view.
const float frontierRadius = raceMinSeparation / 2.0f;   // 130 = houseSpreadFar at step 0 (R47-F10 steps may reach past it)
const float frontierMinimapDotGap = 3.0f;    // minimap px between border dots (1px each)
const uint8_t frontierMinimapAlpha = 80;     // faint: under the house/crate/barb dots on the dark backing
// R51-F4 Race Relations, phase 1: raceRelation[a][b] is SYMMETRIC (setRelation writes both halves; the diagonal stays 0),
// clamped to [kRelationMin, kRelationMax]. It drops when barbs of two races claim work next to each other inside a
// frontier, drifts back to neutral (0) slowly, and a cold pair makes the trader charge the served race more.
// Phase 1 has no warming source; caravans (deferred) must track barbs by stable id with abort + refund, never index pairs.
const float kRelationMin = -1.0f;
const float kRelationMax = 1.0f;
const float kRelationClashDelta = 0.1f;      // drop per counted clash
const int kRelationClashCooldownTicks = ticksPerGameHour / 4;   // per pair: at most 4 counted clashes a game hour
const float kRelationClashRadius = 40.0f;    // a foreign-claimed tree/rock this close to a new claim is a clash
const float kRelationDriftPerTick = 1.0f / (12.0f * ticksPerGameHour);   // full -1 -> 0 recovery in 12 game hours
const float kRelationUneasy = -0.2f;         // at or below: "uneasy" tier (ticker line)
const float kRelationCold = -0.5f;           // at or below: "cold" tier (ticker line, trade toll)
const float kTradePriceMultMax = 1.5f;       // cap on the cold-border trade price multiplier (reached at kRelationMin)
const float kTradePriceMultCold = 1.25f;     // multiplier right at kRelationCold, rising linearly to the cap
const float kRelationMinimapLenPx = 6.0f;    // minimap px: border segment length, across the line between two crates
const float kRelationMinimapPx = 2.0f;       // minimap px: segment thickness (frontier dots are 1px, so it reads over them)
const Color kRelationNeutralColor(150, 168, 196, 230);   // pale slate blue: apart from the tan/grey/sage race tints
const Color kRelationColdColor(206, 84, 70, 240);        // muted brick red
const Color kRelationTickerColdTint(222, 140, 120);      // ticker tint for a pair getting worse
const float quarterMinRadius = 210.0f;       // crate centre distance from the campfire (R44-F4b: was 165)
const float quarterMaxRadius = 250.0f;       // (R44-F4b: was 200)
const float traderGapRadius = quarterMaxRadius;   // R47-F3: trader's waypoint on a gap's bisector, this far from the campfire
const int quarterPlaceTries = 50;            // random layouts tried before the fixed fallback layout
const int quarterSweepSteps = 360;           // R47-F2: startup check of quarterLayoutOk's rules at every 1 deg rotation
const float quarterHouseRadius = 56.0f;      // house spot within this of the crate centre (+25% over R30-F1's 45)
const float quarterClearMargin = 7.0f;       // clearing reaches past the house disc: 5px slab half + 2
const float quarterResourceRadius = 140.0f;  // startup resource check: trees/rocks between the clearing and this radius
const int quarterMinTrees = 25;              // fewer than this nearby -> plant more (expected ~140 at 2000 trees)
const int quarterMinRocks = 3;               // fewer than this nearby -> place more (expected ~7 at 100 rocks)
const int quarterTopUpTries = 400;           // placement attempts per quarter for the top-up
const unsigned quarterReseedSeed = 3001;     // fixed seed (+ race) so the top-up is deterministic
const float quarterRingMinZoom = 2.0f;       // ring hidden below this zoom
const float quarterRingDotGap = 6.0f;        // world px between ring dots (1px each)
const uint8_t quarterRingAlpha = 30;         // very faint, in the race's crate colour
// R44-F4a Outlying Homesteads: every homesteadEvery-th house a race claims is tried as a lone homestead in a wider ring
// houseSpreadNear..houseSpreadFar px from its crate centre, on the side facing away from villageCenter (within
// homesteadArc of straight out). Up to homesteadTries spots per claim; if none passes, the normal quarter disc is used.
// Quarter radii (quarterMin/MaxRadius, quarterHouseRadius) are unchanged in this phase (R47-F10 grows the house radii).
const float houseSpreadNear = 90.0f;         // homestead distance from the crate centre
const float houseSpreadFar = 130.0f;
const int homesteadEvery = 4;                // every 4th house of a race is tried as a homestead
const float homesteadArc = 1.0471976f;       // +-60 deg around the crate's outward direction
const int homesteadTries = 12;               // spots rolled per claim before falling back to the quarter disc
const float homesteadHouseGap = 30.0f;       // no homestead within this of any house (or pending house spot), any race
const float homesteadClutterPad = 2.0f;      // grass kept between the slab and a tree/rock/bush footprint
// R47-F10 Living Build Radius: quarterHouseRadius / houseSpreadFar above are the step-0 values. A race's build radii grow
// in steps with its living population (quarterRadiusFor / houseSpreadFarFor / quarterClearRadiusFor); the step is a
// high-water mark, so it never shrinks. A step is taken only if the new reach disc (houseSpreadFar + slab half) passes
// layoutDiscOk and stays clear of every neighbour's current reach disc; a rejected step leaves the mark unchanged.
// With 3 steps the quarter clearing tops out at 56+24+7 = 87 < houseSpreadNear, so homesteads still read as outside it.
const int quarterStepMax = 3;                 // steps above the startup radii
const int quarterStepBasePop = 10;            // living barbs of a race for step 1 (5 founders each)
const int quarterStepPopEach = 6;             // each further step needs this many more
const float quarterStepPx = 8.0f;             // quarter house disc (and its clearing) grows this much per step
const float houseSpreadStepPx = 8.0f;         // homestead ring's far edge grows this much per step
const float quarterReachRingAlpha = 60.0f;    // hovered crate: faint reach ring in the race's lens colour (1 screen px)
const int quarterReachRingPoints = 72;        // smooth at ~150px radius
const int quarterPulseTicks = 75;             // dust ring on a step: expands old -> new clearing edge, then fades
const float quarterPulseOvershoot = 4.0f;     // ends this far past the new clearing edge
const Color quarterPulseColor(150, 132, 108); // grey-brown dust, a touch lighter than the shaft puffs
const uint8_t quarterPulseMaxAlpha = 110;
const float quarterPulseThickness = 1.0f;

// Lakes (R19-F2): static ponds placed once at startup. Static rect + rim only in v1 (no glint/ripples).
// Only wander paths (barbs, deer) avoid lakes; task paths (chop/mine/build/hunt/flee/wolves) are straight lines and may cross
// one. Nothing is ever placed in a lake, so those crossings are brief, and an animal that ends up inside can always wander out.
const int lakeCountMin = (int)(2 * worldAreaScale);   // R45-F1a: 2-3 per 800x800 of map (4-6 at 1200)
const int lakeCountMax = (int)(3 * worldAreaScale);
const int lakePlaceTries = 60;             // per lake; fewer lakes are kept if a spot can't be found
const float lakeVillageMinDist = 150.0f;   // lake centre at least this far from the campfire (clearing is 50, shrines 40)
const float lakeEdgeMargin = 12.0f;        // from the map edges (and below the HUD strip)
const float lakeMinGap = 30.0f;            // between two lakes
const float lakeClearPx = 3.0f;            // trees, rocks, bushes, builds and wander targets keep this far from the water
const float lakeShorePx = 1.0f;            // shore points sit this far outside the bank
// Blocky pixel lakes (R23-F1): each lake is a grid of lakeTilePx tiles inside its rect, with up to one tile trimmed per
// corner for a rounded, uneven outline. Collision still uses the full rect.
struct lakeSizeDef { int cols, rows; bool shallows; }; // shallows: outermost tile ring is shallow water
const float lakeTilePx = 8.0f;
const lakeSizeDef lakeSizes[] = {
    {4, 3, false},                         // small: all deep
    {5, 4, true},                          // medium
    {7, 5, true},                          // large
};
const int lakeSizeCount = sizeof(lakeSizes) / sizeof(lakeSizes[0]);
const float lakeCornerTrimChance = 0.75f;  // per corner: drop that corner tile
const float lakeBankPx = 2.0f;             // bank band around the tile outline (blocky, follows the trimmed corners)
const Color lakeDeepColorA(60, 95, 140);   // muted desaturated blue: darker and greyer than the rain streaks/tint
const Color lakeDeepColorB(56, 90, 134);   // checker partner, kept very close to A
const Color lakeShallowsColor(82, 112, 146); // deep blue pulled toward the bank
const Color lakeBankColor(118, 138, 150);  // muted grey-blue bank

// Far Reaches (R45-F1b): one ruin site far out from the village (placed once at startup by spawnRuins, after the
// quarters, lakes and bushes), a ridge of ordinary mineable rocks arcing behind it, and one lean-to camp per race beside
// it, built through buildOneOff (jobCamp). Built camps are entries in `camps`; a claim is "a barb of that race holds
// jobCamp" (no stored flag). Everything is placed relative to worldSize, so it moves out with a bigger map.
const float ruinMinDist = 260.0f;          // site centre at least this far from the campfire (relaxed to ruinFallbackDist)
const float ruinFallbackDist = 170.0f;     // second pass if no spot passes at ruinMinDist
const float ruinSiteRadius = 40.0f;        // trees, rocks and bushes cleared within this of the site centre; camps sit inside
const float ruinEdgeMargin = 12.0f;        // site disc keeps this far from the map edges and the HUD strip
const float ruinQuarterGap = quarterResourceRadius + ruinSiteRadius; // site centre -> any crate: outside the startup top-up ring
const float ruinCoreMinDist = zoneCoreRadius + ruinSiteRadius;     // R47-F2: whole site disc outside the safe core (460)
const int ruinPlaceTries = 200;            // per pass
const int ruinStoneCount = 6;              // standing stones in a rough ring, plus one fallen stone in the middle
const float ruinRingRadius = 14.0f;        // ring point -> site centre (stone base centre sits on it)
const float ruinRingJitter = 0.25f;        // radians of random twist per stone
const Vector2f ruinStoneSize(6.0f, 10.0f); // a whole standing stone (broken ones are shorter)
const float ruinBrokenH[2] = {6.0f, 4.0f}; // heights of the two broken stones
const Vector2f ruinFallenSize(10.0f, 6.0f); // the fallen stone, lying on its side in the middle
const Color ruinStoneColor(168, 162, 144); // pale muted sandstone: lighter and warmer than rocks (128) and shrines (138)
const Color ruinStoneTopColor(192, 186, 166);   // 1px weathered cap row
const Color ruinStoneShadeColor(128, 122, 108); // 1px shaded right column
const Color ruinMossColor(88, 108, 72);    // 1px moss at the foot of every other stone
const float ruinHoverPad = 1.0f;           // hover rect grows this much round each stone
// Faint glow over the ruins in the evening tint hours (eveningStartHour..nightStartHour-1), additive like the campfire
const int ruinGlowLayers = 3;
const float ruinGlowRadius = 30.0f;        // outer layer radius (world px)
const Color ruinGlowColor(150, 140, 200);  // pale violet: clear of the orange campfire and warm lamp light
const uint8_t ruinGlowLayerAlpha = 9;      // per layer; ~27 at the centre, a hint rather than a light
const float ruinGlowRainMult = 0.35f;      // rain dims the glow to this fraction
const int ruinGlowBreathFrames = 240;      // slow 4s breathing
const float ruinGlowBreathDepth = 0.25f;   // alpha swings +-25%
// Rock ridge: an arc of ordinary rocks behind the ruins (the side away from the village)
const float ridgeRadius = 50.0f;           // arc radius from the site centre (rock centres), just outside the cleared disc
const float ridgeHalfSpan = 1.2f;          // radians either side of straight away from the village
const float ridgeStepPx = 11.0f;           // arc length between rock centres: 12px rocks just touch
const float ridgeJitterPx = 3.0f;          // radial jitter per rock, so the ridge reads uneven
// Lean-to camps: one per race at the ruins, on the village-facing side, fanned by race
const int campWoodCost = 10;               // paid at claim from the race's stock (houseWoodCost kept back first)
const int campMinHouses = 3;               // the race's own houses before it sends a builder that far out
const int campBuildTicks = 400;            // lighter work than a house (600)
const float campRuinDist = 32.0f;          // camp centre -> site centre: clear of the tallest stones (ring 14 + 10)
const float campSpreadRad = 0.6f;          // angle between neighbouring races' camps (~19px apart at campRuinDist)
const Vector2f campSize(5.0f, 4.0f);       // 5x4 lean-to: smaller than a house (8x7) and a smokehouse (6x5)
const Vector2f campStandOffset(-3.0f, 1.0f); // builder's spot relative to the lean-to top-left: at its left side
const Color campPoleColor(104, 78, 50);    // rough timber poles
const Color campInsideColor(26, 22, 18);   // dark open front under the roof
// MM markers: ruins as a pale square, camps as a small dot in the lifted crate colour
const float MM_RUIN_PX = 3.0f;
const Color MM_RUIN_COLOR(206, 198, 176);
const float MM_CAMP_PX = 2.0f;

// R47-F4 Wayshrines, Huts and Wells: small sites scattered by band at startup (placePoi, after the ruins). Held in `pois`
// with a parallel `poiShapes` footprint vector; both change only together (removePoi). A site is found when any barb
// passes within poiDiscoverRadius or a party reaches it; a party searches it once for a small reward, and it is spent.
// Spent sites stay dimmed for poiSpentLingerTicks, then crumble away (removePoi). Phase 1: huts and wells only.
enum poiKind { poiHut, poiWell, poiKindCount };
const char* const poiNames[poiKindCount] = {"hermit hut", "sunken well"};
const Vector2f poiSizes[poiKindCount] = {Vector2f(5.0f, 4.0f), Vector2f(6.0f, 6.0f)};   // 5x4 lean-to, 6px ring
const int poiBandCounts[zoneCount][poiKindCount] = {   // [band][kind]: none in the safe core, huts in the wilds, wells at the rocky edge
    {0, 0},
    {4, 2},
    {1, 3},
};
const int poiPlaceTries = 300;             // per site; a site that finds no spot is skipped
const float poiEdgeMargin = 16.0f;         // site centre from the map edges and the HUD strip
const float poiMinGap = 150.0f;            // site centre -> any other site centre
const float poiRuinGap = 90.0f;            // site centre -> ruin centre: clear of the cleared disc (40) and the ridge (50)
const float poiQuarterGap = quarterResourceRadius + 60.0f; // site centre -> any crate
const float poiClearPad = 4.0f;            // trees, rocks and bushes kept this far off a site's footprint (startup and regrowth)
const float poiDiscoverRadius = 40.0f;     // a barb centre this close to a site centre finds it
const int poiDiscoverEveryTicks = 30;      // the discovery scan runs this often (O(sites x barbs))
const int poiSpentLingerTicks = 36000;     // ~10 min at 1x: a spent site stays, dimmed, then crumbles away
const int poiSearchTicks = 300;            // ~5s at 1x: a party searching a site (shorter than a ruin delve)
// Rewards: small and varied, rolled once per search. Food chance, then stone chance, else a relic (stone if relics are capped)
struct poiRewardDef { float foodChance, stoneChance; };
const poiRewardDef poiRewards[poiKindCount] = {
    {0.50f, 0.25f},                        // hut: the hermit's stores; 25% relic
    {0.30f, 0.50f},                        // well: dressed stone from the shaft; 20% relic
};
const int poiFoodMin = 3, poiFoodMax = 6;  // food reward range
const int poiStoneMin = 2, poiStoneMax = 4; // stone reward range
// Hut: neutral thatch (not a race colour), sloping down to the left (camps slope right), stone chimney with a smoke pixel
const Color poiThatchColor(146, 126, 84);
const Color poiThatchLitColor(170, 150, 104);
const Color poiThatchEaveColor(116, 98, 66);
const Color poiHutWallColor(96, 72, 48);
const Color poiHutInsideColor(30, 24, 18);
const Color poiChimneyColor(112, 108, 102);
const Color poiSmokeColor(196, 196, 190, 150);
const int poiSmokeFrames = 24;             // smoke pixel rises one row every this many frames, 3 rows then restarts
// Well: grey stone ring (lit top, shaded bottom) round dark water with a 1px glint; dry earth once spent
const Color poiWellStoneColor(122, 120, 112);
const Color poiWellStoneLitColor(152, 150, 140);
const Color poiWellStoneShadeColor(92, 90, 84);
const Color poiWellWaterColor(38, 58, 84);
const Color poiWellWaterShadowColor(22, 32, 46);
const Color poiWellGlintColor(128, 166, 196);
const Color poiWellDryColor(48, 40, 30);
const float poiSpentShade = 0.7f;          // spent sites draw at this brightness
// Glow (additive, after the day/night overlay, any hour): found = soft violet, spent = one faint small layer
const Color poiGlowColor(150, 140, 200);   // same muted violet as the ruins
const int poiGlowLayers = 2;
const float poiGlowRadius = 12.0f;
const uint8_t poiGlowLayerAlpha = 10;
const float poiSpentGlowRadius = 6.0f;
const uint8_t poiSpentGlowAlpha = 4;
const Color MM_POI_COLOR(176, 158, 214);   // found sites on the minimap (undiscovered ones are not shown)
const Color MM_POI_SPENT_COLOR(96, 90, 116);
const float MM_POI_PX = 2.0f;

// Expeditions (R45-F2, phase 1): one party at a time of expPartySize same-race barbs walks from its crate to its race's
// lean-to, delves at the ruin stones, and walks home with a relic (or nothing). Rations are paid from the race's food.
const int expPartySize = 2;                // members per party, all one race
const int expRelicCap = 3;                 // relics a race can ever bring home; a capped race sends no more parties
const int expRationFood = 4;               // food taken from the race's stock at departure; leftovers go back on return
const int expFoodReserve = 6;              // the race keeps at least this much food after paying the rations
const int expLaunchOneIn = 2400;           // 1-in-N per tick that a launch is tried while no party is out
const int expFirstDelayTicks = 7200;       // ~2 min at 1x before the first party can leave
const int expCooldownTicks = 5400;         // after any party ends (home or broken up) before the next can leave
const float expRecruitRadius = 140.0f;     // members are the nearest eligible barbs within this of their crate
const int expMinHealth = 60;               // a wounded barb stays home
const int expDelveTicks = 600;             // ~10s at 1x of digging once both members stand at their stones
const float expRelicChance = 0.6f;         // per delve; otherwise the party comes home empty-handed
const int expRelicStone = 3;               // modest reward: worked stone the relic is set in, added to the race's stock
const float expMemberGapPx = 4.0f;         // members stand this far apart at the camp and the crate
const float expWolfRadius = 24.0f;         // a wolf this close (centre to centre) to a member on the road wounds it
const int expWolfWoundHp = 25;             // hp a wolf takes per wound
const int expWolfMinHp = 20;               // wolves wound, never kill: a wound never takes a member below this
const int expWolfWoundCooldownTicks = 300; // ticks between wounds on one party
const Color relicGlintColor(236, 224, 178);    // 1px relic beside the carrier on the walk home: pale cream gold
const Color relicGlintDimColor(170, 158, 120); // twinkle low phase
const int relicGlintFrames = 12;               // glint alternates bright/dim every this many frames

// R47-F5 The Hollow: a 6x6 stair mouth inside the ruin site disc (placed by spawnRuins, beside the stones, off the
// camps and the ridge). A party of a race with a lean-to walks straight to it, descends (members hidden, the one room
// abstracted as expDelveTicks below), surfaces with one roll of a capped loot table and walks home. No HP, wounds or
// panel below. A party that breaks up (a member lost) is released at the mouth and its rations go back to the larder.
const Vector2f hollowSize(6.0f, 6.0f);     // same footprint as a well; smaller than a house (8x7)
const float hollowRuinDist = 27.0f;        // mouth centre -> site centre: past the stone ring (14 + 10), inside the disc (40)
const float hollowPickChance = 0.5f;       // when both the ruins and the Hollow are open and nearest, chance of the Hollow
const int hollowLootCap = 5;               // hauls the one room ever gives; then it is picked clean and never a target again
struct hollowLootDef { const char* what; int food, stone; float weight; };
constexpr int hollowLootCount = 4;
constexpr int hollowLootMaxFood = 5, hollowLootMaxStone = 4;   // per haul: modest, like the sites (food 3-6, stone 2-4)
constexpr hollowLootDef hollowLoot[hollowLootCount] = {
    {"a jar of old grain",   5, 0, 0.35f},
    {"dressed stone blocks", 0, 4, 0.35f},
    {"a small buried hoard", 2, 2, 0.15f},
    {"nothing but dust",     0, 0, 0.15f},
};
constexpr bool hollowLootCapped() {
    for (int i = 0; i < hollowLootCount; i++)
        if (hollowLoot[i].food > hollowLootMaxFood || hollowLoot[i].stone > hollowLootMaxStone || hollowLoot[i].weight <= 0.0f) return false;
    return true;
}
static_assert(hollowLootCapped(), "every Hollow haul must stay within hollowLootMaxFood / hollowLootMaxStone");
// Look: pale stone lintel and jambs (the ruin palette) round a dark opening with three 1px steps fading into the dark
const Color hollowLintelColor(192, 186, 166);   // = ruinStoneTopColor
const Color hollowJambColor(168, 162, 144);     // = ruinStoneColor
const Color hollowJambShadeColor(128, 122, 108); // = ruinStoneShadeColor, right jamb
const Color hollowDarkColor(18, 16, 14);
const Color hollowStepColors[3] = {Color(96, 90, 78), Color(62, 58, 50), Color(38, 35, 30)};   // top step lit, deeper ones darker
const Color hollowCleanStepColor(44, 41, 36);   // picked clean: all steps one flat dull tone
const int hollowFlashTicks = 40;           // lintel flash countdown when a party surfaces (~0.7s at 1x)
const float hollowFlashMix = 0.7f;         // peak blend of the lintel toward the flash colour
const Color hollowFlashColor(246, 240, 214);    // pale cream-white
const Color lootGlintColor(214, 218, 224);      // 1px loot beside the carrier on the walk home: pale silver, apart from the relic gold
const Color lootGlintDimColor(150, 154, 162);

// Lakeside angling (R19-F1): a barb with no tree/rock/bush in reach walks to a free cached shore point and fishes.
// One angler per shore point; stand spots sit on the bank and are never lakeClear()-checked (they are inside the keep-clear zone).
const float fishSearchRadius = 60.0f;      // wider than barbWorkSearchRadius: lakes are few and away from the village
const int fishCatchTicks = 360;            // ~6s per catch at 1x
const int fishFoodYield = 1;               // below a deer's 2 food
const Color barbFishColor(90, 225, 225);   // bright cyan: far lighter than the pond, clear of build blue and timber teal
const float fishRodPx = 3.0f;              // 1px diagonal rod reaching from the hand out over the water
const Color fishRodColor(90, 225, 225);    // same cyan as the task stripe
const float fishLineReach = 5.0f;          // line runs this far from the rod tip into the water
const float fishLineDrop = 2.0f;           // side-on casts: the line also hangs this far down
const Color fishLineColor(185, 240, 240, 170); // thin pale cyan, a hairline at any zoom
const int fishRingTicks = 45;              // catch ring lifetime (~0.75s)
const float fishRingStartRadius = 1.0f;
const float fishRingEndRadius = 6.0f;
const float fishRingThickness = 1.0f;
const Color fishRingColor(190, 235, 235);  // pale cyan ripple on the water
const uint8_t fishRingMaxAlpha = 150;      // fades linearly to 0

// Farming (R19-F3): 8x8 wheat plots, sown once by an idle barb, then regrowing forever.
// Ripe is derived from growth (no stored flag); no scarecrow. The harvest shares harvestTick() with berry picking.
// Race farmsteads (FARM-F1): each race sows fieldsPerRace plots beside its own storehouse; only that race sows/reaps them.
// R32-F1 Mill Fields: plot k (k > 0) unlocks at fieldMinHouses*k+1 homes and is sown in a fixed slot beside the mill.
constexpr int fieldsPerRace = 4;            // R46-F2b: 4th plot, opened by a 4th home or 4 * popPerPlot barbs
const Vector2f fieldSize(8.0f, 8.0f);
const float fieldBorderPx = 1.0f;          // race-tinted frame drawn outside the soil (10x10 overall)
const int fieldWoodCost = 6;               // cheap: fences and a hoe
const int fieldMinHouses = 1;              // a race's first home (and its storehouse) comes before its field
const int popPerPlot = 4;                  // R46-F2a: every popPerPlot living barbs of a race also open one plot (capped)
const int fieldBuildTicks = 600;           // same work as a house
const int fieldGrowTicks = 10800;          // ~3 min from sowing to ripe at 1x (3 growing stages of ~1 min each)
const int fieldGrowStages = 3;
const int fieldHarvestTicks = 240;         // ~4s to reap
const int fieldFoodYield = 2;
const float fieldSearchRadius = 60.0f;     // like angling: few plots, so idle barbs look a bit farther for one
constexpr float fieldStoreMinRadius = 16.0f; // FARM-F1: plot centre ring around its race's crate centre, kept inside
constexpr float fieldStoreMaxRadius = 30.0f; // the quarter clearing so no tree/rock can stand on it (see fieldInClearing)
const int fieldPlaceTries = 20;            // per tick until a spot is found
const float fieldClearPx = 3.0f;           // houses, shrines and trees keep this far from the plot and its stand spot
const Vector2f fieldStandOffset(3.0f, 10.0f); // builder/harvester hitbox top-left relative to the soil: just below the frame
const float fieldZoneBelowPx = 6.0f;       // reserved below the stand spot's top: the tallest barb body (5) + 1
const Color fieldSoilColor(110, 80, 45);
// FARM-F1: frame + stand-spot pixel. The crate colours dimmed ~15%, so they stay well under the bright task colours.
constexpr Color fieldRaceTints[raceCount] = {
    Color(121, 90, 56),                       // human: dark timber
    Color(99, 95, 87),                        // dwarf: dark stone
    Color(92, 114, 80),                       // elf: dark sage
};
const Color fieldSproutColor(104, 150, 66);   // yellow-green, lighter and warmer than every pine
const Color fieldStalkColor(118, 156, 70);
const Color fieldHeadColor(196, 178, 84);     // yellowing heads (stage 3)
const Color fieldRipeColor(236, 192, 60);     // bright gold heads
const float fieldRipeStalkShade = 0.72f;      // ripe stalk rows: darker gold so the rows still read apart
const Color fieldBobColor(250, 226, 130);     // single pale-gold ear bobbing above a ripe plot
const int fieldBobFrames = 30;                // 1px up/down every ~0.5s
const Color barbHarvestColor(235, 205, 85);   // harvest stripe: gold, clear of mine orange and the other task colours
const Color fieldBarColor(200, 165, 70);      // muted gold harvest progress bar
const int bushFoodYield = 1;                  // food per picked bush (shared harvest path)
// Windmill and milling (FARM-F2): each race builds one small mill beside its first plot (buildOneOff, jobMill). Reapers
// carry the sheaf (carryWheat) to their race's mill, where it is milled for millFoodYield; with no built mill the sheaf
// goes to the storehouse (or the campfire) for fieldFoodYield. The mill spot is chosen with the plot and reserved with it.
constexpr int millFoodYield = 3;              // vs fieldFoodYield (2) for an unmilled sheaf
constexpr int millWoodCost = 8;
constexpr int millStoneCost = 2;              // millstones; houseStoneCost is kept back for the next house
constexpr int millBuildTicks = 600;           // same work as a house
constexpr Vector2f millBodySize(10.0f, 10.0f);
constexpr Vector2f millHubOffset(5.0f, 3.0f); // sail hub relative to the body top-left (upper middle of the tower)
constexpr float millSailLen = 7.0f;           // hub -> blade tip; the sweep reaches 4px above and 2px beside the body
constexpr float millSailInner = 1.0f;         // blades start this far from the hub, so the hub pixel shows
constexpr float millSailWidth = 2.0f;         // blade width, all on one side of its stock like a real sail
constexpr float millGapPx = 3.0f;             // plot frame -> nearest edge of the sail sweep
constexpr Vector2f millStandOffset(4.0f, 11.0f); // builder/delivery hitbox top-left relative to the body: just below the door
constexpr float millClearPx = 3.0f;           // houses, shrines, trees, roads and other plots keep this far from the mill zone
constexpr Vector2f millDoorPos(4.0f, 7.0f);
constexpr Vector2f millDoorSize(2.0f, 3.0f);
constexpr float millOutlinePx = 1.0f;         // drawn inward, so the body stays exactly millBodySize
constexpr Color millBodyColor(146, 136, 114);    // neutral grey-tan, no race tint
constexpr Color millOutlineColor(40, 34, 26);    // dark rim, same family as the crate rim
constexpr Color millDoorColor(52, 42, 30);
constexpr Color millSailColor(196, 188, 166, 220); // pale weathered cloth, a touch translucent so the body reads behind it
constexpr Color millHubColor(58, 48, 36);
constexpr int millTurnFrames = 180;           // sails turn ~3s after each delivery (frames, frozen while paused)
constexpr int millTurnEaseFrames = 40;        // the last ~0.7s of the turn slows to a stop
constexpr float millSailDegPerFrame = 3.0f;   // full speed: one turn every 2s
constexpr int millPuffFrames = 24;            // flour puff ~0.4s per delivery
constexpr int millPuffDots = 5;               // 1px dots drifting out from the door
constexpr float millPuffSpreadPx = 4.0f;      // how far the dots drift by the end of the puff
constexpr Color millPuffColor(236, 230, 214); // flour white
constexpr uint8_t millPuffAlpha = 110;        // faint at the start, fading to 0
// R32-F1 Mill Fields: soil top-left of plot k relative to the mill body top-left, for a mill standing right of plot 0
// (side +1). Both extra plots stack on the far side of the mill from plot 0, one above and one below the sail hub;
// mirrored across the hub for side -1. x = 19: the plot zone's left edge just touches the mill zone (sail + clear px).
// y = -11 / 12: the two plot zones touch each other, centred on the mill body. Index 0 is unused (plot 0 is placed freely).
// R46-F2b: plot 3 sits above the mill, centred on the sail hub (x = 1 mirrors onto itself), its zone's bottom edge just
// touching the top of the mill zone (y = -26); it clears plot 0, the other slots and the door lane on both sides.
constexpr Vector2f fieldSlotOffsets[] = {{0.0f, 0.0f}, {19.0f, -11.0f}, {19.0f, 12.0f}, {1.0f, -26.0f}};
static_assert(sizeof(fieldSlotOffsets) / sizeof(fieldSlotOffsets[0]) == fieldsPerRace, "one slot offset per plot");
constexpr float millDoorLanePx = 12.0f;       // kept free below the mill stand spot: the haulers' approach to the door
constexpr int fieldStaggerStages = 1;         // a replanted plot lies fallow until it is this many stages off its siblings
// R37-F1: a blocked mill slot is retried every fieldSlotRetryTicks; after fieldSlotMaxFails failed checks (or if the slot
// was unavailable when plot 0 was placed) the plot re-sites on a fallback ring around the mill's sail hub. The ring's
// inner edge clears the mill zone (sail sweep + clear px, ~10 from the hub) plus half a plot zone (~8); the outer edge
// stays about one plot farther out than the regular slots (~18-21 from the hub), so the farmstead still reads as one.
constexpr int fieldSlotRetryTicks = 600;      // ~10s at 1x between checks of a blocked slot / fallback attempts
constexpr int fieldSlotMaxFails = 3;          // failed slot checks before the plot gives up its slot and re-sites
constexpr float fieldFallbackMinRadius = 18.0f; // plot centre -> mill hub
constexpr float fieldFallbackMaxRadius = 32.0f;

// Know-how discoveries (R20-F1): practice counters unlock one-off speedups
const int treeChopTicks = 180;                // base work ticks to fell one tree
const int handAxeChopTicks = 150;             // after Hand Axe: 180/150 = 20% faster chopping
const int handAxeTrees = 40;                  // trees felled to learn Hand Axe
const int pickworkRocks = 25;                 // rocks mined to learn Pickwork
const float pickworkMineSpeed = 1.2f;         // mining rate multiplier after Pickwork (20% faster)
const int seedSelectionHarvests = 6;          // wheat harvests to learn Seed Selection
const float seedSelectionGrowSpeed = 1.2f;    // wheat growth rate multiplier after Seed Selection (20% faster)
const int knowPulseFrames = 60;               // newly learned HUD pip pulses white for ~1s (frames, frozen while paused)
const int knowPulsePeriod = 20;               // one white flash per this many frames, fading over the pulse
const float knowPipPx = 6.0f;                 // 6x6 HUD pip
const float knowPipGap = 3.0f;                // between pips
const float knowPipEraGap = 10.0f;            // pips sit this far left of the right-aligned era text
const float knowPipRimPx = 1.0f;              // unlearned pips: hollow 1px outline
const Color knowPipEmptyColor(78, 86, 74);    // dim grey-green rim, same family as the HUD rim
const Color steelToolColor(178, 186, 198);    // Hand Axe head: cool pale steel, clear of chop green and bird blue-white

// Flint spears (R20-F3): an idle barb walks to the campfire and knaps a spear; spear holders hit game animals harder
const int spearWoodCost = 2;                  // POL-F3: paid up front at claim from the crafter's race stock
const int spearStoneCost = 1;
const int spearRefundWood = spearWoodCost;    // POL-F3: returned by refundSpear() when a paid craft is aborted
const int spearRefundStone = spearStoneCost;
const int spearCraftTicks = 300;              // ~5s at 1x; counted in barb.spearTimer, never barb.timer
const int spearForgeTicks = spearCraftTicks / 2; // POL-F5a: dwarves with a built forge knap at it in half the time
const int spearBarbsPerSpear = 3;            // POL-F3: per race, at most 1 spear (held or being made) per this many of its barbs
const int spearHitDamage = 15;                // replaces animalHitDamage (5) for a barb holding a spear
const Vector2f spearStandOffset(-1.0f, 9.0f); // crafter hitbox top-left relative to the campfire centre: just below the stone ring
const int spearShaftPx = 5;                   // diagonal shaft pixels beside the body, leaning right 1px every 2 rows
const float spearShaftBottom = 3.0f;          // lowest shaft pixel, rows below the head top (bodyTop)
const Vector2f spearTipSize(2.0f, 1.0f);      // flint head on top of the shaft: top edge 2px above the head
const Color spearShaftColor(156, 120, 76);    // light ash wood: much lighter than campfire logs so it reads on dark ground
const Color spearTipColor(214, 208, 192);     // pale flint
const int spearKnapFrames = 8;                // crafter's knapping hand ticks up/down every this many frames
// R47-F8: spear and axe tiers. Dwarves make an axe, every other race a spear (no bow); both use the spear glyph above.
// The weapon's tier is the race's F7 tool tier (toolTierOf) read once when the craft finishes, kept for life, and adds
// weaponTierBonus[tier] to the weapon's base hit damage. Every acquire/pay/refund goes through setWeapon().
enum weaponKind { weaponSpear, weaponAxe, weaponKindCount };
struct weaponDef { const char* name; const char* plural; const char* article; const char* killVerb; const char* huntLabel; int baseDamage; };
const weaponDef weaponDefs[weaponKindCount] = {
    {"spear", "spears", "a",  "speared", "hunting (spear)", spearHitDamage},
    {"axe",   "axes",   "an", "cleaved", "hunting (axe)",   spearHitDamage},   // same base: the axe is a dwarf flavour, not a buff
};
const weaponKind raceWeapon[raceCount] = {weaponSpear, weaponAxe, weaponSpear};   // human, dwarf (axe), elf
constexpr int weaponTierBonus[toolTierCount] = {0, 2, 4, 6};   // extra hit damage per tier (15 -> 17/19/21)
static_assert(weaponTierBonus[0] == 0, "an untiered weapon hits for its base damage");

// Build jobs: which structure a barb's movingbspot/building belongs to. jobNone = no job, or a house (buildhouse()).
// jobSpear is a craft, not a structure: it uses movingbspot (so no other system grabs the barb) but never building/timer.
// jobPave (R20-F2) is run by paveTick(): movingbspot while walking, building/timer while laying the cobbles.
enum buildJob {jobNone, jobShrine, jobSmoke, jobStall, jobForge, jobField, jobSpear, jobPave, jobStore, jobMill, jobKeep, jobShaft, jobCamp, jobBarracks, jobHerbHut, buildJobCount};
const int buildJobTicks[buildJobCount] = {houseBuildTicks, shrineBuildTicks, smokehouseBuildTicks, stallBuildTicks, forgeBuildTicks, fieldBuildTicks, spearCraftTicks, paveTicks, storehouseBuildTicks, millBuildTicks, keepBuildTicks, shaftBuildTicks, campBuildTicks, barracksBuildTicks, herbHutBuildTicks};
// Game animals share the deer code (deers/deershape vectors); per-species numbers live in speciesDefs below.
// Sprite offsets are relative to the logic shape (body = hitbox), written for an animal facing right.
enum animalSpecies {speciesDeer, speciesRabbit, speciesCount};
enum animalGait {gaitWalk, gaitHop};       // walk: diagonal leg pairs alternate; hop: whole sprite bobs up, back leg pushes off
const int animalMaxLegs = 4;
const int animalMaxAccent = 2;
const int animalHitDamage = 5;             // health lost per barb hit (unchanged from deer-only hunting)
const int animalHitInvincibleTicks = 60;   // ticks of invulnerability after a hit
const int animalHitFlashTicks = 10;        // red flash for the first this-many invincible ticks
struct speciesDef {
    const char* name;          // tooltip, logs
    Vector2f bodySize;         // logic hitbox and drawn body
    int maxHealth;
    int foodYield;             // food added when barbs kill one (wolf kills give none)
    int cap;                   // max alive of this species
    int arriveOneIn;           // 1-in-N per tick one walks in from a side edge
    int breedOneIn;            // 1-in-N per tick (with 2+ alive) a new one is born beside a random adult; 0 = never
    int stepInterval;          // ticks per 1px step while wandering
    int fleeStepInterval;      // ticks per 1px step while fleeing
    animalGait gait;
    int legFrames;             // frames per gait half cycle
    int legCount;
    float legX[animalMaxLegs]; // back legs first
    float legPx;               // planted leg length below the body
    float legLiftedPx;         // walk gait: lifted leg length
    float hopPx;               // hop gait: how far the sprite rises mid-hop
    Vector2f headPos, headSize;
    int accentPixels;          // deer antler tine / rabbit ear, 1x1 pixels
    Vector2f accentPos[animalMaxAccent];
    int tailPixels;            // 0 or 1
    Vector2f tailPos;
    Color body, head, leg, accent, tail;
};
const speciesDef speciesDefs[speciesCount] = {
    // Deer: 6x3, 2x2 head overhangs the front-top corner, tiny swept-back tine above the back of the head.
    // R45-F1a: both caps scale with map area (10 -> 22 deer, 12 -> 27 rabbits on the 1200 map)
    {"Deer", {6.0f, 3.0f}, 100, 2, (int)(10 * worldAreaScale), 101, 0, deerStepInterval, deerFleeStepInterval,
     gaitWalk, 6, 4, {0.0f, 1.0f, 4.0f, 5.0f}, 2.0f, 1.0f, 0.0f,
     {5.0f, -1.0f}, {2.0f, 2.0f}, 2, {{5.0f, -2.0f}, {4.0f, -3.0f}}, 0, {0.0f, 0.0f},
     Color(192, 133, 82), Color(176, 120, 74), Color(112, 78, 48), Color(178, 162, 132), Color(0, 0, 0)},
    // Rabbit: 4x2 pale sandy body (much lighter than deer so it pops on dark green), 1x2 ear, white scut.
    // Fragile (3 hits) and only 1 food, but breeds fast up to a larger cap.
    {"Rabbit", {4.0f, 2.0f}, 15, 1, (int)(12 * worldAreaScale), 1800, 1200, 2, 1,
     gaitHop, 8, 2, {0.0f, 3.0f, 0.0f, 0.0f}, 1.0f, 1.0f, 1.0f,
     {3.0f, -1.0f}, {2.0f, 2.0f}, 2, {{3.0f, -2.0f}, {3.0f, -3.0f}}, 1, {-1.0f, 0.0f},
     Color(216, 194, 150), Color(200, 178, 136), Color(138, 112, 80), Color(176, 136, 118), Color(244, 242, 234)},
};
const float rabbitBirthOffsetPx = 5.0f;    // kit appears this far beside its parent

// Campfire at villageCenter: burns wood slowly, glows at night (cosmetic only, no barb behaviour yet)
const int campfireTicksPerWood = 5 * ticksPerMinute; // 1 wood per 5 real min at 1x: a sliver of one chopper's output
const int campfireStartFuel = 2 * campfireTicksPerWood; // lit at startup without touching the wood stock
const float campfireStoneRadius = 5.0f;    // 10px stone ring (plus outline): smaller than a 16px tree
const Color campfireStoneColor(95, 95, 90);
const Vector2f campfireLogSize(8.0f, 2.0f);
const float campfireLogAngle = 30.0f;      // logs cross at +/- this many degrees
const Color campfireLogColor(92, 60, 34);
const Color campfireAshColor(70, 66, 60);
const float campfireFlameRadius = 2.5f;    // at full fuel; shrinks to 40% as the fire dies
const Color campfireFlameColor(235, 140, 50);
const Color campfireEmberColor(120, 50, 25);
const int campfireGlowLayers = 4;          // stacked additive circles give a soft radial falloff
const float campfireGlowRadius = 40.0f;    // outer layer radius (world px)
const Color campfireGlowColor(255, 150, 60);
const uint8_t campfireGlowLayerAlpha = 12; // per layer; ~48 at the centre, so the night stays dark

// Blocky polish: draw-only silhouettes and framed progress bars. Logic shapes (tris/octs/bushshapes) are never modified.
struct palette {
    Color rock;          // diamond rocks
    Color barFrame;      // 1px dark rim around every progress bar
    Color barTrack;      // empty part of a bar
    Color barChop, barPick, barBuild, barMine; // muted versions of the barb task colours
    Color hudBar;        // translucent top strip
    Color hudRim;        // 1px line under the strip, separates it from the world
    Color hudText;       // numbers on the strip
    Color hudAlert;      // "Paused" and other attention states
};
const palette pal{
    Color(128, 126, 118),
    Color(18, 22, 18),
    Color(48, 52, 44),
    Color(95, 170, 85),
    Color(205, 115, 160),
    Color(90, 155, 215),
    Color(215, 145, 55),
    Color(10, 14, 12, 185),
    Color(70, 80, 66, 200),
    Color(225, 220, 200),
    Color(240, 210, 90),
};
const int treeTiers = 3;                    // stepped pine: three stacked blocks inside the old 16px triangle footprint
const Vector2f treeTierSize[treeTiers] = {{6.0f, 4.0f}, {10.0f, 4.0f}, {14.0f, 4.0f}};
const float treeTierTop[treeTiers] = {0.0f, 4.0f, 8.0f}; // canopy ends at y+12, stump (y+12..16) shows below
const float treeFootprint = 16.0f;          // tiers are centred across this width
const float treeShadeAmount = 0.78f;        // bottom tier is darkened to this fraction for a little depth
// Mixed woodland: three draw-only kinds picked from a position hash (never from the logic colour)
const Color treeLogicColor(1, 92, 55);      // logic triangle fill; not used for drawing, not read as state
const float treeCanopyBottom = 12.0f;       // every canopy ends here so it sits on the stump (y+12..16)
enum treeKind { treeTallDark, treeShortBright, treeAutumn, treeKindCount };
struct treeKindDef {
    Color body;          // middle/bottom tiers (bottom is shaded)
    Color tip;           // top tier
    float widthFactor;   // scales treeTierSize.x, rounded to even px so tiers stay centred
    float heightFactor;  // scales treeTierSize.y / treeTierTop
};
const treeKindDef treeKinds[treeKindCount] = {
    {Color(6, 72, 46),   Color(6, 72, 46),   1.0f,  1.25f}, // tall dark pine: 15px canopy
    {Color(44, 120, 58), Color(58, 136, 66), 0.75f, 0.75f}, // short bright pine: 9px canopy
    {Color(156, 84, 36), Color(166, 138, 52), 1.0f, 1.0f},  // autumn: muted rust with an ochre tip, 12px canopy
};
const unsigned treeKindHashMod = 8;         // hash % 8: 0 = autumn (1 in 8), 1-3 short bright, 4-7 tall dark
const unsigned treeAutumnSlots = 1;
const unsigned treeShortSlots = 3;
const int rockPointCount = 4;               // rocks draw as diamonds (logic shape keeps its pentagon)
const Vector2f barSize(20.0f, 3.0f);        // inner fill area of a progress bar
const float barFramePx = 1.0f;

// HUD icon table: 8x8 patterns, '.' = empty, 'a'..'d' = the icon's colour key
enum hudIconId { iconWood, iconStone, iconFood, iconPeople, iconHouse, iconSun, iconMoon, iconSpeed, iconPause, iconRain, iconSeason, iconHerb, iconCount };
struct iconDef {
    const char* rows[hudIconGrid];
    Color key[4];
};
const iconDef hudIcons[iconCount] = {
    // wood: brown log with a pale cut end and growth ring
    {{"........", ".aaaabb.", "aaaabccb", "aaaabcdb", "aaaabcdb", "aaaabccb", ".aaaabb.", "........"},
     {Color(112, 74, 42), Color(165, 125, 75), Color(205, 170, 115), Color(135, 95, 55)}},
    // stone: the world rock diamond, lit top-left
    {{"...aa...", "..abaa..", ".abaaaa.", "abaaaaaa", "aaaaaaac", ".aaaaac.", "..aaac..", "...ac..."},
     {pal.rock, Color(176, 172, 160), Color(88, 86, 80), Color::Transparent}},
    // food: berry with a shine and a leaf
    {{"....cc..", "...cc...", "..aaaa..", ".abaaaa.", ".aaaaaa.", ".aaaaaa.", "..aaaa..", "........"},
     {Color(185, 70, 105), Color(235, 160, 180), Color(95, 150, 70), Color::Transparent}},
    // people: head and shoulders
    {{"...aa...", "..aaaa..", "..aaaa..", "...aa...", ".bbbbbb.", "bbbbbbbb", "bbbbbbbb", "bbbbbbbb"},
     {barbSkinColor, Color(105, 125, 160), Color::Transparent, Color::Transparent}},
    // house: red roof, pale walls, dark door
    {{"...aa...", "..aaaa..", ".aaaaaa.", "aaaaaaaa", ".bbbbbb.", ".bbbcbb.", ".bbbcbb.", ".bbbcbb."},
     {Color(150, 72, 52), Color(190, 170, 130), Color(92, 60, 34), Color::Transparent}},
    // sun: disc with eight rays
    {{"a..aa..a", "........", "..bbbb..", "a.bbbb.a", "a.bbbb.a", "..bbbb..", "........", "a..aa..a"},
     {Color(230, 150, 50), Color(245, 205, 90), Color::Transparent, Color::Transparent}},
    // moon: pale crescent with a star
    {{"..aaaa..", ".aaa....", "aaa...b.", "aaa.....", "aaa.....", "aaa.....", ".aaa....", "..aaaa.."},
     {Color(210, 215, 235), Color(240, 210, 90), Color::Transparent, Color::Transparent}},
    // speed: double chevron
    {{"........", "a...a...", "aa..aa..", ".aa..aa.", ".aa..aa.", "aa..aa..", "a...a...", "........"},
     {Color(190, 195, 185), Color::Transparent, Color::Transparent, Color::Transparent}},
    // pause: two bars
    {{"........", ".aa..aa.", ".aa..aa.", ".aa..aa.", ".aa..aa.", ".aa..aa.", ".aa..aa.", "........"},
     {pal.hudAlert, Color::Transparent, Color::Transparent, Color::Transparent}},
    // rain: grey cloud with falling drops
    {{"........", "..aaa...", ".aaaaaa.", "aaaaaaaa", ".aaaaaa.", "........", ".b..b..b", "b..b..b."},
     {Color(150, 158, 172), Color(100, 150, 220), Color::Transparent, Color::Transparent}},
    // season (R51-F3): a leaf on a diagonal stem; drawn with the season's own key (seasonDefs[].iconKey)
    {{"......aa", "....aaaa", "...aaaba", "..aaabaa", ".aaabaa.", ".aabaa..", ".baa....", "b......."},
     {seasonDefs[seasonSpring].iconKey[0], seasonDefs[seasonSpring].iconKey[1], Color::Transparent, Color::Transparent}},
    // herb (R53-F2): two lavender sprigs with pale tips on green stems
    {{"..b.....", ".aba..b.", "..a..aba", "..ac..a.", "...c.ca.", "...cc...", "...c....", "..ccc..."},
     {Color(176, 140, 206), Color(226, 206, 240), Color(96, 140, 76), Color::Transparent}},
};

mt19937 gen(random_device{}());
uniform_int_distribution<int> distrib(0, (int)worldSize - 1);
uniform_int_distribution<int> wanderChance(0, 149);
uniform_int_distribution<int> wanderDX(-50, 50);
uniform_int_distribution<int> wanderDY(-50, 50);
uniform_int_distribution<int> spawnChance(0, 7500);
uniform_int_distribution<int> treespawnChance(0, treeSpawnOneIn - 1);
uniform_int_distribution<int> DeerSpawnY((int)mapEdgeInset, (int)(worldSize - mapEdgeInset));
uniform_int_distribution<int> DeerSpawnRL(0, 1);
uniform_int_distribution<int> DeerSpawnXgotoL((int)mapEdgeInset, (int)(mapEdgeInset + deerEntryDepthPx));
uniform_int_distribution<int> DeerSpawnXgotoR((int)(worldSize - mapEdgeInset - deerEntryDepthPx), (int)(worldSize - mapEdgeInset));

// Whole-pixel draw positions so shapes don't shimmer between pixels; only ever applied to copies
Vector2f snapPx(Vector2f p) { return Vector2f(round(p.x), round(p.y)); }

template <class S>
void drawSnapped(RenderWindow& window, S shape) {
    shape.setPosition(snapPx(shape.getPosition()));
    window.draw(shape);
}

// The one progress-bar helper: dark frame, track, then a whole-pixel fill. pos is the top-left of the fill area.
void drawBar(RenderWindow& window, Vector2f pos, float frac, Color fill) {
    pos = snapPx(pos);
    RectangleShape r(barSize + Vector2f(2 * barFramePx, 2 * barFramePx));
    r.setPosition(pos - Vector2f(barFramePx, barFramePx));
    r.setFillColor(pal.barFrame);
    window.draw(r);
    r.setSize(barSize);
    r.setPosition(pos);
    r.setFillColor(pal.barTrack);
    window.draw(r);
    float w = floor(clamp(frac, 0.0f, 1.0f) * barSize.x);
    if (w <= 0) return;
    r.setSize(Vector2f(w, barSize.y));
    r.setFillColor(fill);
    window.draw(r);
}

Color shade(Color c, float f) {
    return Color((uint8_t)(c.r * f), (uint8_t)(c.g * f), (uint8_t)(c.b * f), c.a);
}

void pushQuad(VertexArray& va, Vector2f p, Vector2f s, Color c) {
    Vector2f a = p, b = p + Vector2f(s.x, 0), d = p + Vector2f(0, s.y), e = p + s;
    for (Vector2f v : {a, b, e, a, e, d}) va.append(Vertex{v, c});
}

// R36-F1: append parts (in order) at origin, each colour modulated by tint; callers draw va once per building kind
void drawParts(VertexArray& va, const partDef* parts, int count, Vector2f origin, Color tint) {
    for (int i = 0; i < count; i++) pushQuad(va, origin + parts[i].pos, parts[i].size, parts[i].color * tint);
}
void drawParts(VertexArray& va, partRange parts, Vector2f origin, Color tint) {
    drawParts(va, buildingParts + parts.first, parts.count, origin, tint);
}

// Stable kind for a tree from its integer position (same tree always looks the same)
treeKind treeKindAt(Vector2f pos) {
    uint32_t h = (uint32_t)(int)pos.x * 73856093u ^ (uint32_t)(int)pos.y * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    unsigned slot = h % treeKindHashMod;
    if (slot < treeAutumnSlots) return treeAutumn;
    if (slot < treeAutumnSlots + treeShortSlots) return treeShortBright;
    return treeTallDark;
}

// Stepped blocky trees, batched into one vertex array; kind (size + colour) comes from a position hash
void drawTrees(RenderWindow& window, const vector<CircleShape>& tris, VertexArray& va, const FloatRect& view) {
    va.clear();
    // R47-F1: ~12k trees on the 2400 map, so only those near the view are batched (margin covers the tallest canopy)
    const float m = 2 * treeFootprint;
    FloatRect nearView(view.position - Vector2f(m, m), view.size + Vector2f(2 * m, 2 * m));
    for (auto& tri : tris) {
        if (!nearView.contains(tri.getPosition())) continue;
        Vector2f p = snapPx(tri.getPosition());
        const treeKindDef& kd = treeKinds[treeKindAt(tri.getPosition())];
        const int last = treeTiers - 1;
        float canopy = round(treeTierTop[last] * kd.heightFactor) + round(treeTierSize[last].y * kd.heightFactor);
        float offset = treeCanopyBottom - canopy; // anchor the canopy bottom to the stump (12 - canopyHeight)
        for (int k = 0; k < treeTiers; k++) {
            Vector2f s(2 * round(treeTierSize[k].x * kd.widthFactor / 2), round(treeTierSize[k].y * kd.heightFactor));
            Color c = k == 0 ? kd.tip : (k == last ? shade(kd.body, treeShadeAmount) : kd.body);
            pushQuad(va, p + Vector2f(floor((treeFootprint - s.x) / 2), offset + round(treeTierTop[k] * kd.heightFactor)), s, c);
        }
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// Rock as a diamond: copy of the logic pentagon with 4 points, same radius/position
CircleShape rockDrawShape(const CircleShape& oct) {
    CircleShape d = oct;
    d.setPointCount(rockPointCount);
    d.setFillColor(pal.rock);
    d.setPosition(snapPx(oct.getPosition()));
    return d;
}

// Bush as a square covering the logic circle's 2r box, keeping its ripe/bare colour
RectangleShape bushDrawShape(const CircleShape& bs) {
    float r = bs.getRadius();
    RectangleShape s(Vector2f(2 * r, 2 * r));
    s.setPosition(snapPx(bs.getPosition()));
    s.setFillColor(bs.getFillColor());
    return s;
}

// Rocks (+ gold flecks), bushes and stumps batched into one vertex array each, culled to the view. Same shapes and
// order as rockDrawShape()/bushDrawShape()/drawSnapped(), but one draw call per layer instead of one per object.
void drawRocks(RenderWindow& window, const vector<CircleShape>& octs, const vector<char>& rockGold, VertexArray& va, const FloatRect& view) {
    va.clear();
    for (int i = 0; i < (int)octs.size(); i++) {
        float r = octs[i].getRadius();
        Vector2f p = snapPx(octs[i].getPosition());
        if (!view.findIntersection(FloatRect(p, Vector2f(2 * r, 2 * r)))) continue;
        bool isGold = i < (int)rockGold.size() && rockGold[i];
        Color c = isGold ? goldRockColor : pal.rock;
        Vector2f pt[rockPointCount];   // CircleShape's point formula: point k at angle k*2pi/n - pi/2 around (r, r)
        for (int k = 0; k < rockPointCount; k++) {
            float a = k * 2 * 3.14159265f / rockPointCount - 3.14159265f / 2;
            pt[k] = p + Vector2f(r + cos(a) * r, r + sin(a) * r);
        }
        for (int k = 1; k + 1 < rockPointCount; k++)
            for (Vector2f v : {pt[0], pt[k], pt[k + 1]}) va.append(Vertex{v, c});
        if (isGold) pushQuad(va, p + goldFleckOffset, Vector2f(1, 1), goldFleckColor);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

void drawBushes(RenderWindow& window, const vector<CircleShape>& bushshapes, VertexArray& va, const FloatRect& view) {
    va.clear();
    for (auto& bs : bushshapes) {
        float d = 2 * bs.getRadius();
        Vector2f p = snapPx(bs.getPosition());
        if (view.findIntersection(FloatRect(p, Vector2f(d, d)))) pushQuad(va, p, Vector2f(d, d), bs.getFillColor());
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

void drawStumps(RenderWindow& window, const vector<RectangleShape>& stumps, VertexArray& va, const FloatRect& view) {
    va.clear();
    for (auto& s : stumps) {
        Vector2f p = snapPx(s.getPosition());
        if (view.findIntersection(FloatRect(p, s.getSize()))) pushQuad(va, p, s.getSize(), s.getFillColor());
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

struct house {int x, y; bool hasFoundation = false; int id = -1; barbRace race = raceHuman;}; // id: stable, from nextHouseId; race: builder's, picks the roof style
int nextHouseId = 0;   // assigned only in buildhouse(); never reused, so ids survive any swapRemove
struct shrine {int x, y; int id = -1;};  // x,y = slab top-left; id: stable, from nextShrineId (refer to shrines by id, never index)
int nextShrineId = 0;  // assigned only in buildshrine(); at most maxShrines are ever standing
// INBOX2-F1: one per race (index = barbRace), held in a fixed array in main; claimed = a builder of that race is on it;
// puffFrames > 0 = a smoked kill's puff burst is in flight (drawn frames, frozen while paused)
struct smokehouse {bool built = false; bool claimed = false; int puffFrames = 0;};
using smokehouseSet = array<smokehouse, raceCount>;
bool hasStall = false;      // market stall standing: traderTick() allows stallExtraLots more lots
bool stallClaimed = false;  // a builder is on the stall
int stallBonusLots = 0;     // extra lots the stall added to the current visit; drawn as crates while the trader is Trading
struct forge {bool built = false; bool claimed = false;}; // the one dwarf forge; claimed = a builder is on it
// POL-F5a: the single forge gate (shared with forge phase 2): race r works at the forge only if it is dwarves and it is built
bool forgeActive(const forge& fg, int r) { return r == raceDwarf && fg.built; }
// R47-F7: the smelt gate is separate: every race smelts at the forge once it is built
bool forgeSmeltOpen(const forge& fg) { return fg.built; }
int raceIngots[raceCount] = {0, 0, 0};   // R47-F7: per-race ingots; smelt adds (capped at kIngotStockMax), R49-F1 wear spends (floored at 0)
int raceWear[raceCount] = {0, 0, 0};             // R49-F1: finished chop/mine jobs since the last ingot was worn away
int raceTierBest[raceCount] = {0, 0, 0};         // R49-F1: highest tier reached, so a regained tier reads "re-forged"
int raceTierDimFrames[raceCount] = {0, 0, 0};    // R49-F1: >0 after a real tier drop: HUD icon + steel mark dimmed (drawn frames)
int raceTierLost[raceCount] = {0, 0, 0};         // R49-F1: the tier just lost, its HUD bar drawn amber while dimmed
int wearSparkCooldown[raceCount] = {0, 0, 0};    // R49-F1: per-race spark rate-limit timer (drawn frames), no pool
int wearSparkAge[raceCount] = {wearSparkFrames, wearSparkFrames, wearSparkFrames};   // R49-F1: the race's one spark; >= life = none
Vector2f wearSparkPos[raceCount];                // R49-F1: where that spark started
bool toolTierSeen = false;                       // R49-F1: set by the first ingot; the HUD tier column stays shown from then on
int forgeSmelterId = -1;                 // R47-F7: barb id at the forge smelting (never an index); -1 = free; reset by clearSmelt()
// R47-F7: the tier lookup. Callers read it once, when a chop or mine starts, into barb.jobTier
int toolTierOf(int r) {
    if (r < 0 || r >= raceCount) return 0;
    int n = raceIngots[r], t = 0;
    while (t + 1 < toolTierCount && n >= toolTierIngots[t + 1]) t++;
    return t;
}
int gold = 0;             // R46-F1a: global nuggets, from gold rocks only
bool goldFound = false;    // R46-F1a: set by the first nugget; the HUD gold cell stays shown from then on
char rollGoldRock() { return bernoulli_distribution(goldRockChance)(gen) ? 1 : 0; }   // one roll per spawned rock
int forgeEmberFrames = 0;  // >0 = ember glows hot; set when a dwarf mines a rock, counted down per drawn frame
// POL-F5b: fixed spark pool; a slot is free once age >= forgeSparkFrames
struct forgeSpark {Vector2f pos, vel; int age = forgeSparkFrames;};
forgeSpark forgeSparks[forgeSparkPoolMax];
// R29-F1: one per race (index = barbRace), held in a fixed array in main; built is the validity flag haulers check
struct storehouse {bool built = false; bool claimed = false; int stored = 0; int flashFrames = 0;}; // flashFrames: deposit flash, per drawn frame
// R34-F1: one per race (index = barbRace), at storehousePos + shaftCrateOffset. built is the validity flag; claimed and
// builderId follow the mill's claim path; seam counts down to 0 (never below). Reset with the quarters in placeQuarters().
// R40-F1: puffs/flash are cosmetic, advanced by shaftTick(); a puff with age < 0 is free
struct shaftPuff {Vector2f pos; int age = -1;};
struct mineShaft {bool built = false; bool claimed = false; int builderId = -1; int seam = kShaftCapacity;
                  shaftPuff puffs[kShaftPuffCap]; int puffNext = 0; int puffTimer = 0; int flashTicks = 0;};
mineShaft mineShafts[raceCount];
Vector2f shaftPos(int r) { return storehousePos[r] + shaftCrateOffset; }
// R40-F1: the dark pit inside the frame (under the lintel, between the posts, above the 1px sill)
RectangleShape shaftPitRect(int r) {
    RectangleShape pit(Vector2f(shaftSize.x - 2 * shaftPostW, shaftSize.y - shaftLintelH - 1.0f));
    pit.setPosition(shaftPos(r) + Vector2f(shaftPostW, shaftLintelH));
    return pit;
}
Vector2f shaftWorkSpot(int r) { return shaftPos(r) + shaftStandOffset; }
bool shaftOpen(int r) { return r >= 0 && r < raceCount && mineShafts[r].built && mineShafts[r].seam > 0; }
// Seam bar above, the frame, and the miner's stand spot below
FloatRect shaftZone(int r) {
    return FloatRect(shaftPos(r) - Vector2f(0.0f, shaftSeamGap + shaftSeamH), Vector2f(shaftSize.x, shaftSeamGap + shaftSeamH + shaftSize.y + keepStandRoomPx));
}
// False if rect comes within shaftClearPx of any race's shaft zone (reserved from the start, built or not)
bool shaftClear(const FloatRect& rect) {
    for (int k = 0; k < raceCount; k++) {
        FloatRect z = shaftZone(k);
        FloatRect zone(z.position - Vector2f(shaftClearPx, shaftClearPx), z.size + Vector2f(2 * shaftClearPx, 2 * shaftClearPx));
        if (rect.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// R47-F6: one per race (index = barbRace), at storehousePos + barracksCrateOffset. The claim is not stored: it is "a barb
// of this race holds jobBarracks" (like the lean-to). slotId[s] = barb id drilling in slot s (never an index), -1 = free;
// set only by startTraining(), freed only by clearTraining() and buildbarracks()' per-tick validation. Reset in placeQuarters().
struct barracksHall {bool built = false; int shakeTicks = 0; int slotId[kDrillSlots] = {-1, -1};};
barracksHall halls[raceCount];
Vector2f barracksPos(int r) { return storehousePos[r] + barracksCrateOffset; }
Vector2f drillSlotSpot(int r, int s) { return barracksPos(r) + drillSlotOffset[clamp(s, 0, kDrillSlots - 1)]; }
FloatRect barracksZone(int r) { return FloatRect(barracksPos(r), barracksZoneSize); }
// False if rect comes within barracksClearPx of any race's barracks zone (reserved from the start, built or not)
bool barracksClear(const FloatRect& rect) {
    for (int k = 0; k < raceCount; k++) {
        FloatRect z = barracksZone(k);
        FloatRect zone(z.position - Vector2f(barracksClearPx, barracksClearPx), z.size + Vector2f(2 * barracksClearPx, 2 * barracksClearPx));
        if (rect.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// R53-F2: one per race slot (only kHerbRace ever builds), at storehousePos + herbHutCrateOffset. The build claim is not
// stored ("a barb of this race holds jobHerbHut"). tenderId = barb id tending the bed (never an index), -1 = none; set
// only by startHerbTend(), freed by endActivity() and herbTick()'s per-tick validation. Fixed arrays, no vectors, so
// nothing to swapRemove. Reset in placeQuarters(). ripe is cached once per tick by herbTick() so the idle-elf join check
// never scans the bed.
struct herbPlant {float growth = 0.0f; bool bloom = false;};   // growth 0..1, ripe at 1; bloom = still in harvest-day flower
struct herbGarden {bool built = false; herbPlant plants[kHerbPlants]; int ripe = 0; int tenderId = -1;
                   int lastSeason = -1; int bloomDay = -1; int spoilTicks = 0; int storedLineDay = -1; bool bareSaid = false;};
herbGarden herbGardens[raceCount];
int raceHerbs[raceCount] = {0, 0, 0};   // R53-F2: herb stock per race (0..kHerbStockMax); phase 2b spends it on cures
Vector2f herbHutPos(int r) { return storehousePos[r] + herbHutCrateOffset; }
Vector2f herbBedPos(int r) { return herbHutPos(r) + herbBedOffset; }
Vector2f herbTendSpot(int r) { return herbBedPos(r) + herbTendOffset; }
Vector2f herbPlantPos(int r, int k) { return herbBedPos(r) + Vector2f(1.0f + herbPlantPitch.x * (k % kHerbCols), 1.0f + herbPlantPitch.y * (k / kHerbCols)); }
FloatRect herbZone(int r) { return FloatRect(herbHutPos(r), herbZoneSize); }
bool herbHutBuilt(int r) { return r >= 0 && r < raceCount && herbGardens[r].built; }
bool anyHerbHut() { for (int r = 0; r < raceCount; r++) if (herbGardens[r].built) return true; return false; }
// False if rect comes within herbClearPx of the herb race's zone (reserved from the start, built or not)
bool herbClear(const FloatRect& rect) {
    FloatRect z = herbZone(kHerbRace);
    FloatRect zone(z.position - Vector2f(herbClearPx, herbClearPx), z.size + Vector2f(2 * herbClearPx, 2 * herbClearPx));
    return !rect.findIntersection(zone).has_value();
}

// False if r comes within storehouseClearPx of any race's crate or its stand spot (reserved from the start, built or not)
bool storehouseClear(const FloatRect& r) {
    for (int k = 0; k < raceCount; k++) {
        Vector2f lo = storehousePos[k] + Vector2f(storehouseStandOffset.x - storehouseClearPx, -storehouseClearPx);
        FloatRect zone(lo, storehouseSize + Vector2f(-storehouseStandOffset.x + 2 * storehouseClearPx, 2 * storehouseClearPx));
        if (r.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// INBOX2-F1: race k's hut footprint including the chimney poking above the roof
FloatRect smokehouseRect(int k) {
    return FloatRect(smokehousePos[k] + Vector2f(0.0f, smokehouseChimneyPos.y),
                     Vector2f(smokehouseSize.x, smokehouseSize.y - smokehouseChimneyPos.y));
}

// INBOX2-F1: false if r comes within smokehouseClearPx of any race's hut (reserved from the start, built or not)
bool smokehouseClear(const FloatRect& r) {
    for (int k = 0; k < raceCount; k++) {
        FloatRect h = smokehouseRect(k);
        FloatRect zone(h.position - Vector2f(smokehouseClearPx, smokehouseClearPx), h.size + Vector2f(2 * smokehouseClearPx, 2 * smokehouseClearPx));
        if (r.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// R31-F1: one per race (index = barbRace). x,y = keep top-left, fixed at startup by placeQuarters(); id is stable, from
// nextKeepId, given when the keep stands. keepQueued[r] (a builder holds jobKeep for it) and keepReservedStone[r] are
// always set together on claim and cleared together on completion or cancelKeep(). Never torn down (outlives any leader).
struct keep {int id = -1; int race = -1; int x = 0, y = 0; bool built = false; bool gilded = false;};   // R46-F1b: gilded
keep keeps[raceCount];
int nextKeepId = 0;
bool keepQueued[raceCount] = {false, false, false};
int keepReservedStone[raceCount] = {0, 0, 0};
int keepBuilderId[raceCount] = {-1, -1, -1};
// R31-F3: barb ids sheltering in (or running to) race r's keep, at most keepSlotMax. Only bellTick() adds; only
// ungarrison() removes, plus bellTick()'s per-tick validation against live barbs.
vector<int> keepSlots[raceCount];

// R31-F2: one curtain-wall segment; x,y = arc midpoint (world px), slot = ring slot (0 = gate centre). Cosmetic only.
struct wallseg {int id = -1; int race = -1; int x = 0, y = 0; int slot = 0;};
vector<wallseg> wallsegs;
int nextWallSegId = 0;
int wallClock = 0;                        // sim ticks counted by wallTick(): the flash timestamp base
int wallNextOrder[raceCount] = {0, 0, 0}; // next index into the rise order (wallSegMax = all slots tried)
int wallTimer[raceCount] = {0, 0, 0};
int wallFlashTick[raceCount] = {-wallFlashTicks, -wallFlashTicks, -wallFlashTicks};  // wallClock of the last placement
int wallFlashSegId[raceCount] = {-1, -1, -1};

// R33-F2: one gatehouse tower; x,y = top-left (world px). race >= 0 once placed at wall completion; id from nextTowerId
// when it stands. towerReservedStone[r] is set on claim (with keepQueued[r], keepBuilderId[r] and jobKeep, reused from
// the keep, which is already built by then) and cleared on completion or cancelTower().
struct gatetower {int id = -1; int race = -1; int x = 0, y = 0; bool built = false;};
gatetower towers[raceCount][towerCount];
int nextTowerId = 0;
int towerReservedStone[raceCount] = {0, 0, 0};

// R33-F2: tower footprint: pennant above, the silhouette, and its builder's stand spot below
FloatRect towerZone(const gatetower& t) {
    return FloatRect(Vector2f((float)t.x, (float)t.y - towerPoleH),
                     Vector2f(towerSize.x + towerFlagSize.x, towerPoleH + towerSize.y + keepStandRoomPx));
}

// R33-F2: false if r comes within keepClearPx of any placed tower's zone (reserved from placement, built or not)
bool towerClear(const FloatRect& r) {
    for (int k = 0; k < raceCount; k++)
        for (auto& t : towers[k]) {
            if (t.race < 0) continue;
            FloatRect z = towerZone(t);
            FloatRect zone(z.position - Vector2f(keepClearPx, keepClearPx), z.size + Vector2f(2 * keepClearPx, 2 * keepClearPx));
            if (r.findIntersection(zone).has_value()) return false;
        }
    return true;
}

// R31-F1: race k's keep footprint: banner pole/flag above, the silhouette, and its builder's stand spot below
FloatRect keepZone(int k) {
    return FloatRect(Vector2f((float)keeps[k].x, (float)keeps[k].y - keepPoleH),
                     Vector2f(keepSize.x + keepFlagSize.x, keepPoleH + keepSize.y + keepStandRoomPx));
}

// R31-F1: false if r comes within keepClearPx of any race's keep zone (reserved from the start, built or not)
bool keepClear(const FloatRect& r) {
    for (int k = 0; k < raceCount; k++) {
        FloatRect z = keepZone(k);
        FloatRect zone(z.position - Vector2f(keepClearPx, keepClearPx), z.size + Vector2f(2 * keepClearPx, 2 * keepClearPx));
        if (r.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// False if r comes within forgeClearPx of the forge footprint (reserved from the start, built or not).
// R29-F1: also keeps the storehouse spots clear, so every placement that avoids the forge avoids them too.
// INBOX2-F1: and every race's smokehouse spot. R31-F1: and every race's keep spot. R33-F2: and every placed gate tower.
bool forgeClear(const FloatRect& r) {
    FloatRect zone(forgePos - Vector2f(forgeClearPx, forgeClearPx), forgeParts[0].size + Vector2f(2 * forgeClearPx, 2 * forgeClearPx));
    return !r.findIntersection(zone).has_value() && storehouseClear(r) && smokehouseClear(r) && keepClear(r) && towerClear(r)
        && shaftClear(r)   // R34-F1
        && barracksClear(r)   // R47-F6
        && herbClear(r);      // R53-F2
}

// R30-F1: race quarters
Vector2f quarterCentre(int r) { return storehousePos[r] + storehouseSize / 2.0f; }
VertexArray quarterRingVerts(PrimitiveType::Triangles);   // dotted rings, built in placeQuarters(); R47-F10: rebuilt per step

// R47-F10: per-race build radius step (high-water mark, reset only by placeQuarters). Every runtime read of the
// quarter house radius, its clearing and the homestead ring's far edge goes through these.
int quarterStep[raceCount] = {};
bool quarterStepCapped[raceCount] = {};   // a step was rejected: geometry only tightens, so it is not retried
struct quarterPulse {int race; float from, to; int age = 0;};   // R47-F10: dust ring on a step
vector<quarterPulse> quarterPulses;
int quarterStepOf(int r) { return (r >= 0 && r < raceCount) ? quarterStep[r] : 0; }
float quarterRadiusAt(int s) { return quarterHouseRadius + quarterStepPx * s; }   // radii at step s (candidate checks)
float houseSpreadFarAt(int s) { return houseSpreadFar + houseSpreadStepPx * s; }
float quarterReachAt(int s) { return max(quarterRadiusAt(s), houseSpreadFarAt(s)) + houseSlabSize.x / 2.0f; }   // outermost slab edge
float quarterRadiusFor(int r) { return quarterRadiusAt(quarterStepOf(r)); }
float quarterClearRadiusFor(int r) { return quarterRadiusFor(r) + quarterClearMargin; }
float houseSpreadFarFor(int r) { return houseSpreadFarAt(quarterStepOf(r)); }
float quarterReachFor(int r) { return quarterReachAt(quarterStepOf(r)); }

// True if a tree/rock at top-left p is inside the village clearing or any race quarter's clearing (removed every frame)
bool inClearing(Vector2f p) {
    float dx = p.x - villageCenter.x, dy = p.y - villageCenter.y;
    if (dx * dx + dy * dy < clearRadiusSq) return true;
    for (int r = 0; r < raceCount; r++) {
        Vector2f d = p - quarterCentre(r);
        float cr = quarterClearRadiusFor(r);   // R47-F10: grows with the race's step
        if (d.x * d.x + d.y * d.y < cr * cr) return true;
    }
    return false;
}

// Appends a 1x1 world-px quad at the pixel containing p
void appendDot(VertexArray& va, Vector2f p, Color col) {
    float x0 = floor(p.x), y0 = floor(p.y), x1 = x0 + 1.0f, y1 = y0 + 1.0f;
    Vector2f q[6] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
    for (auto& v : q) va.append(Vertex{v, col});
}

// Startup layout check: crates far enough apart, each quarter inside the map (below the HUD) and clear of the village clearing
// R44-F4a: shared by quarterLayoutOk() and homestead placement: a disc of radius rad at c lies inside the map (below the
// HUD) and clear of the village clearing
bool layoutDiscOk(Vector2f c, float rad) {
    if (c.x - rad < 0.0f || c.x + rad > worldSize || c.y - rad < hudBarHeight || c.y + rad > worldSize) return false;
    return hypot(c.x - villageCenter.x, c.y - villageCenter.y) >= sqrt(clearRadiusSq) + rad;
}

// R47-F3: a candidate layout (crate top-lefts only). Computed and checked first; placeQuarters() commits exactly one.
struct quarterLayout {Vector2f crate[raceCount];};

quarterLayout quarterLayoutAt(float a0, float r) {
    quarterLayout L;
    for (int k = 0; k < raceCount; k++) {
        float a = a0 + 6.2831853f * k / raceCount;
        L.crate[k] = snapPx(Vector2f(villageCenter.x + cos(a) * r, villageCenter.y + sin(a) * r) - storehouseSize / 2.0f);
    }
    return L;
}

bool quarterLayoutOk(const quarterLayout& L) {
    for (int a = 0; a < raceCount; a++) {
        if (!layoutDiscOk(L.crate[a] + storehouseSize / 2.0f, quarterClearRadiusFor(a))) return false;   // step 0 at startup
        for (int b = a + 1; b < raceCount; b++) {
            Vector2f d = L.crate[a] - L.crate[b];
            if (hypot(d.x, d.y) < raceMinSeparation) return false;
        }
    }
    return true;
}

// R47-F3: the gaps between frontiers, set when the layout is committed. Gap i bisects the i-th and next crate in angle
// order round villageCenter; frontierGapOf[race] holds the two gaps flanking that race's crate.
float frontierGapAngle[raceCount] = {};
int frontierGapOf[raceCount][2] = {};

// R47-F3: true if p lies inside the frontier of a race other than race (the wide work look skips such targets)
bool inOtherFrontier(Vector2f p, int race) {
    for (int r = 0; r < raceCount; r++) {
        if (r == race) continue;
        Vector2f d = p - quarterCentre(r);
        if (d.x * d.x + d.y * d.y < frontierRadius * frontierRadius) return true;
    }
    return false;
}

// Places the crates once, before anything else is spawned: a random rotation/radius of an even 3-way split, checked by
// quarterLayoutOk(); if no try passes, the fixed layout (human north, others 120 deg apart) is used. Builds the rings.
// R47-F3: every candidate is computed and checked without touching any state; only the chosen layout is committed.
bool quarterFallbackUsed = false;   // INBOX-F3: set by placeQuarters() when the fixed layout was used

// R47-F10: the faint dotted ring on each quarter's clearing edge, at its current step
void buildQuarterRings() {
    quarterRingVerts.clear();
    for (int k = 0; k < raceCount; k++) {
        Color col = storehouseColors[k];
        col.a = quarterRingAlpha;
        float cr = quarterClearRadiusFor(k);
        int dots = (int)round(6.2831853f * cr / quarterRingDotGap);
        for (int i = 0; i < dots; i++) {
            float a = 6.2831853f * i / dots;
            appendDot(quarterRingVerts, quarterCentre(k) + Vector2f(cos(a), sin(a)) * cr, col);
        }
    }
}

void placeQuarters() {
    quarterFallbackUsed = false;
    for (int k = 0; k < raceCount; k++) { quarterStep[k] = 0; quarterStepCapped[k] = false; }   // R47-F10: startup radii
    quarterPulses.clear();
    uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(quarterMinRadius, quarterMaxRadius);
    auto commit = [](const quarterLayout& L) {
        for (int k = 0; k < raceCount; k++) {
            storehousePos[k] = L.crate[k];
            smokehousePos[k] = storehousePos[k] + smokehouseCrateOffset;   // INBOX2-F1: hut beside the crate
            Vector2f kp = storehousePos[k] + keepCrateOffset;              // R31-F1: keep below the crate
            keeps[k] = keep{-1, k, (int)kp.x, (int)kp.y, false};
            keepSlots[k].clear();                                          // R31-F3
            wallNextOrder[k] = wallTimer[k] = 0;                           // R31-F2
            wallFlashTick[k] = -wallFlashTicks;
            wallFlashSegId[k] = -1;
            for (auto& t : towers[k]) t = gatetower{};                     // R33-F2
            towerReservedStone[k] = 0;
            mineShafts[k] = mineShaft{};                                   // R34-F1: unbuilt, full seam
            halls[k] = barracksHall{};                                     // R47-F6: unbuilt, slots free
            herbGardens[k] = herbGarden{};                                 // R53-F2: unbuilt, bare bed, no tender
            raceHerbs[k] = 0;
        }
        wallsegs.clear();
    };
    // R47-F2: with the village at the map centre, check quarterLayoutOk() at every rotation and at both radius limits
    // (R47-F3: candidates only, no state is written)
    int sweepFails = 0;
    for (float r : {quarterMinRadius, quarterMaxRadius})
        for (int s = 0; s < quarterSweepSteps; s++)
            if (!quarterLayoutOk(quarterLayoutAt(6.2831853f * s / quarterSweepSteps, r))) sweepFails++;
    printf("Race quarters: rotation sweep %s (%d of %d layouts fail, min separation %.0f)\n", sweepFails ? "FAILS" : "ok",
           sweepFails, 2 * quarterSweepSteps, raceMinSeparation);
    // R47-F3: the fallback is computed and checked up front, so every start reports whether it would hold
    quarterLayout fixedLayout = quarterLayoutAt(-1.5707963f, (quarterMinRadius + quarterMaxRadius) / 2.0f);
    bool fixedOk = quarterLayoutOk(fixedLayout);
    printf("Race quarters: fixed fallback layout %s\n", fixedOk ? "ok" : "FAILS");
    quarterLayout chosen;
    bool ok = false;
    for (int t = 0; t < quarterPlaceTries && !ok && !quarterForceFallback; t++) {
        chosen = quarterLayoutAt(ang(gen), rad(gen));
        ok = quarterLayoutOk(chosen);
    }
    if (!ok) {
        chosen = fixedLayout;
        printf("Race quarters: random layout failed the check%s, using the fixed layout (%s)\n",
               quarterForceFallback ? " (forced)" : "", fixedOk ? "ok" : "STILL FAILS");   // once: placeQuarters runs once per start
        quarterFallbackUsed = true;   // INBOX-F3: main logs one ticker line per layout
    }
    commit(chosen);
    // R47-F3: frontier gaps from the committed crates
    float crateAng[raceCount];
    int order[raceCount];
    for (int k = 0; k < raceCount; k++) {
        Vector2f d = quarterCentre(k) - villageCenter;
        crateAng[k] = atan2(d.y, d.x);
        order[k] = k;
    }
    sort(order, order + raceCount, [&](int a, int b) { return crateAng[a] < crateAng[b]; });
    for (int i = 0; i < raceCount; i++) {
        int a = order[i], b = order[(i + 1) % raceCount];
        float lo = crateAng[a], hi = crateAng[b];
        if (hi <= lo) hi += 6.2831853f;
        frontierGapAngle[i] = (lo + hi) / 2.0f;
        frontierGapOf[a][1] = i;
        frontierGapOf[b][0] = i;
    }
    for (int k = 0; k < raceCount; k++)
        printf("Race quarter %s: crate at (%.0f, %.0f)\n", raceDefs[k].name, storehousePos[k].x, storehousePos[k].y);
    buildQuarterRings();
}

// Know-how (R20-F1): every counter, learned flag and pip pulse lives here; reset as a whole with resetKnowHow()
enum techId {techHandAxe, techPickwork, techSeedSelection, techCount};
struct techDef {const char* name; int threshold; const char* effect; Color pip;};
const techDef techDefs[techCount] = {
    {"Hand Axe",       handAxeTrees,          "chopping 20% faster", steelToolColor},
    {"Pickwork",       pickworkRocks,         "mining 20% faster",   Color(215, 145, 55)},   // muted mine orange (pal.barMine)
    {"Seed Selection", seedSelectionHarvests, "wheat grows 20% faster", Color(200, 165, 70)}, // muted wheat gold (fieldBarColor)
};
struct knowHow {
    int count[techCount] = {};       // trees felled, rocks mined, wheat harvests
    bool learned[techCount] = {};
    int pulseFrames[techCount] = {}; // >0 = HUD pip flashing white after it was learned
};
knowHow know;
void resetKnowHow() { know = knowHow{}; }   // called on (re)start in main; practice() is defined after logEvent

int chopTicks() { return know.learned[techHandAxe] ? handAxeChopTicks : treeChopTicks; }
int wheatGrowTicks() { return know.learned[techSeedSelection] ? (int)(fieldGrowTicks / seedSelectionGrowSpeed) : fieldGrowTicks; }

// A wheat plot (R19-F3). rect/shape are set once the spot is chosen; growth counts up to fieldGrowTicks once sown.
// FARM-F1: held per race in a fieldGrid (fields[race][k]); the race is the row index.
struct plot {
    bool placed = false;    // spot chosen and reserved (houses/shrines/trees avoid it from then on)
    bool claimed = false;   // a barb of this plot's race holds jobField for it (sowing)
    bool built = false;     // sown: wheat grows
    int sowerId = -1;       // stable barb id of the sower while claimed, -1 otherwise (never an index)
    FloatRect rect;         // soil, 8x8
    RectangleShape shape;   // soil fill + race-tinted outline frame
    int growth = 0;
    int fallow = 0;         // R32-F1: ticks left before growth resumes after a sow/reap (staggers the race's plots)
    bool reserved = false;  // R32-F1: plot k > 0 only: slot beside the mill held since plot 0 was placed (until placed)
    bool slotLogged = false; // R32-F1: "slot blocked" printed once
    Vector2f slotPos;       // R32-F1: that slot's soil top-left
    bool fallback = false;  // R37-F1: k > 0, slot given up (blocked too often or unavailable): re-sites on the mill's ring
    int retryTicks = 0;     // R37-F1: >0 = ticks until the next slot check / fallback attempt (counted down in buildfield)
    int slotFails = 0;      // R37-F1: failed checks of the reserved slot
    int stallMask = 0;      // R37-F1: stall gates already logged for this plot (bit per gate), so each prints once
};
using fieldGrid = plot[raceCount][fieldsPerRace];

bool fieldRipe(const plot& fp) { return fp.built && fp.growth >= wheatGrowTicks(); }

// 0..2 growing stages, fieldGrowStages = ripe
int fieldStage(const plot& fp) { return fieldRipe(fp) ? fieldGrowStages : fp.growth * fieldGrowStages / wheatGrowTicks(); }

// R32-F1: restart plot k of a race's row (just sown or reaped) at growth 0. If a growing sibling is less than
// fieldStaggerStages stages ahead of it (effective growth = growth - fallow), it lies fallow until it is that far behind,
// so the race's plots show different stages. Ripe siblings are skipped: they restart (and re-stagger) once reaped.
void replantField(plot (&row)[fieldsPerRace], int k) {
    int step = wheatGrowTicks() * fieldStaggerStages / fieldGrowStages;
    int delay = leanHoursLeft() * ticksPerGameHour;   // R52-F2: no resowing in winter; bare soil until spring, then staggered
    for (int pass = 0; pass < fieldsPerRace; pass++)
        for (int j = 0; j < fieldsPerRace; j++) {
            if (j == k || !row[j].built || fieldRipe(row[j])) continue;
            int g = row[j].growth - row[j].fallow;
            if (abs(g + delay) < step) delay = step - g;
        }
    row[k].growth = 0;
    row[k].fallow = delay;
}

Vector2f fieldWorkSpot(const plot& fp) { return fp.rect.position + fieldStandOffset; }

// Soil + frame + the stand spot below it, padded by fieldClearPx
FloatRect fieldZone(Vector2f pos) {
    float pad = fieldBorderPx + fieldClearPx;
    return FloatRect(pos - Vector2f(pad, pad),
                     Vector2f(fieldSize.x + 2 * pad, fieldStandOffset.y + fieldZoneBelowPx + fieldClearPx + pad));
}

// R32-F1: soil top-left of the zone a plot holds: its spot once placed, else its reserved mill slot. False if neither.
// R37-F1: a fallback plot holds no zone until its ring spot is chosen (then placed, so houses/trees avoid it from there);
// its given-up slot is released, never held, so it can't block its own re-siting or other buildings.
bool plotHeldAt(const plot& fp, Vector2f& pos) {
    if (fp.placed) { pos = fp.rect.position; return true; }
    if (fp.fallback) return false;
    if (fp.reserved) { pos = fp.slotPos; return true; }
    return false;
}

// False if r touches the reserved plot zone (always true before a spot is chosen or a slot reserved)
bool fieldClear(const plot& fp, const FloatRect& r) {
    Vector2f pos;
    return !plotHeldAt(fp, pos) || !r.findIntersection(fieldZone(pos)).has_value();
}

// FARM-F2: one windmill per race in a fixed array (index = barbRace; never removed, so no index goes stale).
// placed: spot chosen with the race's first plot and reserved from then on; built is the validity flag haulers check.
struct mill {
    bool placed = false;
    bool built = false;
    bool claimed = false;   // a barb of this race holds jobMill for it
    int builderId = -1;     // stable barb id of the builder while claimed, -1 otherwise (never an index)
    Vector2f pos;           // body top-left
    int turnFrames = 0;     // >0 = sails turning (milling), counted down per drawn frame
    int puffFrames = 0;     // >0 = flour puff at the door
    float sailAngle = 45.0f; // degrees; starts as an X, the classic resting pose
};
mill mills[raceCount];
int plotsOpen[raceCount] = {};   // R46-F2a: high-water mark of plotsUnlocked(); never shrinks

Vector2f millWorkSpot(const mill& m) { return m.pos + millStandOffset; }

// Body + sail sweep + the stand spot below it, padded by millClearPx
FloatRect millZone(Vector2f pos) {
    Vector2f lo = pos + millHubOffset - Vector2f(millSailLen, millSailLen) - Vector2f(millClearPx, millClearPx);
    float right = pos.x + millHubOffset.x + millSailLen + millClearPx;
    float bottom = pos.y + max(millBodySize.y, millStandOffset.y + fieldZoneBelowPx) + millClearPx;
    return FloatRect(lo, Vector2f(right - lo.x, bottom - lo.y));
}

// Mill body top-left beside a plot at plotPos: side +1 = right of the plot, -1 = left; the sail sweep keeps millGapPx
// from the plot frame, top aligned with the soil so the mill reads as standing at the plot's edge, not on it.
Vector2f millSpotBeside(Vector2f plotPos, int side) {
    float edge = side > 0 ? plotPos.x + fieldSize.x + fieldBorderPx + millGapPx + (millSailLen - millHubOffset.x)
                          : plotPos.x - fieldBorderPx - millGapPx - (millHubOffset.x + millSailLen);
    return Vector2f(edge, plotPos.y);
}

// R32-F1: soil top-left of plot k's slot beside a mill at millPos; side as in millSpotBeside (mill right of plot 0 = +1).
// Side -1 mirrors the offset across the sail hub, so the extra plots always stand on the far side from plot 0.
Vector2f fieldSlotPos(Vector2f millPos, int side, int k) {
    Vector2f o = fieldSlotOffsets[k];
    if (side < 0) o.x = 2.0f * millHubOffset.x - fieldSize.x - o.x;
    return millPos + o;
}

// R32-F1: the haulers' lane below a mill's door: door width padded by millClearPx, from the door down past the stand spot
FloatRect millDoorLane(Vector2f millPos) {
    Vector2f lo = millPos + millDoorPos - Vector2f(millClearPx, 0.0f);
    return FloatRect(lo, Vector2f(millDoorSize.x + 2 * millClearPx, millStandOffset.y + fieldZoneBelowPx + millDoorLanePx - millDoorPos.y));
}

// FARM-F1: false if r touches any race's reserved plot zone. FARM-F2: or any reserved mill zone.
bool fieldsClear(const fieldGrid& fields, const FloatRect& r) {
    for (int q = 0; q < raceCount; q++) {
        for (int k = 0; k < fieldsPerRace; k++)
            if (!fieldClear(fields[q][k], r)) return false;
        if (mills[q].placed && r.findIntersection(millZone(mills[q].pos)).has_value()) return false;
    }
    return true;
}

// FARM-F1: the plot in a race's row whose work spot is (tx,ty), or -1. A reaper is matched to its plot by its target spot.
int fieldIndexAt(const plot (&row)[fieldsPerRace], int tx, int ty) {
    for (int k = 0; k < fieldsPerRace; k++) {
        if (!row[k].placed) continue;
        Vector2f wp = fieldWorkSpot(row[k]);
        if ((int)wp.x == tx && (int)wp.y == ty) return k;
    }
    return -1;
}

vector<FloatRect> lakes;         // water rects, fixed after spawnLakes()
vector<VertexArray> lakeTiles;    // per lake: bank quads then water tiles as triangles (same index as lakes; swap-remove together)
vector<Vector2f> shorePoints;     // 1px spots just outside each lake's rim at its edge midpoints (cached for angling, R19-F1)

// True if segment a->b touches box (slab test). Pure geometry, used only by lakeClear().
bool segmentHitsRect(Vector2f a, Vector2f b, const FloatRect& box) {
    float t0 = 0.0f, t1 = 1.0f;
    Vector2f d = b - a;
    float lo[2] = {box.position.x, box.position.y}, hi[2] = {box.position.x + box.size.x, box.position.y + box.size.y};
    float p[2] = {a.x, a.y}, dv[2] = {d.x, d.y};
    for (int k = 0; k < 2; k++) {
        if (dv[k] == 0.0f) { if (p[k] < lo[k] || p[k] > hi[k]) return false; continue; }
        float ta = (lo[k] - p[k]) / dv[k], tb = (hi[k] - p[k]) / dv[k];
        if (ta > tb) swap(ta, tb);
        t0 = max(t0, ta); t1 = min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

// The one lake check: false if r comes within lakeClearPx of any lake. With pathFrom (top-left of an entity the size of r),
// the straight walk from pathFrom to r.position must also stay clear; a lake the walker already stands in is ignored
// so anything that drifted into water can still leave.
bool lakeClear(const FloatRect& r, const Vector2f* pathFrom = nullptr) {
    for (auto& l : lakes) {
        FloatRect zone(l.position - Vector2f(lakeClearPx, lakeClearPx), l.size + Vector2f(2 * lakeClearPx, 2 * lakeClearPx));
        if (r.findIntersection(zone).has_value()) return false;
        if (!pathFrom) continue;
        FloatRect sweep(zone.position - r.size, zone.size + r.size); // top-left points whose box would touch the zone
        if (sweep.contains(*pathFrom)) continue;
        if (segmentHitsRect(*pathFrom, r.position, sweep)) return false;
    }
    return true;
}

// R45-F1b: Far Reaches. ruinStones empty = no site was found at startup (then no ridge or camps either).
vector<FloatRect> ruinStones;              // standing + fallen stone rects, fixed after spawnRuins()
Vector2f ruinCentre(-1000.0f, -1000.0f);   // site centre, set by spawnRuins()
Vector2f campSpot[raceCount];              // each race's lean-to top-left, fixed after spawnRuins()
struct leanTo {int race = -1; Vector2f pos;};
vector<leanTo> camps;                      // standing lean-tos, at most one per race; a built camp is just its entry here
bool hasRuins() { return !ruinStones.empty(); }
bool campBuilt(int r) {
    for (auto& c : camps) if (c.race == r) return true;
    return false;
}
// False if r touches the cleared ruin site disc (nearest point of the rect to the centre)
bool ruinsClear(const FloatRect& r) {
    if (!hasRuins()) return true;
    float nx = clamp(ruinCentre.x, r.position.x, r.position.x + r.size.x);
    float ny = clamp(ruinCentre.y, r.position.y, r.position.y + r.size.y);
    float dx = nx - ruinCentre.x, dy = ny - ruinCentre.y;
    return dx * dx + dy * dy > ruinSiteRadius * ruinSiteRadius;
}

// R47-F4: sites. pois[i] and poiShapes[i] (footprint rect) always describe the same site; only spawnPois() adds and only
// removePoi() removes, touching both. Anything held across ticks holds poi.id and re-looks it up with findPoiById().
struct poi {
    int id = -1;
    poiKind kind = poiHut;
    bool found = false;        // a barb or a party has been there; glows from now on
    bool spent = false;        // searched once; dimmed, never a party target again
    int spentTicks = 0;        // ticks since spent; removed at poiSpentLingerTicks
};
vector<poi> pois;
vector<RectangleShape> poiShapes;
int nextPoiId = 0;
int findPoiById(int id) {
    for (int i = 0; i < (int)pois.size(); i++) if (pois[i].id == id) return i;
    return -1;
}
Vector2f poiCentre(int i) { return poiShapes[i].getPosition() + poiShapes[i].getSize() / 2.0f; }
void removePoi(int i) {
    if (i < 0 || i >= (int)pois.size()) return;
    printf("Site gone: %s %d crumbles away\n", poiNames[pois[i].kind], pois[i].id);
    pois[i] = move(pois.back()); pois.pop_back();
    poiShapes[i] = move(poiShapes.back()); poiShapes.pop_back();
}
// False if r touches any site's footprint grown by poiClearPad
bool poiClear(const FloatRect& r) {
    for (auto& s : poiShapes) {
        FloatRect z(s.getPosition() - Vector2f(poiClearPad, poiClearPad), s.getSize() + Vector2f(2 * poiClearPad, 2 * poiClearPad));
        if (r.findIntersection(z)) return false;
    }
    return true;
}

// Work target of a barb. targetNone with a set targetx is a tree (or a hunt/wander/build spot, which own their flags).
enum targetKind {targetNone, targetBush, targetRock, targetLake, targetField, targetDepot}; // targetDepot: hauling a load (R29-F1)
enum memKind {memNone, memTree, memRock, memBush};   // INBOX-F2
struct workMem {Vector2f pos; memKind kind = memNone;};   // pos = the work spot a barb stood on; validated by position, never an index
enum carryLoad {carryNone, carryWood, carryStone, carryWheat};   // R29-F1: what a hauler holds; set only while target == targetDepot
// FARM-F2: carryWheat is a reaped sheaf headed for the race's mill (storehouse/campfire if none). It lives in the same
// carryKind field, so every carryLoad reset (dropCarry, deposit, removeBarb) clears it too.
const char* carryName(carryLoad c) { return c == carryWood ? "wood" : c == carryStone ? "stone" : c == carryWheat ? "wheat" : "none"; }

// R39-F1: a barb's planned walk. cells[idx..len) are trail cells still to pass (start and goal cells excluded);
// planned with len == 0 means walk straight (A* failed, too short or off the grid). goalX/Y is the target it was
// planned for: a different targetx/y replans. lastCell is the wear tracker and survives clearRoute().
struct barbRoute {
    int cells[routeMaxCells];
    int len = 0, idx = 0;
    int goalX = -1, goalY = -1;
    bool planned = false;
    int lastCell = -1;           // cell the barb last added wear to (wear once per cell entered)
};

enum raidRole { raidNone, raidAttacker, raidDefender };   // R52-F1

// R53-F4: the one exclusive state a barb is in (work flags like chopping/building stay separate). Set only by
// beginActivity(), cleared only by endActivity(), which also runs the activity's cleanup.
enum barbActivity { actNone, actShelter, actExpedition, actRaidAttack, actRaidDefend, actTemper, actSmelt, actDrill, actWatch,
                    actKneel, actHerb, actCaravan, activityCount };   // R53-F2: actHerb = tending the herb bed; R54-F1: actCaravan
struct activityDef {
    const char* name;
    bool ownsBarb;    // its tick owns the barb outright: every job selector skips it (barbAvailable() false)
    bool atForge;     // walked and worked at the forge (atForgeTask())
    bool yieldsToClaim; // any task claim ends it (clearTraining())
};
const activityDef activityDefs[activityCount] = {
    {"none",          false, false, false},
    {"shelter",       true,  false, false},
    {"expedition",    true,  false, false},
    {"raid (attack)", true,  false, false},
    {"raid (defend)", true,  false, false},
    {"temper",        false, true,  false},
    {"smelt",         false, true,  false},
    {"drill",         false, false, true},
    {"watch",         false, false, true},
    {"kneel",         false, false, true},
    {"tend herbs",    false, false, true},   // R53-F2: any claim outranks tending, like the drill
    {"caravan",       true,  false, false},  // R54-F1: caravanTick() owns the barb for the whole trip
};

struct barb {
    int id = -1;              // stable, unique; assigned only by makeBarb()
    int x, y;
    bool moving = false;
    int targetx = -1, targety = -1;
    int timer = 0;
    bool chopping = false;
    bool movingbspot = false;
    bool building = false;
    bool reservedwood = false;
    int reservedstone = 0;       // stone held for a house foundation (or a shrine); refunded with the wood on cancel
    buildJob jobKind = jobNone;  // structure the movingbspot/building belongs to; each builder skips the others' jobs
    int stepTimer = 0;
    int wanderX = -1, wanderY = -1; // R29-F2: target of the current wander walk; a walk is a wander only while targetx/y match
    targetKind target = targetNone; // what targetx/targety points at; chopping + kind = the work (shares timer/chopping)
    int targetBush = -1;         // bush index (bushes are never removed, so indices are stable)
    int targetShore = -1;        // shorePoints index while target == targetLake (shore points are fixed after startup)
    bool deertargeting = false;
    int targetDeerId = -1;
    int hunger = 0;
    int slowTick = 0;
    barbRace race = raceHuman;   // rolled once in makeBarb()
    int homeHouse = -1;          // house.id of the birth house (stable id, not an index); -1 = founder
    councilDecree lastVote = decreeNone; // ballot from the latest dawn; lives in the struct so it is dropped with the barb
    bool hasSpear = false;       // R20-F3: kept for life; cleared only in makeBarb()
    int spearTimer = 0;          // R20-F3: craft progress while jobKind == jobSpear; separate from timer; cleared by clearSpearCraft()
    bool spearPaid = false;      // POL-F3: craft cost already taken from the race stock; cleared only by refundSpear() or on completion
    bool spearAtForge = false;   // POL-F5a: this craft is at the forge (forgeActive at claim); cleared by clearSpearCraft()
    int weaponTier = 0;          // R47-F8: tool tier snapshotted when the weapon was made (setWeapon(weaponEquip)); 0 when unarmed
    const char* name = "";       // R25-F1: points into barbNames (no heap strings); set in makeBarb()
    barbTrait trait = traitSwift; // R25-F1: rolled once in makeBarb()
    bool shinobi = false;        // R25-F2: Sasuke; set once in makeBarb()
    carryLoad carryKind = carryNone; // R29-F1: load being hauled to a storehouse/the campfire; dropped with the barb, cleared by dropCarry()
    int carryAmount = 1;         // R34-F1: units in the load (kShaftYield stone from a shaft); back to 1 whenever carryKind clears
    int health = barbMaxHealth;  // R30-F5: 0 = dies at the end of the tick (removeBarb)
    int starveTicks = 0;         // R30-F5: ticks spent hungry since the last meal; starving past starveGraceTicks
    bool garrisonInside = false; // INBOX-F1: reached the doorstep and hidden inside (not drawn, not pickable)
    int garrisonHouseId = -1;    // INBOX-F1: house.id (stable, never an index); validated every tick
    workMem mem[memSpotCount];   // INBOX-F2: recent work spots, [0] newest; reset in makeBarb(), moves with the struct on swapRemove
    bool memReturn = false;      // INBOX-F2: a trip just ended (deposit or pick); the next search tries mem[] first
    bool memShared = false;      // INBOX-F2: already shared a fresh spot this trip
    leaderNeed order = needNone; // INBOX-F2b: leader's individual order; needNone = no order. Never touches garrison state
    int orderTicks = 0;          // INBOX-F2b: ticks until the order expires; reset in makeBarb()
    barbRoute route;             // R39-F1: moves with the struct on swapRemove; reset only through clearRoute()
    bool inMine = false;         // R40-F1: down its race's shaft (not drawn); set at the stand spot, cleared only by leaveMine()
    int temperTimer = 0;         // R41-F2: work ticks at the forge; separate from timer; cleared by clearTemper()
    bool temperPaid = false;     // R41-F2: kTemperStoneCost already taken; refunded by clearTemper() unless the temper finished
    int toolCharges = 0;         // R41-F2: tempered rock mines left; set on a finished temper, reset by clearTemper() and makeBarb()
    int smeltTimer = 0;          // R47-F7: work ticks at the forge; separate from timer; cleared by clearSmelt()
    bool smeltPaid = false;      // R47-F7: kSmeltStoneCost already taken; refunded by clearSmelt() unless the smelt finished
    int jobTier = 0;             // R47-F7: tool tier read once when the current chop/mine started (toolTierOf); scales chopTicks/mineTicks
    int drillSlot = -1;          // R47-F6: slot index in its race's halls[].slotId while training; -1 otherwise
    int drillTimer = 0;          // R47-F6: work ticks since the last hit (separate from timer); reset by clearTraining()
    int drillHits = 0;           // R47-F6: hits this session; reset by clearTraining()
    int drillCount = 0;          // R47-F6: the drill counter, hits over the barb's life; reset only in makeBarb()
    int watchStop = 0;           // R51-F1: patrol waypoint index round the ring
    int watchPreyId = -1;        // R51-F1: prowler.id being fought (stable id, re-resolved every tick); -1 = patrolling
    int watchCooldown = 0;       // R51-F1: ticks until the next strike
    int raidPreyId = -1;         // R52-F1: barb.id of the foe being fought (re-resolved every tick); -1 = none
    int raidCooldown = 0;        // R52-F1: ticks until the next strike
    bool raidBeaten = false;     // R52-F1: knocked down to kRaidHealthFloor; out of the fight
    int raidFlash = 0;           // R52-F1: hit flash, drawn frames left (counted down in main after drawBarbs())
    int sick = 0;                // R52-F4: ticks of cough left (0 = well); set only by catchCough(), cleared by clearSick()
    int sickImmune = 0;          // R52-F4: ticks of immunity left after a recovery
    int cure = 0;                // R52-F4: reserved for phase 2 (herb hut remedy); unused in phase 1
    barbActivity activity = actNone; // R53-F4: the one exclusive state; written only by beginActivity()/endActivity()
    raceStock loot{0, 0, 0};     // R53-F1: plunder pack carried home; set only by raidTick() on a win, emptied only by settleLoot()
    int lootFrom = -1;           // R53-F1: race the pack was taken from (gets it back on a drop); -1 = no pack
    int herbTimer = 0;           // R53-F2: work ticks at the herb bed (separate from timer); reset by endActivity()
};
// R53-F4: the only writer of barb.activity on the way in. Starting a second activity over a live one is a bug: a debug
// build aborts; a release build ends the old one (with its cleanup) first so no state is left half-set.
void endActivity(barb& b);
void beginActivity(barb& b, barbActivity a) {
    if (b.activity == a) return;
    if (b.activity != actNone) {
        fprintf(stderr, "ACTIVITY CLASH - barb %d: %s started over %s\n", b.id, activityDefs[a].name, activityDefs[b.activity].name);
        if (kDebugBuild) abort();
        endActivity(b);
    }
    b.activity = a;
}
// R39-F1: the one route reset, called wherever moving/movingbspot is cleared and before a barb is swapRemoved
void clearRoute(barb& b) {
    b.route.len = b.route.idx = 0;
    b.route.goalX = b.route.goalY = -1;
    b.route.planned = false;
}
bool barbHidden(const barb& b) { return b.activity == actShelter && b.garrisonInside; }   // INBOX-F1
// R45-F2: the one eligibility gate every job selector checks: false while bellTick() (garrison), expeditionTick() or
// raidTick() owns the barb (R53-F4: one table check, activityDef::ownsBarb)
bool barbAvailable(const barb& b) { return !activityDefs[b.activity].ownsBarb; }
// R47-F7: busy at the forge (temperTick() or smeltTick() walks and works the barb); wander, hunts and orders skip it
bool atForgeTask(const barb& b) { return activityDefs[b.activity].atForge; }

// R51-F1: the one way out of the watch state. Leaves movement alone, like clearTraining(); watchTick() stops the walk
// on its own exits. Called from clearTraining() (so every claim path, the bell and removeBarb() end a watch too).
// R53-F4: endActivity(b, a) ends a only if it is the live one; the cleanup lives in endActivity()
void endActivity(barb& b, barbActivity a);
void clearWatch(barb& b) { endActivity(b, actWatch); }

// R52-F3: the one way out of kneeling. Leaves movement alone like clearWatch(), so a task assigned right after keeps
// its walk; riteTick() stops the walk on its own exits. Called from clearTraining(), so every claim path, the bell,
// expeditions and removeBarb() (via releaseBarbWork()) end a kneel too.
void clearKneel(barb& b) { endActivity(b, actKneel); }

// R47-F6: the one way out of the drill state (session done, interrupted, reassigned, barracks gone, sheltering, death).
// Frees the barb's slot if it still holds it and leaves the drill counter alone. Like clearSmelt() it leaves movement
// alone, so a new assignment made right after is not undone (drillTick() clears movement on its own exits). Safe on a
// barb that isn't training; every claim path that sets moving/chopping/building/timer on an idle barb calls it first.
// R51-F1/R52-F3: a claim that outranks the drill outranks the watch and the kneel too (R53-F4: activityDef::yieldsToClaim)
void clearTraining(barb& b) {
    if (activityDefs[b.activity].yieldsToClaim) endActivity(b);
}

// R45-F2: the one party. Members are held by barb id (never an index) and re-resolved by expeditionTick() every tick;
// removeBarb() invalidates a dying member's id through expeditionMemberLost(), so the party dissolves the next tick.
struct expeditionParty {
    enum Phase {Out, Delve, Back} phase = Out;
    bool active = false;
    int race = -1;
    int memberIds[expPartySize] = {};
    Vector2f delveSpot[expPartySize];   // each member's stand spot at its own ruin stone, picked at launch
    int rations = 0;                    // food carried; a hungry member eats one
    int timer = 0;                      // delve ticks, counted once every member stands at its stone
    int woundCooldown = 0;
    bool relic = false;                 // found on the delve; memberIds[0] carries it home
    const char* lostName = nullptr;     // name of a member removed by removeBarb() (names point into static tables)
    int poiId = -1;                     // R47-F4: target site (poi.id, re-looked up every tick); -1 = the ruins
    poiKind siteKind = poiHut;          // R47-F4: kept for the ticker after the site is gone
    int rewardFood = 0, rewardStone = 0; // R47-F4: a site's reward, carried home and paid on return
    bool hollow = false;                // R47-F5: this trip goes down the Hollow (poiId stays -1)
    int lootIdx = -1;                   // R47-F5: hollowLoot row rolled on surfacing; -1 = not yet
};
expeditionParty party;
// R47-F5: the Hollow. Placed with the ruins (no ruins, no Hollow); lootLeft counts down to 0 = picked clean.
bool hollowPlaced = false;
Vector2f hollowPos(-1000.0f, -1000.0f);   // top-left, fixed after spawnRuins()
int hollowLootLeft = hollowLootCap;
int hollowFlash = 0;                      // lintel flash ticks left, counted down by expeditionTick()
bool hollowOpen() { return hollowPlaced && hollowLootLeft > 0; }
// Below right now: hidden (drawn, hovered and picked like a miner down a shaft)
bool barbInHollow(const barb& b) { return b.activity == actExpedition && party.active && party.hollow && party.phase == expeditionParty::Delve; }
RectangleShape hollowShape() {   // the mouth's footprint, for picking and the select ring of a member below
    RectangleShape s(hollowSize);
    s.setPosition(hollowPos);
    return s;
}
// R47-F5: the loot carrier on the walk home (memberIds[0]); -1 = nobody or an empty haul
int lootCarrierId() {
    if (!party.active || !party.hollow || party.phase != expeditionParty::Back || party.lootIdx < 0) return -1;
    const hollowLootDef& l = hollowLoot[party.lootIdx];
    return l.food + l.stone > 0 ? party.memberIds[0] : -1;
}
int raceRelics[raceCount] = {0, 0, 0};   // relics each race has brought home, capped at expRelicCap
int expeditionCooldown = expFirstDelayTicks;
void expeditionMemberLost(const barb& b) {
    if (!party.active || b.activity != actExpedition) return;
    for (auto& id : party.memberIds) if (id == b.id) { id = -1; party.lostName = b.name; }
}
// R45-F2: the relic carrier on the walk home, for drawBarbs(); -1 = nobody
int relicCarrierId() {
    return party.active && party.relic && party.phase == expeditionParty::Back ? party.memberIds[0] : -1;
}

// R52-F1: the one raid. Raiders and defenders are held by barb id (never an index) and re-resolved by raidTick() every
// tick; a missing id is dropped there, and removeBarb() invalidates a dying member's id through raidMemberLost().
struct raidParty {
    enum Phase {March, Fight, Home} phase = March;
    bool active = false;
    int att = -1, def = -1;                 // aggressor and target race
    int raiderIds[kRaidPartySize] = {-1, -1, -1};
    int defenderIds[kRaidDefenderMax] = {-1, -1, -1};
    int ticks = 0;                          // since launch (kRaidMaxTicks backstop)
    int fightTicks = 0, holdTicks = 0, scanTimer = 0;
    int defendersBeaten = 0;
    bool won = false;                       // outcome, for the walk home
};
raidParty raid;
bool raidThreat[raceCount] = {};            // R52-F1: set by raidTick() (runs first), read by bellTick() like a wolf at the crate
int raidPairCooldown[raceCount][raceCount] = {
    {kRaidPairCooldownTicks, kRaidPairCooldownTicks, kRaidPairCooldownTicks},
    {kRaidPairCooldownTicks, kRaidPairCooldownTicks, kRaidPairCooldownTicks},
    {kRaidPairCooldownTicks, kRaidPairCooldownTicks, kRaidPairCooldownTicks}};   // kept symmetric
static_assert(raceCount == 3, "raidPairCooldown initialiser is written for three races");

// R54-F1: the one caravan. Members are held by barb id (never an index) and re-resolved by caravanTick() every tick;
// removeBarb() invalidates a dying member's id through caravanMemberLost(), and the trip aborts the next tick.
// The cart's goods live here, with the race that gets them back on an abort.
struct caravanTrip {
    enum Phase {Out, Trade, Home} phase = Out;
    bool active = false;
    int from = -1, to = -1;                 // sending race and the rival crate's race
    int memberIds[kTradeMembers] = {-1, -1};
    raceStock cargo{0, 0, 0};               // on the cart now
    int cargoOwner = -1;                    // race the cargo goes back to on an abort (sender out, rival home)
    int giveKind = -1;                      // kind sent out (0 food, 1 wood, 2 stone)
    int ticks = 0, stayTicks = 0;
};
caravanTrip caravan;
static_assert(kTradeMembers == 2, "caravanTrip::memberIds initialiser is written for two members");
int tradePairCooldown[raceCount][raceCount] = {
    {kTradePairCooldownTicks, kTradePairCooldownTicks, kTradePairCooldownTicks},
    {kTradePairCooldownTicks, kTradePairCooldownTicks, kTradePairCooldownTicks},
    {kTradePairCooldownTicks, kTradePairCooldownTicks, kTradePairCooldownTicks}};   // kept symmetric
int relFriendlyTicks[raceCount][raceCount] = {};   // R54-F1: friendly timer per pair (symmetric); > 0 = friendly
bool pairFriendly(int a, int b) {
    return a >= 0 && a < raceCount && b >= 0 && b < raceCount && a != b && relFriendlyTicks[a][b] > 0;
}
void caravanMemberLost(const barb& b) {
    if (!caravan.active) return;
    for (auto& id : caravan.memberIds) if (id == b.id) id = -1;
}
int caravanCartId() { return caravan.active ? caravan.memberIds[0] : -1; }   // R54-F1: drawBarbs() puts the cart on this barb
bool caravanLaden() { return caravan.active && caravan.cargo.food + caravan.cargo.wood + caravan.cargo.stone > 0; }

// R52-F1: the one way out of the raid state, on every exit (outcome, abort, beaten defender, removeBarb, backstop)
bool inRaid(const barb& b) { return b.activity == actRaidAttack || b.activity == actRaidDefend; }   // R53-F4
void clearRaid(barb& b) {
    if (inRaid(b)) endActivity(b);   // R53-F4: the raid fields are reset there
}
// R52-F4: the add/remove pair for the cough. catchCough() is the only place `sick` is set; clearSick() wipes the whole
// state (removeBarb() calls it, so nothing outlives the barb)
void catchCough(barb& b) {
    b.sick = uniform_int_distribution<int>(sickMinTicks, sickMaxTicks)(gen);
    b.sickImmune = 0;
}
void clearSick(barb& b) {
    b.sick = 0;
    b.sickImmune = 0;
    b.cure = 0;
}
bool lootLaden(const barb& b) { return b.loot.food + b.loot.wood + b.loot.stone > 0; }   // R53-F1
void raidMemberLost(const barb& b) {
    if (!raid.active || !inRaid(b)) return;
    for (auto& id : raid.raiderIds) if (id == b.id) id = -1;
    for (auto& id : raid.defenderIds) if (id == b.id) id = -1;
}

const raceDef& raceOf(const barb& b) { return raceDefs[b.race]; }
const traitDef& traitOf(const barb& b) { return traitDefs[b.trait]; }
int forageBonus(const barb& b) { return b.trait == traitForager ? foragerFoodBonus : 0; }
int hungerThresholdOf(const barb& b) {
    float s = b.trait == traitSturdy ? sturdyHungerScale : b.trait == traitGlutton ? gluttonHungerScale : 1.0f;
    return (int)(hungerThreshold * s);
}
bool isGentry(const barb& b) { return b.homeHouse < 0; }   // R24-F1: founders; derived, no stored flag

// RACE-F1: the one way to reach a race's stockpile. An out-of-range race falls back to race 0 instead of indexing past the array.
int clampRace(int r) { return (r >= 0 && r < raceCount) ? r : 0; }
raceStock& stockOfRace(int r) { return stock[clampRace(r)]; }
raceStock& stockOf(const barb& b) { return stockOfRace(b.race); }
bool canAfford(const raceStock& s, int w, int st, int f = 0) { return s.wood >= w && s.stone >= st && s.food >= f; }
void spend(raceStock& s, int w, int st, int f = 0) {   // callers check canAfford first; the max() keeps a stock from going negative
    s.wood = max(0, s.wood - w);
    s.stone = max(0, s.stone - st);
    s.food = max(0, s.food - f);
}

// R28-F2: leaders are held by barb id (never index); a missing id means the race has no leader
int raceLeaderId[raceCount] = {-1, -1, -1};
bool raceElected[raceCount] = {false, false, false};
leaderNeed raceNeed[raceCount] = {needNone, needNone, needNone};   // R30-F2: set at dawn by the leader; needNone = global weights
int findBarbById(const vector<barb>& barbs, int id) {
    if (id < 0) return -1;
    for (int i = 0; i < (int)barbs.size(); i++) if (barbs[i].id == id) return i;
    return -1;
}
bool isLeader(const barb& b) {
    return b.race >= 0 && b.race < raceCount && raceElected[b.race] && raceLeaderId[b.race] >= 0 && raceLeaderId[b.race] == b.id;
}
int mineTicks(const barb& b) {   // R47-F7: / the tool tier snapshotted at job start (b.jobTier), never looked up per tick
    return (int)(rockMineTicks / raceOf(b).mineSpeed / (know.learned[techPickwork] ? pickworkMineSpeed : 1.0f)
                 / toolTierSpeed[clamp(b.jobTier, 0, toolTierCount - 1)]);
}
int chopTicks(const barb& b) { return (int)(chopTicks() / toolTierSpeed[clamp(b.jobTier, 0, toolTierCount - 1)]); }   // R47-F7
// R47-F7: called on the tick a chop or mine begins (the barb is not yet chopping): the one place the tier is read
void startToolJob(barb& b) { if (!b.chopping) b.jobTier = toolTierOf(b.race); }

// R29-F1: every way out of a haul other than the deposit itself; the load is lost. Safe on a barb that isn't hauling.
void dropCarry(barb& b) {
    if (b.carryKind != carryNone) printf("LOAD DROPPED - barb %d (%s)\n", b.id, carryName(b.carryKind));
    b.carryKind = carryNone;
    b.carryAmount = 1;   // R34-F1
    if (b.target == targetDepot) b.target = targetNone;
}

// R53-F4: the only writer of barb.activity on the way out. Runs the live activity's cleanup (what clearTraining(),
// clearWatch(), clearKneel(), clearRaid(), clearTemper() and clearSmelt() used to do) and leaves movement alone, so a
// task assigned right after keeps its walk; each tick stops the walk on its own exits. Safe on a barb with no activity.
void endActivity(barb& b) {
    switch (b.activity) {
        case actShelter:     // ungarrison() frees the keep slot and resets the walk
            b.garrisonInside = false;
            b.garrisonHouseId = -1;
            break;
        case actExpedition:  // expeditionReset() (called by every exit) resets the walk and work
            break;
        case actRaidAttack:
        case actRaidDefend:
            b.raidPreyId = -1;
            b.raidCooldown = 0;
            b.raidBeaten = false;
            break;
        case actTemper:      // R41-F2: refund the stone if still owed (the finish path clears temperPaid first)
            if (b.temperPaid) {
                stockOf(b).stone += kTemperStoneCost;
                printf("TEMPER REFUNDED - barb %d, %s +%d stone\n", b.id, racePlurals[clampRace(b.race)], kTemperStoneCost);
            }
            b.temperPaid = false;
            b.temperTimer = 0;
            break;
        case actSmelt:       // R47-F7: refund if still owed and free the forge
            if (b.smeltPaid) {
                stockOf(b).stone += kSmeltStoneCost;
                printf("SMELT REFUNDED - barb %d, %s +%d stone\n", b.id, racePlurals[clampRace(b.race)], kSmeltStoneCost);
            }
            if (forgeSmelterId == b.id) forgeSmelterId = -1;
            b.smeltPaid = false;
            b.smeltTimer = 0;
            break;
        case actDrill:       // R47-F6: free the slot if still held; the drill counter stays
            if (b.race >= 0 && b.race < raceCount && b.drillSlot >= 0 && b.drillSlot < kDrillSlots && halls[b.race].slotId[b.drillSlot] == b.id)
                halls[b.race].slotId[b.drillSlot] = -1;
            b.drillSlot = -1;
            b.drillTimer = 0;
            b.drillHits = 0;
            break;
        case actWatch:       // R51-F1
            b.watchPreyId = -1;
            b.watchCooldown = 0;
            break;
        case actHerb:        // R53-F2: free the bed if still held; picked herbs stay picked
            if (b.race >= 0 && b.race < raceCount && herbGardens[b.race].tenderId == b.id) herbGardens[b.race].tenderId = -1;
            b.herbTimer = 0;
            break;
        case actKneel:       // R52-F3: nothing else to reset
        case actCaravan:     // R54-F1: the cart's goods live on the caravan entry, not the barb
        case actNone:
        case activityCount:
            break;
    }
    b.activity = actNone;
}
void endActivity(barb& b, barbActivity a) { if (b.activity == a) endActivity(b); }

// R41-F2: every way out of a temper (finished, interrupted, reassigned, forge gone, sheltering, death) goes through here.
// Refunds the stone if it is still owed (in endActivity()) and resets toolCharges even with no temper live (the tempered
// edge is lost with the forge); the finish path clears temperPaid first and sets the charges after.
void clearTemper(barb& b) {
    endActivity(b, actTemper);
    b.toolCharges = 0;
}

// R47-F7: every way out of a smelt (finished, interrupted, reassigned, forge gone, sheltering, expedition, death) goes
// through here. Refunds the stone if still owed and frees the forge (in endActivity()); leaves movement alone.
void clearSmelt(barb& b) { endActivity(b, actSmelt); }

// R34-F1: true if b's work target is its own race's shaft stand spot (the spot is fixed, so this holds in any shaft state)
bool atShaftTarget(const barb& b) {
    if (b.race < 0 || b.race >= raceCount || b.target != targetRock) return false;
    Vector2f s = shaftWorkSpot(b.race);
    return b.targetx == (int)s.x && b.targety == (int)s.y;
}
// R34-F1: work ticks for the barb's current mining job: doubled at the shaft
// R41-F2: a tempered pick (toolCharges > 0) scales rock mining only; this is the one place the charge speedup applies
int mineTicksFor(const barb& b) {
    if (atShaftTarget(b)) return kShaftTickMult * mineTicks(b);
    return b.toolCharges > 0 ? (int)(mineTicks(b) / kTemperMineSpeed) : mineTicks(b);
}
// R40-F1: the one miner a shaft serves: the first ungarrisoned own-race barb targeting its stand spot (-1 = none).
// minematerials() derives its per-tick shaftMiner[] from this, so the tooltip and the work bar agree with the work loop.
int shaftMinerOf(const vector<barb>& barbs, int r) {
    for (int j = 0; j < (int)barbs.size(); j++)
        if (barbs[j].race == r && barbs[j].activity != actShelter && atShaftTarget(barbs[j])) return j;
    return -1;
}
// R40-F1: down the shaft right now. Separate from barbHidden() (garrison only); re-checks the job so a stale flag never hides a barb
bool barbUnderground(const barb& b) {
    return (b.inMine && b.activity != actShelter && atShaftTarget(b) && mineShafts[b.race].built) || barbInHollow(b);   // R47-F5: or down the Hollow
}
// R40-F1: the one way out of the pit. withLoad = climbing out with stone (starts the lintel flash). Safe if not in the mine.
void leaveMine(barb& b, bool withLoad = false) {
    if (!b.inMine) return;
    b.inMine = false;
    if (withLoad && b.race >= 0 && b.race < raceCount) mineShafts[b.race].flashTicks = kShaftFlashTicks;
    printf("SHAFT CLIMB OUT - barb %d%s\n", b.id, withLoad ? " with stone" : "");
}

struct deer {            // any game animal; species picks its row in speciesDefs
animalSpecies species = speciesDeer;
int x, y;
int id = -1;
int health = 100;
int stepTimer = 0;
bool moving = false;
int targetx = -1, targety = -1;
bool invincible = false;
int invincibleTimer = 0;
int fleeTimer = 0;      // ticks of flee left; only deerwander touches the flee fields (never health/invincible)
int fleeCooldown = 0;
};

int nextDeerId = 0;

const speciesDef& speciesOf(const deer& d) { return speciesDefs[d.species]; }

int countSpecies(const vector<deer>& deers, animalSpecies s) {
	int n = 0;
	for (auto& d : deers) if (d.species == s) n++;
	return n;
}

struct trader {
    enum Phase {Arriving, Trading, Leaving} phase = Arriving;
    Vector2f exit;
    int stepTimer = 0;
    int timer = 0;
    RectangleShape shape;
    int race = -1;            // POL-F4: race served this visit; -1 = nobody (campfire, buys nothing)
    bool buysFood = false;    // POL-F4: food-for-wood fallback (no race could pay in stone)
    bool toStore = false;     // POL-F4: walking to/at race's storehouse; false = campfire
    Vector2f gap;             // R47-F3: waypoint in the gap between two frontiers, walked through on the way in and out
    bool pastGap = false;     // R47-F3: reached the gap on this leg (reset when leaving)
};
int raceCooldown[raceCount] = {};   // POL-F4: visits left before a race can be served again

// Wolves own their shape (like the trader), so there is no parallel vector to keep in sync
struct wolf {
    enum Phase {Hunting, Leaving} phase = Hunting;
    Vector2f exit;
    int stepTimer = 0;
    RectangleShape shape;
};

// R48-F1: prowlers own their state (no parallel vector). Targets are stable ids (barb.id / house.id), never indices,
// re-resolved every tick. fade > 0 = dissolving (dawn or campfire, one path); removed when it runs out.
struct prowler {
    int id = -1;
    Vector2f pos;             // hitbox top-left
    int targetBarbId = -1;    // at most one of the two targets is set
    int targetHouseId = -1;
    int sniffedHouseId = -1;  // last house reached; skipped by the search so the prowler moves on
    int searchTimer = 0;      // ticks until the next nearest-target search
    int stepTimer = 0;
    int biteCooldown = 0;
    int fade = 0;
    int hp = prowlerMaxHp;    // R51-F1: also set explicitly at the spawn site; 0 = slain, dissolving
    int flashFrames = 0;      // R51-F1: drawn frames of white hit flash left (counted down after drawProwlers())
};
int nextProwlerId = 0;
vector<Vector2f> watchTorchPts;   // R51-F1: this frame's torch pixels, filled by drawBarbs(), drawn by drawWatchTorches()

char tradeMsg[64] = "";
int tradeMsgTimer = 0;

// tint.a == 0: normal ticker colour. POL-F2: text is the base text (names copied in, never a barb index/pointer);
// count > 1 renders as " xN"; quickFade lines (race-tinted) fade over the last raceEventFadeTicks only.
struct gameEvent {string text; int age = 0; Color tint = Color::Transparent; int count = 1; bool quickFade = false;};
deque<gameEvent> events;
int foodFlashFrames = 0;   // >0: the newest ticker line (a paid birth) is tinted birthFlashColor

void pushEvent(const string& text, Color tint, bool quickFade) {
    foodFlashFrames = 0;   // any newer line ends the flash, so it never lands on the wrong line
    if (!events.empty() && events.back().text == text && events.back().tint == tint) {   // POL-F2: base text + tint
        events.back().count++;
        events.back().age = 0;   // repeated: stays up as long as a fresh line would
        return;
    }
    events.push_back({text, 0, tint, 1, quickFade});
    while ((int)events.size() > eventMaxLines) events.pop_front();
}

void logEvent(const string& text, Color tint = Color::Transparent) { pushEvent(text, tint, false); }

// POL-F2: a line about race r's barbs, tinted with that race's ticker colour
void logRaceEvent(const string& text, int r) { pushEvent(text, raceEventColors[clampRace(r)], true); }

string eventText(const gameEvent& e) { return e.count > 1 ? e.text + " x" + to_string(e.count) : e.text; }

// R51-F4: symmetric relations matrix; written only through setRelation(), so [a][b] == [b][a] always
float raceRelation[raceCount][raceCount] = {};
int relationClashCooldown[raceCount][raceCount] = {};   // ticks until the pair's next clash counts (kept symmetric too)
enum relationTier { relNeutral, relUneasy, relCold };
relationTier relationTierOf(float v) { return v <= kRelationCold ? relCold : v <= kRelationUneasy ? relUneasy : relNeutral; }

// clamps, writes both halves, and logs one ticker line when the pair changes tier
void setRelation(int a, int b, float v) {
    if (a < 0 || a >= raceCount || b < 0 || b >= raceCount || a == b) return;
    relationTier before = relationTierOf(raceRelation[a][b]);
    v = clamp(v, kRelationMin, kRelationMax);
    raceRelation[a][b] = raceRelation[b][a] = v;
    relationTier after = relationTierOf(v);
    if (after == before) return;
    string pair = string(racePlurals[a]) + " and " + racePlurals[b];
    string line;
    if (after > before)
        line = after == relCold ? pair + " turn cold: the trader charges more" : pair + " are uneasy over the border woods";
    else
        line = after == relUneasy ? pair + " thaw, but stay wary" : pair + " are at peace again";
    printf("RELATION - %s %s (%.2f)\n", pair.c_str(), after > before ? "worse" : "better", v);
    logEvent(line, after > before ? kRelationTickerColdTint : Color::Transparent);
}

// R53-F1: "3 food, 2 wood and 2 stone" (empty kinds skipped)
string lootText(const raceStock& s) {
    string parts[3];
    int n = 0;
    if (s.food > 0) parts[n++] = to_string(s.food) + " food";
    if (s.wood > 0) parts[n++] = to_string(s.wood) + " wood";
    if (s.stone > 0) parts[n++] = to_string(s.stone) + " stone";
    string out;
    for (int k = 0; k < n; k++) out += string(k == 0 ? "" : k == n - 1 ? " and " : ", ") + parts[k];
    return out;
}
// R53-F1: the one way a plunder pack ends. home = deposited into the raider's own race stock (back at its crate); otherwise
// dropped (abort, release away from home, death) and handed back to the robbed race. Always leaves the barb with no pack,
// so nothing outlives a swapRemove. Safe on a barb with no pack.
void settleLoot(barb& b, bool home) {
    if (!lootLaden(b)) { b.loot = raceStock{0, 0, 0}; b.lootFrom = -1; return; }
    string what = lootText(b.loot);
    int own = clampRace(b.race);
    if (home) {
        raceStock& s = stockOfRace(own);
        s.food += b.loot.food; s.wood += b.loot.wood; s.stone += b.loot.stone;
        printf("LOOT DEPOSITED - %s (id %d) brings %s home to the %s\n", b.name, b.id, what.c_str(), racePlurals[own]);
    } else if (b.lootFrom >= 0 && b.lootFrom < raceCount) {
        raceStock& s = stockOfRace(b.lootFrom);
        s.food += b.loot.food; s.wood += b.loot.wood; s.stone += b.loot.stone;
        printf("LOOT DROPPED - %s (id %d) drops %s, back to the %s\n", b.name, b.id, what.c_str(), racePlurals[b.lootFrom]);
        logEvent(string(racePlurals[b.lootFrom]) + " recover " + what + " dropped by the " + racePlurals[own], kRaidTickerTint);
    } else printf("LOOT LOST - %s (id %d) drops %s (no owner)\n", b.name, b.id, what.c_str());
    b.loot = raceStock{0, 0, 0};
    b.lootFrom = -1;
}

// a claim clash between races a and b inside owner's frontier; counted at most once per pair per cooldown
void relationClash(int a, int b, int owner) {
    if (a < 0 || a >= raceCount || b < 0 || b >= raceCount || a == b) return;
    if (relationClashCooldown[a][b] > 0) return;
    relationClashCooldown[a][b] = relationClashCooldown[b][a] = kRelationClashCooldownTicks;
    printf("CLAIM CLASH - %s vs %s inside the %s frontier\n", racePlurals[a], racePlurals[b], racePlurals[clampRace(owner)]);
    setRelation(a, b, raceRelation[a][b] - kRelationClashDelta);
}

// slow drift of every pair toward neutral, and the clash cooldowns; once per tick
void relationTick() {
    for (int a = 0; a < raceCount; a++)
        for (int b = a + 1; b < raceCount; b++) {
            if (relationClashCooldown[a][b] > 0) relationClashCooldown[a][b] = --relationClashCooldown[b][a];
            float v = raceRelation[a][b];
            if (v != 0.0f) setRelation(a, b, v > 0.0f ? max(0.0f, v - kRelationDriftPerTick) : min(0.0f, v + kRelationDriftPerTick));
        }
}

// race r's coldest standing with any other race (0 if none below neutral)
float coldestRelationOf(int r) {
    float v = 0.0f;
    for (int o = 0; r >= 0 && r < raceCount && o < raceCount; o++) if (o != r) v = min(v, raceRelation[r][o]);
    return v;
}

// trade price multiplier for race r: 1 unless r has a cold pair, then kTradePriceMultCold..kTradePriceMultMax (capped)
float tradePriceMultOf(int r) {
    float v = coldestRelationOf(r);
    if (v > kRelationCold) return 1.0f;
    float depth = clamp((kRelationCold - v) / (kRelationCold - kRelationMin), 0.0f, 1.0f);
    return min(kTradePriceMultMax, kTradePriceMultCold + (kTradePriceMultMax - kTradePriceMultCold) * depth);
}

// per-lot trade cost for race r (stone, or the food fallback), with the cold-border markup rounded up
int tradeCostOf(int r, bool food) {
    int base = food ? tradeFoodCost : tradeStoneCost;
    return (int)ceil(base * tradePriceMultOf(r) - 0.001f);
}

// Know-how (R20-F1): one unit of practice; at the threshold the tech is learned once, with a ticker line
void practice(techId t) {
    know.count[t]++;
    if (know.learned[t] || know.count[t] < techDefs[t].threshold) return;
    know.learned[t] = true;
    know.pulseFrames[t] = knowPulseFrames;
    printf("---KNOW-HOW--- %s learned after %d (%s)\n", techDefs[t].name, know.count[t], techDefs[t].effect);
    logEvent(string("Learned ") + techDefs[t].name + ": " + techDefs[t].effect);
}

// Ages ticker lines once per rendered frame so fade time is real-time regardless of sim speed
void eventTick() {
    if (foodFlashFrames > 0) foodFlashFrames--;
    for (auto& e : events) e.age++;
    while (!events.empty() && events.front().age >= eventFadeTicks) events.pop_front();
}

uint8_t eventFade(const gameEvent& e, uint8_t full) {
    int hold = e.quickFade ? eventFadeTicks - raceEventFadeTicks : eventHoldTicks;
    if (e.age <= hold) return full;
    float f = 1.0f - (float)(e.age - hold) / (eventFadeTicks - hold);
    return (uint8_t)(full * clamp(f, 0.0f, 1.0f));
}

// Returns the strip's screen rect (empty when nothing is shown) so the tooltip can avoid it
FloatRect drawEvents(RenderWindow& window, const Font& font) {
    if (events.empty()) return FloatRect();
    window.setView(window.getDefaultView());
    float lineH = font.getLineSpacing(eventFontSize);
    float width = eventStripMinWidth;
    uint8_t newest = 0;
    for (auto& e : events) {
        Text t(font, eventText(e), eventFontSize);
        FloatRect lb = t.getLocalBounds();
        width = max(width, lb.position.x + lb.size.x + 2 * eventPad.x);
        newest = max(newest, eventFade(e, eventStripAlpha));
    }
    float height = events.size() * lineH + 2 * eventPad.y;
    Vector2f topLeft(eventMargin.x, window.getDefaultView().getSize().y - eventMargin.y - height);
    RectangleShape strip({width, height});
    strip.setPosition(topLeft);
    strip.setFillColor(Color(0, 0, 0, newest));
    window.draw(strip);
    for (int i = 0; i < (int)events.size(); i++) {
        Text t(font, eventText(events[i]), eventFontSize);
        Color c = events[i].tint.a > 0 ? events[i].tint : eventTextColor;   // R26-F1: dusk line in night blue
        if (foodFlashFrames > 0 && i == (int)events.size() - 1) {
            float f = (float)foodFlashFrames / birthFlashFrames;   // 1 -> 0: eases back to the normal colour
            c.r = (uint8_t)(c.r + (birthFlashColor.r - c.r) * f);
            c.g = (uint8_t)(c.g + (birthFlashColor.g - c.g) * f);
            c.b = (uint8_t)(c.b + (birthFlashColor.b - c.b) * f);
        }
        c.a = eventFade(events[i], eventTextAlpha);
        t.setFillColor(c);
        t.setPosition(topLeft + eventPad + Vector2f(0.0f, i * lineH));
        window.draw(t);
    }
    return FloatRect(topLeft, {width, height});
}

struct tree  {int x, y; bool dying = false;};
struct rock  {int x, y; bool dying = false;};
struct bush  {int x, y; bool ripe = true; bool picking = false; int regrow = 0;};

// Nearest candidate to pos strictly inside radius: posOf(i) gives candidate i's position and
// skip(i) == true excludes it. Returns the index or -1; ties keep the lowest index. If outDSq is
// given it receives the winner's squared distance (radius^2 when nothing was found).
template<typename PosFn, typename SkipFn>
int findNearest(Vector2f pos, float radius, int count, PosFn posOf, SkipFn skip, float* outDSq = nullptr) {
    int best = -1;
    float bestDSq = radius * radius;
    for (int i = 0; i < count; i++) {
        if (skip(i)) continue;
        Vector2f d = posOf(i) - pos;
        float dSq = d.x * d.x + d.y * d.y;
        if (dSq < bestDSq) {bestDSq = dSq; best = i;}
    }
    if (outDSq) *outDSq = bestDSq;
    return best;
}

template<typename T>
void swapRemove(vector<T>& v, int i) {
    v[i] = move(v.back());
    v.pop_back();
}

// R47-F1: which band a world position is in. The core disc wins over the rim where they overlap.
int zoneAt(Vector2f p) {
    float dx = p.x - villageCenter.x, dy = p.y - villageCenter.y;
    if (dx * dx + dy * dy < zoneCoreRadius * zoneCoreRadius) return zoneCore;
    float rim = min(min(p.x, p.y), min(worldSize - p.x, worldSize - p.y));
    return rim < zoneEdgeBandPx ? zoneEdge : zoneWild;
}

// R47-F1: band areas (world px^2), measured once; the startup tree/rock targets per band follow from them
float zoneArea[zoneCount] = {};
int zoneTreeTarget[zoneCount] = {};
int zoneRockTarget[zoneCount] = {};
void computeZoneTargets() {
    int n = (int)(worldSize / zoneAreaSamplePx);
    for (int z = 0; z < zoneCount; z++) zoneArea[z] = 0.0f;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            zoneArea[zoneAt(Vector2f((x + 0.5f) * zoneAreaSamplePx, (y + 0.5f) * zoneAreaSamplePx))] += zoneAreaSamplePx * zoneAreaSamplePx;
    for (int z = 0; z < zoneCount; z++) {
        float units = zoneArea[z] / (baseWorldSize * baseWorldSize);
        zoneTreeTarget[z] = (int)(zoneTreeDensity[z] * units);
        zoneRockTarget[z] = (int)(zoneRockDensity[z] * units);
    }
    zoneTreeTarget[zoneCore] = min(zoneTreeTarget[zoneCore], zoneCoreTreeCap);
    printf("Bands: core %.0fk px2 (%d trees, %d rocks), wild %.0fk (%d, %d), edge %.0fk (%d, %d)\n",
           zoneArea[zoneCore] / 1000, zoneTreeTarget[zoneCore], zoneRockTarget[zoneCore],
           zoneArea[zoneWild] / 1000, zoneTreeTarget[zoneWild], zoneRockTarget[zoneWild],
           zoneArea[zoneEdge] / 1000, zoneTreeTarget[zoneEdge], zoneRockTarget[zoneEdge]);
}

// R47-F1: a top-left spot (for a size x size footprint) whose centre is in band z; rolled inside the band's bounding box
// (the core's is a square round villageCenter) so a small band doesn't waste rolls. False after zoneRollTries misses.
bool rollInZone(int z, float size, Vector2i& out) {
    float lo = 0.0f, hiX = worldSize - 1.0f, loY = 0.0f, hiY = worldSize - 1.0f;
    if (z == zoneCore) {
        lo = max(0.0f, villageCenter.x - zoneCoreRadius); hiX = min(worldSize - 1.0f, villageCenter.x + zoneCoreRadius);
        loY = max(0.0f, villageCenter.y - zoneCoreRadius); hiY = min(worldSize - 1.0f, villageCenter.y + zoneCoreRadius);
    }
    uniform_int_distribution<int> rx((int)lo, (int)hiX), ry((int)loY, (int)hiY);
    for (int k = 0; k < zoneRollTries; k++) {
        Vector2i p(rx(gen), ry(gen));
        if (zoneAt(Vector2f(p.x + size / 2, p.y + size / 2)) == z) { out = p; return true; }
    }
    return false;
}

// R47-F1: 8-way compass word for a direction from the village (screen y grows downward, so north is -y)
const char* compassName(Vector2f d) {
    static const char* const names[8] = {"east", "north-east", "north", "north-west", "west", "south-west", "south", "south-east"};
    float ang = atan2(-d.y, d.x);   // radians, 0 = east, counter-clockwise
    int k = (int)lround(ang / 0.78539816f);
    return names[((k % 8) + 8) % 8];
}

// R47-F1: tree spatial grid. cells[c] holds indices into tris/trees, bucketed by work spot (tris pos + treeWorkOffset).
// Rebuilt from tris once at the start of minematerials() each tick (so every add/remove elsewhere is picked up), and
// again lazily after a chop there swapRemoves a tree (stale). zoneTrees counts trees per band at the last rebuild.
struct treeGridT {
    vector<vector<int>> cells = vector<vector<int>>(treeGridN * treeGridN);
    bool stale = true;
    int zoneTrees[zoneCount] = {};
};
treeGridT treeGrid;

int treeGridCoord(float v) { return clamp((int)floor(v / treeGridCellPx), 0, treeGridN - 1); }

void treeGridRebuild(const vector<CircleShape>& tris) {
    for (auto& c : treeGrid.cells) c.clear();   // keeps capacity: no allocation after the first few ticks
    for (int z = 0; z < zoneCount; z++) treeGrid.zoneTrees[z] = 0;
    for (int i = 0; i < (int)tris.size(); i++) {
        Vector2f tp = tris[i].getPosition();
        Vector2f p = tp + treeWorkOffset;
        treeGrid.cells[treeGridCoord(p.y) * treeGridN + treeGridCoord(p.x)].push_back(i);   // ascending i per cell
        treeGrid.zoneTrees[zoneAt(tp + Vector2f(treeFootprint / 2, treeFootprint / 2))]++;
    }
    treeGrid.stale = false;
}

// findNearest over trees by work spot, through the grid; same contract (strictly inside radius, ties keep the lowest
// index, outDSq = radius^2 when nothing is found). radius must be finite.
template<typename SkipFn>
int findNearestTree(const vector<CircleShape>& tris, Vector2f pos, float radius, SkipFn skip, float* outDSq = nullptr) {
    if (treeGrid.stale) treeGridRebuild(tris);
    int best = -1;
    float bestDSq = radius * radius;
    int x0 = treeGridCoord(pos.x - radius), x1 = treeGridCoord(pos.x + radius);
    int y0 = treeGridCoord(pos.y - radius), y1 = treeGridCoord(pos.y + radius);
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            for (int i : treeGrid.cells[cy * treeGridN + cx]) {
                if (skip(i)) continue;
                Vector2f d = tris[i].getPosition() + treeWorkOffset - pos;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < bestDSq || (dSq == bestDSq && best != -1 && i < best)) {bestDSq = dSq; best = i;}
            }
    if (outDSq) *outDSq = bestDSq;
    return best;
}

// R47-F1 band ground tint: a static grid of translucent quads, built once. Each vertex blends the band tints with
// smoothstep weights across each boundary (core wins over the rim, as in zoneAt), plus a +-zoneTintJitter alpha hash
// dither, so the wash fades between bands with no hard line. Rows are contiguous, so only the visible span is drawn.
vector<Vertex> zoneTintVerts;
int zoneTintN = 0;   // tiles per side

float smooth01(float e0, float e1, float x) {
    float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

Color zoneTintAt(Vector2f p) {
    float dc = hypot(p.x - villageCenter.x, p.y - villageCenter.y);
    float rim = min(min(p.x, p.y), min(worldSize - p.x, worldSize - p.y));
    float c = 1.0f - smooth01(zoneCoreRadius - zoneTintFeatherPx, zoneCoreRadius + zoneTintFeatherPx, dc);
    float e = 1.0f - smooth01(zoneEdgeBandPx - zoneTintFeatherPx, zoneEdgeBandPx + zoneTintFeatherPx, rim);
    float w[zoneCount] = {c, (1.0f - c) * (1.0f - e), (1.0f - c) * e};
    float r = 0, g = 0, b = 0, a = 0;
    for (int z = 0; z < zoneCount; z++) {
        r += w[z] * zoneTintColors[z].r; g += w[z] * zoneTintColors[z].g;
        b += w[z] * zoneTintColors[z].b; a += w[z] * zoneTintColors[z].a;
    }
    uint32_t h = (uint32_t)(int)p.x * 73856093u ^ (uint32_t)(int)p.y * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    int jitter = (int)(h % (uint32_t)(2 * zoneTintJitter + 1)) - zoneTintJitter;
    return Color((uint8_t)lround(r), (uint8_t)lround(g), (uint8_t)lround(b), (uint8_t)clamp((int)lround(a) + jitter, 0, 255));
}

void buildZoneTint() {
    zoneTintN = (int)ceil(worldSize / zoneTintTilePx);
    zoneTintVerts.clear();
    zoneTintVerts.reserve((size_t)zoneTintN * zoneTintN * 6);
    for (int ty = 0; ty < zoneTintN; ty++)
        for (int tx = 0; tx < zoneTintN; tx++) {
            float x0 = tx * zoneTintTilePx, y0 = ty * zoneTintTilePx;
            float x1 = min(worldSize, x0 + zoneTintTilePx), y1 = min(worldSize, y0 + zoneTintTilePx);
            Vertex a{{x0, y0}, zoneTintAt({x0, y0})}, b{{x1, y0}, zoneTintAt({x1, y0})};
            Vertex d{{x0, y1}, zoneTintAt({x0, y1})}, e{{x1, y1}, zoneTintAt({x1, y1})};
            for (const Vertex& v : {a, b, e, a, e, d}) zoneTintVerts.push_back(v);
        }
}

// World layer, straight after the clear: under trails, lakes and everything else
void drawZoneTint(RenderWindow& window, const FloatRect& view) {
    if (zoneTintN == 0) return;
    int tx0 = clamp((int)floor(view.position.x / zoneTintTilePx), 0, zoneTintN - 1);
    int tx1 = clamp((int)floor((view.position.x + view.size.x) / zoneTintTilePx), 0, zoneTintN - 1);
    int ty0 = clamp((int)floor(view.position.y / zoneTintTilePx), 0, zoneTintN - 1);
    int ty1 = clamp((int)floor((view.position.y + view.size.y) / zoneTintTilePx), 0, zoneTintN - 1);
    for (int ty = ty0; ty <= ty1; ty++)
        window.draw(&zoneTintVerts[((size_t)ty * zoneTintN + tx0) * 6], (size_t)(tx1 - tx0 + 1) * 6, PrimitiveType::Triangles);
}

// The tree whose (int) work spot is exactly (x, y), lowest index first; -1 if none
int treeAtWorkSpot(const vector<CircleShape>& tris, int x, int y) {
    if (treeGrid.stale) treeGridRebuild(tris);
    for (int i : treeGrid.cells[treeGridCoord((float)y) * treeGridN + treeGridCoord((float)x)]) {
        Vector2f p = tris[i].getPosition() + treeWorkOffset;
        if ((int)p.x == x && (int)p.y == y) return i;
    }
    return -1;
}

// R44-F2: no task stripe -> idle. Shared by barbColor() and the H beacons so both agree on who is jobless
bool barbIdle(const barb& b) { return !b.deertargeting && !b.building && !atForgeTask(b) && !b.chopping; }   // R47-F7: or smelting

// Priority when flags overlap: hunting > building > picking > harvesting > fishing > mining > chopping > idle
Color barbColor(const barb& b) {
    if (barbIdle(b)) return barbIdleColor;
    if (b.deertargeting) return barbHuntColor;
    if (b.building) return barbBuildColor;
    if (b.activity == actTemper) return barbMineColor;   // R41-F2: pick work at the forge; never overlaps chopping/building
    if (b.activity == actSmelt) return barbMineColor;    // R47-F7: stone work at the forge, same stripe
    if (b.chopping && b.target == targetBush) return barbPickColor;
    if (b.chopping && b.target == targetField) return barbHarvestColor;
    if (b.chopping && b.target == targetLake) return barbFishColor;
    if (b.chopping && b.target == targetRock) return barbMineColor;
    return barbChopColor;   // only chopping is left once barbIdle() is false
}

// R47-F8: the weapon a race makes, its hit damage at the barb's snapshotted tier, and the tooltip text ("spear, tier 2")
const weaponDef& weaponOf(const barb& b) { return weaponDefs[raceWeapon[clampRace(b.race)]]; }
int weaponDamage(const barb& b) { return weaponOf(b).baseDamage + weaponTierBonus[clamp(b.weaponTier, 0, toolTierCount - 1)]; }
string weaponLabel(const barb& b) { return string(weaponOf(b).name) + ", tier " + to_string(b.weaponTier); }

// Same priority as barbColor() so the tooltip always matches the colour on screen
const char* barbTaskName(const barb& b) {
    if (b.activity == actShelter) return b.garrisonInside ? "sheltering" : "running to shelter";   // INBOX-F1
    if (b.activity == actExpedition)   // R45-F2
        return party.phase == expeditionParty::Out ? "expedition: setting out"
             : party.phase == expeditionParty::Delve ? (party.hollow ? "expedition: below the Hollow" : "expedition: delving")   // R47-F5
             : "expedition: heading home";
    if (b.activity == actRaidDefend) return b.raidPreyId >= 0 ? "defending the crate: fighting" : "defending the crate";   // R52-F1
    if (b.activity == actRaidAttack)
        return raid.phase == raidParty::Home ? (lootLaden(b) ? "raiding: carrying plunder home" : "raiding: heading home")   // R53-F1
             : b.raidBeaten ? "raiding: beaten"
             : raid.phase == raidParty::Fight ? (b.raidPreyId >= 0 ? "raiding: fighting" : "raiding: at the crate")
             : "raiding: on the march";
    if (b.activity == actCaravan)   // R54-F1
        return caravan.phase == caravanTrip::Trade ? "caravan: trading at the crate"
             : caravan.phase == caravanTrip::Home ? "caravan: heading home" : "caravan: on the road";
    if (b.deertargeting) return b.hasSpear ? weaponOf(b).huntLabel : "hunting";   // R47-F8: "hunting (axe)" for dwarves
    if (b.building) return "building";
    if (b.activity == actTemper) return "tempering pick";   // R41-F2
    if (b.activity == actSmelt) return "smelting ingots";   // R47-F7
    if (b.activity == actDrill) return "drilling";          // R47-F6: no stripe (idle colour); the tooltip names it
    if (b.activity == actWatch) return b.watchPreyId >= 0 ? "on watch: fighting" : "on watch";   // R51-F1: no stripe either, just the torch
    if (b.activity == actKneel) return b.moving ? "going to the Rite" : "kneeling at the Rite";   // R52-F3
    if (b.activity == actHerb) return b.moving ? "going to the herb patch" : "tending herbs";      // R53-F2: no stripe either
    if (b.chopping && b.target == targetBush) return "picking";
    if (b.chopping && b.target == targetField) return "harvesting";
    if (b.chopping && b.target == targetLake) return "fishing";
    if (b.chopping && b.target == targetRock) return "mining";
    if (b.chopping) return "chopping";
    return "idle";
}

// Index of the deer with this id, or -1 if it is gone (killed by any system)
int findDeerById(const vector<deer>& deers, int id) {
	if (id < 0) return -1;
	for (int i = 0; i < (int)deers.size(); ++i)
		if (deers[i].id == id) return i;
	return -1;
}

// Drop a hunt as one unit so no barb is left half-targeting
void clearHunt(barb& b) {
	b.deertargeting = false;
	b.targetDeerId = -1;
	b.moving = false; clearRoute(b);
	b.timer = 0;
	b.targetx = b.targety = -1;
	dropCarry(b);   // R29-F1
}

// Any barb whose target deer no longer exists stops hunting. Safe to call after any deer removal.
void validateDeerTargets(vector<barb>& barbs, const vector<deer>& deers) {
	for (auto& b : barbs)
		if (b.deertargeting && findDeerById(deers, b.targetDeerId) == -1) clearHunt(b);
}

void deerkilling(vector<deer>& deers, vector<RectangleShape>& deershape, vector<barb>& barbs, vector<RectangleShape>& squares, smokehouseSet& sh) {
	const float deerInfluenceDSq = 625.0f;
	validateDeerTargets(barbs, deers);
	for (int i = 0; i < (int)deers.size(); ) {
		auto& ds = deershape[i];
		auto& d = deers[i];
		Vector2f deerpos = ds.getPosition();
		const speciesDef& sp = speciesOf(d);
		if (d.invincible) {
			ds.setFillColor(d.invincibleTimer > animalHitInvincibleTicks - animalHitFlashTicks ? Color::Red : sp.body);
			if (--d.invincibleTimer <= 0) {
				d.invincible = false;
				ds.setFillColor(sp.body);
			}
		}
		bool removed = false;
		for (int j = 0; j < (int)barbs.size(); ++j) {
			auto& b = barbs[j];
			auto& sq = squares[j];
			if (!barbAvailable(b) || b.building || b.movingbspot || b.chopping || b.timer > 0 || b.carryKind != carryNone || atForgeTask(b)) continue;   // R29-F1: haulers finish the trip; R41-F2: so do temperers (R47-F7: and smelters); R45-F2
			if (b.activity == actWatch) continue;   // R51-F1: watchmen keep to the watch
			if (!b.deertargeting) {
				Vector2f bpos = sq.getPosition();
				float dx = deerpos.x - bpos.x, dy = deerpos.y - bpos.y;
				if (dx * dx + dy * dy >= deerInfluenceDSq) continue;
				clearTraining(b);   // R47-F6: a deer at the yard pulls a driller off the dummy
				b.deertargeting = true;
				b.targetDeerId = d.id;
				b.target = targetNone;   // a hunt drops any walk to a bush or shore point (work in progress is never hijacked)
				b.targetBush = b.targetShore = -1;
			} else if (b.targetDeerId != d.id) {
				continue;
			}
			b.targetx = (int)deerpos.x;
			b.targety = (int)deerpos.y;
			b.moving = true;
			if (!d.invincible && sq.getGlobalBounds().findIntersection(ds.getGlobalBounds()).has_value()) {
				bool speared = b.hasSpear;
				d.health -= speared ? weaponDamage(b) : animalHitDamage;   // R47-F8: base + tier bonus
				d.invincible = true;
				d.invincibleTimer = animalHitInvincibleTicks;
				ds.setFillColor(Color::Red);
				printf("%s Health: %d\n", sp.name, d.health);
				if (d.health <= 0) {
					const char* name = sp.name;   // sp stays valid (table row); d and ds do not
					int yield = sp.foodYield;
					swapRemove(deers, i);      // d and ds are invalid from here on
					swapRemove(deershape, i);
					removed = true;
					bool ownHut = b.race >= 0 && b.race < raceCount && sh[b.race].built;   // INBOX2-F1: only the hunter's own race's hut
					int smoked = ownHut ? smokehouseFoodBonus : 0;
					raceStock& rs = stockOf(b);   // RACE-F1: the hunter's race eats it
					rs.food += yield + smoked;
					if (smoked > 0) sh[b.race].puffFrames = smokePuffFrames;   // short faint puff over that race's hut
					string arms = speared ? " (" + weaponLabel(b) + ")" : string();   // R47-F8
					printf("%s KILLED%s - %s food: %d (+%d, +%d smoked)\n", name, arms.c_str(), racePlurals[clampRace(b.race)], rs.food, yield, smoked);
					logRaceEvent(string(b.name) + (speared ? string(" ") + weaponOf(b).killVerb + " a " : string(" killed a ")) + name + " (+" + to_string(yield) + " food"
					         + (smoked > 0 ? ", +" + to_string(smoked) + " smoked" : string()) + ", " + to_string(rs.food) + ")", b.race);
					break;
				}
			}
		}
		if (removed) {
			validateDeerTargets(barbs, deers); // frees every barb that was chasing the dead deer
			continue;                          // re-check index i: it now holds the swapped-in deer
		}
		++i;
	}
}

// Wolves enter from an edge, chase the nearest deer and kill it on contact (no food). After one kill,
// or once deer are scarce (<= wolfMinDeer), they walk back out, so they can never wipe the deer out.
void wolfTick(vector<wolf>& wolves, vector<deer>& deers, vector<RectangleShape>& deershape, vector<barb>& barbs) {
	int deerCount = countSpecies(deers, speciesDeer);   // only deer count: rabbits are never wolf prey
	if ((int)wolves.size() < maxWolves && deerCount > wolfMinDeer
	    && uniform_int_distribution<int>(0, wolfSpawnChance - 1)(gen) == 0) {
		// R47-F1: in from a random side of the wild band's outer edge (where it meets the rocky rim), well clear of the
		// core; it leaves the same way. No clear spot in wolfSpawnTries = no wolf this time.
		wolf w;
		uniform_real_distribution<float> along(zoneEdgeBandPx, worldSize - zoneEdgeBandPx);
		bool found = false;
		for (int k = 0; k < wolfSpawnTries && !found; k++) {
			float a = along(gen);
			switch (uniform_int_distribution<int>(0, 3)(gen)) {
				case 0:  w.exit = Vector2f(zoneEdgeBandPx, a); break;
				case 1:  w.exit = Vector2f(worldSize - zoneEdgeBandPx, a); break;
				case 2:  w.exit = Vector2f(a, zoneEdgeBandPx); break;
				default: w.exit = Vector2f(a, worldSize - zoneEdgeBandPx); break;
			}
			Vector2f d = w.exit - villageCenter;
			found = hypot(d.x, d.y) >= zoneCoreRadius + wolfCoreMarginPx;
		}
		if (found) {
			w.shape.setSize(wolfSize);
			w.shape.setFillColor(wolfColor);
			w.shape.setPosition(w.exit);
			wolves.push_back(w);
			const char* dir = compassName(w.exit - villageCenter);
			printf("Wolf arriving from the %s (%.0f, %.0f)\n", dir, w.exit.x, w.exit.y);
			logEvent(string("A wolf comes out of the wilds to the ") + dir);
		}
	}
	for (int i = 0; i < (int)wolves.size(); ) {
		wolf& w = wolves[i];
		Vector2f pos = w.shape.getPosition();
		if (w.phase == wolf::Hunting && deerCount <= wolfMinDeer) w.phase = wolf::Leaving;
		Vector2f goal = w.exit;
		int prey = -1;
		if (w.phase == wolf::Hunting) {
			prey = findNearest(pos, noSearchRadius, (int)deershape.size(),
			                   [&](int k) { return deershape[k].getPosition(); },
			                   [&](int k) { return deers[k].species != speciesDeer; });
			if (prey != -1) goal = deershape[prey].getPosition();
		}
		if (prey != -1 && w.shape.getGlobalBounds().findIntersection(deershape[prey].getGlobalBounds()).has_value()) {
			swapRemove(deers, prey);
			swapRemove(deershape, prey);
			validateDeerTargets(barbs, deers);   // no barb keeps hunting the dead deer
			deerCount--;
			w.phase = wolf::Leaving;
			printf("Wolf killed a deer - deer left: %d\n", deerCount);
			logEvent("Wolf killed a deer");
			++i;
			continue;
		}
		Vector2f diff = goal - pos;
		float dSq = diff.x * diff.x + diff.y * diff.y;
		if (w.phase == wolf::Leaving && dSq < 1.0f) {
			swapRemove(wolves, i);   // re-check index i: it now holds the swapped-in wolf
			continue;
		}
		if (dSq >= 1.0f && ++w.stepTimer >= wolfStepInterval) {
			w.stepTimer = 0;
			w.shape.move(diff / sqrt(dSq));
		}
		++i;
	}
}

// Pushes one animal onto deers and its shape onto deershape together (same index)
void addAnimal(vector<deer>& deers, vector<RectangleShape>& deershape, deer d) {
	const speciesDef& sp = speciesOf(d);
	d.id = nextDeerId++;
	d.health = sp.maxHealth;
	deers.push_back(d);
	RectangleShape ds;
	ds.setPosition(Vector2f(d.x, d.y));
	ds.setSize(sp.bodySize);
	ds.setFillColor(sp.body);
	deershape.push_back(ds);
}

// Each species walks in from a side edge under its own cap; species with a breed rate also multiply in place
void deerspawn(vector<deer>& deers, vector<RectangleShape>& deershape) {
	for (int s = 0; s < speciesCount; s++) {
		const speciesDef& sp = speciesDefs[s];
		int n = countSpecies(deers, (animalSpecies)s);
		if (n >= sp.cap) continue;
		deer d;
		d.species = (animalSpecies)s;
		if (uniform_int_distribution<int>(0, sp.arriveOneIn - 1)(gen) == 0) {
			if (DeerSpawnRL(gen) == 0) {
				d.x = (int)-deerOffMapPx;
				d.targetx = DeerSpawnXgotoL(gen);
			} else {
				d.x = (int)(worldSize + deerOffMapPx);
				d.targetx = DeerSpawnXgotoR(gen);
			}
			d.y = DeerSpawnY(gen);
			d.targety = d.y;
			d.moving  = true;
			Vector2f from(d.x, d.y);
			if (!lakeClear(FloatRect(Vector2f(d.targetx, d.targety), sp.bodySize + Vector2f(0.0f, sp.legPx)), &from)) continue; // retry next tick
			addAnimal(deers, deershape, d);
			printf("%s arrived - %d now\n", sp.name, n + 1);
		} else if (sp.breedOneIn > 0 && n >= 2 && uniform_int_distribution<int>(0, sp.breedOneIn - 1)(gen) == 0) {
			int pick = uniform_int_distribution<int>(0, n - 1)(gen);
			int parent = -1;
			for (int k = 0; k < (int)deers.size(); k++)
				if (deers[k].species == s && pick-- == 0) { parent = k; break; }
			Vector2f p = deershape[parent].getPosition();
			float side = uniform_int_distribution<int>(0, 1)(gen) == 0 ? -rabbitBirthOffsetPx : rabbitBirthOffsetPx;
			d.x = (int)clamp(p.x + side, 0.0f, worldSize - sp.bodySize.x);
			d.y = (int)p.y;
			if (!lakeClear(FloatRect(Vector2f(d.x, d.y), sp.bodySize + Vector2f(0.0f, sp.legPx)))) continue; // no kits in the water
			addAnimal(deers, deershape, d);
			printf("%s born - %d now\n", sp.name, n + 1);
		}
	}
}

// Ends a flee and starts its cooldown; the deer goes back to normal wandering from where it stopped
void endFlee(deer& d) {
    d.fleeTimer = 0;
    d.fleeCooldown = deerFleeCooldown;
    d.moving = false;
    d.stepTimer = 0;
}

void deerwander(vector<RectangleShape>& deershape, vector<deer>& deers, const vector<RectangleShape>& squares, const vector<barb>& barbs) {
    static int fleeCheckTick = 0;
    bool checkFlee = ++fleeCheckTick >= deerFleeCheckInterval;
    if (checkFlee) fleeCheckTick = 0;
    for (int i = 0; i < (int)deershape.size(); i++) {
        auto& ds = deershape[i];
        auto& d  = deers[i];
        Vector2f pos = ds.getPosition();
        if (d.fleeTimer > 0) {
            if (--d.fleeTimer <= 0) endFlee(d);
        } else if (d.fleeCooldown > 0) {
            d.fleeCooldown--;
        } else if (checkFlee) {
            // Nearest barb inside the flee radius (squared distance, barbs only)
            float bestSq = deerFleeRadiusSq;
            Vector2f away;
            bool found = false;
            for (int k = 0; k < (int)squares.size() && k < (int)barbs.size(); k++) {
                if (barbHidden(barbs[k])) continue;   // INBOX-F1: sheltering barbs are indoors
                Vector2f diff = pos - squares[k].getPosition();
                float dSq = diff.x * diff.x + diff.y * diff.y;
                if (dSq < bestSq) { bestSq = dSq; away = diff; found = true; }
            }
            if (found) {
                float len = sqrt(bestSq);
                Vector2f dir = len > 0.0f ? away / len : Vector2f(1.0f, 0.0f);
                Vector2f size = ds.getSize();
                Vector2f goal = pos + dir * deerFleeDistance;
                goal.x = clamp(goal.x, deerFleeEdgeMargin, worldSize - size.x - deerFleeEdgeMargin);
                goal.y = clamp(goal.y, hudBarHeight + deerFleeEdgeMargin, worldSize - size.y - speciesOf(d).legPx - deerFleeEdgeMargin);
                d.targetx = (int)goal.x;
                d.targety = (int)goal.y;
                d.moving = true;
                d.stepTimer = 0;
                d.fleeTimer = deerFleeTicks;
                printf("%s %d fleeing\n", speciesOf(d).name, d.id);
            }
        }
        if (!d.moving) {
            if (wanderChance(gen) != 0) continue;
            int tx = (int)pos.x + wanderDX(gen);
            int ty = (int)pos.y + wanderDY(gen);
            if (tx > 0 && tx < worldSize && ty > 0 && ty < worldSize && lakeClear(FloatRect(Vector2f(tx, ty), ds.getSize() + Vector2f(0.0f, speciesOf(d).legPx)), &pos)) {
                d.targetx = tx;
                d.targety = ty;
                d.moving  = true;
            }
        } else {
            Vector2f diff = Vector2f(d.targetx, d.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;
            if (dSq < 1.0f) {
                if (d.fleeTimer > 0) endFlee(d);   // reached safety early: flee over, cooldown starts
                d.moving    = false;
                d.stepTimer = 0;
            } else if (++d.stepTimer >= (d.fleeTimer > 0 ? speciesOf(d).fleeStepInterval : speciesOf(d).stepInterval)) {
                d.stepTimer = 0;
                ds.move(diff / sqrt(dSq));
            }
        }
    }
}

int clockk() {
    static int timer = 0;
    if (++timer >= ticksPerGameHour) {
        game_time = (game_time + 1) % 24;   // 0..23: midnight follows 23:00
        printf("The time is now: %d:00\n", game_time);
        timer = 0;
        if (game_time == 0) {   // R51-F3: a new day; announce a new season on its first midnight
            int was = seasonOf(game_day);
            game_day++;
            int now = seasonOf(game_day);
            if (now != was) {
                printf("SEASON - %s (day %d, year %d)\n", seasonDefs[now].name, game_day + 1, game_day / yearDays + 1);
                logEvent(seasonDefs[now].arrivalLine, seasonDefs[now].ticker);
            }
        }
        if (game_time == nightEndHour && seasonOf(game_day) == seasonAutumn && dayInSeason(game_day) == daysPerSeason - 1)
            logEvent(frostLine, seasonDefs[seasonWinter].ticker);   // R51-F3: once, at dawn of autumn's last day
        // R26-F1: only inside this hour-change edge, so each fires once per day
        if (game_time == nightStartHour) logEvent("Dusk falls - " + to_string(game_time) + ":00", duskEventColor);
        if (game_time == nightEndHour)   logEvent("Dawn breaks - " + to_string(game_time) + ":00");
    }
    return game_time;
}

bool weatherTick() {
    static int wtimer = 0;
    if (++wtimer >= (raining ? rainWeatherTicks : clearWeatherTicks)) {
        raining = !raining;
        wtimer = 0;
        printf(raining ? "Rain starts - berries ripening\n" : "Rain stops\n");
        logEvent(raining ? "Rain starts - berries ripening" : "Rain stops");
    }
    return raining;
}

int stepLimit() { return raining ? rainStepInterval : stepInterval; }

bool isHungry(const barb& b) { return b.hunger >= hungerThresholdOf(b); }

int stepLimit(const barb& b) {
    int lim = stepLimit() * (isHungry(b) ? hungerWorkSlowdown : 1);
    float speed = (b.trait == traitSwift ? swiftStepSpeed : 1.0f) * (b.shinobi ? shinobiStepSpeed : 1.0f); // R25-F1, R25-F2
    if (b.sick > 0) speed *= sickSpeedMult;   // R52-F4: the cough slows the walk through the same multiplier
    if (lootLaden(b)) speed *= kLootSpeedMult;   // R53-F1: a plunder pack slows the walk home the same way
    return speed != 1.0f ? max(1, (int)(lim / speed)) : lim;
}

// Worn trails: per-cell wear from barb steps, decaying over time. Tiers: 0 none, 1 path, 2 road.
vector<int> trailWear(trailGridN * trailGridN, 0);
vector<uint8_t> trailTier(trailGridN * trailGridN, 0);
vector<uint8_t> trailBucket(trailGridN * trailGridN, 0);   // R38-F1: wear / trailWearBucket last seen per cell
bool trailDirty = false;
vector<uint8_t> trailPaved(trailGridN * trailGridN, 0);   // R20-F2: 1 = cobbled; same grid as trailWear, skipped by trailTick decay
int pavedCount = 0;
int paveClaim = -1;      // the one cell being paved, -1 = none. Released by paveTick() whenever no barb holds jobPave
                         // (paver removed from barbs) or the paver lost movingbspot (taken off the job), and on finish/abandon.
bool paveDirty = false;
int causewayTimer[raceCount] = {0, 0, 0};          // R33-F3: ticks since the race's last causeway attempt
int causewayCells[raceCount] = {0, 0, 0};          // R33-F3: cells the race has paved (not counted in pavedCount)
bool causewayDone[raceCount] = {false, false, false};

// One reset path for the whole trail grid (wear, tiers, paving); called on (re)start in main
void resetTrails() {
    trailWear.assign(trailGridN * trailGridN, 0);
    trailTier.assign(trailGridN * trailGridN, 0);
    trailBucket.assign(trailGridN * trailGridN, 0);
    trailPaved.assign(trailGridN * trailGridN, 0);
    pavedCount = 0;
    paveClaim = -1;
    for (int r = 0; r < raceCount; r++) { causewayTimer[r] = causewayCells[r] = 0; causewayDone[r] = false; }   // R33-F3
    trailDirty = paveDirty = true;
}

int trailCell(Vector2f p) {
    int cx = (int)(p.x / trailCellSize), cy = (int)(p.y / trailCellSize);
    if (p.x < 0 || p.y < 0 || cx >= trailGridN || cy >= trailGridN) return -1;
    return cy * trailGridN + cx;
}

// R38-F1: tiers rise at the thresholds but fall only trailTierHysteresis below them, so a cell hovering at a
// threshold no longer flickers. Every tier change goes through here (steps, decay, paving, causeway).
void trailSetTier(int c) {
    int w = trailWear[c];
    uint8_t cur = trailTier[c];
    uint8_t up = w >= trailRoadThreshold ? 2 : w >= trailPathThreshold ? 1 : 0;
    uint8_t down = w >= trailRoadThreshold - trailTierHysteresis ? 2 : w >= trailPathThreshold - trailTierHysteresis ? 1 : 0;
    uint8_t t = max(up, min(cur, down));
    if (t != cur) { trailTier[c] = t; trailDirty = true; }
    uint8_t bk = (uint8_t)(w / trailWearBucket);
    if (bk != trailBucket[c]) { trailBucket[c] = bk; if (t != 0) trailDirty = true; }
}

// R38-F1: alpha lerped from the cell's wear bucket within its tier's range
uint8_t trailAlpha(int c) {
    int w = trailBucket[c] * trailWearBucket;
    bool road = trailTier[c] == 2;
    int lo = road ? trailRoadThreshold : trailPathThreshold, hi = road ? trailMaxWear : trailRoadThreshold;
    int aMin = road ? trailRoadAlphaMin : trailPathAlphaMin, aMax = road ? trailRoadAlphaMax : trailPathAlphaMax;
    float f = clamp((float)(w - lo) / (float)(hi - lo), 0.0f, 1.0f);
    return (uint8_t)(aMin + f * (aMax - aMin) + 0.5f);
}

// R39-F1: wear is added once per cell entered (trailWearPerCell), never per pixel
void trailStep(barb& b, Vector2f p) {
    int c = trailCell(p);
    if (c == b.route.lastCell) return;
    b.route.lastCell = c;
    if (c < 0 || trailWear[c] >= trailMaxWear) return;
    trailWear[c] = min(trailMaxWear, trailWear[c] + trailWearPerCell);
    trailSetTier(c);
}

void trailTick() {
    static int dtimer = 0;
    int interval = raining ? trailDecayIntervalTicks / rainErodeMult : trailDecayIntervalTicks;
    if (++dtimer < interval) return; // dtimer >= interval falls through, so rain starting mid-countdown can't stall it
    dtimer = 0;
    for (int c = 0; c < (int)trailWear.size(); c++) {
        if (trailWear[c] == 0 || trailPaved[c]) continue;   // cobbles hold their road tier
        trailWear[c]--;
        trailSetTier(c);
    }
}

// R39-F1: path tier and road tier (paved cells included) shave trailSpeedBonus[tier] ticks off the step limit
int stepLimit(const barb& b, Vector2f pos) {
    int c = trailCell(pos);
    return max(1, stepLimit(b) - (c >= 0 ? trailSpeedBonus[trailTier[c]] : 0));
}

// R39-F1 A* scratch, sized once and reused by every plan (stamps instead of clearing; the heap keeps its capacity)
vector<float> routeG(trailGridN * trailGridN, 0.0f);     // cost so far
vector<float> routeLenG(trailGridN * trailGridN, 0.0f);  // geometric length so far (cells), for the detour cap
vector<int> routeFrom(trailGridN * trailGridN, -1);
vector<int> routeSeen(trailGridN * trailGridN, 0);       // == routeStamp: G/LenG/From valid this plan
vector<int> routeDone(trailGridN * trailGridN, 0);       // == routeStamp: expanded this plan
vector<pair<float, int>> routeOpen;                      // min-heap on f via greater<>
int routeStamp = 0;

// Lake water blocks a route cell (centre inside a lake rect); the bank zone stays walkable so shore spots are reachable
bool routeCellBlocked(int c) {
    Vector2f centre(((c % trailGridN) + 0.5f) * trailCellSize, ((c / trailGridN) + 0.5f) * trailCellSize);
    for (auto& l : lakes) if (l.contains(centre)) return true;
    return false;
}

float routeOctile(int a, int b) {
    int dx = abs(a % trailGridN - b % trailGridN), dy = abs(a / trailGridN - b / trailGridN);
    return (float)max(dx, dy) + 0.41421356f * (float)min(dx, dy);
}

// Plans r from `from` to `to` (top-left positions) and tags it with the goal. Returns false (r stays planned with
// len 0 = straight walk) when start/goal is off the grid, they share a cell, or A* hits routeExpandCap or the detour cap.
// h is the plain octile distance (cells cost <= 1), so the search leans greedy and stays inside the expansion cap.
bool planRoute(barbRoute& r, Vector2f from, Vector2f to, int goalX, int goalY) {
    r.planned = true; r.goalX = goalX; r.goalY = goalY; r.len = r.idx = 0;
    int s = trailCell(from), g = trailCell(to);
    if (s < 0 || g < 0 || s == g) return false;
    if (++routeStamp == INT32_MAX) { fill(routeSeen.begin(), routeSeen.end(), 0); fill(routeDone.begin(), routeDone.end(), 0); routeStamp = 1; }
    float maxLen = routeDetourCap * routeOctile(s, g) + 0.001f;
    routeOpen.clear();
    routeG[s] = routeLenG[s] = 0.0f; routeFrom[s] = -1; routeSeen[s] = routeStamp;
    routeOpen.push_back({routeOctile(s, g), s});
    int expanded = 0;
    bool found = false;
    while (!routeOpen.empty() && expanded < routeExpandCap) {
        pop_heap(routeOpen.begin(), routeOpen.end(), greater<>());
        int n = routeOpen.back().second;
        routeOpen.pop_back();
        if (routeDone[n] == routeStamp) continue;
        routeDone[n] = routeStamp;
        expanded++;
        if (n == g) { found = true; break; }
        int nx = n % trailGridN, ny = n / trailGridN;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int x = nx + dx, y = ny + dy;
            if (x < 0 || y < 0 || x >= trailGridN || y >= trailGridN) continue;
            int m = y * trailGridN + x;
            if (routeDone[m] == routeStamp || (m != g && routeCellBlocked(m))) continue;
            if (dx != 0 && dy != 0 && (routeCellBlocked(ny * trailGridN + x) || routeCellBlocked(y * trailGridN + nx))) continue;   // no corner-cutting into water
            float step = (dx != 0 && dy != 0) ? 1.41421356f : 1.0f;
            float len = routeLenG[n] + step;
            if (len + routeOctile(m, g) > maxLen) continue;   // detour cap
            float cost = routeG[n] + step * (1.0f - routeWearDiscount * (float)min(trailWear[m], pathWearClamp) / (float)pathWearClamp);
            if (routeSeen[m] == routeStamp && cost >= routeG[m]) continue;
            routeSeen[m] = routeStamp; routeG[m] = cost; routeLenG[m] = len; routeFrom[m] = n;
            routeOpen.push_back({cost + routeOctile(m, g), m});
            push_heap(routeOpen.begin(), routeOpen.end(), greater<>());
        }
    }
    if (!found) return false;
    int count = 0;
    for (int c = routeFrom[g]; c != s && c >= 0; c = routeFrom[c]) count++;   // cells strictly between start and goal
    if (count > routeMaxCells) return false;
    r.len = count;
    int k = count;
    for (int c = routeFrom[g]; c != s && c >= 0; c = routeFrom[c]) r.cells[--k] = c;
    return true;
}

// Where the barb at pos should head this step: the next waypoint cell's centre, or the goal once the route is spent
// (or when it is a straight walk). Replans on a new goal, a waypoint cell gone blocked, or a barb that strayed.
Vector2f routeAim(barb& b, Vector2f pos) {
    barbRoute& r = b.route;
    Vector2f goal((float)b.targetx, (float)b.targety);
    if (!r.planned || r.goalX != b.targetx || r.goalY != b.targety) planRoute(r, pos, goal, b.targetx, b.targety);
    int cur = trailCell(pos);
    for (int k = r.idx; k < r.len && k < r.idx + routeLookahead; k++)
        if (r.cells[k] == cur) { r.idx = k + 1; break; }
    if (r.idx >= r.len) return goal;
    int c = r.cells[r.idx];
    Vector2f centre(((c % trailGridN) + 0.5f) * trailCellSize, ((c / trailGridN) + 0.5f) * trailCellSize);
    Vector2f d = centre - pos;
    if (routeCellBlocked(c) || d.x * d.x + d.y * d.y > routeStrayPx * routeStrayPx) {
        if (!planRoute(r, pos, goal, b.targetx, b.targety) || r.len == 0) return goal;   // straight fallback
        c = r.cells[0];
        centre = Vector2f(((c % trailGridN) + 0.5f) * trailCellSize, ((c / trailGridN) + 0.5f) * trailCellSize);
    }
    return centre;
}

// R39-F1: the one walk step for every non-wander walk (and wander with straight = true). Counts the step timer,
// moves 1px toward the route's next waypoint (or straight at targetx/y) and wears the trail once per cell entered.
// Callers keep their own arrival checks and only call this while the barb is >= 1px from targetx/targety.
void walkStep(barb& b, RectangleShape& sq, bool straight = false) {
    Vector2f pos = sq.getPosition();
    if (++b.stepTimer < stepLimit(b, pos)) return;
    b.stepTimer = 0;
    Vector2f aim = straight ? Vector2f((float)b.targetx, (float)b.targety) : routeAim(b, pos);
    Vector2f d = aim - pos;
    float dSq = d.x * d.x + d.y * d.y;
    if (dSq < 0.0001f) { d = Vector2f((float)b.targetx, (float)b.targety) - pos; dSq = d.x * d.x + d.y * d.y; }
    if (dSq < 0.0001f) return;
    sq.move(d / sqrt(dSq));
    trailStep(b, sq.getPosition());
}

// Rebuilds the tinted cell quads only when some cell changed tier or wear bucket (va reused, capacity kept)
void drawTrails(RenderWindow& window, VertexArray& va) {
    if (trailDirty) {
        va.clear();
        for (int c = 0; c < (int)trailTier.size(); c++) {
            if (trailTier[c] == 0) continue;
            Color col = trailPaved[c] ? trailRoadColor : trailTier[c] == 2 ? trailRoadTint : trailPathTint;   // paved underlay unchanged
            if (!trailPaved[c]) col.a = trailAlpha(c);
            float x0 = (c % trailGridN) * trailCellSize, y0 = (c / trailGridN) * trailCellSize;
            float x1 = x0 + trailCellSize, y1 = y0 + trailCellSize;
            Vector2f q[6] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
            for (auto& v : q) va.append(Vertex{v, col});
        }
        trailDirty = false;
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R20-F2: each paved cell is a 2x2 checker of 4px slate cobbles, drawn over the trail tint. Parity is global, so
// neighbouring cells continue the pattern. Rebuilt only when a cell is paved.
void drawPaving(RenderWindow& window, VertexArray& va) {
    if (paveDirty) {
        va.clear();
        float h = trailCellSize / 2.0f;
        for (int c = 0; c < (int)trailPaved.size(); c++) {
            if (!trailPaved[c]) continue;
            float cx = (c % trailGridN) * trailCellSize, cy = (c / trailGridN) * trailCellSize;
            if (trailPaved[c] == causewayPaved) {   // R33-F3: pale stone, 1px bed joints every course, head joints offset per course
                pushQuad(va, Vector2f(cx, cy), Vector2f(trailCellSize, trailCellSize), causewayColor);
                int courses = (int)(trailCellSize / causewayCoursePx);
                for (int k = 0; k < courses; k++) {
                    float y = cy + k * causewayCoursePx;
                    pushQuad(va, Vector2f(cx, y + causewayCoursePx - 1.0f), Vector2f(trailCellSize, 1.0f), causewayJointColor);
                    float shift = (k % 2 == 0) ? causewayCoursePx - 1.0f : causewayCoursePx / 2.0f - 1.0f;
                    for (float x = shift; x < trailCellSize; x += causewayCoursePx)
                        pushQuad(va, Vector2f(cx + x, y), Vector2f(1.0f, causewayCoursePx - 1.0f), causewayJointColor);
                }
                continue;
            }
            for (int q = 0; q < 4; q++) {
                int qx = q % 2, qy = q / 2;
                Color col = (qx + qy) % 2 == 0 ? paveColorA : paveColorB;
                float x0 = cx + qx * h, y0 = cy + qy * h, x1 = x0 + h, y1 = y0 + h;
                Vector2f v[6] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
                for (auto& p : v) va.append(Vertex{p, col});
            }
        }
        paveDirty = false;
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// Cosmetic flock: crosses the map along a heading, scatters from nearby barbs, regroups by cohesion
struct bird {Vector2f pos, vel; int flapOffset = 0;};

void birdTick(vector<bird>& birds, Vector2f& heading, const vector<RectangleShape>& squares, const vector<barb>& barbs) {
    static int gap = birdGapTicks / 4;
    if (birds.empty()) {
        if (--gap > 0) return;
        gap = birdGapTicks;
        uniform_real_distribution<float> along(100.0f, worldSize - 100.0f), spread(-birdSpawnSpread, birdSpawnSpread), jitter(-0.4f, 0.4f);
        Vector2f start;
        float ang;
        switch (uniform_int_distribution<int>(0, 3)(gen)) {
            case 0:  start = {-20.0f, along(gen)};             ang = 0.0f;   break;
            case 1:  start = {worldSize + 20.0f, along(gen)};  ang = 3.1416f; break;
            case 2:  start = {along(gen), -20.0f};             ang = 1.5708f; break;
            default: start = {along(gen), worldSize + 20.0f};  ang = -1.5708f; break;
        }
        ang += jitter(gen);
        heading = Vector2f(cos(ang), sin(ang));
        for (int i = 0; i < birdFlockSize; i++)
            birds.push_back({start + Vector2f(spread(gen), spread(gen)), heading * birdCruiseSpeed,
                             uniform_int_distribution<int>(0, 2 * birdFlapFrames - 1)(gen)});
        printf("Birds arriving\n");
        return;
    }
    Vector2f center(0.0f, 0.0f);
    for (auto& b : birds) center += b.pos;
    center /= (float)birds.size();
    const float fleeSq = birdFleeRadius * birdFleeRadius, sepSq = birdSeparationDist * birdSeparationDist;
    bool allOut = true;
    for (int i = 0; i < (int)birds.size(); i++) {
        bird& b = birds[i];
        Vector2f acc = (center - b.pos) * birdCohesion + (heading * birdCruiseSpeed - b.vel) * birdAlign;
        for (int j = 0; j < (int)birds.size(); j++) {
            if (j == i) continue;
            Vector2f d = b.pos - birds[j].pos;
            float dSq = d.x * d.x + d.y * d.y;
            if (dSq > 0.0001f && dSq < sepSq) acc += d / sqrt(dSq) * birdSeparation;
        }
        for (int k = 0; k < (int)squares.size() && k < (int)barbs.size(); k++) {
            if (barbHidden(barbs[k])) continue;   // INBOX-F1: sheltering barbs are indoors
            Vector2f d = b.pos - squares[k].getPosition();
            float dSq = d.x * d.x + d.y * d.y;
            if (dSq > 0.0001f && dSq < fleeSq) acc += d / sqrt(dSq) * birdFleeForce;
        }
        b.vel += acc;
        float sp = sqrt(b.vel.x * b.vel.x + b.vel.y * b.vel.y);
        if (sp > birdMaxSpeed) b.vel *= birdMaxSpeed / sp;
        else if (sp < birdMinSpeed) b.vel = sp > 0.0001f ? b.vel * (birdMinSpeed / sp) : heading * birdMinSpeed;
        b.pos += b.vel;
        if (b.pos.x > -birdExitMargin && b.pos.x < worldSize + birdExitMargin && b.pos.y > -birdExitMargin && b.pos.y < worldSize + birdExitMargin)
            allOut = false;
    }
    if (allOut) birds.clear();
}

// Wings alternate between 3x2 (out) and 2x2 (in) for a subtle flap flicker; centred so the bird doesn't jitter
void drawBirds(RenderWindow& window, const vector<bird>& birds, RectangleShape& shape, int frame) {
    shape.setFillColor(birdColor);
    for (auto& b : birds) {
        bool out = ((frame + b.flapOffset) / birdFlapFrames) % 2 == 0;
        Vector2f sz = out ? birdWingsOut : birdWingsIn;
        shape.setSize(sz);
        shape.setOrigin(Vector2f(floor(sz.x / 2.0f), floor(sz.y / 2.0f))); // whole-px origin keeps edges pixel-aligned
        shape.setPosition(Vector2f(round(b.pos.x), round(b.pos.y)));
        window.draw(shape);
    }
}

// Rain streaks, batched; each streak's spawn spot comes from hash(cycle, index) and it falls for one cycle
void drawRain(RenderWindow& window, VertexArray& va, int frame) {
    va.clear();
    for (int i = 0; i < rainStreakCount; i++) {
        uint32_t phase = (uint32_t)i * 2654435761u % rainStreakLifeFrames; // stagger respawns across streaks
        uint32_t f = (uint32_t)frame + phase;
        uint32_t cycle = f / rainStreakLifeFrames;
        int age = (int)(f % rainStreakLifeFrames);
        uint32_t h = cycle * 73856093u ^ (uint32_t)i * 19349663u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        float x = (float)(h % (uint32_t)worldSize) - age * rainStreakDriftPx + rainStreakLifeFrames * rainStreakDriftPx / 2;
        float y = (float)((h >> 10) % (uint32_t)worldSize) + age * rainStreakFallPx - rainStreakLifeFrames * rainStreakFallPx / 2;
        Vector2f p = snapPx(Vector2f(fmod(x + worldSize, worldSize), fmod(y + worldSize, worldSize))); // wrap so streaks stay on the map
        for (int k = 0; k < rainStreakPixels; k++)
            pushQuad(va, p + Vector2f(k == rainStreakPixels - 1 ? -1.0f : 0.0f, (float)k), Vector2f(1.0f, 1.0f), rainStreakColor);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R47-F9 Ancestor Stones. Independent of the barb vectors: a stone keeps only its race and spot, never an index or id.
struct grave {
    Vector2f pos;        // top-left of the 3x4 stone, whole px
    int race = 0;
    int age = 0;         // sim ticks since it was raised
    int glow = 0;        // ticks of brightening left (refreshed while a same-race barb is near)
};
vector<grave> graves;                       // oldest first, at most graveMax
float graveMorale[raceCount] = {};          // the one morale term per race, recomputed by graveTick() every tick
float graveRestAcc[raceCount] = {};         // carries the fractional rest ticks between ticks
bool graveHungerRest[raceCount] = {};       // this tick, race r's hunger does not grow (read in hungerTick)

// R50-F1a The Veil: hidden soul-ether. Holds only values (race, cell amounts), never a barb index or id.
struct veilState {
    float ether[2][raceCount][veilGridN * veilGridN] = {};   // double buffer: [front] is live, the other is the scratch
    int front = 0;
    float pool[raceCount] = {};   // per-race total, kept by deposits/draws and recomputed each diffusion step
    int stepTimer = 0;
    int slowedBirths = 0;         // births the veil held back (for phase 2 omens)
    int lastSlowedRace = -1;      // house race of the last held-back birth
    int lastDrawnRace = -1;       // race of the soul the last birth drew, -1 if none (phase 2 ring tint)
    int omenCooldown = 0;         // R50-F1b: ticks until the next omen line may show
    int moteTimer = 0;            // R50-F1b: mote spawn pass runs once per veilStepTicks
    float faith[raceCount] = {};  // R51-F2: 0..1, derived from pool[r] and smoothed in veilTick (read-only toward pool)
    int faithTier[raceCount] = {};   // R51-F2: 0 low, 1 middling, 2 high (ticker lines on reaching 2 and falling to 0)
    bool faithSeen = false;       // R51-F2: HUD faith tick column shown from the first stir of faith on
    int faithShrineId[raceCount] = {-1, -1, -1};   // R51-F2: shrine (by id) each race's motes rise over, -1 = none yet
};
static_assert(raceCount == 3, "faithShrineId initialiser lists one -1 per race");
veilState veil;
// R50-F1b: own vectors, values only (the name points into the static name table, never at a barb)
struct veilSite {Vector2f pos; int race = 0; const char* name = ""; bool shimmered = false;};
struct veilMote {Vector2f pos; int race = 0; int age = 0; float phase = 0.0f; int shrineId = -1;};   // R51-F2: shrineId for faith motes
vector<veilSite> veilSites;
vector<veilMote> veilMotes;
vector<veilMote> faithMotes;   // R51-F2: shrine motes, own cap (faithMoteMax); each re-finds its shrine by id every tick

int veilCell(int cx, int cy) { return clamp(cy, 0, veilGridN - 1) * veilGridN + clamp(cx, 0, veilGridN - 1); }
int veilCellX(Vector2f p) { return clamp((int)(p.x / veilCellPx), 0, veilGridN - 1); }
int veilCellY(Vector2f p) { return clamp((int)(p.y / veilCellPx), 0, veilGridN - 1); }

float veilTotal() {
    float t = 0.0f;
    for (int r = 0; r < raceCount; r++) t += veil.pool[r];
    return t;
}

// Called from removeBarb() only, with the race and spot copied out before the swapRemove
void veilDeposit(int race, Vector2f p, float amount) {
    int r = clampRace(race);
    veil.ether[veil.front][r][veilCell(veilCellX(p), veilCellY(p))] += amount;
    veil.pool[r] += amount;
    printf("VEIL - %s soul settles at %d,%d (pool %.2f)\n", racePlurals[r], (int)p.x, (int)p.y, veil.pool[r]);
}

// Ether of one race (or every race when race < 0) in the 3x3 cells round p
float veilLocal(int race, Vector2f p) {
    int cx = veilCellX(p), cy = veilCellY(p);
    float s = 0.0f;
    for (int r = 0; r < raceCount; r++) {
        if (race >= 0 && r != race) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= veilGridN || y >= veilGridN) continue;
                s += veil.ether[veil.front][r][y * veilGridN + x];
            }
    }
    return s;
}

// Birth rate factor at p: 1 while the veil is empty (granary rule only), else veilBirthFloor..1 by the nearby souls
float veilBirthFactor(Vector2f p) {
    if (veilTotal() < veilEmptyEpsilon) return 1.0f;
    float fill = min(1.0f, veilLocal(-1, p) / veilFullLocal);
    return veilBirthFloor + (1.0f - veilBirthFloor) * fill;
}

// A birth at p draws up to amount from one race's souls nearby, picked weighted by what each race has there.
// Drains that race's 3x3 cells in proportion; returns the race drawn, or -1 if nothing was near.
int veilDraw(Vector2f p, float amount) {
    float local[raceCount], all = 0.0f;
    for (int r = 0; r < raceCount; r++) { local[r] = veilLocal(r, p); all += local[r]; }
    if (all <= 0.0f) return -1;
    uniform_real_distribution<float> pick(0.0f, all);
    float roll = pick(gen);
    int r = raceCount - 1;
    for (int k = 0; k < raceCount; k++) {
        if (roll < local[k]) { r = k; break; }
        roll -= local[k];
    }
    if (local[r] <= 0.0f) return -1;
    float take = min(amount, local[r]), keep = 1.0f - take / local[r];
    int cx = veilCellX(p), cy = veilCellY(p);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= veilGridN || y >= veilGridN) continue;
            veil.ether[veil.front][r][y * veilGridN + x] *= keep;
        }
    veil.pool[r] = max(0.0f, veil.pool[r] - take);
    return r;
}

// R51-F2: once per veil step, ease each race's faith toward its pool (read only) and log the high/low tier changes
void veilFaithStep() {
    for (int r = 0; r < raceCount; r++) {
        float target = min(1.0f, max(0.0f, veil.pool[r]) / faithFullPool);
        float& f = veil.faith[r];
        f += (target - f) * faithSmoothing;
        if (f < veilZeroBelow) f = 0.0f;
        if (f > faithSeenAbove) veil.faithSeen = true;
        int& tier = veil.faithTier[r];
        if (tier < 2 && f >= faithHigh) {
            tier = 2;
            string line = string("The ") + racePlurals[r] + " feel the Veil close";
            printf("FAITH - %s (faith %.2f, pool %.2f)\n", line.c_str(), f, veil.pool[r]);
            logEvent(line, veilOmenColor);
        } else if (tier == 2 && f < faithHigh - faithTierHysteresis) {
            tier = 1;
            printf("FAITH - %s ease from high faith (%.2f)\n", racePlurals[r], f);
        } else if (tier == 0 && f >= faithLow + faithTierHysteresis) {
            tier = 1;
            printf("FAITH - %s faith stirs (%.2f)\n", racePlurals[r], f);
        } else if (tier >= 1 && f < faithLow) {
            tier = 0;
            string line = string("The ") + racePlurals[r] + "' faith grows thin";   // every plural ends in s
            printf("FAITH - %s (faith %.2f)\n", line.c_str(), f);
            logEvent(line, veilOmenColor);
        }
    }
}

// Every veilStepTicks: diffuse (no-flux edges, so diffusion alone keeps the total) and decay each race's grid from
// the front buffer into the back one, then flip. Recomputes the per-race pools.
void veilTick() {
    if (++veil.stepTimer < veilStepTicks) return;
    veil.stepTimer = 0;
    if (veilTotal() <= 0.0f) { veilFaithStep(); return; }   // nothing in the veil: skip the sweep (faith still eases down)
    const int src = veil.front, dst = 1 - veil.front;
    for (int r = 0; r < raceCount; r++) {
        const float* cur = veil.ether[src][r];
        float* nxt = veil.ether[dst][r];
        float total = 0.0f;
        for (int y = 0; y < veilGridN; y++)
            for (int x = 0; x < veilGridN; x++) {
                int i = y * veilGridN + x;
                float c = cur[i];
                float n = (x > 0 ? cur[i - 1] : c) + (x < veilGridN - 1 ? cur[i + 1] : c)
                        + (y > 0 ? cur[i - veilGridN] : c) + (y < veilGridN - 1 ? cur[i + veilGridN] : c);
                float v = (c + veilDiffusion * (n * 0.25f - c)) * (1.0f - veilDecay);
                if (v < veilZeroBelow) v = 0.0f;
                nxt[i] = v;
                total += v;
            }
        veil.pool[r] = total;
    }
    veil.front = dst;
    veilFaithStep();   // R51-F2: after the pools are recomputed
}

// R50-F1b: a rare omen line. Every omen shares one cooldown and fires only 1 in veilOmenOneIn eligible times.
bool veilOmen(const string& text) {
    if (veil.omenCooldown > 0) return false;
    if (uniform_int_distribution<int>(0, veilOmenOneIn - 1)(gen) != 0) return false;
    veil.omenCooldown = veilOmenCooldownTicks;
    printf("VEIL OMEN - %s\n", text.c_str());
    logEvent(text, veilOmenColor);
    return true;
}

// R50-F1b: called from removeBarb() only, beside veilDeposit, with everything copied by value
void veilAddSite(int race, Vector2f p, const char* name) {
    if ((int)veilSites.size() >= veilSiteMax) veilSites.erase(veilSites.begin());   // oldest-first eviction
    veilSite s;
    s.pos = p;
    s.race = clampRace(race);
    s.name = name ? name : "";
    veilSites.push_back(s);
}

// R50-F1b: ages motes every tick; once per veilStepTicks each site whose soul lingers may raise a mote (capped),
// and sites whose soul has thinned out are forgotten.
void veilOmenTick() {
    if (veil.omenCooldown > 0) veil.omenCooldown--;
    for (auto& m : veilMotes) m.age++;
    for (int i = (int)veilMotes.size() - 1; i >= 0; i--)
        if (veilMotes[i].age >= veilMoteTicks) swapRemove(veilMotes, i);
    if (++veil.moteTimer < veilStepTicks) return;
    veil.moteTimer = 0;
    static uniform_real_distribution<float> roll(0.0f, 1.0f);
    static uniform_real_distribution<float> scatter(-veilMoteScatterPx, veilMoteScatterPx);
    for (int i = (int)veilSites.size() - 1; i >= 0; i--) {
        veilSite& s = veilSites[i];
        float local = veilLocal(s.race, s.pos);
        if (local < veilSiteQuietBelow) { veilSites.erase(veilSites.begin() + i); continue; }
        if ((int)veilMotes.size() >= veilMoteMax) continue;
        float fill = min(1.0f, local / veilFullLocal);
        if (roll(gen) >= fill / veilMoteOneIn) continue;
        veilMote m;
        m.pos = s.pos + Vector2f(scatter(gen), scatter(gen) * 0.5f);
        m.race = s.race;
        m.phase = roll(gen) * 6.2832f;
        veilMotes.push_back(m);
        if (!s.shimmered) {
            s.shimmered = true;
            veilOmen(string("Something shimmers where ") + s.name + " fell");
        }
    }
}

// Called from removeBarb() only. The stone stands where the barb's feet were, nudged off any stone already there.
void addGrave(const barb& b, const RectangleShape& sq) {
    Vector2f p(floor(sq.getPosition().x + sq.getSize().x / 2.0f - 1.0f), floor(sq.getPosition().y + sq.getSize().y) - graveSize.y);
    for (int tries = 0; tries < 4; tries++) {
        bool hit = false;
        for (auto& g : graves)
            if (fabs(g.pos.x - p.x) < graveSize.x + 1.0f && fabs(g.pos.y - p.y) < graveSize.y) { hit = true; break; }
        if (!hit) break;
        p.x += graveSpreadPx;
    }
    p.x = clamp(p.x, 0.0f, worldSize - graveSize.x);
    p.y = clamp(p.y, 0.0f, worldSize - graveSize.y);
    if ((int)graves.size() >= graveMax) graves.erase(graves.begin());   // oldest-first eviction
    grave g;
    g.pos = p;
    g.race = clampRace(b.race);
    graves.push_back(g);
    printf("GRAVE - stone raised for %s (%s) at %d,%d - %d stones\n", b.name, raceOf(b).name, (int)p.x, (int)p.y, (int)graves.size());
}

// Ages the stones, lights the ones a same-race barb is beside, and computes each race's morale term once.
// Runs before hungerTick(), which reads graveHungerRest.
void graveTick(const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    int own[raceCount] = {};
    for (auto& g : graves) {
        g.age++;
        if (g.glow > 0) g.glow--;
        own[clampRace(g.race)]++;
        Vector2f c = g.pos + graveSize / 2.0f;
        for (int i = 0; i < (int)barbs.size() && i < (int)squares.size(); i++) {
            const barb& b = barbs[i];
            if (b.race != g.race || barbHidden(b) || barbUnderground(b)) continue;
            Vector2f d = squares[i].getPosition() + squares[i].getSize() / 2.0f - c;
            if (d.x * d.x + d.y * d.y <= graveNoticePx * graveNoticePx) { g.glow = graveGlowTicks; break; }
        }
    }
    for (int r = 0; r < raceCount; r++) {
        graveMorale[r] = min(graveMoraleMax, own[r] * graveMoralePerStone);
        graveRestAcc[r] += graveMorale[r];
        graveHungerRest[r] = graveRestAcc[r] >= 1.0f;
        if (graveHungerRest[r]) graveRestAcc[r] -= 1.0f;
    }
}

// Returns true on ticks where the barb makes work progress; hungry barbs skip some ticks
bool workTick(barb& b) {
    if (!isHungry(b)) return true;
    if (++b.slowTick >= hungerWorkSlowdown) { b.slowTick = 0; return true; }
    return false;
}

void hungerTick(vector<barb>& barbs) {
    static int hungryLogCooldown = 0;
    if (hungryLogCooldown > 0) hungryLogCooldown--;
    // Pass 1: hunger grows (once per barb per tick); the "no food" line is logged at most once per tick
    int hungryCommoners[raceCount] = {};   // RACE-F1: each race eats from its own larder, so the reserve is per race
    // R51-F3: in winter, every 1/(winterHungerMult-1) ticks hunger grows one extra step (1.15 -> ~1 extra per 6.7 ticks)
    static float winterHungerCarry = 0.0f;
    int growth = 1;
    if (isWinter()) {
        winterHungerCarry += winterHungerMult - 1.0f;
        if (winterHungerCarry >= 1.0f) { winterHungerCarry -= 1.0f; growth++; }
    } else winterHungerCarry = 0.0f;
    for (auto& b : barbs) {
        if (!isHungry(b)) {
            if (!graveHungerRest[clampRace(b.race)]) b.hunger += growth;   // R47-F9: ancestor stones' morale rest tick
            if (isHungry(b) && stockOf(b).food <= 0 && hungryLogCooldown == 0) {
                logRaceEvent(string(b.name) + " is hungry - no food (" + racePlurals[clampRace(b.race)] + ")", b.race);
                hungryLogCooldown = eventHungryCooldownTicks;
            }
        }
        if (isHungry(b) && !isGentry(b) && b.activity != actExpedition) hungryCommoners[clampRace(b.race)]++;   // R45-F2: away members eat rations
    }
    // Pass 2 (R24-F1): eating, Gentry first. While a Commoner is hungry, Gentry leave gentryFoodReserve
    // so the Commoners are never shut out of the larder entirely.
    // R25-F1: a Glutton takes gluttonMealFood, but never digs into `keep` (callers guarantee food > keep, so bite >= 1)
    auto eat = [](barb& b, int keep) {
        raceStock& rs = stockOf(b);
        int bite = min(b.trait == traitGlutton ? gluttonMealFood : 1, rs.food - keep);
        if (bite <= 0) return;   // R30-F5: no food taken = no meal, no heal
        rs.food -= bite;
        b.hunger = 0;
        b.slowTick = 0;
        b.starveTicks = 0;                                       // R30-F5
        b.health = min(barbMaxHealth, b.health + eatHealHp);
        printf("BARB ATE (%s, %s %d) - %s food: %d\n", isGentry(b) ? "Gentry" : "Commoner", b.name, bite, racePlurals[clampRace(b.race)], rs.food);
    };
    for (auto& b : barbs) {
        int gentryKeep = hungryCommoners[clampRace(b.race)] > 0 ? gentryFoodReserve : 0;
        if (isGentry(b) && isHungry(b) && b.activity != actExpedition && stockOf(b).food > gentryKeep) eat(b, gentryKeep);
    }
    for (auto& b : barbs)
        if (!isGentry(b) && isHungry(b) && b.activity != actExpedition && stockOf(b).food > 0) eat(b, 0);   // R45-F2: rations only, in expeditionTick()
    // Pass 3 (R30-F5): whoever is still hungry after the meal goes on starving; damage only past the grace period.
    // Health is only lowered here; deaths are removed by starveDeathTick() at the end of the tick.
    for (auto& b : barbs) {
        if (!isHungry(b) || b.health <= 0) continue;
        b.starveTicks++;
        int over = b.starveTicks - starveGraceTicks;
        bool leanFloor = isWinter() && b.health <= leanStarveFloorHp;   // R52-F2: no starvation deaths in winter
        if (over > 0 && over % starveDamageTicks == 0 && !leanFloor) {
            b.health--;
            if (b.health % 25 == 0) printf("STARVING - %s %d hp\n", b.name, b.health);
        }
    }
}

bool isStarving(const barb& b) { return isHungry(b) && b.starveTicks >= starveGraceTicks; }

// R52-F4: the cough. Runs after hungerTick() (so a meal's heal lands first) and before starveDeathTick(), which removes
// a barb the cough brought to 0 hp. Buckets are rebuilt from scratch every tick, so they never hold a stale index.
void sickTick(vector<barb>& barbs) {
    static unordered_map<int, vector<int>> buckets;   // house id (founders: -1 - race) -> barb indices, this tick only
    static deque<int> deathTicks;                     // sim ticks of recent cough deaths, for the rolling cap
    static int tickNow = 0;
    static uniform_real_distribution<float> roll(0.0f, 1.0f);
    tickNow++;
    while (!deathTicks.empty() && tickNow - deathTicks.front() >= sickDeathWindowTicks) deathTicks.pop_front();
    float winter = isWinter() ? sickWinterMult : 1.0f;

    for (auto& kv : buckets) kv.second.clear();
    int anySick = 0, raceAlive[raceCount] = {};
    for (int i = 0; i < (int)barbs.size(); i++) {
        const barb& b = barbs[i];
        raceAlive[clampRace(b.race)]++;
        if (b.sick > 0) anySick++;
        if (b.activity == actExpedition) continue;   // away from home: neither catches nor spreads it
        buckets[b.homeHouse >= 0 ? b.homeHouse : -1 - clampRace(b.race)].push_back(i);
    }

    // Seed: a new outbreak only once the last one has burnt out, so each outbreak gets exactly one ticker line
    if (anySick == 0 && !barbs.empty() && roll(gen) * sickSeedOneIn < winter) {
        vector<int> open;
        for (auto& kv : buckets) for (int i : kv.second) if (barbs[i].sickImmune <= 0 && barbs[i].health > 0) open.push_back(i);
        if (!open.empty()) {
            barb& p = barbs[open[uniform_int_distribution<int>(0, (int)open.size() - 1)(gen)]];
            int r = clampRace(p.race);
            // R53-F3: a blessed race may shrug off the first case (curses act on the spread below, not the seed)
            float seedMult = min(1.0f, omenMult(r, omenCough));
            if (seedMult < 1.0f && roll(gen) >= seedMult) {
                printf("COUGH OUTBREAK AVERTED - %s (%s omen x%.2f)\n", p.name, racePlurals[r], seedMult);
            } else {
                catchCough(p);
                printf("COUGH OUTBREAK - %s (%s), %s\n", p.name, raceDefs[r].name, isWinter() ? "winter" : "out of season");
                logRaceEvent(string(isWinter() ? "Winter cough" : "A cough") + " in the " + raceDefs[r].name + " quarter - " + p.name + " is sick", r);
            }
        }
    }

    // Spread: each well, non-immune barb rolls once per sick housemate's worth of chance; crowded houses spread faster
    for (auto& kv : buckets) {
        const vector<int>& m = kv.second;
        int sickHere = 0;
        for (int i : m) if (barbs[i].sick > 0) sickHere++;
        if (sickHere == 0 || sickHere == (int)m.size()) continue;
        float chance = sickSpreadPerKin * sickHere * winter * ((int)m.size() >= sickCrowdSize ? sickCrowdMult : 1.0f);
        for (int i : m) {
            barb& b = barbs[i];
            if (b.sick > 0 || b.sickImmune > 0 || b.health <= 0) continue;
            // R53-F3: the race's omen scales the catch rate
            if (roll(gen) < chance * omenMult(clampRace(b.race), omenCough)) { catchCough(b); printf("COUGH SPREADS - %s (house %d)\n", b.name, kv.first); }
        }
    }

    // Course: slow health loss, the death cap and the floors, then natural recovery with a brief immunity
    int colony = (int)barbs.size();
    for (auto& b : barbs) {
        if (b.sickImmune > 0) b.sickImmune--;
        if (b.sick <= 0 || b.health <= 0) continue;
        if (b.sick % sickDamageTicks == 0) {
            int r = clampRace(b.race);
            bool mayDie = (int)deathTicks.size() < sickDeathCap && colony > sickColonyFloor && raceAlive[r] > sickRaceFloor;
            if (b.health > 1) b.health--;
            else if (mayDie) {
                b.health = 0;   // removed by starveDeathTick() at the end of this tick
                deathTicks.push_back(tickNow);
                colony--; raceAlive[r]--;
                printf("COUGH DEATH - %s (%s)\n", b.name, raceDefs[r].name);
            }
        }
        if (--b.sick == 0) {
            b.sickImmune = sickImmuneTicks;
            printf("COUGH RECOVERED - %s, %d hp\n", b.name, b.health);
        }
    }
}

Color hungryTint(Color c) {
    auto mix = [](uint8_t a, uint8_t t) { return (uint8_t)(a + (t - a) * hungryTintAmount); };
    return Color(mix(c.r, hungryTintColor.r), mix(c.g, hungryTintColor.g), mix(c.b, hungryTintColor.b));
}

// Direction from shore point k toward its water. spawnLakes() pushes each lake's points as top, bottom, left, right.
Vector2f shoreWaterDir(int k) {
    static const Vector2f dirs[4] = {{0.0f, 1.0f}, {0.0f, -1.0f}, {1.0f, 0.0f}, {-1.0f, 0.0f}};
    return dirs[k % 4];
}

// Hitbox top-left for an angler at shore point k: the body sits wholly on the bank, its water-facing edge on the point.
Vector2f shoreStandSpot(int k, Vector2f bodySize) {
    Vector2f p = shorePoints[k], w = shoreWaterDir(k);
    float x = w.x < 0.0f ? p.x : (w.x > 0.0f ? p.x - bodySize.x + 1.0f : p.x);
    float y = w.y < 0.0f ? p.y : p.y - bodySize.y + 1.0f;
    return Vector2f(x, y);
}

// Rod and line of a fishing barb, from its snapped hitbox (same body top drawBarbs uses; no bob while working).
// The hand is the pixel beside the body on the water side; the rod steps diagonally out over the water.
struct fishPose {Vector2f hand, step, tip, lineEnd;};
fishPose fishPoseOf(const barb& b, const RectangleShape& sq) {
    Vector2f base = snapPx(sq.getPosition()), size = sq.getSize();
    float bodyTop = base.y + (isHungry(b) ? barbHungryDroopPx : 0.0f);
    Vector2f w = shoreWaterDir(b.targetShore);
    fishPose f;
    f.hand = Vector2f(w.x < 0.0f ? base.x - 1.0f : base.x + size.x, bodyTop + 1.0f);
    f.step = w.x != 0.0f ? Vector2f(w.x, -1.0f) : Vector2f(1.0f, w.y);   // side casts lift the rod; up/down casts reach over
    f.tip = f.hand + f.step * fishRodPx;
    f.lineEnd = f.tip + w * fishLineReach + (w.x != 0.0f ? Vector2f(0.0f, fishLineDrop) : Vector2f(0.0f, 0.0f));
    return f;
}

// Read-only on game state: squares stay the hitbox, the sprite is built from temporary shapes.
// Plain white 2x4 rectangle (no head/hair); the task shows as a 2x1 stripe and a tinted tool pixel.
// Pose comes from existing flags only: walking bob, chop/mine/build tool tick, pick arm dip, hungry droop.
void drawBarbs(RenderWindow& window, const vector<barb>& barbs, const vector<RectangleShape>& squares, int frame, float zoom) {
    RectangleShape body(barbBodySize), stripe(Vector2f(barbBodySize.x, barbStripeH)), tool(barbToolSize), hat, tip, dot(Vector2f(1.0f, 1.0f));
    bool showTraits = zoom >= traitDotMinZoom;
    int carrierId = relicCarrierId();   // R45-F2
    int lootId = lootCarrierId();       // R47-F5
    int cartId = caravanCartId();       // R54-F1
    bool cartLaden = caravanLaden();
    static VertexArray lookVerts(PrimitiveType::Triangles);   // R35-F1: every race mark this frame, one draw after the loop
    lookVerts.clear();
    watchTorchPts.clear();   // R51-F1
    auto addLookQuad = [](Vector2f p, Vector2f sz, Color c) {
        Vector2f q[4] = {p, p + Vector2f(sz.x, 0.0f), p + sz, p + Vector2f(0.0f, sz.y)};
        int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx) lookVerts.append(Vertex{q[k], c});
    };
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        const barb& b = barbs[i];
        if (barbHidden(b) || barbUnderground(b)) continue;   // INBOX-F1: indoors; R40-F1: down the shaft
        const raceDef& rd = raceOf(b);
        Vector2f bodySize = rd.bodySize;
        bool hungry = isHungry(b);
        int phase = frame + b.id * 3;   // desync barbs so they don't animate in lockstep
        bool working = b.chopping || (b.building && b.timer > 0);
        bool walking = b.moving && !working;

        Vector2f base = snapPx(squares[i].getPosition());
        float feet = base.y + squares[i].getSize().y;    // bottom edge of the hitbox
        float bob = (walking && (phase / barbBobFrames) % 2 == 1) ? -barbBobPx : 0.0f;
        float droop = hungry ? barbHungryDroopPx : 0.0f;

        float bodyH = bodySize.y - droop;
        float bodyTop = feet - bodyH + bob;

        Color tc = barbColor(b);                         // task colour; barbIdleColor means no task
        bool idle = tc == barbIdleColor;
        if (hungry) tc = hungryTint(tc);
        if (b.shinobi) {   // R25-F2: faint pale rim one px around the body, drawn underneath it
            body.setSize(Vector2f(bodySize.x + 2.0f, bodyH + 2.0f));
            body.setFillColor(shinobiOutlineColor);
            body.setPosition(Vector2f(base.x - 1.0f, bodyTop - 1.0f));
            window.draw(body);
        }
        bool raidHit = b.raidFlash > 0;   // R52-F1: 2-frame hit flash, body plus a 1px rim so it reads over the off-white body
        if (raidHit) {
            body.setSize(Vector2f(bodySize.x + 2.0f, bodyH + 2.0f));
            body.setFillColor(kRaidFlashColor);
            body.setPosition(Vector2f(base.x - 1.0f, bodyTop - 1.0f));
            window.draw(body);
        }
        Color bodyColor = b.shinobi ? shinobiBodyColor : barbBodyColor;
        body.setSize(Vector2f(bodySize.x, bodyH));
        body.setFillColor(raidHit ? kRaidFlashColor : hungry ? hungryTint(bodyColor) : bodyColor);
        body.setPosition(Vector2f(base.x, bodyTop));
        window.draw(body);
        if (showTraits) {   // R25-F1: 1px badge on the left shoulder, off the body so it never blends with stripe or sash
            dot.setFillColor(traitOf(b).dotColor);
            dot.setPosition(Vector2f(base.x - 1.0f, bodyTop + 1.0f));
            window.draw(dot);
        }
        if (!idle) {
            stripe.setSize(Vector2f(bodySize.x, barbStripeH));
            stripe.setFillColor(tc);
            stripe.setPosition(Vector2f(base.x, bodyTop + barbStripeTop));
            window.draw(stripe);
        }
        if (isGentry(b)) {   // R24-F1: sash on its own row; a slouching dwarf (2 rows) wears it on row 0 so it never hits the stripe
            float sashTop = gentrySashTop + gentrySashH <= bodyH ? gentrySashTop : 0.0f;
            stripe.setSize(Vector2f(bodySize.x, gentrySashH));
            stripe.setFillColor(gentrySashColor);
            stripe.setPosition(Vector2f(base.x, bodyTop + sashTop));
            window.draw(stripe);
        }
        if (b.shinobi) {   // R25-F2: 1px headband on the top body row (stripe is row 1, sash row 2)
            stripe.setSize(Vector2f(bodySize.x, 1.0f));
            stripe.setFillColor(shinobiBandColor);
            stripe.setPosition(Vector2f(base.x, bodyTop));
            window.draw(stripe);
        }
        if (b.sick > 0) {   // R52-F4: sallow 1px face pixel, top-left body cell (row 0: above the task stripe and the sash,
            dot.setFillColor(sickDotColor);   // clear of the dwarf beard at x=1); drawn over the shinobi band, at every zoom
            dot.setPosition(Vector2f(base.x, bodyTop));
            window.draw(dot);
        }
        if (zoom >= drillPipMinZoom && b.drillCount >= kDrillPipCount) {   // R47-F6: 1px dark-red pip, bottom-right body pixel,
            float row = bodyH - 1.0f;                                      // only on a row below the task stripe and off the sash
            if (row > barbStripeTop && !(isGentry(b) && row >= gentrySashTop && row < gentrySashTop + gentrySashH)) {
                dot.setFillColor(drillPipColor);
                dot.setPosition(Vector2f(base.x + bodySize.x - 1.0f, bodyTop + row));
                window.draw(dot);
            }
        }
        if (rd.hatSize.x > 0.0f) {                       // race mark above the head, never on the body
            float hatTop = bodyTop - rd.hatSize.y;
            hat.setSize(rd.hatSize);
            hat.setFillColor(rd.hatColor);
            hat.setPosition(Vector2f(base.x, hatTop));
            window.draw(hat);
            if (rd.hatTipSize.x > 0.0f) {
                tip.setSize(rd.hatTipSize);
                tip.setFillColor(rd.hatColor);
                tip.setPosition(Vector2f(base.x, hatTop - rd.hatTipSize.y));
                window.draw(tip);
            }
        }
        const raceLook& lk = raceLooks[clampRace(b.race)];   // R35-F1: batched, drawn after every barb
        for (int m = 0; m < lk.markCount; m++) {
            const lookMark& mk = lk.marks[m];
            float mx = base.x + mk.x + (mk.fromRight ? bodySize.x : 0.0f);
            float mh = mk.toFeet ? bodyH - mk.y : mk.h;
            if (mh <= 0.0f) continue;   // a drooping body can be too short for the cloak
            addLookQuad(Vector2f(mx, bodyTop + mk.y), Vector2f(mk.w, mh), (mk.rainDarkens && raining) ? lk.rainColor : mk.color);
        }
        if (zoom >= crownMinZoom && isLeader(b)) {   // R28-F2: gold band on the very top of the head or hat (stays under the ballot pip)
            float headTop = bodyTop - rd.hatSize.y - rd.hatTipSize.y;
            stripe.setSize(crownSize);
            stripe.setFillColor(crownColor);
            stripe.setPosition(Vector2f(base.x + floor((bodySize.x - crownSize.x) / 2.0f), headTop - crownSize.y));
            window.draw(stripe);
        }

        float side = base.x + bodySize.x;                // tool/arm sits just right of the body
        tool.setFillColor(tc);
        if (b.chopping && b.target == targetLake && b.targetShore >= 0 && b.targetShore < (int)shorePoints.size()) {
            fishPose f = fishPoseOf(b, squares[i]);
            Vertex line[2] = {Vertex{f.tip + Vector2f(0.5f, 0.5f), fishLineColor}, Vertex{f.lineEnd + Vector2f(0.5f, 0.5f), fishLineColor}};
            window.draw(line, 2, PrimitiveType::Lines);   // hairline under the rod so the tip pixel stays solid
            tool.setPosition(f.hand);
            window.draw(tool);                            // hand, in the (hunger-tinted) task colour
            tool.setFillColor(fishRodColor);
            for (int k = 1; k <= (int)fishRodPx; k++) {
                tool.setPosition(f.hand + f.step * (float)k);
                window.draw(tool);
            }
        } else if (b.chopping && (b.target == targetBush || b.target == targetField)) {   // pick / reap: same arm dip
            bool dip = (phase / barbPickFrames) % 2 == 1;
            tool.setPosition(Vector2f(side, dip ? feet - barbToolSize.y : bodyTop + 1.0f));
            window.draw(tool);
        } else if (working && b.timer > 0) {
            bool raised = (phase / barbToolFrames) % 2 == 0;
            if (b.chopping && b.target == targetNone && know.learned[techHandAxe]) tool.setFillColor(steelToolColor); // R20-F1: 1px axe head
            tool.setPosition(Vector2f(side, raised ? bodyTop - barbToolSize.y : bodyTop + 1.0f));
            window.draw(tool);
            if (raised && b.chopping && (b.target == targetNone || b.target == targetRock) && b.jobTier > 0) {   // R47-F7: the one tier mark,
                dot.setFillColor(raceTierDimFrames[clampRace(b.race)] > 0 ? toolTierMarkDimColor : toolTierMarkColor); // 1px steel edge on top of the raised tool (R49-F1: dim after a tier drop)
                dot.setPosition(Vector2f(side, bodyTop - barbToolSize.y - 1.0f));
                window.draw(dot);
            }
            if (lk.axeGlint && raised && (phase / (barbToolFrames * 2)) % dwarfGlintEvery == 0)   // R35-F1: up-right of the head, clear of the horn
                addLookQuad(Vector2f(side + 1.0f, bodyTop - barbToolSize.y - 1.0f), Vector2f(1.0f, 1.0f), dwarfGlintColor);
        } else if (b.jobKind == jobSpear && b.spearTimer > 0) {   // R20-F3: knapping flint at the fire, hand taps the stone
            bool raised = (phase / spearKnapFrames) % 2 == 0;
            tool.setFillColor(spearTipColor);
            tool.setPosition(Vector2f(side, raised ? bodyTop + 1.0f : bodyTop + 2.0f));
            window.draw(tool);
        } else if (b.activity == actDrill && !b.moving) {   // R47-F6: at the dummy: wind up (head row) just before a hit, jab (arm row) just after
            bool windUp = b.drillTimer >= kDrillHitTicks - kDrillJabTicks;
            bool jab = b.drillHits > 0 && b.drillTimer < kDrillJabTicks;
            if (windUp || jab) {
                tool.setFillColor(drillJabColor);
                float x = b.drillSlot == 0 ? side : base.x - 1.0f;   // slot 0 stands left of the dummy, slot 1 right
                tool.setPosition(Vector2f(x, windUp ? bodyTop : bodyTop + 1.0f));
                window.draw(tool);
            }
        }
        if (b.toolCharges > 0) {   // R41-F2: tempered pick: 2x2 steel pip beside the left foot, one column clear of the trait dot
            stripe.setSize(temperPipSize);
            stripe.setFillColor(temperPipColor);
            stripe.setPosition(Vector2f(base.x - 1.0f - temperPipSize.x, bodyTop + bodyH - temperPipSize.y));   // bobs with the body
            window.draw(stripe);
        }
        if (b.hasSpear && !b.chopping && !b.building) {   // R20-F3: spear carried upright-ish; stowed while working
            tool.setFillColor(spearShaftColor);
            Vector2f top;
            for (int k = 0; k < spearShaftPx; k++) {
                top = Vector2f(side + (float)(k / 2), bodyTop + spearShaftBottom - (float)k);
                tool.setPosition(top);
                window.draw(tool);
            }
            tool.setSize(spearTipSize);
            tool.setFillColor(spearTipColor);
            tool.setPosition(top - Vector2f(0.0f, spearTipSize.y));   // top edge = bodyTop - 2
            window.draw(tool);
            tool.setSize(barbToolSize);
            if (b.activity == actRaidAttack) {   // R52-F1: 1px red pennant right of the shaft's top pixel, under the tip
                dot.setFillColor(kRaidPennantColor);
                dot.setPosition(top + Vector2f(1.0f, 0.0f));
                window.draw(dot);
            }
        }
        if (lootLaden(b)) {   // R53-F1: 2px violet pack on the bottom body row, one column out from the spear's foot (the
            stripe.setSize(kLootBundleSize);   // shaft's side+1 pixels sit on rows 0-1, so this is clear for humans and elves)
            stripe.setFillColor(kLootBundleColor);
            stripe.setPosition(Vector2f(side + 1.0f, bodyTop + bodyH - kLootBundleSize.y));   // bobs with the body
            window.draw(stripe);
        }
        if (b.id == cartId) {   // R54-F1: 2x2 cart on the ground (no bob), one column clear of the arm/spear foot and the
            Vector2f cart(side + kTradeCartGapPx, feet - kTradeCartSize.y);   // health bar; coin pip on its top-right while laden
            stripe.setSize(kTradeCartSize);
            stripe.setFillColor(kTradeCartColor);
            stripe.setPosition(cart);
            window.draw(stripe);
            if (cartLaden) {
                dot.setFillColor(kTradeCoinColor);
                dot.setPosition(cart + Vector2f(kTradeCartSize.x - 1.0f, -1.0f));
                window.draw(dot);
            }
        }
        if (b.activity == actWatch)   // R51-F1: torch held out left at head height, clear of horns, ears and the trait dot; drawn after the overlay
            watchTorchPts.push_back(Vector2f(base.x - 2.0f, bodyTop - 1.0f));
        if (b.id == carrierId) {   // R45-F2: 1px relic held low, one column out from the arm/spear column, twinkling
            dot.setFillColor((frame / relicGlintFrames) % 2 == 0 ? relicGlintColor : relicGlintDimColor);
            dot.setPosition(Vector2f(side + 1.0f, bodyTop + bodyH - 1.0f));
            window.draw(dot);
        } else if (b.id == lootId) {   // R47-F5: Hollow loot, same 1px spot, pale silver twinkle
            dot.setFillColor((frame / relicGlintFrames) % 2 == 0 ? lootGlintColor : lootGlintDimColor);
            dot.setPosition(Vector2f(side + 1.0f, bodyTop + bodyH - 1.0f));
            window.draw(dot);
        }
        if (zoom >= healthBarMinZoom && b.health < barbMaxHealth) {   // R30-F5: 1px bar under the feet, only when hurt
            float barX = base.x + floor((bodySize.x - healthBarW) / 2.0f), barY = feet + healthBarGap;
            float fillW = min(healthBarW, ceil(healthBarW * max(b.health, 0) / barbMaxHealth));   // any hp left shows >= 1px
            stripe.setSize(Vector2f(healthBarW, 1.0f));
            stripe.setFillColor(healthBarBackColor);
            stripe.setPosition(Vector2f(barX, barY));
            window.draw(stripe);
            if (fillW > 0.0f) {
                stripe.setSize(Vector2f(fillW, 1.0f));
                stripe.setFillColor(healthBarFillColor);
                window.draw(stripe);
            }
        }
    }
    if (lookVerts.getVertexCount() > 0) window.draw(lookVerts);   // still inside the barb pass: under trader, trees and overlays
}

// Roofed houses: slab is drawn separately underneath. Window is dark glass here; drawHouseLights lights it at night.
// R36-F1: batched via drawParts. The race roof rows come from houseStyle (shared with the garrison retint and the hover
// outline), tinted with the race roof colour; the bottom roof row is one ramp step darker as an eave.
void drawHouses(RenderWindow& window, const vector<house>& houses, const vector<RectangleShape>& houseshapes) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        auto& hs = houseshapes[i];
        const houseStyle& st = houseStyles[houses[i].race];
        Vector2f o = snapPx(hs.getPosition());
        drawParts(va, houseShadowParts, o, Color::White);
        if (st.chimneySize.x > 0.0f) {                  // behind the roof so its base tucks in
            partDef chimney{st.chimneyPos, st.chimneySize, houseChimneyDwarfColor};
            drawParts(va, &chimney, 1, o, Color::White);
        }
        partDef roof[houseStyleMaxRows];
        for (int k = 0; k < st.rows; k++)
            roof[k] = {Vector2f(st.left[k], st.top + k), Vector2f(st.width[k], 1.0f), k == st.rows - 1 ? partRampMid : partRampLight};
        drawParts(va, roof, st.rows, o, st.roof);
        drawParts(va, partRange{st.bodyFirst, st.bodyCount}, o, Color::White);   // R41-F1: race walls/door/window
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// Lit windows, drawn after the night overlay (which leaves the default view set) so they read as lamplight.
// R41-F1: window rect, lamp and halo colour come from the house's race style.
void drawHouseLights(RenderWindow& window, const View& worldView, const vector<house>& houses, const vector<RectangleShape>& houseshapes) {
    window.setView(worldView);
    RectangleShape glow, lamp;
    const Vector2f halo(houseWindowGlowPx, houseWindowGlowPx);
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        const houseStyle& st = houseStyles[clampRace(houses[i].race)];
        Vector2f w = snapPx(houseshapes[i].getPosition()) + st.windowPos;
        glow.setSize(st.windowSize + 2.0f * halo);
        glow.setPosition(w - halo);
        glow.setFillColor(st.windowGlow);
        window.draw(glow, BlendAdd);
        lamp.setSize(st.windowSize);
        lamp.setPosition(w);
        lamp.setFillColor(st.windowNight);
        window.draw(lamp);
    }
}

// Game animals (deer, rabbit) from speciesDefs: legs, tail, body = hitbox, head, accent pixels. Facing and gait come from state + frame.
void drawDeer(RenderWindow& window, const vector<deer>& deers, const vector<RectangleShape>& deershape, int frame) {
    RectangleShape r;
    for (int i = 0; i < (int)deershape.size() && i < (int)deers.size(); i++) {
        const deer& d = deers[i];
        const speciesDef& sp = speciesOf(d);
        const RectangleShape& ds = deershape[i];
        Vector2f o = snapPx(ds.getPosition());
        float bw = ds.getSize().x, bh = ds.getSize().y;
        bool left = d.moving ? d.targetx < ds.getPosition().x : d.id % 2 == 1;
        Color body = ds.getFillColor();                  // keeps the red hit flash from deerkilling
        bool flashing = body != sp.body;

        int legPhase = ((frame + d.id * 4) / sp.legFrames) % 2;
        float rise = (sp.gait == gaitHop && d.moving && legPhase == 1) ? sp.hopPx : 0.0f; // mid-hop: sprite lifts
        o.y -= rise;
        // mirror a right-facing x offset (and width) across the body
        auto at = [&](Vector2f p, float w) { return o + Vector2f(left ? bw - p.x - w : p.x, p.y); };
        r.setFillColor(sp.leg);
        for (int k = 0; k < sp.legCount; k++) {
            float len = sp.legPx;
            if (sp.gait == gaitWalk && d.moving && (k % 2) == legPhase) len = sp.legLiftedPx; // diagonal pairs (0+2, 1+3) alternate
            if (sp.gait == gaitHop && k == 0) len += rise;  // back leg stays on the ground pushing off; front leg tucks
            r.setSize(Vector2f(1.0f, len));
            r.setPosition(at(Vector2f(sp.legX[k], bh), 1.0f));
            window.draw(r);
        }
        r.setSize(Vector2f(1.0f, 1.0f));
        r.setFillColor(sp.tail);
        for (int k = 0; k < sp.tailPixels; k++) {
            r.setPosition(at(sp.tailPos, 1.0f));
            window.draw(r);
        }
        r.setSize(Vector2f(bw, bh));
        r.setPosition(o);
        r.setFillColor(body);
        window.draw(r);
        r.setSize(sp.headSize);
        r.setPosition(at(sp.headPos, sp.headSize.x));
        r.setFillColor(flashing ? body : sp.head);
        window.draw(r);
        r.setSize(Vector2f(1.0f, 1.0f));
        r.setFillColor(sp.accent);
        for (int k = 0; k < sp.accentPixels; k++) {
            r.setPosition(at(sp.accentPos[k], 1.0f));
            window.draw(r);
        }
    }
}

// POL-F4: a race the trader can still serve: in range and with living members
bool traderRaceAlive(const vector<barb>& barbs, int r) {
    if (r < 0 || r >= raceCount) return false;
    for (auto& b : barbs) if (b.race == r) return true;
    return false;
}

// POL-F4: eligible race (off cooldown, alive, can pay) with the least wood, ties lowest index; -1 if none
int traderPickRace(const vector<barb>& barbs, bool food) {
    int best = -1;
    for (int r = 0; r < raceCount; r++) {
        if (raceCooldown[r] > 0 || !traderRaceAlive(barbs, r)) continue;
        int cost = tradeCostOf(r, food);   // R51-F4: cold-border markup
        bool pays = food ? stock[r].food >= tradeFoodReserve + cost : stock[r].stone >= cost;
        if (pays && (best < 0 || stock[r].wood < stock[best].wood)) best = r;
    }
    return best;
}

Vector2f traderGoal(const trader& t) {
    if (!t.pastGap) return t.gap;   // R47-F3: each leg first walks the gap between frontiers
    if (t.phase == trader::Leaving) return t.exit;
    return t.toStore ? storehousePos[t.race] + traderStoreOffset : villageCenter;
}

void traderTick(optional<trader>& tr, const vector<barb>& barbs, const storehouse (&stores)[raceCount]) {
    static int ttimer = 0;
    if (tradeMsgTimer > 0) tradeMsgTimer--;
    if (!tr) {
        if (++ttimer < traderIntervalTicks) return;
        ttimer = 0;
        trader t;
        t.race = traderPickRace(barbs, false);
        if (t.race < 0) { t.race = traderPickRace(barbs, true); t.buysFood = t.race >= 0; }
        for (int r = 0; r < raceCount; r++) if (raceCooldown[r] > 0) raceCooldown[r]--;
        if (t.race >= 0) raceCooldown[t.race] = traderRaceCooldownVisits;
        t.toStore = t.race >= 0 && stores[t.race].built;
        // R47-F3: enter and leave along a gap's bisector (one flanking the served race; any gap for the campfire), from
        // the map edge 10px out, through a waypoint traderGapRadius from the campfire, so the walk stays between frontiers
        int g = t.race >= 0 ? frontierGapOf[t.race][uniform_int_distribution<int>(0, 1)(gen)]
                            : uniform_int_distribution<int>(0, raceCount - 1)(gen);
        Vector2f dir(cos(frontierGapAngle[g]), sin(frontierGapAngle[g]));
        t.gap = villageCenter + dir * traderGapRadius;
        float reachX = dir.x > 0.0f ? (worldSize + 10.0f - villageCenter.x) / dir.x
                     : dir.x < 0.0f ? (-10.0f - villageCenter.x) / dir.x : numeric_limits<float>::max();
        float reachY = dir.y > 0.0f ? (worldSize + 10.0f - villageCenter.y) / dir.y
                     : dir.y < 0.0f ? (-10.0f - villageCenter.y) / dir.y : numeric_limits<float>::max();
        t.exit = villageCenter + dir * min(reachX, reachY);
        if (reachX < reachY) t.exit.y = clamp(t.exit.y, mapEdgeInset, worldSize - mapEdgeInset);   // keep off the corners
        else                 t.exit.x = clamp(t.exit.x, mapEdgeInset, worldSize - mapEdgeInset);
        t.shape.setSize(traderSize);
        t.shape.setFillColor(traderColor);
        t.shape.setPosition(t.exit);
        tr = t;
        const char* buys = t.buysFood ? "food" : "stone";
        if (t.race < 0) {
            printf("Trader arriving: no race can pay, heading to the campfire\n");
            logEvent("Trader arriving: nothing to buy, heads to the campfire");
        } else if (!t.toStore) {
            printf("Trader arriving for %s %s (no storehouse, campfire)\n", racePlurals[t.race], buys);
            logRaceEvent(string("Trader comes for ") + racePlurals[t.race] + "' " + buys + ", at the campfire", t.race);
        } else {
            printf("Trader arriving for %s %s\n", racePlurals[t.race], buys);
            logRaceEvent(string("Trader comes for ") + racePlurals[t.race] + "' " + buys, t.race);
        }
        return;
    }
    trader& t = *tr;
    // POL-F4: re-validate the served race and its storehouse every tick; fall back to the campfire if either is gone
    if (t.race >= 0 && !traderRaceAlive(barbs, t.race)) {
        if (t.phase == trader::Arriving) {
            printf("Trader: %s gone, heading to the campfire\n", racePlurals[t.race]);
            logEvent(string("Trader: ") + racePlurals[t.race] + " gone, heads to the campfire");
        }
        t.race = -1;
        t.buysFood = false;
        t.toStore = false;
    } else if (t.toStore && !stores[t.race].built) {   // only the walk in uses the crate; Trading/Leaving just drop it
        if (t.phase == trader::Arriving) {
            printf("Trader: %s storehouse gone, heading to the campfire\n", racePlurals[t.race]);
            logRaceEvent(string("Trader: ") + racePlurals[t.race] + "' storehouse gone, to the campfire", t.race);
        }
        t.toStore = false;
    }
    if (t.phase == trader::Trading) {
        if (++t.timer >= traderTradeTicks) {
            t.phase = trader::Leaving;
            t.pastGap = false;   // R47-F3: back out through the same gap
            printf("Trader leaving\n");
            logEvent("Trader leaving");
        }
        return;
    }
    Vector2f goal = traderGoal(t);
    Vector2f diff = goal - t.shape.getPosition();
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq < 1.0f) {
        if (!t.pastGap) {   // R47-F3: through the gap; head on to the crate/campfire (or the edge when leaving)
            t.pastGap = true;
            return;
        }
        if (t.phase == trader::Leaving) {
            tr.reset();
            return;
        }
        // POL-F4: deal only with the race picked at spawn (re-validated above), in stone or the food fallback
        int tr_r = t.race;
        int lots = 0, paid = 0;
        const char* what = t.buysFood ? "food" : "stone";
        if (tr_r >= 0) {
            raceStock& rs = stock[tr_r];
            int base = t.buysFood ? tradeFoodCost : tradeStoneCost;
            int cost = tradeCostOf(tr_r, t.buysFood);   // R51-F4: a cold pair raises the price per lot
            if (cost > base) {
                char toll[96];
                snprintf(toll, sizeof(toll), "Border tension: %s pay %d %s a lot (was %d)", racePlurals[tr_r], cost, what, base);
                printf("TRADE TOLL - %s x%.2f\n", racePlurals[tr_r], tradePriceMultOf(tr_r));
                logEvent(toll, kRelationTickerColdTint);
            }
            int avail = t.buysFood ? max(0, rs.food - tradeFoodReserve) : rs.stone;
            lots = min(avail / cost, tradeMaxLots + (hasStall ? stallExtraLots : 0));
            paid = lots * cost;
            (t.buysFood ? rs.food : rs.stone) -= paid;
            rs.wood += lots * tradeWoodGain;
        }
        stallBonusLots = max(0, lots - tradeMaxLots);
        if (lots > 0 && stallBonusLots > 0)
            snprintf(tradeMsg, sizeof(tradeMsg), "%s traded: -%d %s +%d wood (stall +%d)", racePlurals[tr_r], paid, what, lots * tradeWoodGain, stallBonusLots);
        else if (lots > 0) snprintf(tradeMsg, sizeof(tradeMsg), "%s traded: -%d %s +%d wood", racePlurals[tr_r], paid, what, lots * tradeWoodGain);
        else if (tr_r >= 0) snprintf(tradeMsg, sizeof(tradeMsg), "Trader: %s have no %s to sell", racePlurals[tr_r], what);
        else          snprintf(tradeMsg, sizeof(tradeMsg), "Trader: nothing to buy");
        tradeMsgTimer = tradeMsgTicks;
        if (lots > 0) logRaceEvent(tradeMsg, tr_r);   // POL-F2: tinted by the race that traded
        else          logEvent(tradeMsg);
        if (tr_r >= 0) printf("TRADE - %s, %d %s for %d wood - stone: %d food: %d wood: %d\n", racePlurals[tr_r], paid, what,
                              lots * tradeWoodGain, stock[tr_r].stone, stock[tr_r].food, stock[tr_r].wood);
        else printf("TRADE - nobody could pay\n");
        t.phase = trader::Trading;
        t.timer = 0;
    } else if (++t.stepTimer >= traderStepInterval) {
        t.stepTimer = 0;
        t.shape.move(dSq > 1.0f ? diff / sqrt(dSq) : diff);
    }
}

struct hoverHit {string label; optional<RectangleShape> rect; optional<CircleShape> circle;
                 int homeOfBarb = -1;     // hovered barb's home house id -> outline that house
                 int hoveredHouse = -1;   // hovered house id -> pip over each resident
                 int hoveredBarb = -1;    // INBOX-F2b: hovered barb id -> its order pip shows at any zoom
                 int hoveredCrate = -1;   // R47-F10: hovered crate's race -> faint build reach ring
                 optional<Color> tint;   // POL-F1: ledger outline + text colour (default tooltip colours when unset)
                 optional<Vector2f> targetPos;}; // R44-F1: hovered barb's work target centre (world px), unset if none

// R44-F1: centre of a barb's current work target, or nullopt (no target, wandering, or a hunted deer that is gone).
// A hunt reads the deer's live shape by id, index-checked against both parallel vectors; never a stored index.
optional<Vector2f> barbTargetPos(const barb& b, const RectangleShape& sq, const vector<deer>& deers, const vector<RectangleShape>& deershape) {
    if (b.deertargeting) {
        int k = findDeerById(deers, b.targetDeerId);
        if (k < 0 || k >= (int)deershape.size()) return nullopt;
        FloatRect db = deershape[k].getGlobalBounds();
        return db.position + db.size / 2.0f;
    }
    if (b.targetx < 0 || b.targety < 0) return nullopt;
    if (b.targetx == b.wanderX && b.targety == b.wanderY) return nullopt;   // a wander walk is not a job
    return Vector2f((float)b.targetx, (float)b.targety) + sq.getSize() / 2.0f;   // targetx/y is where the body's top-left goes
}

int weaponsOfRace(const vector<barb>& barbs, int r);   // POL-F3 (R47-F8: was spearsOfRace), defined with craftspear

// POL-F1: ledger string cached on the hover target (race index, raceCount = campfire) and the numbers it shows;
// the string is only rebuilt when either changes
struct ledgerCacheT { int target = -1; vector<int> key; string text; };
ledgerCacheT ledgerCache;

string ledgerLine(int target, const vector<barb>& barbs, const storehouse (&stores)[raceCount]) {
    int hungry[raceCount] = {}, spears[raceCount] = {};
    for (auto& b : barbs) if (isHungry(b)) hungry[clampRace(b.race)]++;
    for (int r = 0; r < raceCount; r++) spears[r] = weaponsOfRace(barbs, r);
    vector<int> races;   // storehouse: its race; campfire: every race still hauling there (no crate yet)
    if (target < raceCount) races.push_back(target);
    else for (int r = 0; r < raceCount; r++) if (!stores[r].built) races.push_back(r);
    vector<int> key{target < raceCount ? 0 : campfireWood};
    for (int r : races) key.insert(key.end(), {r, stock[r].wood, stock[r].stone, stock[r].food, hungry[r], spears[r], raceIngots[r], raceWear[r]});
    if (ledgerCache.target == target && ledgerCache.key == key) return ledgerCache.text;
    string s = target < raceCount ? "" : "Campfire: fire wood " + to_string(campfireWood);
    bool full = races.size() == 1;   // several races at the campfire: stock only, so the single line stays short
    for (int r : races) {
        if (!s.empty()) s += "; ";
        s += string(racePlurals[r]) + (full ? ": " : " ") + to_string(stock[r].wood) + "w " + to_string(stock[r].stone) + "s "
           + to_string(stock[r].food) + "f";
        if (full) s += ", " + to_string(hungry[r]) + " hungry, " + to_string(spears[r]) + " "
                       + (spears[r] == 1 ? weaponDefs[raceWeapon[r]].name : weaponDefs[raceWeapon[r]].plural);   // R47-F8: dwarves count axes
        if (full && raceIngots[r] > 0)   // R47-F7: shown from the first ingot
        {
            s += "\n" + to_string(raceIngots[r]) + (raceIngots[r] == 1 ? " ingot" : " ingots") + ", tool tier " + to_string(toolTierOf(r));
            int left = max(1, kWearJobs - raceWear[r]);   // R49-F1: read-only, jobs until the next ingot wears away
            s += ", wear in " + to_string(left) + (left == 1 ? " job" : " jobs");
        }
    }
    ledgerCache = {target, key, s};
    return s;
}

float rectDistSq(const FloatRect& r, Vector2f p) {
    float dx = max(max(r.position.x - p.x, p.x - (r.position.x + r.size.x)), 0.0f);
    float dy = max(max(r.position.y - p.y, p.y - (r.position.y + r.size.y)), 0.0f);
    return dx * dx + dy * dy;
}

// Fresh scan each frame, no stored selection. Returns the first hit: barbs, trader, wolves, deer, rocks, bushes, houses.
optional<hoverHit> pickHover(Vector2f p, float radius, const vector<barb>& barbs, const vector<RectangleShape>& squares,
                             const optional<trader>& tr, const vector<wolf>& wolves, const vector<deer>& deers, const vector<RectangleShape>& deershape,
                             const vector<rock>& rocks, const vector<CircleShape>& octs,
                             const vector<bush>& bushes, const vector<CircleShape>& bushshapes,
                             const vector<house>& houses, const vector<RectangleShape>& houseshapes, const fieldGrid& fields,
                             const storehouse (&stores)[raceCount]) {
    float rSq = radius * radius;
    auto near = [&](const Shape& s) { return rectDistSq(s.getGlobalBounds(), p) <= rSq; };
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++)
        if (!barbHidden(barbs[i]) && !barbUnderground(barbs[i]) && near(squares[i])) {   // INBOX-F1: sheltering barbs can't be hovered; R40-F1: miners below via their shaft
            const barb& b = barbs[i];
            string label = string(b.name) + ", " + traitOf(b).name + ", " + raceOf(b).name + ": " + barbTaskName(b) + (isStarving(b) ? " (starving)" : isHungry(b) ? " (hungry)" : "");
            if (b.hasSpear) label += ", " + weaponLabel(b);   // R47-F8: "spear, tier 2" / "axe, tier 0"
            if (b.drillCount > 0) label += ", " + to_string(b.drillCount) + " drills";   // R47-F6: the drill counter
            if (b.homeHouse >= 0) {   // kin counted here, on hover only
                int kin = 0;
                for (auto& o : barbs) if (o.id != b.id && o.homeHouse == b.homeHouse) kin++;
                label += ", of house " + to_string(b.homeHouse + 1) + ", " + to_string(kin) + " kin";
            }
            if (b.sick > 0) label += ", coughing";   // R52-F4
            if (b.activity == actCaravan && caravan.active) {   // R54-F1: where the cart goes and what it carries
                int dest = caravan.phase == caravanTrip::Home ? caravan.from : caravan.to;
                label += string("\nCaravan ") + (caravan.phase == caravanTrip::Home ? "home to the " : "to the ") + racePlurals[clampRace(dest)];
                if (b.id == caravanCartId() && caravanLaden()) label += ", cart: " + lootText(caravan.cargo);
            }
            if (isLeader(b) && raceNeed[b.race] != needNone) label += string("\nNeeds: ") + leaderNeedNames[raceNeed[b.race]];   // R30-F2
            if (b.order != needNone)   // INBOX-F2b: hours rounded up so a live order never reads 0h
                label += string("\nOrder: ") + leaderNeedNames[b.order] + " (" +
                         to_string((b.orderTicks + ticksPerGameHour - 1) / ticksPerGameHour) + "h left)";
            if (kDebugBuild && debugActivityTip) label += string("\nActivity: ") + activityDefs[b.activity].name;   // R53-F4
            hoverHit h{label, squares[i], nullopt};
            h.homeOfBarb = b.homeHouse;
            h.hoveredBarb = b.id;   // INBOX-F2b: order pip
            h.targetPos = barbTargetPos(b, squares[i], deers, deershape);   // R44-F1
            return h;
        }
    for (int r = 0; r < raceCount; r++) {   // R40-F1: a miner below ground is hovered through its shaft frame
        int m = shaftMinerOf(barbs, r);
        if (m < 0 || m >= (int)squares.size() || !barbUnderground(barbs[m])) continue;
        RectangleShape frame(shaftSize);
        frame.setPosition(shaftPos(r));
        if (!near(frame)) continue;
        hoverHit h{string(raceDefs[r].name) + " miner at work, seam " + to_string(mineShafts[r].seam) + "/" + to_string(kShaftCapacity),
                   frame, nullopt};
        h.homeOfBarb = barbs[m].homeHouse;
        h.hoveredBarb = barbs[m].id;
        return h;
    }
    if (tr && near(tr->shape)) {
        const char* ph = tr->phase == trader::Arriving ? "arriving" : tr->phase == trader::Trading ? "trading" : "leaving";
        string label = string("Trader: ") + ph;   // POL-F4: who it serves and what it buys
        if (tr->race >= 0) label += string(", for ") + racePlurals[tr->race] + ", buys " + (tr->buysFood ? "food" : "stone");
        else               label += ", buys nothing";
        return hoverHit{label, tr->shape, nullopt};
    }
    for (auto& w : wolves)
        if (near(w.shape)) return hoverHit{w.phase == wolf::Hunting ? "Wolf: hunting" : "Wolf: leaving", w.shape, nullopt};
    for (int i = 0; i < (int)deershape.size() && i < (int)deers.size(); i++)
        if (near(deershape[i])) return hoverHit{string(speciesOf(deers[i]).name) + " " + to_string(deers[i].health) + "hp", deershape[i], nullopt};
    for (int i = 0; i < (int)octs.size() && i < (int)rocks.size(); i++)
        if (near(octs[i])) return hoverHit{rocks[i].dying ? "Rock: being mined" : "Rock", nullopt, rockDrawShape(octs[i])};
    for (int i = 0; i < (int)bushshapes.size() && i < (int)bushes.size(); i++)
        if (near(bushshapes[i])) return hoverHit{bushes[i].ripe ? "Bush: ripe" : "Bush: bare", bushDrawShape(bushshapes[i]), nullopt};
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++)
        if (near(houseshapes[i])) {
            int sheltering = 0;   // INBOX-F1
            for (auto& b : barbs) if (barbHidden(b) && b.garrisonHouseId == houses[i].id) sheltering++;
            hoverHit h{"House " + to_string(houses[i].id + 1) + (sheltering > 0 ? ", " + to_string(sheltering) + " sheltering" : string()),
                       houseshapes[i], nullopt};
            h.hoveredHouse = houses[i].id;
            return h;
        }
    for (auto& c : camps) {   // R45-F1b
        RectangleShape body(campSize);
        body.setPosition(snapPx(c.pos));
        if (near(body)) return hoverHit{string(raceDefs[clampRace(c.race)].name) + " lean-to camp", body, nullopt};
    }
    for (int i = 0; i < (int)pois.size() && i < (int)poiShapes.size(); i++) {   // R47-F4
        RectangleShape body(poiShapes[i].getSize() + Vector2f(2 * ruinHoverPad, 2 * ruinHoverPad));
        body.setPosition(poiShapes[i].getPosition() - Vector2f(ruinHoverPad, ruinHoverPad));
        if (!near(body)) continue;
        string label = pois[i].kind == poiHut ? "Hermit hut" : "Sunken well";
        if (pois[i].spent) label += pois[i].kind == poiHut ? ": searched, abandoned" : ": searched, dry";
        else if (pois[i].found) label += ": found, not yet searched";
        return hoverHit{label, body, nullopt};
    }
    if (hollowPlaced) {   // R47-F5: the stair mouth
        RectangleShape mouth(hollowSize + Vector2f(2 * ruinHoverPad, 2 * ruinHoverPad));
        mouth.setPosition(hollowPos - Vector2f(ruinHoverPad, ruinHoverPad));
        if (near(mouth)) {
            string label = "The Hollow: stairs into the dark";
            if (party.active && party.hollow && party.phase == expeditionParty::Delve)
                label += "\n" + string(raceDefs[clampRace(party.race)].name) + " party below";
            label += hollowLootLeft > 0 ? "\nhauls left " + to_string(hollowLootLeft) + "/" + to_string(hollowLootCap) : string("\npicked clean");
            return hoverHit{label, mouth, nullopt};
        }
    }
    for (auto& st : ruinStones) {   // R45-F1b: any stone outlines that stone
        RectangleShape stone(st.size + Vector2f(2 * ruinHoverPad, 2 * ruinHoverPad));
        stone.setPosition(st.position - Vector2f(ruinHoverPad, ruinHoverPad));
        if (near(stone)) return hoverHit{"Ruins: old standing stones", stone, nullopt};
    }
    for (int r = 0; r < raceCount; r++) {   // POL-F1: a standing crate shows its race's ledger line
        if (!stores[r].built) continue;
        RectangleShape crate(storehouseSize);
        crate.setPosition(storehousePos[r]);
        if (!near(crate)) continue;
        hoverHit h{ledgerLine(r, barbs, stores), crate, nullopt};
        for (int o = 0; o < raceCount; o++)   // R54-F1: friendly tier and its decay, outside the cached ledger string
            if (pairFriendly(r, o)) {
                int hrs = (relFriendlyTicks[r][o] + ticksPerGameHour - 1) / ticksPerGameHour;   // rounded up: never reads 0h
                h.label += string("\nFriendly with the ") + racePlurals[o] + ", fades in " + to_string(hrs) + "h without trade";
            }
        h.tint = lensRaceColors[r];
        h.hoveredCrate = r;   // R47-F10
        return h;
    }
    {   // POL-F1: the campfire shows its shared fire wood plus any race still hauling there
        float cr = campfireStoneRadius + 1.0f;   // covers the 1px stone ring drawn outward
        CircleShape ring(cr);
        ring.setOrigin({cr, cr});
        ring.setPosition(villageCenter);
        if (near(ring)) {
            hoverHit h{ledgerLine(raceCount, barbs, stores), nullopt, ring};
            int only = -1, n = 0;   // one race still hauling here: its colour; none or several: the neutral pip colour
            for (int r = 0; r < raceCount; r++) if (!stores[r].built) { only = r; n++; }
            h.tint = n == 1 ? lensRaceColors[only] : ledgerPipColor;
            return h;
        }
    }
    auto plotState = [&](int r, int k) {   // R32-F1: one plot's state, for the per-plot list
        const plot& fp = fields[r][k];
        if (fp.built) return fp.fallow > 0 ? string("fallow") : fieldRipe(fp) ? string("ripe")
                                           : to_string(fp.growth * 100 / wheatGrowTicks()) + "% grown";
        if (fp.claimed) return string("being sown");
        if (k > 0 && !fp.placed && fp.fallback) return string("re-siting");   // R37-F1: slot given up, ring spot pending
        if (k > 0 && mills[r].placed && !fp.placed && !fp.reserved) return string("no room");
        if (k < plotsOpen[r]) return string("unsown");   // R46-F2a: already unlocked (by homes or population)
        int homes = fieldMinHouses * k + 1;   // R46-F2b: either gate of plotsUnlocked() opens it
        return "unlocks at " + to_string((k + 1) * popPerPlot) + " barbs or " + to_string(homes) + (homes == 1 ? " home" : " homes");
    };
    auto farmLine = [&](int r) {   // FARM-F2: "mill: built/none, plots n/cap" for race r; R32-F1: then one line per plot
        int sown = 0, pop = 0;
        for (auto& fp : fields[r]) if (fp.built) sown++;
        for (auto& b : barbs) if (b.race == r) pop++;   // R46-F2b: hover only, so no per-tick cost
        string s = string("mill: ") + (mills[r].built ? "built" : "none") + ", plots " + to_string(sown) + "/" + to_string(fieldsPerRace)
                 + "\n  pop " + to_string(pop) + " / plots " + to_string(plotsOpen[r]);
        for (int k = 0; k < fieldsPerRace; k++) s += "\n  plot " + to_string(k + 1) + ": " + plotState(r, k);
        return s;
    };
    for (int r = 0; r < raceCount; r++) {   // FARM-F2: a standing mill shows its race and the farmstead line
        if (!mills[r].built) continue;
        RectangleShape body(millBodySize);
        body.setPosition(snapPx(mills[r].pos));
        if (near(body)) return hoverHit{string(raceDefs[r].name) + " " + farmLine(r), body, nullopt};
    }
    for (int r = 0; r < raceCount; r++)   // FARM-F1: sown plots show their race and growth
        for (int k = 0; k < fieldsPerRace; k++) {
            const plot& fp = fields[r][k];
            if (!fp.built || !near(fp.shape)) continue;
            return hoverHit{string(raceDefs[r].name) + " wheat field " + to_string(k + 1) + ": " + plotState(r, k) + "\n" + farmLine(r),
                            fp.shape, nullopt};
        }
    return nullopt;
}

// UIO-F2: click pick, barbs only; nearest barb within radius (world px). Returns its id, or -1 for an empty click.
int pickBarbAt(Vector2f p, float radius, const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    float bestSq = radius * radius;
    int best = -1;
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        if (barbHidden(barbs[i])) continue;   // INBOX-F1
        FloatRect box = squares[i].getGlobalBounds();
        if (barbInHollow(barbs[i])) box = FloatRect(hollowPos, hollowSize);   // R47-F5: picked through the stair mouth
        else if (barbUnderground(barbs[i])) box = FloatRect(shaftPos(barbs[i].race), shaftSize);   // R40-F1: picked through its shaft
        float dSq = rectDistSq(box, p);
        if (dSq <= bestSq) { bestSq = dSq; best = barbs[i].id; }
    }
    return best;
}

// UIO-F2: thin ring around the selected barb, drawn above the overlays (like the hover outline) so it reads at night
void drawSelectRing(RenderWindow& window, const View& worldView, const RectangleShape& square, float zoom) {
    window.setView(worldView);
    CircleShape ring(selectRingRadius, 24);
    ring.setOrigin({selectRingRadius, selectRingRadius});
    ring.setPosition(square.getPosition() + square.getSize() / 2.0f);
    ring.setFillColor(Color::Transparent);
    ring.setOutlineThickness(selectRingPx / zoom);
    ring.setOutlineColor(selectRingColor);
    window.draw(ring);
}

// UIO-F2: compact inspect panel, top-right just under the HUD strip and always above the minimap.
// Rows: name, race, job (red "Starving" when hungry), then Hunger and Health bars via drawBar. Returns the panel rect for clicks/hover.
FloatRect drawInspectPanel(RenderWindow& window, const Font& font, const barb& b, const FloatRect& mmRect) {
    window.setView(window.getDefaultView());
    Vector2f win = window.getDefaultView().getSize();
    bool hungry = isHungry(b);   // read once: drives both the job line and the hunger bar colour
    Text name(font, b.name, inspectFontSize);
    Text race(font, string("Race: ") + raceOf(b).name, inspectFontSize);
    Text job(font, hungry ? string("Starving") : string("Job: ") + barbTaskName(b), inspectFontSize);
    Text hungerL(font, "Hunger", inspectFontSize);
    Text healthL(font, "Health", inspectFontSize);
    Text ref(font, "Ag", inspectFontSize);   // one ascender+descender reference so every row shares a baseline and height
    FloatRect rb = ref.getLocalBounds();
    float rowH = max(rb.size.y, barSize.y + 2 * barFramePx);
    float labelW = max(hungerL.getLocalBounds().size.x, healthL.getLocalBounds().size.x);
    float barRowW = labelW + inspectLabelGap + barSize.x + 2 * barFramePx;
    float innerW = max({name.getLocalBounds().size.x, race.getLocalBounds().size.x, job.getLocalBounds().size.x, barRowW,
                        inspectMinWidth - 2 * inspectPad.x});
    const int rows = 5;
    Vector2f size(round(innerW + 2 * inspectPad.x), round(rows * rowH + (rows - 1) * inspectRowGap + 2 * inspectPad.y));
    Vector2f pos(round(win.x - inspectMargin - size.x), round(hudBarHeight + inspectMargin));
    if (mmRect.size.x > 0 && pos.y + size.y > mmRect.position.y - inspectMargin) return FloatRect();   // window too short: never cover the minimap

    RectangleShape panel(size);
    panel.setPosition(pos);
    panel.setFillColor(inspectPanelColor);
    panel.setOutlineThickness(-1.0f);   // inward, so the panel stays exactly size
    panel.setOutlineColor(inspectBorderColor);
    window.draw(panel);

    float x = pos.x + inspectPad.x;
    auto rowY = [&](int r) { return pos.y + inspectPad.y + r * (rowH + inspectRowGap); };
    auto put = [&](Text& t, float tx, int r, Color c) {
        t.setFillColor(c);
        t.setPosition(Vector2f(round(tx - t.getLocalBounds().position.x), round(rowY(r) - rb.position.y)));
        window.draw(t);
    };
    put(name, x, 0, inspectNameColor);
    put(race, x, 1, inspectTextColor);
    put(job, x, 2, hungry ? uiStarvingRed : inspectTextColor);
    put(hungerL, x, 3, inspectTextColor);
    put(healthL, x, 4, inspectTextColor);
    // bars right-aligned in the panel, vertically centred on their label rows; pos passed to drawBar is the fill's top-left
    float barX = pos.x + size.x - inspectPad.x - barFramePx - barSize.x;
    float barDy = (rowH - barSize.y) / 2.0f;
    float hungerFrac = (float)b.hunger / max(1, hungerThresholdOf(b));
    drawBar(window, Vector2f(barX, rowY(3) + barDy), hungerFrac, hungry ? uiStarvingRed : inspectHungerFill);
    drawBar(window, Vector2f(barX, rowY(4) + barDy), (float)b.health / barbMaxHealth, inspectHealthFill);
    return FloatRect(pos, size);
}

// INBOX-F2b: 2x1 job pip under an ordered barb's feet, in the task colour of the ordered job (same colours as the work
// stripe). Every ordered barb at orderPipMinZoom and above; below that only the hovered one. Drawn after the overlays like
// the ballot pips so the colour reads true; indoor or sheltering barbs show none.
void drawOrderPips(RenderWindow& window, const View& worldView, float zoom, int hoveredBarb,
                   const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    bool all = zoom >= orderPipMinZoom;
    if (!all && hoveredBarb < 0) return;
    window.setView(worldView);
    const Color pipColors[needCount] = {barbChopColor, barbMineColor, barbPickColor};   // indexed like leaderNeed
    RectangleShape pip(orderPipSize);
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        const barb& b = barbs[i];
        if (b.order == needNone || !barbAvailable(b) || barbUnderground(b)) continue;   // R40-F1: no pip over an empty stand spot
        if (!all && b.id != hoveredBarb) continue;
        Vector2f base = snapPx(squares[i].getPosition());
        float feet = base.y + squares[i].getSize().y;
        bool barShown = zoom >= healthBarMinZoom && b.health < barbMaxHealth;   // same test as drawBarbs: sit one row under it
        float y = feet + healthBarGap + (barShown ? 1.0f + healthBarGap : 0.0f);
        pip.setFillColor(pipColors[b.order]);
        pip.setPosition(Vector2f(base.x + floor((raceOf(b).bodySize.x - orderPipSize.x) / 2.0f), y));
        window.draw(pip);
    }
}

// Hover-only household marks, drawn after the overlays like the hover outline
void drawHousehold(RenderWindow& window, const View& worldView, const hoverHit& h, float zoom,
                   const vector<barb>& barbs, const vector<RectangleShape>& squares,
                   const vector<house>& houses, const vector<RectangleShape>& houseshapes) {
    window.setView(worldView);
    if (h.hoveredHouse >= 0) {
        RectangleShape pip(householdPipSize);
        pip.setFillColor(householdColor);
        for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
            if (barbs[i].homeHouse != h.hoveredHouse || barbHidden(barbs[i]) || barbUnderground(barbs[i])) continue;   // INBOX-F1: no pip over an indoor barb; R40-F1: or one down the shaft
            Vector2f top = snapPx(squares[i].getPosition());
            float cx = top.x + round((squares[i].getSize().x - householdPipSize.x) / 2.0f);
            pip.setPosition(Vector2f(cx, top.y - householdPipGap - householdPipSize.y));
            window.draw(pip);
        }
    }
    if (h.homeOfBarb >= 0) {
        for (int i = 0; i < (int)houses.size() && i < (int)houseshapes.size(); i++) {
            if (houses[i].id != h.homeOfBarb) continue;
            // outline the whole drawn sprite (stepped roof down to the walls), not just the 4x4 logic shape
            Vector2f o = snapPx(houseshapes[i].getPosition());
            const houseStyle& st = houseStyles[houses[i].race];
            float top = st.top;
            if (st.chimneySize.x > 0.0f) top = min(top, st.chimneyPos.y);
            Vector2f tl(o.x + st.left[st.rows - 1], o.y + top);
            Vector2f br(o.x + st.left[st.rows - 1] + st.width[st.rows - 1], o.y + houseWallPos.y + houseWallSize.y);
            RectangleShape r(br - tl);
            r.setPosition(tl);
            r.setFillColor(Color::Transparent);
            r.setOutlineThickness(tooltipOutlinePx / zoom);
            r.setOutlineColor(householdColor);
            window.draw(r);
            break;
        }
    }
}

// R44-F1: faint dotted line from a barb's centre to its work target plus a pulsing pale-yellow ring on the target.
// Drawn above the overlays (like the hover outline) and under the HUD; the line is skipped when the target is very close.
void drawTargetLine(RenderWindow& window, const View& worldView, Vector2f from, Vector2f to, float zoom, int frame) {
    window.setView(worldView);
    Vector2f d = to - from;
    float len = sqrt(d.x * d.x + d.y * d.y);
    if (len >= targetLineMinDist) {
        float dash = targetDashPx / zoom, step = (targetDashPx + targetGapPx) / zoom;
        if (len / step > targetMaxDashes) { step = len / targetMaxDashes; dash = step / 2.0f; }
        Vector2f u = d / len;
        Color lc = targetLineColor;
        lc.a = targetLineAlpha;
        VertexArray va(PrimitiveType::Lines);
        for (float s = 0.0f; s < len; s += step) {
            va.append(Vertex{from + u * s, lc});
            va.append(Vertex{from + u * min(s + dash, len), lc});
        }
        window.draw(va);
    }
    float wave = (sin(frame * targetRingPulseSpeed) + 1.0f) / 2.0f;   // 0..1
    float rad = targetRingRadius + targetRingPulse * wave;
    Color rc = targetRingColor;
    rc.a = (uint8_t)(targetRingAlphaLo + (targetRingAlphaHi - targetRingAlphaLo) * wave);
    CircleShape ring(rad, 20);
    ring.setOrigin({rad, rad});
    ring.setPosition(to);
    ring.setFillColor(Color::Transparent);
    ring.setOutlineThickness(selectRingPx / zoom);
    ring.setOutlineColor(rc);
    window.draw(ring);
}

// R44-F2: four corner brackets around box (world rect) appended to va as triangles. Each corner is an L of two
// hairline bars drawn inward from the padded box's corner.
void appendBeacon(VertexArray& va, FloatRect box, float line, Color c) {
    Vector2f tl = box.position - Vector2f(beaconPadPx, beaconPadPx);
    Vector2f sz = box.size + Vector2f(2 * beaconPadPx, 2 * beaconPadPx);
    float armX = min(beaconArmPx, sz.x / 2.0f), armY = min(beaconArmPx, sz.y / 2.0f);
    auto bar = [&](Vector2f p, Vector2f s) {
        Vector2f a = p, b = p + Vector2f(s.x, 0), d = p + Vector2f(0, s.y), e = p + s;
        va.append(Vertex{a, c}); va.append(Vertex{b, c}); va.append(Vertex{d, c});
        va.append(Vertex{b, c}); va.append(Vertex{e, c}); va.append(Vertex{d, c});
    };
    for (int cx = 0; cx < 2; cx++)
        for (int cy = 0; cy < 2; cy++) {
            Vector2f corner = tl + Vector2f(cx * sz.x, cy * sz.y);
            float hx = cx ? corner.x - armX : corner.x;              // horizontal arm grows inward
            float vy = cy ? corner.y - armY : corner.y;              // vertical arm grows inward
            float lx = cx ? corner.x - line : corner.x;              // bars sit inside the padded box edge
            float ly = cy ? corner.y - line : corner.y;
            bar(Vector2f(hx, ly), Vector2f(armX, line));
            bar(Vector2f(lx, vy), Vector2f(line, armY));
        }
}

// R44-F2: while H is held, bracket idle barbs, ripe wheat plots and ripe bushes inside the view. One VertexArray,
// one draw, above the overlays. Only barbs, plots and bushes are looped (never trees); capped at beaconMaxCount.
void drawBeacons(RenderWindow& window, const View& worldView, float zoom, int frame,
                 const vector<barb>& barbs, const vector<RectangleShape>& squares, const fieldGrid& fields,
                 const vector<bush>& bushes, const vector<CircleShape>& bushshapes) {
    window.setView(worldView);
    float margin = beaconPadPx + beaconArmPx;
    FloatRect view(worldView.getCenter() - worldView.getSize() / 2.0f - Vector2f(margin, margin),
                   worldView.getSize() + Vector2f(2 * margin, 2 * margin));
    float wave = (sin(frame * beaconPulseSpeed) + 1.0f) / 2.0f;   // 0..1
    float alpha = beaconAlphaLo + (beaconAlphaHi - beaconAlphaLo) * wave;
    if (zoom < beaconFadeZoom)   // far zoom: dim so a crowd of brackets stays sparse-looking
        alpha *= beaconFarAlphaMult + (1.0f - beaconFarAlphaMult) * (zoom - minZoom) / (beaconFadeZoom - minZoom);
    Color c = beaconColor;
    c.a = (uint8_t)clamp(alpha, 0.0f, 255.0f);
    float line = beaconLinePx / zoom;
    VertexArray va(PrimitiveType::Triangles);
    int count = 0;
    for (int i = 0; i < (int)barbs.size() && i < (int)squares.size() && count < beaconMaxCount; i++) {
        const barb& b = barbs[i];
        if (!barbIdle(b) || !barbAvailable(b) || barbHidden(b) || barbUnderground(b)) continue;   // R45-F2: a party member is not jobless
        FloatRect box(squares[i].getPosition(), squares[i].getSize());
        if (!view.findIntersection(box)) continue;
        appendBeacon(va, box, line, c);
        count++;
    }
    for (int r = 0; r < raceCount && count < beaconMaxCount; r++)
        for (int k = 0; k < fieldsPerRace && count < beaconMaxCount; k++) {
            const plot& fp = fields[r][k];
            if (!fieldRipe(fp) || !view.findIntersection(fp.rect)) continue;
            appendBeacon(va, fp.rect, line, c);
            count++;
        }
    for (int i = 0; i < (int)bushes.size() && i < (int)bushshapes.size() && count < beaconMaxCount; i++) {
        if (!bushes[i].ripe || bushes[i].picking) continue;
        float d = 2 * bushshapes[i].getRadius();
        FloatRect box(snapPx(bushshapes[i].getPosition()), Vector2f(d, d));   // same box bushDrawShape() fills
        if (!view.findIntersection(box)) continue;
        appendBeacon(va, box, line, c);
        count++;
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// Outline-only copy of the hit shape, drawn above the overlays so it stays readable at night
void drawHoverOutline(RenderWindow& window, const View& worldView, hoverHit& h, float zoom) {
    window.setView(worldView);
    float th = (h.tint ? ledgerOutlinePx : tooltipOutlinePx) / zoom;   // POL-F1: ledger hits use the race tint
    Color oc = h.tint.value_or(tooltipOutlineColor);
    if (h.rect) {
        h.rect->setFillColor(Color::Transparent);
        h.rect->setOutlineThickness(th);
        h.rect->setOutlineColor(oc);
        drawSnapped(window, *h.rect);
    }
    if (h.circle) {
        h.circle->setFillColor(Color::Transparent);
        h.circle->setOutlineThickness(th);
        h.circle->setOutlineColor(oc);
        drawSnapped(window, *h.circle);
    }
}

// Places the label beside the cursor, flipping left/above to stay on screen and off the HUD and ticker
void drawTooltip(RenderWindow& window, const Font& font, const string& label, Vector2f mouse, const FloatRect& hudRect, const FloatRect& tickerRect,
                 Color textColor = tooltipTextColor) {
    window.setView(window.getDefaultView());
    Vector2f win = window.getDefaultView().getSize();
    Text t(font, label, tooltipFontSize);
    FloatRect lb = t.getLocalBounds();
    Vector2f size(lb.size.x + 2 * tooltipPad.x, lb.size.y + 2 * tooltipPad.y);
    float right = mouse.x + tooltipOffset.x, left = mouse.x - tooltipFlipGap - size.x;
    float below = mouse.y + tooltipOffset.y, above = mouse.y - tooltipFlipGap - size.y;
    Vector2f cands[4] = {{right, below}, {left, below}, {right, above}, {left, above}};
    Vector2f pos(clamp(right, tooltipEdgeMargin, max(tooltipEdgeMargin, win.x - tooltipEdgeMargin - size.x)),
                 clamp(below, tooltipEdgeMargin, max(tooltipEdgeMargin, win.y - tooltipEdgeMargin - size.y)));
    for (auto& c : cands) {
        FloatRect box(c, size);
        bool inside = c.x >= tooltipEdgeMargin && c.y >= tooltipEdgeMargin
                   && c.x + size.x <= win.x - tooltipEdgeMargin && c.y + size.y <= win.y - tooltipEdgeMargin;
        bool hitsHud = hudRect.size.x > 0 && box.findIntersection(hudRect).has_value();
        bool hitsTicker = tickerRect.size.x > 0 && box.findIntersection(tickerRect).has_value();
        if (inside && !hitsHud && !hitsTicker) { pos = c; break; }
    }
    pos = Vector2f(round(pos.x), round(pos.y));
    RectangleShape panel(size);
    panel.setPosition(pos);
    panel.setFillColor(tooltipPanelColor);
    window.draw(panel);
    t.setFillColor(textColor);
    t.setPosition(pos + tooltipPad - lb.position);
    window.draw(t);
}

// Everything the HUD shows, gathered once per frame in main
struct hudValues {
    raceStock stock[raceCount];   // RACE-F1: per-race stockpiles (rows in drawStockPanel)
    int members[raceCount];       // RACE-F1: living barbs per race; an empty race never shows red or announces
    int campfireWood;             // RACE-F1: the shared campfire pool
    int hungry, people, houses, hour, speed;
    int gentry, commoners;   // R24-F1: founders vs house-born
    bool night, paused, raining;
    const char* trader;      // Away / Arriving / Trading / Leaving
    const char* tradeLine;   // last trade message, "" when expired
    const char* era;         // settlement rank: Camp / Hamlet / Town
    bool learned[techCount]; // know-how pips (R20-F1)
    int pulse[techCount];    // frames of white pulse left per pip
    int gold;                // R46-F1a: global nuggets
    bool goldFound;          // R46-F1a: gold cell hidden until the first nugget
    int day;                 // R51-F3: game_day; the season comes from seasonOf()
};

// Draws one table icon at pos (top-left, screen px), one quad per filled cell
void drawIcon(RenderWindow& window, hudIconId id, Vector2f pos, float px = hudIconPx,   // px: screen px per icon cell
              const Color* keyOverride = nullptr) {   // R51-F3: 4 colours replacing the table key (season icon)
    const iconDef& d = hudIcons[id];
    const Color* key = keyOverride ? keyOverride : d.key;
    VertexArray va(PrimitiveType::Triangles);
    pos = snapPx(pos);
    for (int y = 0; y < hudIconGrid; y++)
        for (int x = 0; x < hudIconGrid && d.rows[y][x]; x++) {
            char ch = d.rows[y][x];
            if (ch < 'a' || ch > 'd') continue;
            pushQuad(va, pos + Vector2f(x * px, y * px), Vector2f(px, px), key[ch - 'a']);
        }
    window.draw(va);
}

// RACE-F1: the one shortage test, shared by the HUD's red numbers and the shortage ticker line
int stockValue(const raceStock& s, int need) { return need == needWood ? s.wood : need == needStone ? s.stone : s.food; }
bool stockShort(const raceStock& s, int need) { return stockValue(s, need) < stockShortThreshold; }

// The old full text block, now shown as a tooltip while hovering the strip
string hudDetailText(const hudValues& v) {
    char buf[380];
    snprintf(buf, sizeof(buf), "Settlement: %s\nHungry: %d\nPopulation: %d (Gentry %d, Commoners %d)\nHouses: %d\nTime: %02d:00 (%s)\nSpeed: %dx%s\nWeather: %s\nTrader: %s%s%s",
             v.era, v.hungry, v.people, v.gentry, v.commoners, v.houses, v.hour, v.night ? "Night" : "Day", v.speed, v.paused ? " PAUSED" : "",
             v.raining ? "Rain" : "Clear", v.trader, v.tradeLine[0] ? "\n" : "", v.tradeLine);
    string s = buf;
    s += string("\nSeason: ") + seasonDefs[seasonOf(v.day)].name + " (day " + to_string(dayInSeason(v.day) + 1) + " of "
         + to_string(daysPerSeason) + ", year " + to_string(v.day / yearDays + 1) + ")"
         + (seasonOf(v.day) == seasonWinter ? " - hunger grows faster" : "");   // R51-F3
    s += "\nStockpiles (wood/stone/food):";   // RACE-F1
    for (int r = 0; r < raceCount; r++)
        s += string("\n  ") + racePlurals[r] + ": " + to_string(v.stock[r].wood) + " / " + to_string(v.stock[r].stone) + " / " + to_string(v.stock[r].food);
    s += "\n  Campfire wood: " + to_string(v.campfireWood);
    if (veil.faithSeen) {   // R51-F2: the stock panel's faith tick, in words
        s += currentDecree == decreeRite ? "\nFaith of the Veil (Rite day):" : "\nFaith of the Veil:";   // R52-F3: why the tick is dim
        for (int r = 0; r < raceCount; r++)
            s += string("\n  ") + racePlurals[r] + ": " + to_string((int)round(veil.faith[r] * 100.0f)) + "%"
                 + (veil.faithTier[r] == 2 ? " (high)" : veil.faithTier[r] == 0 ? " (low)" : "");
    }
    s += "\nKnow-how:";
    for (int t = 0; t < techCount; t++)   // learned techs, or progress toward each
        s += string("\n  ") + techDefs[t].name + ": " + (v.learned[t] ? "learned"
             : to_string(min(know.count[t], techDefs[t].threshold)) + "/" + to_string(techDefs[t].threshold));
    return s;
}

// Full-width top strip of icon + number cells; returns the strip rect for the hover/tooltip checks
FloatRect drawHud(RenderWindow& window, const Font& font, const hudValues& v) {
    window.setView(window.getDefaultView());
    Vector2f win = window.getDefaultView().getSize();
    FloatRect bar({0.0f, 0.0f}, {win.x, hudBarHeight});
    RectangleShape r(bar.size);
    r.setFillColor(pal.hudBar);
    window.draw(r);
    r.setSize({win.x, hudRimPx});
    r.setPosition({0.0f, hudBarHeight - hudRimPx});
    r.setFillColor(pal.hudRim);
    window.draw(r);

    char hour[8], speed[8];
    snprintf(hour, sizeof(hour), "%02d:00", v.hour);
    snprintf(speed, sizeof(speed), "%dx", v.speed);
    // R51-F3: key = icon colour override; minW = reserved text width (the season cell reserves its widest name)
    struct cell { hudIconId icon; string text; Color color; const Color* key = nullptr; float minW = 0.0f; };
    const seasonDef& sd = seasonDefs[seasonOf(v.day)];
    float seasonW = 0.0f;
    for (int k = 0; k < seasonCount; k++) seasonW = max(seasonW, Text(font, seasonDefs[k].name, hudFontSize).getLocalBounds().size.x);
    vector<cell> cells = {   // RACE-F1: wood/stone/food moved to the per-race stock panel under the strip
        {iconPeople, to_string(v.people), pal.hudText},
        {iconHouse,  to_string(v.houses), pal.hudText},
        {v.night ? iconMoon : iconSun, hour, pal.hudText},
        {iconSeason, sd.name, pal.hudText, sd.iconKey, seasonW},   // R51-F3: leaf icon in the season's colours
        {v.paused ? iconPause : iconSpeed, v.paused ? string("Paused") : string(speed), v.paused ? pal.hudAlert : pal.hudText},
    };
    if (v.goldFound) cells.push_back({iconCount, "gold " + to_string(v.gold), hudGoldColor}); // R46-F1a: text only (iconCount = no icon)
    if (v.raining) cells.push_back({iconRain, "Rain", pal.hudText}); // last, so it never shifts the other cells

    // Centre on a fixed reference string so every number sits on the same baseline
    Text ref(font, hudMinNumber, hudFontSize);
    FloatRect rb = ref.getLocalBounds();
    float inner = hudBarHeight - hudRimPx;
    float textY = round((inner - rb.size.y) / 2 - rb.position.y);
    float iconY = round((inner - hudIconSize) / 2);
    float x = hudPadX;
    for (auto& c : cells) {
        if (c.icon != iconCount) {
            drawIcon(window, c.icon, {x, iconY}, hudIconPx, c.key);
            x += hudIconSize + hudIconGap;
        }
        Text t(font, c.text, hudFontSize);
        FloatRect tb = t.getLocalBounds();
        t.setFillColor(c.color);
        t.setPosition({round(x - tb.position.x), textY});
        window.draw(t);
        x += max(max(tb.size.x, rb.size.x), c.minW) + hudCellGap;
    }

    // Settlement rank as plain text, right-aligned on the shared baseline; skipped if the cells ever reach that far
    Text era(font, v.era, hudFontSize);
    FloatRect eb = era.getLocalBounds();
    float eraX = round(win.x - hudPadX - eb.size.x);
    if (eraX >= x) {
        era.setFillColor(pal.hudText);
        era.setPosition({eraX - eb.position.x, textY});
        window.draw(era);
    }

    // Know-how pips (R20-F1), right-aligned just left of the era text so the rain cell never moves them.
    // Unlearned: hollow dim rim. Learned: solid tech colour. Newly learned: flashes white, fading over ~1s.
    float pipsW = techCount * knowPipPx + (techCount - 1) * knowPipGap;
    float pipX = round(eraX - knowPipEraGap - pipsW);
    float pipY = round((inner - knowPipPx) / 2);
    if (eraX >= x && pipX >= x) {
        RectangleShape pip({knowPipPx, knowPipPx});
        for (int t = 0; t < techCount; t++) {
            pip.setPosition({pipX + t * (knowPipPx + knowPipGap), pipY});
            if (!v.learned[t]) {
                pip.setSize({knowPipPx - 2 * knowPipRimPx, knowPipPx - 2 * knowPipRimPx});
                pip.setPosition(pip.getPosition() + Vector2f(knowPipRimPx, knowPipRimPx));
                pip.setFillColor(Color::Transparent);
                pip.setOutlineThickness(knowPipRimPx);   // outline grows outward: 6x6 overall
                pip.setOutlineColor(knowPipEmptyColor);
            } else {
                Color c = techDefs[t].pip;
                if (v.pulse[t] > 0) {
                    int age = knowPulseFrames - v.pulse[t];
                    float fade = (float)v.pulse[t] / knowPulseFrames;                         // 1 -> 0 over the pulse
                    float wave = 0.5f + 0.5f * cos(2.0f * 3.14159265f * age / knowPulsePeriod); // 1 at each flash
                    float w = fade * wave;
                    c.r = (uint8_t)(c.r + (255 - c.r) * w);
                    c.g = (uint8_t)(c.g + (255 - c.g) * w);
                    c.b = (uint8_t)(c.b + (255 - c.b) * w);
                }
                pip.setSize({knowPipPx, knowPipPx});
                pip.setFillColor(c);
                pip.setOutlineThickness(0.0f);
            }
            window.draw(pip);
        }
    }
    return bar;
}

// UIO-F1: the minimap's outer rect in default-view (UI) coords; used for drawing, click tests and hover suppression
FloatRect minimapRect(const RenderWindow& window) {
    Vector2f ui = window.getDefaultView().getSize();
    return FloatRect({floor(ui.x - MM_MARGIN - MM_SIZE), floor(ui.y - MM_MARGIN - MM_SIZE)}, {MM_SIZE, MM_SIZE});
}
float minimapScale() { return (MM_SIZE - 2 * MM_BORDER_PX) / worldSize; }   // screen px per world px
Vector2f minimapToWorld(const RenderWindow& window, Vector2f ui) {
    Vector2f inner = minimapRect(window).position + Vector2f(MM_BORDER_PX, MM_BORDER_PX);
    return (ui - inner) / minimapScale();
}

void minimapDot(VertexArray& va, Vector2f inner, Vector2f world, float px, Color c) {
    Vector2f p = inner + world * minimapScale() - Vector2f(px / 2, px / 2);
    p = Vector2f(floor(p.x), floor(p.y));   // whole pixels so 1px dots stay crisp
    float lo = MM_SIZE - 2 * MM_BORDER_PX - px;   // keep every dot inside the border
    p.x = clamp(p.x, inner.x, inner.x + lo);
    p.y = clamp(p.y, inner.y, inner.y + lo);
    Vector2f q[4] = {p, p + Vector2f(px, 0.0f), p + Vector2f(px, px), p + Vector2f(0.0f, px)};
    for (int k : {0, 1, 2, 0, 2, 3}) va.append(Vertex{q[k], c});
}

// UIO-F1: one pass each over houses, storehouses and barbs into a single vertex array (one draw); never touches trees.
// Layers: backing + border, houses (2px, crate colour), storehouses (3px, lighter crate colour), barbs (1px race colour, red when starving),
// then the camera viewport rect on top.
void drawMinimap(RenderWindow& window, VertexArray& va, const View& worldView,
                 const vector<barb>& barbs, const vector<RectangleShape>& squares,
                 const vector<house>& houses, const vector<RectangleShape>& houseshapes,
                 const storehouse (&stores)[raceCount]) {
    window.setView(window.getDefaultView());
    FloatRect r = minimapRect(window);
    RectangleShape back(r.size);
    back.setPosition(r.position);
    back.setFillColor(MM_BG);
    back.setOutlineThickness(-MM_BORDER_PX);
    back.setOutlineColor(MM_BORDER);
    window.draw(back);

    Vector2f inner = r.position + Vector2f(MM_BORDER_PX, MM_BORDER_PX);
    va.clear();
    // R47-F3: each race's frontier as a faint dotted circle in its lifted crate colour, under every other dot
    float frontierPx = frontierRadius * minimapScale();
    int frontierDots = max(8, (int)round(6.2831853f * frontierPx / frontierMinimapDotGap));
    for (int k = 0; k < raceCount; k++) {
        Color col = MM_STORE_COLORS[k];
        col.a = frontierMinimapAlpha;
        for (int i = 0; i < frontierDots; i++) {
            float a = 6.2831853f * i / frontierDots;
            minimapDot(va, inner, quarterCentre(k) + Vector2f(cos(a), sin(a)) * frontierRadius, 1.0f, col);
        }
    }
    // R51-F4: one border segment per race pair, midway between the two crates and across the line joining them, over the
    // faint frontier dots: 2px thick and near-opaque, slate blue at neutral easing to brick red as the pair turns cold
    for (int a = 0; a < raceCount; a++)
        for (int b = a + 1; b < raceCount; b++) {
            Vector2f ca = quarterCentre(a), cb = quarterCentre(b), d = cb - ca;
            float len = sqrt(d.x * d.x + d.y * d.y);
            if (len < 1.0f) continue;
            Vector2f across(-d.y / len, d.x / len), mid = (ca + cb) / 2.0f;
            float t = clamp(-raceRelation[a][b] / -kRelationMin, 0.0f, 1.0f);   // 0 neutral (or better) .. 1 at the floor
            auto mix = [t](uint8_t x, uint8_t y) { return (uint8_t)round(x + (y - x) * t); };
            Color col(mix(kRelationNeutralColor.r, kRelationColdColor.r), mix(kRelationNeutralColor.g, kRelationColdColor.g),
                      mix(kRelationNeutralColor.b, kRelationColdColor.b), mix(kRelationNeutralColor.a, kRelationColdColor.a));
            float halfWorld = kRelationMinimapLenPx / 2.0f / minimapScale();
            int steps = max(2, (int)round(kRelationMinimapLenPx));   // ~1 minimap px apart so the 2px dots form a solid bar
            for (int i = 0; i <= steps; i++)
                minimapDot(va, inner, mid + across * (-halfWorld + 2.0f * halfWorld * i / steps), kRelationMinimapPx, col);
        }
    for (int i = 0; i < (int)houses.size(); i++)
        minimapDot(va, inner, houseshapes[i].getPosition() + houseshapes[i].getSize() / 2.0f, MM_HOUSE_PX, storehouseColors[houses[i].race]);
    for (int k = 0; k < raceCount; k++)
        if (stores[k].built) minimapDot(va, inner, quarterCentre(k), MM_STORE_PX, MM_STORE_COLORS[k]);
    if (hasRuins()) minimapDot(va, inner, ruinCentre, MM_RUIN_PX, MM_RUIN_COLOR);   // R45-F1b: site, then camps over it
    for (auto& c : camps) minimapDot(va, inner, c.pos + campSize / 2.0f, MM_CAMP_PX, MM_STORE_COLORS[clampRace(c.race)]);
    for (int i = 0; i < (int)pois.size() && i < (int)poiShapes.size(); i++)   // R47-F4: found sites only, spent ones dim
        if (pois[i].found) minimapDot(va, inner, poiCentre(i), MM_POI_PX, pois[i].spent ? MM_POI_SPENT_COLOR : MM_POI_COLOR);
    for (int i = 0; i < (int)barbs.size(); i++)
        if (!barbHidden(barbs[i]))   // INBOX-F1
        minimapDot(va, inner, squares[i].getPosition() + squares[i].getSize() / 2.0f, MM_BARB_PX,
                   isStarving(barbs[i]) ? uiStarvingRed : MM_BARB_COLORS[barbs[i].race]);
    if (va.getVertexCount() > 0) window.draw(va);

    float s = minimapScale(), innerSize = MM_SIZE - 2 * MM_BORDER_PX;
    Vector2f tl = (worldView.getCenter() - worldView.getSize() / 2.0f) * s;
    Vector2f br = (worldView.getCenter() + worldView.getSize() / 2.0f) * s;
    tl = Vector2f(clamp(round(tl.x), 0.0f, innerSize), clamp(round(tl.y), 0.0f, innerSize));
    br = Vector2f(clamp(round(br.x), 0.0f, innerSize), clamp(round(br.y), 0.0f, innerSize));
    RectangleShape view(br - tl);
    view.setPosition(inner + tl);
    view.setFillColor(Color::Transparent);
    view.setOutlineThickness(-1.0f);   // inward, so the rect never spills onto the border
    view.setOutlineColor(MM_VIEW_COLOR);
    window.draw(view);
}

void drawOverlay(RenderWindow& window, RectangleShape& shape, Color c) {
    shape.setFillColor(c);
    window.setView(window.getDefaultView());
    window.draw(shape);
}

struct campfire {int fuel = campfireStartFuel; bool lit = true;};

// Keeps up to one wood of buffer: takes 1 wood only once the fuel drops to one wood's worth,
// so with steady wood the fire stays at full strength and it fades over campfireTicksPerWood once wood runs out
void campfireTick(campfire& f) {
    if (f.fuel <= campfireTicksPerWood && campfireWood > 0) {   // RACE-F1: only the shared pool, never a race's stock
        campfireWood--;
        f.fuel += campfireTicksPerWood;
        printf("CAMPFIRE FED - campfire wood: %d\n", campfireWood);
    }
    if (f.fuel > 0) f.fuel--;
    if (f.lit && f.fuel == 0) {
        f.lit = false;
        printf("Campfire burned out\n");
        logEvent("Campfire burned out");
    } else if (!f.lit && f.fuel > 0) {
        f.lit = true;
        printf("Campfire relit\n");
        logEvent("Campfire relit");
    }
}

float campfireIntensity(const campfire& f) { return min(1.0f, (float)f.fuel / campfireTicksPerWood); }

float campfireFlicker(int frame) { return 0.88f + 0.12f * sin(frame * 0.37f) * sin(frame * 0.13f + 1.0f); }

Color lerpColor(Color a, Color b, float t) {
    return Color((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t));
}

// UIO-F3: hunger gradient fed green -> ochre at the threshold -> uiStarvingRed once starving (over the grace period)
Color lensHungerColor(const barb& b) {
    if (isStarving(b)) return uiStarvingRed;
    if (!isHungry(b)) return lerpColor(lensFedColor, lensHungryColor, clamp((float)b.hunger / max(1, hungerThresholdOf(b)), 0.0f, 1.0f));
    return lerpColor(lensHungryColor, uiStarvingRed, clamp((float)b.starveTicks / starveGraceTicks, 0.0f, 1.0f));
}

// Soft disc as a triangle fan: lensCentreAlpha at the centre fading to lensRimAlpha at the rim. Skipped when off camera.
void appendLensDisc(VertexArray& va, const FloatRect& cam, Vector2f c, float r, Color col) {
    if (c.x + r < cam.position.x || c.x - r > cam.position.x + cam.size.x
     || c.y + r < cam.position.y || c.y - r > cam.position.y + cam.size.y) return;
    Color in = col, out = col;
    in.a = lensCentreAlpha;
    out.a = lensRimAlpha;
    for (int s = 0; s < lensDiscSegments; s++) {
        float a0 = 2.0f * 3.14159265f * s / lensDiscSegments, a1 = 2.0f * 3.14159265f * (s + 1) / lensDiscSegments;
        va.append(Vertex{c, in});
        va.append(Vertex{c + Vector2f(cos(a0), sin(a0)) * r, out});
        va.append(Vertex{c + Vector2f(cos(a1), sin(a1)) * r, out});
    }
}

// UIO-F3: ground-layer lens tints, drawn in the world view before the house slabs, so barbs and buildings sit on top.
// Loops only barbs (Hunger) or houses + built storehouses (Race); never trees. One draw call.
void drawLens(RenderWindow& window, VertexArray& va, const View& worldView, Lens lens,
              const vector<barb>& barbs, const vector<RectangleShape>& squares,
              const vector<house>& houses, const vector<RectangleShape>& houseshapes,
              const storehouse (&stores)[raceCount]) {
    if (lens == Lens::Off) return;
    window.setView(worldView);
    FloatRect cam(worldView.getCenter() - worldView.getSize() / 2.0f, worldView.getSize());
    va.clear();
    if (lens == Lens::Hunger) {
        for (int i = 0; i < (int)barbs.size() && i < (int)squares.size(); i++)
            if (!barbHidden(barbs[i]))   // INBOX-F1
            appendLensDisc(va, cam, squares[i].getPosition() + squares[i].getSize() / 2.0f, lensBarbRadius, lensHungerColor(barbs[i]));
    } else {
        for (int i = 0; i < (int)houses.size() && i < (int)houseshapes.size(); i++)
            appendLensDisc(va, cam, houseshapes[i].getPosition() + houseshapes[i].getSize() / 2.0f, lensHouseRadius, lensRaceColors[houses[i].race]);
        for (int k = 0; k < raceCount; k++)
            if (stores[k].built) appendLensDisc(va, cam, quarterCentre(k), lensStoreRadius, lensRaceColors[k]);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// RACE-F1: per-race stock rows, top-left just under the HUD strip. Skipped (empty rect) if a short/narrow window would
// make it touch the inspect panel, minimap or ticker, like the lens legend.
FloatRect drawStockPanel(RenderWindow& window, const Font& font, const hudValues& v,
                         const FloatRect& inspectR, const FloatRect& mmR, const FloatRect& tickerR) {
    window.setView(window.getDefaultView());
    Text ref(font, "Ag", stockFontSize);   // shared row height/baseline
    FloatRect rb = ref.getLocalBounds();
    float iconSize = hudIconGrid * stockIconPx;
    float rowH = max(rb.size.y, max(stockSwatchPx, iconSize));
    float nameW = 0.0f, numW = Text(font, stockMinNumber, stockFontSize).getLocalBounds().size.x;
    vector<Text> names, nums;
    for (int r = 0; r < raceCount; r++) {
        names.emplace_back(font, raceDefs[r].name, stockFontSize);
        nameW = max(nameW, names.back().getLocalBounds().size.x);
        for (int n = 0; n < needCount; n++) {
            nums.emplace_back(font, to_string(stockValue(v.stock[r], n)), stockFontSize);
            numW = max(numW, nums.back().getLocalBounds().size.x);   // one width for every number column, so columns line up
        }
    }
    float cellW = iconSize + stockIconGap + numW;
    float innerW = stockSwatchPx + stockSwatchGap + nameW + needCount * (stockCellGap + cellW);
    const int tierBars = toolTierCount - 1;   // R49-F1: tool tier icon column, shown once any race has smelted an ingot
    float tierIconW = tierBars * tierIconBarW + (tierBars - 1) * tierIconBarGap;
    if (toolTierSeen) innerW += stockCellGap + tierIconW;
    if (veil.faithSeen) innerW += stockCellGap + faithTickW;   // R51-F2: faith tick
    // R53-F2: herb count (icon + number), last in the row, shown only once a race has a herb hut and only in that row
    bool herbCol = anyHerbHut();
    float herbNumW = Text(font, stockMinNumber, stockFontSize).getLocalBounds().size.x;
    if (herbCol) {
        for (int r = 0; r < raceCount; r++)
            if (herbHutBuilt(r)) herbNumW = max(herbNumW, Text(font, to_string(raceHerbs[r]), stockFontSize).getLocalBounds().size.x);
        innerW += stockCellGap + iconSize + stockIconGap + herbNumW;
    }
    float barRowH = rowH + larderBarGap + larderBarH;   // R52-F2: each race row carries its 2px larder bar underneath
    Vector2f size(round(innerW + 2 * stockPad.x), round(raceCount * barRowH + (raceCount - 1) * stockRowGap + 2 * stockPad.y));
    Vector2f pos(round(stockMargin), round(hudBarHeight + stockMargin));
    FloatRect box(pos, size);
    auto near = [&](const FloatRect& o) {
        if (o.size.x <= 0) return false;
        FloatRect grown(o.position - Vector2f(stockMargin, stockMargin), o.size + Vector2f(2 * stockMargin, 2 * stockMargin));
        return box.findIntersection(grown).has_value();
    };
    if (near(inspectR) || near(mmR) || near(tickerR)) return FloatRect();

    RectangleShape panel(size);
    panel.setPosition(pos);
    panel.setFillColor(inspectPanelColor);
    panel.setOutlineThickness(-1.0f);   // inward, so the panel stays exactly size
    panel.setOutlineColor(inspectBorderColor);
    window.draw(panel);

    const hudIconId icons[needCount] = {iconWood, iconStone, iconFood};   // indexed like leaderNeed
    RectangleShape sw({stockSwatchPx, stockSwatchPx});
    sw.setOutlineThickness(-1.0f);
    sw.setOutlineColor(lensSwatchRim);
    for (int r = 0; r < raceCount; r++) {
        float y = pos.y + stockPad.y + r * (barRowH + stockRowGap);
        float textY = round(y + (rowH - rb.size.y) / 2.0f - rb.position.y);
        bool empty = v.members[r] <= 0;
        float x = pos.x + stockPad.x;
        {   // R52-F2: larder bar across the row's full inner width: dim track, fill = days of food / days to spring
            RectangleShape bar(Vector2f(round(innerW), larderBarH));
            bar.setPosition(Vector2f(x, round(y + rowH + larderBarGap)));
            bar.setFillColor(larderTrackColor);
            window.draw(bar);
            float fillW = empty ? 0.0f : round(innerW * larderFill[r]);
            if (fillW > 0.0f) {
                bar.setSize(Vector2f(fillW, larderBarH));
                bar.setFillColor(larderCovered[r] ? larderOkColor : larderShortColor);
                window.draw(bar);
            }
        }
        sw.setFillColor(stockSwatchColors[r]);
        sw.setPosition(Vector2f(x, round(y + (rowH - stockSwatchPx) / 2.0f)));
        window.draw(sw);
        x += stockSwatchPx + stockSwatchGap;
        Text& nm = names[r];
        nm.setFillColor(empty ? stockEmptyRaceColor : lensTextColor);
        nm.setPosition(Vector2f(round(x - nm.getLocalBounds().position.x), textY));
        window.draw(nm);
        x += nameW;
        for (int n = 0; n < needCount; n++) {
            x += stockCellGap;
            drawIcon(window, icons[n], Vector2f(x, round(y + (rowH - iconSize) / 2.0f)), stockIconPx);
            Text& t = nums[r * needCount + n];
            bool isShort = !empty && stockShort(v.stock[r], n);
            t.setFillColor(empty ? stockEmptyRaceColor : isShort ? stockShortColor : lensTextColor);
            t.setPosition(Vector2f(round(x + iconSize + stockIconGap - t.getLocalBounds().position.x), textY));
            window.draw(t);
            x += cellW;
        }
        if (toolTierSeen) {   // R49-F1: rising steel bars, one per tier above 0, bottom-aligned with the stock icons
            x += stockCellGap;
            int tier = toolTierOf(r);
            bool dim = raceTierDimFrames[r] > 0;
            float iconTop = round(y + (rowH - iconSize) / 2.0f);
            RectangleShape bar;
            for (int k = 1; k <= tierBars; k++) {
                float h = round(iconSize * (k + 1) / (tierBars + 1));   // 4, 6, 8 px for three bars
                bar.setSize(Vector2f(tierIconBarW, h));
                bar.setPosition(Vector2f(x + (k - 1) * (tierIconBarW + tierIconBarGap), iconTop + iconSize - h));
                Color c = tierIconEmptyColor;
                if (k <= tier) c = empty ? stockEmptyRaceColor : dim ? lerpColor(toolTierMarkColor, tierIconEmptyColor, tierDropDimMix) : toolTierMarkColor;
                else if (dim && k <= raceTierLost[r]) {   // the tier just lost: amber, fading back to empty over the dim
                    float f = (float)raceTierDimFrames[r] / tierDropDimFrames;
                    c = lerpColor(tierIconEmptyColor, toolWearColor, f);
                }
                bar.setFillColor(c);
                window.draw(bar);
            }
            x += tierIconW;
        }
        if (veil.faithSeen) {   // R51-F2: one 2px tick, taller and paler with faith, bottom-aligned with the stock icons
            x += stockCellGap;
            float f = clamp(veil.faith[r], 0.0f, 1.0f);
            float h = round(faithTickMinH + (iconSize - faithTickMinH) * f);
            float iconTop = round(y + (rowH - iconSize) / 2.0f);
            RectangleShape tick(Vector2f(faithTickW, h));
            tick.setPosition(Vector2f(x, iconTop + iconSize - h));
            Color tc = empty ? stockEmptyRaceColor : lerpColor(faithTickLowColor, faithTickHighColor, f);
            if (currentDecree == decreeRite) tc.a = (uint8_t)(tc.a * riteFaithTickDim);   // R52-F3: dims all Rite day
            tick.setFillColor(tc);
            window.draw(tick);
            x += faithTickW;
        }
        if (herbCol && herbHutBuilt(r)) {   // R53-F2: sprig icon + herb stock, purple; dim when the race is empty or the store bare
            x += stockCellGap;
            drawIcon(window, iconHerb, Vector2f(x, round(y + (rowH - iconSize) / 2.0f)), stockIconPx);
            Text t(font, to_string(raceHerbs[r]), stockFontSize);
            t.setFillColor(empty ? stockEmptyRaceColor : raceHerbs[r] <= 0 ? stockShortColor : herbStockColor);
            t.setPosition(Vector2f(round(x + iconSize + stockIconGap - t.getLocalBounds().position.x), textY));
            window.draw(t);
        }
    }
    return box;
}

// RACE-F1: one ticker line when a living race's wood, stone or food drops below stockShortThreshold (the HUD's red test);
// the race's flag for that resource re-arms once the stock is back at or above the threshold. O(barbs + races) per tick.
void stockShortageTick(const vector<barb>& barbs) {
    int members[raceCount] = {};
    for (auto& b : barbs) members[clampRace(b.race)]++;
    for (int r = 0; r < raceCount; r++)
        for (int n = 0; n < needCount; n++) {
            bool isShort = members[r] > 0 && stockShort(stock[r], n);
            if (!isShort) { stockAnnounced[r][n] = false; continue; }
            if (stockAnnounced[r][n]) continue;
            stockAnnounced[r][n] = true;
            printf("SHORTAGE - %s low on %s (%d)\n", racePlurals[r], leaderNeedNames[n], stockValue(stock[r], n));
            logEvent(string(racePlurals[r]) + " are short of " + leaderNeedNames[n], stockShortColor);
        }
}

// UIO-F3: lens name + colour legend, top-left just under the HUD strip (the inspect panel is top-right, the minimap
// bottom-right, the ticker bottom-left). Skipped if a short/narrow window would make it touch any of them.
// RACE-F1: stacks just below the race stock panel (stockR) when that is shown.
FloatRect drawLensLegend(RenderWindow& window, const Font& font, Lens lens,
                         const FloatRect& inspectR, const FloatRect& mmR, const FloatRect& tickerR, const FloatRect& stockR) {
    if (lens == Lens::Off) return FloatRect();
    window.setView(window.getDefaultView());
    struct row { string label; Color swatch; };
    vector<row> rows;
    if (lens == Lens::Hunger) rows = {{"Fed", lensFedColor}, {"Hungry", lensHungryColor}, {"Starving", uiStarvingRed}};
    else for (int k = 0; k < raceCount; k++) rows.push_back({raceDefs[k].name, lensRaceColors[k]});

    Text title(font, string("Lens: ") + lensNames[(int)lens] + "  [L]", lensFontSize);
    Text ref(font, "Ag", lensFontSize);   // shared row height/baseline, like the inspect panel
    FloatRect rb = ref.getLocalBounds();
    float rowH = max(rb.size.y, lensSwatchPx);
    float innerW = title.getLocalBounds().size.x;
    vector<Text> labels;
    for (auto& r : rows) {
        labels.emplace_back(font, r.label, lensFontSize);
        innerW = max(innerW, lensSwatchPx + lensSwatchGap + labels.back().getLocalBounds().size.x);
    }
    int n = 1 + (int)rows.size();
    Vector2f size(round(innerW + 2 * lensPad.x), round(n * rowH + (n - 1) * lensRowGap + 2 * lensPad.y));
    Vector2f pos(round(lensMargin), round(hudBarHeight + lensMargin));
    if (stockR.size.x > 0) pos.y = round(stockR.position.y + stockR.size.y + lensMargin);   // RACE-F1: under the stock panel
    FloatRect box(pos, size);
    auto near = [&](const FloatRect& o) {   // touching or within lensMargin of another panel
        if (o.size.x <= 0) return false;
        FloatRect grown(o.position - Vector2f(lensMargin, lensMargin), o.size + Vector2f(2 * lensMargin, 2 * lensMargin));
        return box.findIntersection(grown).has_value();
    };
    if (near(inspectR) || near(mmR) || near(tickerR)) return FloatRect();

    RectangleShape panel(size);
    panel.setPosition(pos);
    panel.setFillColor(inspectPanelColor);
    panel.setOutlineThickness(-1.0f);   // inward, so the panel stays exactly size
    panel.setOutlineColor(inspectBorderColor);
    window.draw(panel);

    auto rowY = [&](int r) { return pos.y + lensPad.y + r * (rowH + lensRowGap); };
    auto put = [&](Text& t, float tx, int r, Color c) {
        t.setFillColor(c);
        t.setPosition(Vector2f(round(tx - t.getLocalBounds().position.x), round(rowY(r) + (rowH - rb.size.y) / 2.0f - rb.position.y)));
        window.draw(t);
    };
    float x = pos.x + lensPad.x;
    put(title, x, 0, lensTitleColor);
    RectangleShape sw({lensSwatchPx, lensSwatchPx});
    sw.setOutlineThickness(-1.0f);
    sw.setOutlineColor(lensSwatchRim);
    for (int i = 0; i < (int)rows.size(); i++) {
        sw.setFillColor(rows[i].swatch);   // full colour here: the world discs are the same hue at low alpha
        sw.setPosition(Vector2f(x, round(rowY(i + 1) + (rowH - lensSwatchPx) / 2.0f)));
        window.draw(sw);
        put(labels[i], x + lensSwatchPx + lensSwatchGap, i + 1, lensTextColor);
    }
    return box;
}

void drawCampfire(RenderWindow& window, const campfire& f, int frame) {
    CircleShape ring(campfireStoneRadius);
    ring.setOrigin({campfireStoneRadius, campfireStoneRadius});
    ring.setPosition(villageCenter);
    ring.setFillColor(Color::Transparent);
    ring.setOutlineThickness(1.0f);
    ring.setOutlineColor(campfireStoneColor);
    window.draw(ring);

    RectangleShape log(campfireLogSize);
    log.setOrigin(campfireLogSize / 2.0f);
    log.setPosition(villageCenter);
    log.setFillColor(campfireLogColor);
    for (float a : {campfireLogAngle, -campfireLogAngle}) {
        log.setRotation(degrees(a));
        window.draw(log);
    }

    float in = campfireIntensity(f);
    float r = in > 0.0f ? campfireFlameRadius * (0.4f + 0.6f * in) * campfireFlicker(frame) : 1.0f;
    CircleShape flame(r);
    flame.setOrigin({r, r});
    flame.setPosition(villageCenter);
    flame.setFillColor(in > 0.0f ? lerpColor(campfireEmberColor, campfireFlameColor, in) : campfireAshColor);
    window.draw(flame);
}

// Additive glow, drawn after the night overlay so it lifts the darkness around the fire; fades with fuel
void drawCampfireGlow(RenderWindow& window, const View& worldView, const campfire& f, int frame) {
    float in = campfireIntensity(f);
    if (in <= 0.0f) return;
    window.setView(worldView);
    CircleShape c;
    c.setPosition(villageCenter);
    Color col = campfireGlowColor;
    col.a = (uint8_t)(campfireGlowLayerAlpha * in * campfireFlicker(frame));
    c.setFillColor(col);
    for (int i = 1; i <= campfireGlowLayers; i++) {
        float r = campfireGlowRadius * i / campfireGlowLayers;
        c.setRadius(r);
        c.setOrigin({r, r});
        c.setPointCount(48);
        window.draw(c, BlendAdd);
    }
}

// R30-F1: false if a house slab at spot (tx,ty) would touch a standing house's slab or one still being walked to / built
bool houseSpotFree(const vector<barb>& barbs, const vector<house>& houses, int tx, int ty) {
    FloatRect r(Vector2f(tx - 4.0f, ty - 4.0f), houseSlabSize + Vector2f(2.0f, 2.0f));   // slab grown by 1px: a sliver of grass between
    for (auto& h : houses)
        if (r.findIntersection(FloatRect(Vector2f(h.x - 3.0f, h.y - 3.0f), houseSlabSize))) return false;
    for (auto& o : barbs)
        if (o.movingbspot && o.jobKind == jobNone
            && r.findIntersection(FloatRect(Vector2f(o.targetx - 3.0f, o.targety - 3.0f), houseSlabSize))) return false;
    return true;
}

// Centre of race r's house nearest its crate centre (the quarter road's far end); false if the race has no house
bool nearestQuarterHouse(int r, const vector<house>& houses, Vector2f& best) {
    Vector2f a = quarterCentre(r);
    float bestDSq = -1.0f;
    for (auto& h : houses) {
        if (h.race != r) continue;
        Vector2f c((float)h.x + 2.0f, (float)h.y + 2.0f);   // centre of the 4px house
        float dSq = (c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y);
        if (bestDSq < 0.0f || dSq < bestDSq) { bestDSq = dSq; best = c; }
    }
    return bestDSq >= 0.0f;
}

// FARM-F1: false if a quarter road from a to b would cross any reserved plot zone (FARM-F2: or mill zone)
bool roadClearOfFields(const fieldGrid& fields, Vector2f a, Vector2f b) {
    for (int q = 0; q < raceCount; q++) {
        for (int k = 0; k < fieldsPerRace; k++) {
            Vector2f pos;   // R32-F1: reserved mill slots too
            if (plotHeldAt(fields[q][k], pos) && segmentHitsRect(a, b, fieldZone(pos))) return false;
        }
        if (mills[q].placed && segmentHitsRect(a, b, millZone(mills[q].pos))) return false;   // FARM-F2
    }
    return true;
}

// R43-F1: seeds race r's quarter road as real trail cells: an integer 4-connected grid walk from the crate's cell to
// the nearest own house's cell. Each in-bounds cell that is not paved and not over a plot/mill zone is raised to
// trailPathThreshold + trailSeedBoost (never lowered), then decays like any trail. Called only on crate/house placement.
// R44-F4a: with `to` set, the walk goes to that point instead (a homestead's own road).
void seedQuarterTrail(int r, const vector<house>& houses, const fieldGrid& fields, const Vector2f* to = nullptr) {
    if (r < 0 || r >= raceCount) return;
    Vector2f best;
    if (to) best = *to;
    else if (!nearestQuarterHouse(r, houses, best)) return;
    int c0 = trailCell(quarterCentre(r)), c1 = trailCell(best);
    if (c0 < 0 || c1 < 0) return;
    int x = c0 % trailGridN, y = c0 / trailGridN, x1 = c1 % trailGridN, y1 = c1 / trailGridN;
    int dx = abs(x1 - x), dy = abs(y1 - y), sx = x1 > x ? 1 : -1, sy = y1 > y ? 1 : -1;
    const int seedWear = trailPathThreshold + trailSeedBoost;
    int seeded = 0;
    for (int ix = 0, iy = 0;;) {
        if (x >= 0 && y >= 0 && x < trailGridN && y < trailGridN) {
            int c = y * trailGridN + x;
            Vector2f lo((float)x * trailCellSize, (float)y * trailCellSize), hi = lo + Vector2f(trailCellSize, trailCellSize);
            if (!trailPaved[c] && trailWear[c] < seedWear
                && roadClearOfFields(fields, lo, hi) && roadClearOfFields(fields, Vector2f(hi.x, lo.y), Vector2f(lo.x, hi.y))) {
                trailWear[c] = seedWear;
                trailSetTier(c);
                seeded++;
            }
        }
        if (ix >= dx && iy >= dy) break;
        // step the axis whose next cell boundary the crate->house line crosses first (ties go to y)
        if ((1 + 2 * ix) * dy < (1 + 2 * iy) * dx) { x += sx; ix++; } else { y += sy; iy++; }
    }
    printf("QUARTER TRAIL SEEDED - %s, %d cells\n", raceDefs[r].name, seeded);
}

// R44-F4a: a house is a homestead if it stands outside its race's quarter disc
bool isHomestead(const house& h) {
    if (h.race < 0 || h.race >= raceCount) return false;
    Vector2f d = Vector2f((float)h.x + 2.0f, (float)h.y + 2.0f) - quarterCentre(h.race);
    float cr = quarterClearRadiusFor(h.race);   // R47-F10
    return d.x * d.x + d.y * d.y > cr * cr;
}

// R44-F4a: the checks every house spot (tx,ty = top-left of the 4px house) must pass: no overlap with other house slabs,
// reserved buildings, water or plots, and a quarter road from crate centre qc that misses plots, smokehouse and keep
bool houseSpotOk(const vector<barb>& barbs, const vector<house>& houses, const fieldGrid& fields, int race, Vector2f qc, int tx, int ty) {
    FloatRect slab(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize);
    Vector2f door(tx + 2.0f, ty + 2.0f);
    if (!houseSpotFree(barbs, houses, tx, ty)) return false;  // packed quarter: no overlapping houses
    if (!forgeClear(slab)) return false;                      // house + roof + slab
    if (!lakeClear(slab)) return false;                       // same footprint, no houses in the water
    if (!fieldsClear(fields, slab)) return false;             // nor on a wheat plot
    if (!roadClearOfFields(fields, qc, door)) return false;   // FARM-F1: its road can't cross one
    {   // INBOX2-F1: nor its own race's smokehouse
        FloatRect h = smokehouseRect(race);
        FloatRect pad(h.position - Vector2f(smokehouseRoadPadPx, smokehouseRoadPadPx), h.size + Vector2f(2 * smokehouseRoadPadPx, 2 * smokehouseRoadPadPx));
        if (segmentHitsRect(qc, door, pad)) return false;
    }
    {   // R31-F1: nor its own race's keep
        FloatRect z = keepZone(race);
        FloatRect pad(z.position - Vector2f(keepRoadPadPx, keepRoadPadPx), z.size + Vector2f(2 * keepRoadPadPx, 2 * keepRoadPadPx));
        if (segmentHitsRect(qc, door, pad)) return false;
    }
    {   // R47-F6: nor its own race's barracks and drill yard
        FloatRect z = barracksZone(race);
        FloatRect pad(z.position - Vector2f(barracksRoadPadPx, barracksRoadPadPx), z.size + Vector2f(2 * barracksRoadPadPx, 2 * barracksRoadPadPx));
        if (segmentHitsRect(qc, door, pad)) return false;
    }
    if (race == kHerbRace) {   // R53-F2: nor the herb hut and bed
        FloatRect z = herbZone(race);
        FloatRect pad(z.position - Vector2f(herbRoadPadPx, herbRoadPadPx), z.size + Vector2f(2 * herbRoadPadPx, 2 * herbRoadPadPx));
        if (segmentHitsRect(qc, door, pad)) return false;
    }
    return true;
}

// R44-F4a: extra checks for a homestead spot outside the quarter: map/HUD bounds and village clearing (shared with
// quarterLayoutOk), homesteadHouseGap from every house and pending house spot, out of other races' quarters, no
// tree/rock/bush on the slab, and a dry walk from the crate (reachable, not orphaned behind a lake)
bool homesteadSpotOk(const vector<barb>& barbs, const vector<house>& houses, int race, Vector2f qc, int tx, int ty,
                     const vector<tree>& trees, const vector<rock>& rocks, const vector<CircleShape>& bushshapes) {
    Vector2f c(tx + 2.0f, ty + 2.0f);
    if (!layoutDiscOk(c, houseSlabSize.x / 2.0f + homesteadClutterPad)) return false;
    const float gapSq = homesteadHouseGap * homesteadHouseGap;
    for (auto& h : houses) {
        Vector2f d = c - Vector2f(h.x + 2.0f, h.y + 2.0f);
        if (d.x * d.x + d.y * d.y < gapSq) return false;
    }
    for (auto& o : barbs) {
        if (!(o.movingbspot && o.jobKind == jobNone)) continue;   // another barb's pending house spot (claims race each other)
        Vector2f d = c - Vector2f(o.targetx + 2.0f, o.targety + 2.0f);
        if (d.x * d.x + d.y * d.y < gapSq) return false;
    }
    for (int r = 0; r < raceCount; r++) {
        if (r == race) continue;
        Vector2f d = c - quarterCentre(r);
        float m = quarterClearRadiusFor(r) + homesteadHouseGap;   // R47-F10
        if (d.x * d.x + d.y * d.y < m * m) return false;
    }
    FloatRect slab(Vector2f(tx - 3.0f - homesteadClutterPad, ty - 3.0f - homesteadClutterPad),
                   houseSlabSize + Vector2f(2 * homesteadClutterPad, 2 * homesteadClutterPad));
    for (auto& t : trees)
        if (slab.findIntersection(FloatRect(Vector2f((float)t.x, (float)t.y), Vector2f(treeFootprint, treeFootprint)))) return false;
    for (auto& k : rocks)
        if (slab.findIntersection(FloatRect(Vector2f((float)k.x, (float)k.y), Vector2f(12.0f, 12.0f)))) return false; // 6px-radius pentagon
    for (auto& bs : bushshapes)
        if (slab.findIntersection(bs.getGlobalBounds())) return false;
    Vector2f from = qc - houseSlabSize / 2.0f;   // lakeClear's path test works in slab top-left coordinates
    if (!lakeClear(FloatRect(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize), &from)) return false;
    return true;
}

void buildhouse(vector<barb>& barbs,vector<RectangleShape>& squares, vector<house>& houses, vector<RectangleShape>& houseshapes, const fieldGrid& fields,
                const storehouse (&stores)[raceCount], const vector<tree>& trees, const vector<rock>& rocks, const vector<CircleShape>& bushshapes) {
    bool canclaim = (int)houses.size() < (int)barbs.size();
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (!barbAvailable(b)) continue;      // INBOX-F1: bellTick() owns this barb; R45-F2: or expeditionTick()
        if (b.jobKind != jobNone) continue;   // buildshrine() / buildsmokehouse() / buildstall() / buildforge() / buildfield() own this barb
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (b.race < 0 || b.race >= raceCount) continue;
            if (!canclaim || !canAfford(stockOf(b), houseWoodCost, 0)) continue;   // RACE-F1: the builder's race pays
            Vector2f qc = quarterCentre(b.race);
            int tx = 0, ty = 0;
            bool homestead = false, triedHomestead = false;
            int own = 0;   // R44-F4a: this race's houses, standing or claimed
            for (auto& h : houses) if (h.race == b.race) own++;
            for (auto& o : barbs) if (o.race == b.race && o.movingbspot && o.jobKind == jobNone) own++;
            if ((own + 1) % homesteadEvery == 0) {
                Vector2f out = qc - villageCenter;
                float base = atan2(out.y, out.x);
                uniform_real_distribution<float> arc(-homesteadArc, homesteadArc), spread(houseSpreadNear, houseSpreadFarFor(b.race));   // R47-F10
                for (int k = 0; k < homesteadTries && !homestead; k++) {
                    float a = base + arc(gen), r = spread(gen);
                    tx = (int)floor(qc.x + cos(a) * r) - 2;   // house top-left; its centre is +2
                    ty = (int)floor(qc.y + sin(a) * r) - 2;
                    homestead = homesteadSpotOk(barbs, houses, b.race, qc, tx, ty, trees, rocks, bushshapes)
                             && houseSpotOk(barbs, houses, fields, b.race, qc, tx, ty);
                }
                triedHomestead = !homestead;
            }
            if (!homestead) {
                int qr = (int)quarterRadiusFor(b.race);   // R47-F10: the race's current step
                uniform_int_distribution<int> quarterSpotD(-qr, qr);   // R30-F1: house offset per axis, disc-rejected
                int dx = quarterSpotD(gen), dy = quarterSpotD(gen);
                if (dx * dx + dy * dy > qr * qr) continue; // R30-F1: inside the own quarter's disc
                tx = (int)qc.x + dx; ty = (int)qc.y + dy;
                if (!houseSpotOk(barbs, houses, fields, b.race, qc, tx, ty)) continue;     // reroll next tick
                {   // R47-F10: a grown disc can reach a bush clump (bushes are never cleared), so keep the slab off them
                    FloatRect slab(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize);
                    bool onBush = false;
                    for (auto& bs : bushshapes) if (slab.findIntersection(bs.getGlobalBounds())) { onBush = true; break; }
                    if (onBush) continue;
                }
                if (triedHomestead)   // logged once, on the fallback claim itself
                    printf("HOMESTEAD - %s: no spot in %d tries, built in the quarter instead\n", raceDefs[b.race].name, homesteadTries);
            } else {
                printf("HOMESTEAD CLAIMED - %s at (%d, %d), %.0fpx from the crate\n", raceDefs[b.race].name, tx, ty,
                       hypot(tx + 2.0f - qc.x, ty + 2.0f - qc.y));
            }
            raceStock& rs = stockOf(b);
            clearTraining(b);   // R47-F6: a house claim outranks the drill
            b.reservedwood = true;
            b.reservedstone = canAfford(rs, houseWoodCost, houseStoneCost) ? houseStoneCost : 0; // 0 stone -> plain house
            spend(rs, houseWoodCost, b.reservedstone);
            b.targetx = tx;
            b.targety = ty;
            b.moving = true;
            b.movingbspot = true;
            break;
        } else if (b.movingbspot) {
            Vector2f pos  = sq.getPosition();
            Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;

            if (dSq < 1.0f) {
                if (!b.reservedwood || (int)houses.size() >= (int)barbs.size()) {
                    if (b.reservedwood) stockOf(b).wood += houseWoodCost;   // RACE-F1: back to the builder's race
                    stockOf(b).stone += b.reservedstone;
                    b.reservedwood = false;
                    b.reservedstone = 0;
                    b.moving = b.movingbspot = b.building = false; clearRoute(b);
                    b.timer = 0;
                    continue;
                }
                b.building = true;
                if (workTick(b)) b.timer++;
                if (b.timer >= houseBuildTicks) {
                    b.reservedwood = false;
                    house h{b.targetx, b.targety, b.reservedstone > 0, nextHouseId++, b.race};
                    b.reservedstone = 0;
                    houses.push_back(h);
                    RectangleShape hs(Vector2f(4, 4));
                    hs.setPosition(Vector2f(h.x, h.y));
                    hs.setFillColor(houseWallColor);
                    hs.setOutlineColor(Color(54, 35, 16));
                    hs.setOutlineThickness(1);
                    houseshapes.push_back(hs);
                    bool far = isHomestead(h);   // R44-F4a
                    if (h.race >= 0 && h.race < raceCount && stores[h.race].built) {
                        seedQuarterTrail(h.race, houses, fields);   // R43-F1: only on placement, never per tick
                        if (far) {   // R44-F4a: a homestead gets its own road from the crate
                            Vector2f c(h.x + 2.0f, h.y + 2.0f);
                            seedQuarterTrail(h.race, houses, fields, &c);
                        }
                    }
                    b.building = b.moving = b.movingbspot = false; clearRoute(b);
                    b.timer = 0;
		            printf(far ? "---HOMESTEAD BUILT---\n" : "---HOUSE BUILT---\n");
                    logEvent(far ? "Homestead built" : "House built");
                }
            } else {
                walkStep(b, sq);   // R39-F1
            }
        }
    }
}

// Slab top-left of a shrine built from this standing spot
Vector2f shrineSlabPos(int tx, int ty) { return Vector2f((float)tx, (float)ty) + shrineStandOffset; }

// Index of the shrine with this id, or -1 if there is none. Anything that remembers a shrine keeps its id.
int findShrineById(const vector<shrine>& shrines, int id) {
    if (id < 0) return -1;
    for (int i = 0; i < (int)shrines.size(); ++i)
        if (shrines[i].id == id) return i;
    return -1;
}

// R51-F2: just above the middle of a shrine's gold cap, where its faith motes start
Vector2f faithMoteOrigin(const shrine& s) {
    return Vector2f((float)s.x + shrineSlabSize.x / 2.0f, (float)s.y - shrineCapH - 1.0f);
}

// R51-F2: every tick, ages shrine motes (dropping any whose shrine id no longer resolves), re-finds each race's shrine
// by id (picking a fresh one, nearest its quarter and not held by another race if possible, when it is missing), and
// rolls a mote over it with chance faithMoteChanceMax * faith^2. Reads veil.faith only; never touches veil.pool.
void faithShrineTick(const vector<shrine>& shrines) {
    for (auto& m : faithMotes) m.age++;
    for (int i = (int)faithMotes.size() - 1; i >= 0; i--)
        if (faithMotes[i].age >= faithMoteTicks || findShrineById(shrines, faithMotes[i].shrineId) < 0) swapRemove(faithMotes, i);
    static uniform_real_distribution<float> roll(0.0f, 1.0f);
    static uniform_real_distribution<float> scatter(-faithMoteScatterPx, faithMoteScatterPx);
    for (int r = 0; r < raceCount; r++) {
        int si = findShrineById(shrines, veil.faithShrineId[r]);
        if (si < 0) {   // not found (none yet, or gone): choose again
            veil.faithShrineId[r] = -1;
            Vector2f home = quarterCentre(r);
            int best = -1;
            bool bestFree = false;
            float bestD = 0.0f;
            for (int k = 0; k < (int)shrines.size(); k++) {
                bool free = true;
                for (int o = 0; o < raceCount; o++)
                    if (o != r && veil.faithShrineId[o] == shrines[k].id && findShrineById(shrines, veil.faithShrineId[o]) >= 0) free = false;
                Vector2f d = faithMoteOrigin(shrines[k]) - home;
                float dd = d.x * d.x + d.y * d.y;
                if (best < 0 || (free && !bestFree) || (free == bestFree && dd < bestD)) { best = k; bestFree = free; bestD = dd; }
            }
            if (best < 0) continue;   // no shrine standing: faith still counts, there is just nowhere to show it
            si = best;
            veil.faithShrineId[r] = shrines[si].id;
            printf("FAITH - %s motes will rise over shrine %d\n", racePlurals[r], shrines[si].id);
        }
        float f = veil.faith[r];
        if (f <= 0.0f || (int)faithMotes.size() >= faithMoteMax) continue;
        if (roll(gen) >= faithMoteChanceMax * f * f) continue;
        veilMote m;
        m.pos = faithMoteOrigin(shrines[si]) + Vector2f(scatter(gen), 0.0f);
        m.race = r;
        m.phase = roll(gen) * 6.2832f;
        m.shrineId = shrines[si].id;
        faithMotes.push_back(m);
    }
}

// Runs after buildhouse(), so a barb free for a house this tick takes the house first.
// Only spends stone above a house foundation's worth, and only once housing is ahead of the shrine count.
void buildshrine(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, vector<shrine>& shrines, const fieldGrid& fields) {
    int pending = 0;
    for (auto& b : barbs) if (b.jobKind == jobShrine) pending++;
    int planned = (int)shrines.size() + pending;
    bool canclaim = planned < maxShrines
                 && (int)houses.size() >= (planned + 1) * shrineHousesPerShrine;   // RACE-F1: the stone check is per builder's race, below
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (!barbAvailable(b)) continue;   // INBOX-F1: bellTick() owns this barb; R45-F2: or expeditionTick()
        if (b.jobKind != jobNone && b.jobKind != jobShrine) continue; // buildsmokehouse() / buildstall() / buildforge() own this barb
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (!canclaim || !canAfford(stockOf(b), 0, shrineStoneCost + houseStoneCost)) continue;
            uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(shrineMinRadius, shrineMaxRadius);
            bool found = false;
            int tx = 0, ty = 0;
            for (int k = 0; k < shrinePlaceTries && !found; k++) {
                float a = ang(gen), r = rad(gen);
                tx = (int)(villageCenter.x + cos(a) * r);
                ty = (int)(villageCenter.y + sin(a) * r);
                Vector2f p = shrineSlabPos(tx, ty);
                found = true;
                for (auto& s : shrines) {
                    Vector2f d = p - Vector2f((float)s.x, (float)s.y);
                    if (d.x * d.x + d.y * d.y < shrineMinSpacing * shrineMinSpacing) found = false;
                }
                FloatRect stoneRect(p - Vector2f(0.0f, shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH));
                if (!smokehouseClear(stoneRect)) found = false;                     // keep every race's smokehouse spot free
                if (!forgeClear(stoneRect)) found = false;                          // and the forge spot
                if (!lakeClear(stoneRect)) found = false;
                if (!fieldsClear(fields, stoneRect)) found = false;
                for (auto& o : barbs) {
                    if (o.jobKind != jobShrine) continue;
                    Vector2f d = p - shrineSlabPos(o.targetx, o.targety);
                    if (d.x * d.x + d.y * d.y < shrineMinSpacing * shrineMinSpacing) found = false;
                }
            }
            canclaim = false;   // one claim attempt per tick; keep looping so builders already en route still progress
            if (!found) continue;
            spend(stockOf(b), 0, shrineStoneCost);
            clearTraining(b);   // R47-F6
            b.reservedstone = shrineStoneCost;
            b.jobKind = jobShrine;
            b.targetx = tx;
            b.targety = ty;
            b.moving = true;
            b.movingbspot = true;
        } else if (b.movingbspot && b.jobKind == jobShrine) {
            Vector2f pos  = sq.getPosition();
            Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;

            if (dSq < 1.0f) {
                if ((int)shrines.size() >= maxShrines) {   // cap reached while walking: refund
                    stockOf(b).stone += b.reservedstone;
                    b.reservedstone = 0;
                    b.jobKind = jobNone;
                    b.moving = b.movingbspot = b.building = false; clearRoute(b);
                    b.timer = 0;
                    continue;
                }
                b.building = true;
                if (workTick(b)) b.timer++;
                if (b.timer >= shrineBuildTicks) {
                    Vector2f p = shrineSlabPos(b.targetx, b.targety);
                    shrine s{(int)p.x, (int)p.y, nextShrineId++};
                    shrines.push_back(s);
                    b.reservedstone = 0;
                    b.jobKind = jobNone;
                    b.building = b.moving = b.movingbspot = false; clearRoute(b);
                    b.timer = 0;
                    printf("---SHRINE BUILT--- id %d (%d/%d)\n", s.id, (int)shrines.size(), maxShrines);
                    logEvent("Shrine stone raised");
                }
            } else {
                walkStep(b, sq);   // R39-F1
            }
        }
    }
}

// Grey 4x8 standing slab with a 2px pale-gold cap on top. Drawn with the houses, under barbs and trees.
void drawShrines(RenderWindow& window, const vector<shrine>& shrines) {
    RectangleShape slab(shrineSlabSize), cap(Vector2f(shrineSlabSize.x, shrineCapH));
    slab.setFillColor(shrineSlabColor);
    cap.setFillColor(shrineCapColor);
    for (auto& s : shrines) {
        Vector2f o((float)s.x, (float)s.y);
        slab.setPosition(o);
        cap.setPosition(o - Vector2f(0.0f, shrineCapH));
        window.draw(slab);
        window.draw(cap);
    }
}

// Shared plumbing for the one-off village structures (smokehouse, stall, forge): one builder each, fixed spot.
// While !claimed, the first free barb that ready() accepts is sent to `stand`. ready(b) returns -1 to stop looking
// this tick (colony-wide condition unmet), 0 to skip this barb, 1 to claim (ready() spends any cost before returning 1).
// Once claimed, only the builder holding `kind` is advanced. Returns true on the tick the structure is finished;
// the builder's jobKind and build flags are reset on that path.
// INBOX2-F1: onlyRace >= 0 limits both the claim and the builder to that race (per-race structures sharing one job kind).
template <class ReadyFn>
bool buildOneOff(vector<barb>& barbs, vector<RectangleShape>& squares, bool& claimed, buildJob kind,
                 Vector2f stand, const char* name, ReadyFn ready, int onlyRace = -1) {
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (!barbAvailable(b)) continue;   // INBOX-F1: bellTick() owns this barb; R45-F2: or expeditionTick()
        if (onlyRace >= 0 && b.race != onlyRace) continue;
        if (!claimed) {
            if (b.jobKind != jobNone || b.chopping || b.timer > 0 || b.moving || b.movingbspot || b.building) continue;
            int r = ready(b);
            if (r < 0) return false;
            if (r == 0) continue;
            claimed = true;
            clearTraining(b);   // R47-F6: any one-off build claim outranks the drill
            b.jobKind = kind;
            b.targetx = (int)stand.x;
            b.targety = (int)stand.y;
            b.moving = true;
            b.movingbspot = true;
            printf("%s CLAIMED - barb %d %s wood: %d stone: %d\n", name, b.id, racePlurals[clampRace(b.race)], stockOf(b).wood, stockOf(b).stone);
            return false;
        }
        if (b.jobKind != kind || !b.movingbspot) continue;
        Vector2f pos  = sq.getPosition();
        Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
        float dSq = diff.x * diff.x + diff.y * diff.y;
        if (dSq < 1.0f) {
            b.building = true;
            if (workTick(b)) b.timer++;
            if (b.timer >= buildJobTicks[kind]) {
                b.jobKind = jobNone;
                b.building = b.moving = b.movingbspot = false; clearRoute(b);
                b.timer = 0;
                return true;
            }
        } else walkStep(b, sq);   // R39-F1
        return false;
    }
    return false;
}

// Runs after buildshrine(). INBOX2-F1: one hut per race, built only once that race's crate stands (its quarter is
// live) and it has smokehouseMinHouses of its own houses; a builder of that race claims it and its race pays.
// Huts are independent, so several races can build at once. claimed stays set once a hut stands.
// Houses keep first call on stone; wood is whatever buildhouse() left this tick.
void buildsmokehouse(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses,
                     const storehouse (&stores)[raceCount], smokehouseSet& sh) {
    for (int r = 0; r < raceCount; r++) {
        smokehouse& hut = sh[r];
        if (hut.built || !stores[r].built) continue;
        if (hut.claimed) {   // builder lost without releaseBarbWork() (should not happen): free the claim
            bool held = false;
            for (auto& b : barbs) if (b.jobKind == jobSmoke && b.race == r) held = true;
            if (!held) hut.claimed = false;
        }
        bool done = buildOneOff(barbs, squares, hut.claimed, jobSmoke, smokehousePos[r] + smokehouseStandOffset, "SMOKEHOUSE",
            [&](const barb&) {
                int raceHouses = 0;
                for (auto& h : houses) if (h.race == r) raceHouses++;
                if (raceHouses < smokehouseMinHouses) return -1;
                if (!storehouseClear(smokehouseRect(r)) || !lakeClear(smokehouseRect(r)))
                    return -1;   // fixed offset from the crate: only trips if the layout constants are moved onto it
                if (!canAfford(stock[r], smokehouseWoodCost, smokehouseStoneCost + houseStoneCost)) return -1;
                spend(stock[r], smokehouseWoodCost, smokehouseStoneCost);   // RACE-F1: the race's own stock
                return 1;
            }, r);
        if (!done) continue;
        hut.built = true;
        printf("---SMOKEHOUSE BUILT--- %s: +%d food per own deer/rabbit kill\n", raceDefs[r].name, smokehouseFoodBonus);
        logRaceEvent(string(racePlurals[r]) + " build a smokehouse (+" + to_string(smokehouseFoodBonus) + " food per kill)", r);
    }
}

// Dark 6x5 hut: 2 roof rows over 3 wall rows, a 1x2 door and a grey 1x2 chimney. Drawn with the crates.
// INBOX2-F1: one per race with a standing crate; the roof takes that race's muted colour.
void drawSmokehouse(RenderWindow& window, const smokehouseSet& sh, const storehouse (&stores)[raceCount]) {
    RectangleShape r;
    for (int k = 0; k < raceCount; k++) {
        if (!sh[k].built || !stores[k].built) continue;
        r.setSize(smokehouseSize);
        r.setPosition(smokehousePos[k]);
        r.setFillColor(smokehouseWallColor);
        window.draw(r);
        r.setSize(Vector2f(smokehouseSize.x, smokehouseRoofH));
        r.setFillColor(smokehouseRoofRaceColors[k]);
        window.draw(r);
        r.setSize(smokehouseDoorSize);
        r.setPosition(smokehousePos[k] + smokehouseDoorPos);
        r.setFillColor(smokehouseDoorColor);
        window.draw(r);
        r.setSize(smokehouseChimneySize);
        r.setPosition(smokehousePos[k] + smokehouseChimneyPos);
        r.setFillColor(smokehouseChimneyColor);
        window.draw(r);
    }
}

// INBOX2-F1: no idle smoke. After one of a race's own smoked kills, its chimney lets out smokePuffCount faint puffs,
// staggered, each rising and fading over smokePuffLifeFrames. A race without a hut draws nothing. Aged per drawn
// frame (frozen while paused). Drawn above the trees and under the day/night overlay.
void drawSmoke(RenderWindow& window, smokehouseSet& sh, bool paused) {
    RectangleShape p;
    for (int k = 0; k < raceCount; k++) {
        smokehouse& hut = sh[k];
        if (!hut.built) { hut.puffFrames = 0; continue; }
        if (hut.puffFrames <= 0) continue;
        int elapsed = smokePuffFrames - hut.puffFrames;
        Vector2f top = smokehousePos[k] + smokehouseChimneyPos;   // chimney top-left
        for (int n = 0; n < smokePuffCount; n++) {
            int age = elapsed - n * smokePuffStaggerFrames;
            if (age < 0 || age >= smokePuffLifeFrames) continue;
            float t = (float)age / smokePuffLifeFrames;
            float sz = t < 0.5f ? smokePuffPx : 1.0f;
            Vector2f at(top.x - (sz - 1.0f) * 0.5f + smokeDriftPx * t, top.y - sz - smokeRisePx * t);
            p.setSize(Vector2f(sz, sz));
            p.setPosition(Vector2f(floor(at.x), floor(at.y)));
            Color c = smokeColor;
            c.a = (uint8_t)(smokeAlpha * (1.0f - t));
            p.setFillColor(c);
            window.draw(p);
        }
        if (!paused) hut.puffFrames--;
    }
}

// Runs after buildsmokehouse(). One builder ever; costs nothing but the builder's time.
// If the builder is lost before finishing (no barb holds jobStall), the claim is released so another idle barb can take it.
void buildstall(vector<barb>& barbs, vector<RectangleShape>& squares) {
    if (hasStall) return;
    if (stallClaimed) {
        bool held = false;
        for (auto& b : barbs) if (b.jobKind == jobStall) held = true;
        if (!held) stallClaimed = false;
    }
    if (!buildOneOff(barbs, squares, stallClaimed, jobStall, stallPos + stallStandOffset, "MARKET STALL",
                     [](const barb&) { return 1; })) return;
    hasStall = true;
    stallClaimed = false;
    printf("---MARKET STALL BUILT--- traders take +%d lots\n", stallExtraLots);
    logEvent("Market stall built (+" + to_string(stallExtraLots) + " trade lots)");
}

// Runs after buildstall(). One builder ever, and it must be a dwarf; only once dwarves have built
// forgeMinDwarfHouses houses. Costs wood and stone, with houseStoneCost kept back for the next house.
void buildforge(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, forge& fg) {
    if (fg.built) return;
    bool done = buildOneOff(barbs, squares, fg.claimed, jobForge, forgePos + forgeStandOffset, "FORGE",
        [&](const barb& b) {
            int dwarfHouses = 0;
            for (auto& h : houses) if (h.race == raceDwarf) dwarfHouses++;
            if (dwarfHouses < forgeMinDwarfHouses || !canAfford(stockOfRace(raceDwarf), forgeWoodCost, forgeStoneCost + houseStoneCost)) return -1;
            if (b.race != raceDwarf) return 0;
            spend(stockOf(b), forgeWoodCost, forgeStoneCost);   // RACE-F1: dwarf stock only
            return 1;
        });
    if (!done) return;
    fg.built = true;
    printf("---FORGE BUILT--- (%d wood, %d stone)\n", forgeWoodCost, forgeStoneCost);
    logEvent("Dwarves light a forge");
}

// 7x5 slate forge: darker roof row, hearth mouth with a 1px breathing ember, 2x2 iron anvil.
// The ember glows hotter for ~2s after a dwarf mines a rock. Drawn with the shrines, under barbs and trees.
// POL-F5b: the forge stone bonus is pending while a dwarf it applies to (forgeActive) is hauling stone
bool forgeBonusPending(const vector<barb>& barbs, const forge& fg) {
    for (auto& b : barbs) if (b.carryKind == carryStone && forgeActive(fg, b.race)) return true;
    return false;
}

// POL-F5b: 3-4 sparks out of the hearth mouth into free pool slots; none spawn if the pool is full
void spawnForgeSparks() {
    uniform_int_distribution<int> count(forgeSparkMin, forgeSparkMax);
    uniform_real_distribution<float> rise(forgeSparkRiseMin, forgeSparkRiseMax), drift(-forgeSparkDriftMax, forgeSparkDriftMax);
    int n = count(gen);
    for (auto& s : forgeSparks) {
        if (n <= 0) break;
        if (s.age < forgeSparkFrames) continue;
        s.pos = forgePos + forgeSparkOrigin;
        s.vel = Vector2f(drift(gen), -rise(gen));
        s.age = 0;
        n--;
    }
}

// POL-F5b: 1px sparks, rising and fading; aged per drawn frame (not while paused)
void drawForgeSparks(RenderWindow& window, bool paused) {
    RectangleShape r(Vector2f(1.0f, 1.0f));
    for (auto& s : forgeSparks) {
        if (s.age >= forgeSparkFrames) continue;
        Color c = forgeSparkColor;
        c.a = (uint8_t)(forgeSparkMaxAlpha * (1.0f - (float)s.age / forgeSparkFrames));
        r.setFillColor(c);
        r.setPosition(snapPx(s.pos));
        window.draw(r);
        if (!paused) { s.pos += s.vel; s.age++; }
    }
}

// POL-F5b: bonusPending (forgeBonusPending) keeps the ember in the hot range while bonus stone is on its way
// R47-F7: smelting (forgeSmelting) adds a thin ash-grey wisp: smeltWispPuffs staggered 1px puffs rising over the hearth
void drawForge(RenderWindow& window, const forge& fg, int frame, bool bonusPending, bool smelting) {
    if (!fg.built) return;
    RectangleShape r;
    for (auto& p : forgeParts) {
        r.setSize(p.size);
        r.setPosition(forgePos + p.pos);
        r.setFillColor(p.color);
        window.draw(r);
    }
    float t = 0.5f + 0.5f * sin(frame * 6.2831853f / forgeEmberBreathFrames);
    Color lo = forgeEmberDim, hi = forgeEmberLit;
    if (forgeEmberFrames > 0 || bonusPending) { lo = forgeEmberLit; hi = forgeEmberHot; }
    auto mix = [t](uint8_t a, uint8_t c) { return (uint8_t)(a + (c - a) * t); };
    r.setSize(Vector2f(1.0f, 1.0f));
    r.setPosition(forgePos + forgeEmberPos);
    r.setFillColor(Color(mix(lo.r, hi.r), mix(lo.g, hi.g), mix(lo.b, hi.b)));
    window.draw(r);
    if (!smelting) return;
    for (int k = 0; k < smeltWispPuffs; k++) {   // R47-F7: each puff climbs and fades, drifting a pixel side to side
        int age = (frame + k * smeltWispFrames / smeltWispPuffs) % smeltWispFrames;
        float u = (float)age / smeltWispFrames;
        float sway = (age / (smeltWispFrames / 3)) % 2 == 0 ? 0.0f : 1.0f;
        Color c = smeltWispColor;
        c.a = (uint8_t)(smeltWispMaxAlpha * (1.0f - u));
        r.setFillColor(c);
        r.setPosition(snapPx(forgePos + smeltWispOrigin + Vector2f(sway, -u * smeltWispRisePx)));
        window.draw(r);
    }
}

// R29-F1: runs after buildforge(). One storehouse per race, built through buildOneOff() by a barb of that race once the
// race has a house of its own. All share jobStore, so only one is under construction at a time; if its builder is lost
// before finishing (no barb holds jobStore), the claim is released.
void buildstorehouses(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, storehouse (&stores)[raceCount],
                      const fieldGrid& fields) {
    bool held = false;
    for (auto& b : barbs) if (b.jobKind == jobStore) held = true;
    int active = -1;
    for (int r = 0; r < raceCount; r++) {
        if (stores[r].built) continue;
        if (stores[r].claimed && !held) stores[r].claimed = false;
        if (stores[r].claimed) active = r;
    }
    for (int r = 0; r < raceCount; r++) {
        storehouse& st = stores[r];
        if (st.built || (active >= 0 && r != active)) continue;
        bool done = buildOneOff(barbs, squares, st.claimed, jobStore, storehousePos[r] + storehouseStandOffset, "STOREHOUSE",
            [&](const barb& b) {
                int raceHouses = 0;
                for (auto& h : houses) if (h.race == r) raceHouses++;
                if (raceHouses < storehouseMinRaceHouses || !canAfford(stock[r], storehouseWoodCost, 0)) return -1;
                if (b.race != r) return 0;
                spend(stock[r], storehouseWoodCost, 0);   // RACE-F1: the race's own wood
                return 1;
            });
        if (done) {
            st.built = true;
            st.claimed = false;
            printf("---STOREHOUSE BUILT--- %s (%d wood)\n", raceDefs[r].name, storehouseWoodCost);
            seedQuarterTrail(r, houses, fields);   // R43-F1: crate placed -> seed its trail to the nearest house, once
            for (auto& h : houses)                 // R44-F4a: and to each homestead built before the crate
                if (h.race == r && isHomestead(h)) { Vector2f c(h.x + 2.0f, h.y + 2.0f); seedQuarterTrail(r, houses, fields, &c); }
            logEvent(string(racePlurals[r]) + " build a storehouse");
        }
        if (st.claimed) return;   // one under construction at a time
    }
}

// R34-F1: runs after buildstorehouses(). Each race with a standing crate builds one shaft at its fixed spot, through
// buildOneOff() by a barb of that race (onlyRace), paying shaftWoodCost from its own wood. The claim follows the mill's
// path: held only while its builder (by id, same race) still holds jobShaft. Races build independently.
void buildshafts(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    for (int r = 0; r < raceCount; r++) {
        mineShaft& ms = mineShafts[r];
        if (ms.built || !stores[r].built) continue;
        if (ms.claimed) {
            int i = findBarbById(barbs, ms.builderId);
            if (i < 0 || barbs[i].jobKind != jobShaft || barbs[i].race != r) {
                printf("SHAFT CLAIM RELEASED - %s, builder %d lost\n", racePlurals[r], ms.builderId);
                ms.claimed = false;
                ms.builderId = -1;
            }
        }
        bool done = buildOneOff(barbs, squares, ms.claimed, jobShaft, shaftWorkSpot(r), "MINE SHAFT",
            [&](const barb& b) {
                if (!lakeClear(shaftZone(r)) || !canAfford(stock[r], shaftWoodCost + houseWoodCost, 0)) return -1;   // a house's wood first
                spend(stock[r], shaftWoodCost, 0);
                ms.builderId = b.id;
                return 1;
            }, r);
        if (done) {
            ms.built = true;
            ms.claimed = false;
            ms.builderId = -1;
            printf("---MINE SHAFT BUILT--- %s, seam of %d stone (%d wood)\n", racePlurals[r], ms.seam, shaftWoodCost);
            logEvent(string(racePlurals[r]) + " sink a mine shaft");
        }
    }
}

// R34-F1: 12x12 timber frame (2px posts, 1px sill), dark pit, 2px lintel, and a 1px grey seam bar above on a dark
// track. The pit eases faintly toward amber while a chopping barb of that race is within shaftTintRadius of the stand
// spot. Exhausted: greyed frame, dimmed lintel, flat black pit, empty track. World layer, after the crates.
void drawMineShafts(RenderWindow& window, const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    RectangleShape frame(shaftSize), pit(Vector2f(shaftSize.x - 2 * shaftPostW, shaftSize.y - shaftLintelH - 1.0f));
    RectangleShape lintel(Vector2f(shaftSize.x, shaftLintelH)), seam(Vector2f(shaftSize.x, shaftSeamH));
    for (int r = 0; r < raceCount; r++) {
        const mineShaft& ms = mineShafts[r];
        if (!ms.built) continue;
        bool dead = ms.seam <= 0;
        bool worked = false;
        for (int i = 0; !dead && !worked && i < (int)barbs.size() && i < (int)squares.size(); i++) {
            const barb& b = barbs[i];
            if (b.race != r || !b.chopping || barbHidden(b)) continue;
            Vector2f d = squares[i].getPosition() - shaftWorkSpot(r);
            worked = d.x * d.x + d.y * d.y <= shaftTintRadius * shaftTintRadius;
        }
        Vector2f p = shaftPos(r);
        frame.setPosition(p);
        frame.setFillColor(dead ? shaftDeadFrameColor : shaftFrameColor);
        window.draw(frame);
        pit.setPosition(p + Vector2f(shaftPostW, shaftLintelH));
        pit.setFillColor(dead ? shaftDeadPitColor : worked ? lerpColor(shaftPitColor, shaftAmberColor, shaftTintMix) : shaftPitColor);
        window.draw(pit);
        lintel.setPosition(p);
        Color lc = dead ? shaftDeadLintelColor : shaftLintelColor;
        if (ms.flashTicks > 0) lc = lerpColor(lc, kShaftFlashColor, kShaftFlashMix * ms.flashTicks / kShaftFlashTicks);   // R40-F1: climb-out
        lintel.setFillColor(lc);
        window.draw(lintel);
        Vector2f sp = p - Vector2f(0.0f, shaftSeamGap + shaftSeamH);
        seam.setSize(Vector2f(shaftSize.x, shaftSeamH));
        seam.setPosition(sp);
        seam.setFillColor(shaftSeamTrackColor);
        window.draw(seam);
        float w = round(shaftSize.x * (float)ms.seam / kShaftCapacity);
        if (w > 0.0f) {
            seam.setSize(Vector2f(w, shaftSeamH));
            seam.setFillColor(shaftSeamColor);
            window.draw(seam);
        }
        RectangleShape puff(Vector2f(kShaftPuffSize, kShaftPuffSize));   // R40-F1: faint dust off the lintel, fading as it rises
        for (const shaftPuff& pf : ms.puffs) {
            if (pf.age < 0) continue;
            float t = (float)pf.age / kShaftPuffLifeTicks;
            Color c = kShaftPuffColor;
            c.a = (uint8_t)(kShaftPuffAlpha * (1.0f - t));
            puff.setFillColor(c);
            puff.setPosition(snapPx(pf.pos - Vector2f(0.0f, kShaftPuffRisePx * t)));
            window.draw(puff);
        }
    }
}

// R40-F1: per sim tick. Counts down each lintel flash, ages puffs, and while a shaft's miner is underground drops a
// new puff every kShaftPuffEveryTicks into a fixed kShaftPuffCap ring (the oldest is overwritten; nothing is erased).
void shaftTick(const vector<barb>& barbs) {
    uniform_int_distribution<int> puffX((int)shaftPostW, (int)(shaftSize.x - shaftPostW - kShaftPuffSize));
    for (int r = 0; r < raceCount; r++) {
        mineShaft& ms = mineShafts[r];
        if (ms.flashTicks > 0) ms.flashTicks--;
        for (shaftPuff& pf : ms.puffs)
            if (pf.age >= 0 && ++pf.age >= kShaftPuffLifeTicks) pf.age = -1;
        int m = ms.built ? shaftMinerOf(barbs, r) : -1;
        if (m < 0 || !barbUnderground(barbs[m])) { ms.puffTimer = 0; continue; }
        if (++ms.puffTimer < kShaftPuffEveryTicks) continue;
        ms.puffTimer = 0;
        shaftPuff& pf = ms.puffs[ms.puffNext];
        pf.pos = shaftPos(r) + Vector2f((float)puffX(gen), -kShaftPuffSize);   // resting on the lintel's top edge
        pf.age = 0;
        ms.puffNext = (ms.puffNext + 1) % kShaftPuffCap;
    }
}

// R29-F1: 10x10 race-coloured crate (R36-F1: lidded, see drawStorehouses). The fill eases toward white
// for a moment after each deposit. Drawn with the forge, under barbs and trees.
// R30-F1: ground layer, the faint dotted quarter rings at zoom >= quarterRingMinZoom (at 1x a 1px dot ring is just
// noise). R43-F1: the crate -> house road is now seeded trail cells (seedQuarterTrail), drawn by drawTrails.
void drawQuarters(RenderWindow& window, float zoom) {
    if (zoom >= quarterRingMinZoom && quarterRingVerts.getVertexCount() > 0) window.draw(quarterRingVerts);
}

// R36-F1: lidded crate via drawParts: a 10px lid lipping over an 8px race-tinted body, a 1px rope band, a ground shadow.
// The deposit flash eases the tint toward white, so lid and body brighten together.
void drawStorehouses(RenderWindow& window, const storehouse (&stores)[raceCount]) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (int r = 0; r < raceCount; r++) {
        const storehouse& st = stores[r];
        if (!st.built) continue;
        float t = storehouseFlashMix * (float)st.flashFrames / storehouseFlashFrames;
        Vector2f o = snapPx(storehousePos[r]);
        drawParts(va, storeShadowParts, o, Color::White);
        drawParts(va, storeBodyParts, o, lerpColor(storehouseColors[r], Color::White, t));
        drawParts(va, storeTrimParts, o, Color::White);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// 7x6 stall: striped 2-row awning on two 1px posts over a 2-row counter. While a trader is Trading on a visit
// the stall enlarged, one 1px crate per bonus lot sits under the awning so the bigger trade reads on screen.
void drawStall(RenderWindow& window, const optional<trader>& tr) {
    if (!hasStall) return;
    RectangleShape r(Vector2f(1.0f, stallAwningH));
    for (int x = 0; x < (int)stallSize.x; x++) {
        r.setPosition(stallPos + Vector2f((float)x, 0.0f));
        r.setFillColor(x % 2 == 0 ? stallAwningColorB : stallAwningColorA);
        window.draw(r);
    }
    float postH = stallSize.y - stallAwningH - stallCounterH;
    r.setSize(Vector2f(1.0f, postH));
    r.setFillColor(stallPostColor);
    r.setPosition(stallPos + Vector2f(0.0f, stallAwningH));
    window.draw(r);
    r.setPosition(stallPos + Vector2f(stallSize.x - 1.0f, stallAwningH));
    window.draw(r);
    r.setSize(Vector2f(stallSize.x, stallCounterH));
    r.setFillColor(stallCounterColor);
    r.setPosition(stallPos + Vector2f(0.0f, stallSize.y - stallCounterH));
    window.draw(r);
    if (!tr || tr->phase != trader::Trading) return;
    r.setSize(Vector2f(1.0f, 1.0f));
    for (int k = 0; k < stallBonusLots && k < stallExtraLots; k++) {
        r.setPosition(stallPos + Vector2f(1.0f + 2.0f * k, stallSize.y - stallCounterH - 1.0f));
        r.setFillColor(k % 2 == 0 ? stallGoodsWoodColor : stallGoodsStoneColor);
        window.draw(r);
    }
}

// FARM-F1: true if a plot zone sits so far inside race r's quarter clearing that no tree or rock can touch it.
// Trees/rocks are removed when their top-left is in the clearing, so the zone grown up/left by a tree footprint must
// lie inside the clearing disc (a rect is inside a disc iff its four corners are). Pure geometry: no tree/rock scan.
bool fieldInClearing(const FloatRect& zone, int r) {
    Vector2f lo = zone.position - Vector2f(treeFootprint, treeFootprint), hi = zone.position + zone.size;
    Vector2f c = quarterCentre(r);
    for (Vector2f p : {lo, Vector2f(hi.x, lo.y), Vector2f(lo.x, hi.y), hi}) {
        Vector2f d = p - c;
        float cr = quarterClearRadiusFor(r);   // R47-F10
        if (d.x * d.x + d.y * d.y > cr * cr) return false;
    }
    return true;
}

// Picks a plot spot once, in a ring around race r's crate (inside its quarter clearing, so far from the campfire
// buildings): clear of every crate and its stand spot, the forge, lakes, the other plots, houses and shrines (standing
// or claimed), and race r's quarter road (crate to nearest house, plus roads to houses still being built).
// Reserved from then on via fieldsClear(); buildhouse() keeps new roads off it.
// FARM-F2: with withMill (the race's first plot), the race's mill spot beside the plot (right, else left) must pass the
// same checks; both are reserved together, so the mill always stands at a fixed offset from its plot.
// R32-F1: plot 0's mill side also reserves the slots of plots k > 0 (fieldSlotPos) that pass zoneOk, stay off road-tier
// trail cells, plot 0, the mill and its door lane; a side where every slot passes is preferred. Plot k > 0 is then
// placed only in its reserved slot (checked again with zoneOk); a blocked or never-reserved slot fails, with no random spot.

// R32-F1: false if zone covers a road-tier (worn or cobbled) trail cell
bool zoneOffRoads(const FloatRect& zone) {
    int x0 = max(0, (int)(zone.position.x / trailCellSize)), y0 = max(0, (int)(zone.position.y / trailCellSize));
    int x1 = min(trailGridN - 1, (int)((zone.position.x + zone.size.x) / trailCellSize));
    int y1 = min(trailGridN - 1, (int)((zone.position.y + zone.size.y) / trailCellSize));
    for (int cy = y0; cy <= y1; cy++)
        for (int cx = x0; cx <= x1; cx++)
            if (trailTier[cy * trailGridN + cx] == 2) return false;
    return true;
}

void setPlotSpot(plot& fp, int r, Vector2f pos) {
    fp.placed = true;
    fp.reserved = false;   // the placed spot holds the zone from now on
    fp.fallback = false;   // R37-F1: re-sited (or never needed to)
    fp.rect = FloatRect(pos, fieldSize);
    fp.shape.setSize(fieldSize);
    fp.shape.setPosition(pos);
    fp.shape.setFillColor(fieldSoilColor);
    fp.shape.setOutlineColor(fieldRaceTints[r]);
    fp.shape.setOutlineThickness(fieldBorderPx);
}

bool placeField(fieldGrid& fields, int r, int kPlot, const vector<barb>& barbs, const vector<house>& houses,
                const vector<shrine>& shrines, bool withMill) {
    plot& fp = fields[r][kPlot];
    uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(fieldStoreMinRadius, fieldStoreMaxRadius);
    Vector2f qc = quarterCentre(r), road;
    bool hasRoad = nearestQuarterHouse(r, houses, road);
    auto zoneOk = [&](const FloatRect& zone) {
        if (!fieldInClearing(zone, r) || !forgeClear(zone) || !lakeClear(zone) || !fieldsClear(fields, zone)) return false;
        bool ok = !hasRoad || !segmentHitsRect(qc, road, zone);
        for (int i = 0; ok && i < (int)houses.size(); i++)
            if (zone.findIntersection(FloatRect(Vector2f(houses[i].x - 3.0f, houses[i].y - 3.0f), houseSlabSize))) ok = false;
        for (auto& b : barbs) {   // houses (and their future roads) and shrines still being walked to / built
            if (!ok) break;
            if (b.movingbspot && b.jobKind == jobNone) {
                if (zone.findIntersection(FloatRect(Vector2f(b.targetx - 3.0f, b.targety - 3.0f), houseSlabSize))) ok = false;
                if (b.race == r && segmentHitsRect(qc, Vector2f(b.targetx + 2.0f, b.targety + 2.0f), zone)) ok = false;
            }
            if (b.jobKind == jobShrine
                && zone.findIntersection(FloatRect(shrineSlabPos(b.targetx, b.targety) - Vector2f(0.0f, shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH)))) ok = false;
        }
        for (auto& s : shrines)
            if (ok && zone.findIntersection(FloatRect(Vector2f((float)s.x, (float)s.y - shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH)))) ok = false;
        return ok;
    };
    // R37-F1: zone keeps off plot 0's zone, the mill zone and the mill's door lane (shared by slotOk and the fallback ring)
    auto farmsteadOk = [&](const FloatRect& zone, Vector2f plot0Pos, Vector2f millPos) {
        return !zone.findIntersection(fieldZone(plot0Pos)) && !zone.findIntersection(millZone(millPos))
            && !zone.findIntersection(millDoorLane(millPos));
    };
    if (kPlot > 0) {   // R32-F1: the slot reserved beside the mill; R37-F1: or, once given up, the fallback ring
        if (fp.reserved && !fp.fallback) {
            fp.reserved = false;   // own reservation off while checking, so fieldsClear() doesn't see it
            FloatRect zone = fieldZone(fp.slotPos);
            if (zoneOk(zone) && zoneOffRoads(zone)) {
                setPlotSpot(fp, r, fp.slotPos);
                printf("FIELD SPOT chosen for the %s at %d,%d (mill slot %d)\n", racePlurals[r], (int)fp.slotPos.x, (int)fp.slotPos.y, kPlot);
                return true;
            }
            fp.reserved = true;    // restored on every failed check
            fp.slotFails++;
            if (!fp.slotLogged) printf("FIELD SLOT BLOCKED - %s plot %d at %d,%d, retrying every %d ticks\n", racePlurals[r],
                                       kPlot + 1, (int)fp.slotPos.x, (int)fp.slotPos.y, fieldSlotRetryTicks);
            fp.slotLogged = true;
            if (fp.slotFails < fieldSlotMaxFails) { fp.retryTicks = fieldSlotRetryTicks; return false; }
            fp.reserved = false;   // given up: the slot is released and the plot re-sites on the ring below
            fp.fallback = true;
            printf("FIELD SLOT GIVEN UP - %s plot %d after %d blocked checks, re-siting near the mill\n", racePlurals[r],
                   kPlot + 1, fp.slotFails);
        }
        const plot& p0 = fields[r][0];
        if (!fp.fallback || !mills[r].placed || !p0.placed) return false;
        Vector2f hub = mills[r].pos + millHubOffset;
        uniform_real_distribution<float> frad(fieldFallbackMinRadius, fieldFallbackMaxRadius);
        for (int t = 0; t < fieldPlaceTries; t++) {
            float a = ang(gen), d = frad(gen);
            Vector2f pos(round(hub.x + cos(a) * d - fieldSize.x / 2), round(hub.y + sin(a) * d - fieldSize.y / 2));
            FloatRect zone = fieldZone(pos);   // zoneOk: clearing, forge, lakes, fieldsClear (other plots, mills), roads
            if (!zoneOk(zone) || !zoneOffRoads(zone) || !farmsteadOk(zone, p0.rect.position, mills[r].pos)) continue;
            setPlotSpot(fp, r, pos);
            printf("FIELD SPOT chosen for the %s at %d,%d (fallback ring, %d px from the mill hub)\n", racePlurals[r],
                   (int)pos.x, (int)pos.y, (int)d);
            return true;
        }
        fp.retryTicks = fieldSlotRetryTicks;   // no ring spot yet: try again later, not every tick
        return false;
    }
    // R32-F1: slot k beside a mill at millPos (plot 0 at pos) is usable if it passes zoneOk, avoids the roads, plot 0's
    // zone, the mill zone, the door lane and the slots before it
    auto slotOk = [&](Vector2f pos, Vector2f millPos, int side, int k) {
        FloatRect zone = fieldZone(fieldSlotPos(millPos, side, k));
        if (!zoneOk(zone) || !zoneOffRoads(zone) || !farmsteadOk(zone, pos, millPos)) return false;
        for (int j = 1; j < k; j++)
            if (zone.findIntersection(fieldZone(fieldSlotPos(millPos, side, j)))) return false;
        return true;
    };
    int bestSide = 0, bestSlots = -1;
    Vector2f bestPos;
    for (int k = 0; k < fieldPlaceTries && bestSlots < fieldsPerRace - 1; k++) {
        float a = ang(gen), d = rad(gen);
        Vector2f pos(round(qc.x + cos(a) * d - fieldSize.x / 2), round(qc.y + sin(a) * d - fieldSize.y / 2));
        if (!zoneOk(fieldZone(pos))) continue;
        if (!withMill) { bestPos = pos; bestSlots = 0; break; }
        for (int s : {1, -1}) {
            Vector2f mp = millSpotBeside(pos, s);
            if (!zoneOk(millZone(mp))) continue;
            int n = 0;
            for (int j = 1; j < fieldsPerRace; j++) if (slotOk(pos, mp, s, j)) n++;
            if (n > bestSlots) { bestSlots = n; bestSide = s; bestPos = pos; }
        }
    }
    if (bestSlots < 0) return false;
    Vector2f pos = bestPos;
    if (withMill) {
        mills[r].placed = true;
        mills[r].pos = millSpotBeside(pos, bestSide);
        printf("MILL SPOT chosen for the %s at %d,%d (%s of the plot)\n", racePlurals[r],
               (int)mills[r].pos.x, (int)mills[r].pos.y, bestSide > 0 ? "right" : "left");
        for (int j = 1; j < fieldsPerRace; j++) {   // reserve the extra plots' slots now, with plot 0 and the mill
            plot& sp = fields[r][j];
            if (sp.placed) continue;
            if (!slotOk(pos, mills[r].pos, bestSide, j)) {   // R37-F1: re-sites on the mill's ring instead of never sowing
                sp.reserved = false;
                sp.fallback = true;
                printf("FIELD SLOT %d unavailable for the %s: that plot will re-site near the mill\n", j, racePlurals[r]);
                continue;
            }
            sp.slotPos = fieldSlotPos(mills[r].pos, bestSide, j);
            sp.reserved = true;
            printf("FIELD SLOT %d reserved for the %s at %d,%d\n", j, racePlurals[r], (int)sp.slotPos.x, (int)sp.slotPos.y);
        }
    }
    setPlotSpot(fp, r, pos);
    printf("FIELD SPOT chosen for the %s at %d,%d\n", racePlurals[r], (int)pos.x, (int)pos.y);
    return true;
}

// Runs after buildstorehouses(). Each race sows its own plots once it has a house and a standing storehouse; only a
// barb of that race may claim one (jobField through buildOneOff(), which clears it on completion). All plots share
// jobField, so only one is being sown at a time. A claim is held only while its own sower (by id, same race) still
// holds jobField; otherwise it is released so another barb of that race can take it.
// R46-F2a: the one plot gate. Race r may sow max(home unlock, raceBarbs / popPerPlot) plots, capped at fieldsPerRace
// so food stays earned. Nothing opens before the race's first home. Kept as a high-water mark in plotsOpen[r], so a
// shrinking race (or a lost home) never closes a plot; each growth past one plot logs one "Farmstead expanded" line.
int plotsUnlocked(int r, int raceHouses, int raceBarbs) {
    if (raceHouses >= fieldMinHouses) {
        int byHouses = (raceHouses - 1) / fieldMinHouses + 1;   // plot k at fieldMinHouses*k+1 homes (R32-F1)
        int n = min(fieldsPerRace, max(byHouses, raceBarbs / popPerPlot));
        if (n > plotsOpen[r]) {
            if (n > 1) {
                printf("FARMSTEAD EXPANDED - %s now %d/%d plots (homes %d, barbs %d)\n",
                       racePlurals[r], n, fieldsPerRace, raceHouses, raceBarbs);
                logRaceEvent(string(racePlurals[r]) + ": Farmstead expanded (" + to_string(n) + " plots)", r);
            }
            plotsOpen[r] = n;
        }
    }
    return plotsOpen[r];
}

void buildfield(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, const vector<shrine>& shrines,
                const storehouse (&stores)[raceCount], fieldGrid& fields) {
    int raceHouses[raceCount] = {};
    for (auto& h : houses) if (h.race >= 0 && h.race < raceCount) raceHouses[h.race]++;
    int raceBarbs[raceCount] = {};   // R46-F2a: counted once per tick
    for (auto& b : barbs) if (b.race >= 0 && b.race < raceCount) raceBarbs[b.race]++;
    int unlocked[raceCount];
    for (int r = 0; r < raceCount; r++) unlocked[r] = plotsUnlocked(r, raceHouses[r], raceBarbs[r]);
    int activeRace = -1, activeK = -1;
    for (int r = 0; r < raceCount; r++)
        for (int k = 0; k < fieldsPerRace; k++) {
            plot& fp = fields[r][k];
            if (fp.retryTicks > 0) fp.retryTicks--;   // R37-F1: per-plot countdown to the next slot / fallback attempt
            if (fp.built || !fp.claimed) continue;
            int i = findBarbById(barbs, fp.sowerId);
            if (i < 0 || barbs[i].jobKind != jobField || barbs[i].race != r) {   // per race: no other barb keeps this claim
                printf("FIELD CLAIM RELEASED - %s plot %d, sower %d lost\n", racePlurals[r], k, fp.sowerId);
                fp.claimed = false;
                fp.sowerId = -1;
                continue;
            }
            activeRace = r;
            activeK = k;
        }
    for (int r = 0; r < raceCount; r++)
        for (int k = 0; k < fieldsPerRace; k++) {
            plot& fp = fields[r][k];
            if (fp.built || (activeRace >= 0 && (r != activeRace || k != activeK))) continue;
            bool plotOpen = k < unlocked[r];   // R46-F2a: plotsUnlocked() is the only plot-count gate
            if (!fp.placed) {
                // R37-F1: the first gate holding the plot back; the slot gate also covers the wait between retries
                int gate = !stores[r].built ? 1 : !plotOpen ? 2 : !canAfford(stock[r], fieldWoodCost, 0) ? 4
                         : (k > 0 && !mills[r].placed) ? 8 : 0;   // R32-F1: extra plots sit beside the mill
                if (gate == 0 && (fp.retryTicks > 0 || !placeField(fields, r, k, barbs, houses, shrines, k == 0 && !mills[r].placed)))
                    gate = 8;
                if (gate) {
                    // logged once per plot and gate; extra plots only, once plot 0 is sown (plot 0's start-up waits are normal)
                    if (k > 0 && fields[r][0].built && !(fp.stallMask & gate)) {
                        fp.stallMask |= gate;
                        const char* why = gate == 1 ? "store" : gate == 2 ? "homes/pop" : gate == 4 ? "wood" : "slot";
                        printf("FIELD STALLED - %s plot %d waits on %s (plots open %d/%d, wood %d/%d, store %s, slot %s)\n",
                               racePlurals[r], k + 1, why, unlocked[r], k + 1, stock[r].wood, fieldWoodCost,
                               stores[r].built ? "built" : "none",
                               fp.fallback ? "re-siting" : fp.reserved ? "reserved" : "none");
                    }
                    continue;
                }
            }
            bool done = buildOneOff(barbs, squares, fp.claimed, jobField, fieldWorkSpot(fp), "FIELD",
                [&](const barb& b) {
                    if (!plotOpen || !canAfford(stock[r], fieldWoodCost, 0)) return -1;
                    if (b.race != r) return 0;   // only that race sows its plot
                    spend(stock[r], fieldWoodCost, 0);   // RACE-F1: the race's own wood
                    fp.sowerId = b.id;
                    return 1;
                });
            if (done) {
                fp.built = true;
                fp.claimed = false;
                fp.sowerId = -1;
                replantField(fields[r], k);   // R32-F1: may lie fallow first, so it ripens out of step with its siblings
                printf("---FIELD SOWN--- %s plot %d, ripe in %d ticks (+%d fallow), +%d food per harvest\n", racePlurals[r], k + 1,
                       wheatGrowTicks(), fp.fallow, fieldFoodYield);
                logEvent(string(racePlurals[r]) + " sow a wheat field");
            }
            if (fp.claimed) return;   // one being sown at a time
        }
}

// FARM-F2: runs after buildfield(). Each race builds one mill at the spot reserved beside its first plot, once that plot
// is sown; only a barb of that race may claim it (jobMill through buildOneOff()). All mills share jobMill, so one is under
// construction at a time. A claim is held only while its builder (by id, same race) still holds jobMill. O(barbs x races).
void buildmills(vector<barb>& barbs, vector<RectangleShape>& squares, const fieldGrid& fields) {
    int active = -1;
    for (int r = 0; r < raceCount; r++) {
        mill& m = mills[r];
        if (m.built || !m.claimed) continue;
        int i = findBarbById(barbs, m.builderId);
        if (i < 0 || barbs[i].jobKind != jobMill || barbs[i].race != r) {
            printf("MILL CLAIM RELEASED - %s, builder %d lost\n", racePlurals[r], m.builderId);
            m.claimed = false;
            m.builderId = -1;
            continue;
        }
        active = r;
    }
    for (int r = 0; r < raceCount; r++) {
        mill& m = mills[r];
        if (m.built || !m.placed || !fields[r][0].built || (active >= 0 && r != active)) continue;
        bool done = buildOneOff(barbs, squares, m.claimed, jobMill, millWorkSpot(m), "MILL",
            [&](const barb& b) {
                if (!canAfford(stock[r], millWoodCost, millStoneCost + houseStoneCost)) return -1;
                if (b.race != r) return 0;   // only that race builds its mill
                spend(stock[r], millWoodCost, millStoneCost);   // RACE-F1: the race's own stock
                m.builderId = b.id;
                return 1;
            });
        if (done) {
            m.built = true;
            m.claimed = false;
            m.builderId = -1;
            printf("---MILL BUILT--- %s, +%d food per milled sheaf (%d wood, %d stone)\n", racePlurals[r], millFoodYield, millWoodCost, millStoneCost);
            logEvent(string(racePlurals[r]) + " build a windmill");
        }
        if (m.claimed) return;   // one under construction at a time
    }
}

// POL-F3: the single refund path. Every abort of a paid spear craft (death, interrupt, reassignment) ends here;
// returns the up-front cost to the crafter's race stock exactly once. No-op if nothing is owed.
// R47-F8: only ever called from setWeapon(weaponRefund / weaponDrop).
void refundSpear(barb& b) {
    if (!b.spearPaid) return;
    b.spearPaid = false;
    raceStock& rs = stockOf(b);
    rs.wood += spearRefundWood;
    rs.stone += spearRefundStone;
    printf("SPEAR REFUNDED - barb %d, %s +%d wood +%d stone (%s)\n", b.id, racePlurals[clampRace(b.race)], spearRefundWood, spearRefundStone, weaponOf(b).name);
    logRaceEvent(string(b.name) + "'s " + weaponOf(b).name + " refunded", b.race);
}

// R47-F8: the one equip/unequip helper. Every write of hasSpear/spearPaid/weaponTier outside makeBarb() goes through here.
//   weaponPay    - craft claimed: take the cost from the race stock up front (spearPaid)
//   weaponEquip  - craft finished: the cost is consumed, the barb holds its race's weapon at the race's current tool tier
//   weaponRefund - craft aborted: refundSpear() returns the cost if still owed; a held weapon is untouched
//   weaponDrop   - barb leaves (death/removal): refund any owed cost and unarm
enum weaponChange { weaponPay, weaponEquip, weaponRefund, weaponDrop };
void setWeapon(barb& b, weaponChange c) {
    switch (c) {
        case weaponPay:
            spend(stockOf(b), spearWoodCost, spearStoneCost);
            b.spearPaid = true;
            break;
        case weaponEquip:
            b.spearPaid = false;   // POL-F3: paid at claim; finishing consumes it, so a later refund returns nothing
            b.hasSpear = true;
            b.weaponTier = toolTierOf(b.race);   // read once, here; kept for life
            break;
        case weaponRefund:
            refundSpear(b);
            break;
        case weaponDrop:
            refundSpear(b);
            b.hasSpear = false;
            b.weaponTier = 0;
            break;
    }
}

// POL-F3: weapons (spears, dwarf axes) held or being made by race r (the per-race cap counts both)
int weaponsOfRace(const vector<barb>& barbs, int r) {
    int n = 0;
    for (auto& b : barbs)
        if (b.race == r && (b.hasSpear || b.jobKind == jobSpear)) n++;
    return n;
}

// R20-F3: every way out of a spear craft (finished, interrupted, removed) clears the same barb fields.
// POL-F3: refunds via refundSpear() first; the finish path clears spearPaid beforehand so nothing is returned.
void clearSpearCraft(barb& b) {
    setWeapon(b, weaponRefund);   // R47-F8
    b.jobKind = jobNone;
    b.moving = b.movingbspot = b.building = false; clearRoute(b);
    b.spearTimer = 0;
    b.spearAtForge = false;   // POL-F5a
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    dropCarry(b);   // R29-F1
}

// Runs after buildfield(). One crafter at a time: an idle barb without a spear walks to the campfire and knaps for
// spearCraftTicks (in spearTimer, never timer). POL-F3: spears (held + in progress) are capped per race at 1 per
// spearBarbsPerSpear of its barbs; the race with the fewest spears per barb goes first. The cost is paid at claim
// (spearPaid) and any abort is refunded by refundSpear(); completion never needs materials.
// POL-F5a: dwarves with a built forge (forgeActive) knap at the forge stand spot in spearForgeTicks instead.
void craftspear(vector<barb>& barbs, vector<RectangleShape>& squares, const forge& fg) {
    int crafter = -1;
    for (int i = 0; i < (int)barbs.size(); i++) {
        if (barbs[i].jobKind == jobSpear) crafter = i;
        else if (barbs[i].spearPaid) setWeapon(barbs[i], weaponRefund);   // reassigned without going through clearSpearCraft
    }
    Vector2f stand = villageCenter + spearStandOffset;
    if (crafter < 0) {
        int members[raceCount] = {}, spears[raceCount] = {};
        for (auto& b : barbs) members[clampRace(b.race)]++;
        for (int r = 0; r < raceCount; r++) spears[r] = weaponsOfRace(barbs, r);
        bool tried[raceCount] = {};
        for (int pass = 0; pass < raceCount; pass++) {
            int pick = -1;   // fewest spears per barb among races under their cap that can pay
            for (int r = 0; r < raceCount; r++) {
                if (tried[r] || members[r] == 0) continue;
                if ((spears[r] + 1) * spearBarbsPerSpear > members[r]) continue;
                if (!canAfford(stockOfRace(r), spearWoodCost, spearStoneCost)) continue;
                if (pick < 0 || spears[r] * members[pick] < spears[pick] * members[r]) pick = r;
            }
            if (pick < 0) return;
            tried[pick] = true;
            for (auto& b : barbs) {
                if (!barbAvailable(b) || b.race != pick || b.hasSpear || b.jobKind != jobNone || b.deertargeting || b.chopping || b.timer > 0
                    || b.moving || b.movingbspot || b.building) continue;
                setWeapon(b, weaponPay);   // POL-F3: paid up front by the crafter's race (R47-F8: via the helper)
                clearTraining(b);          // R47-F6
                b.jobKind = jobSpear;
                b.target = targetNone;
                b.spearAtForge = forgeActive(fg, pick);   // POL-F5a
                Vector2f spot = b.spearAtForge ? forgePos + forgeStandOffset : stand;
                b.targetx = (int)spot.x;
                b.targety = (int)spot.y;
                b.moving = b.movingbspot = true;
                b.spearTimer = 0;
                printf("SPEAR CLAIMED - barb %d, %s %d/%d %s (paid %d wood, %d stone)%s\n", b.id, racePlurals[pick],
                       spears[pick] + 1, members[pick] / spearBarbsPerSpear, weaponOf(b).plural, spearWoodCost, spearStoneCost,
                       b.spearAtForge ? " at the forge" : "");
                logRaceEvent(string(b.name) + " begins " + weaponOf(b).article + " " + weaponOf(b).name, b.race);
                return;
            }
        }
        return;
    }
    barb& b = barbs[crafter];
    auto& sq = squares[crafter];
    if (!b.movingbspot) { clearSpearCraft(b); return; }   // interrupt: something else took the barb; clearSpearCraft refunds
    if (b.spearAtForge && !forgeActive(fg, b.race)) {      // POL-F5a: forge gone mid-craft -> refund (via refundSpear) and free the barb
        printf("SPEAR ABORTED - barb %d, forge lost\n", b.id);
        clearSpearCraft(b);   // refundSpear() + clears moving/movingbspot/building
        b.chopping = false;
        return;
    }
    Vector2f pos  = sq.getPosition();
    Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq >= 1.0f) {
        walkStep(b, sq);   // R39-F1
        return;
    }
    b.moving = false; clearRoute(b);   // standing at the fire: no walking bob while knapping
    if (workTick(b)) b.spearTimer++;
    if (b.spearTimer < (b.spearAtForge ? spearForgeTicks : spearCraftTicks)) return;
    setWeapon(b, weaponEquip);   // R47-F8: consumes the cost (so clearSpearCraft refunds nothing) and snapshots the tier
    clearSpearCraft(b);
    printf("---SPEAR MADE--- barb %d, %s, %s\n", b.id, racePlurals[clampRace(b.race)], weaponLabel(b).c_str());
    logRaceEvent(string(b.name) + " made " + weaponOf(b).article + " " + weaponLabel(b), b.race);
}

void fieldTick(fieldGrid& fields) {
    for (int r = 0; r < raceCount; r++)
        for (auto& fp : fields[r])
            if (fp.built && fp.fallow > 0) fp.fallow--;   // R32-F1: resting before it grows again
            else if (fp.built && fp.growth < wheatGrowTicks()) {
                fp.growth++;
                if (fp.growth == wheatGrowTicks()) printf("WHEAT RIPE - %s\n", racePlurals[r]);
            }
}

// Soil with a dim 1px race-tinted frame; inside, three 6px crop rows (1px soil margin all round). Stalks sit on y = 2/4/6,
// heads on y = 1/3/5. Sprouts -> green rows -> yellowing heads -> ripe gold with one ear bobbing above the frame.
// FARM-F1: one race-tinted pixel marks the stand spot below the frame (hidden under the reaper while it works).
// Drawn with the village structures, under barbs and trees.
void drawField(RenderWindow& window, const plot& fp, int race, int frame) {
    if (!fp.built) return;
    drawSnapped(window, fp.shape);
    Vector2f o = snapPx(fp.rect.position);
    int stage = fieldStage(fp);
    const float rowW = fieldSize.x - 2.0f;
    RectangleShape r(Vector2f(1.0f, 1.0f));
    r.setFillColor(fieldRaceTints[race]);
    r.setPosition(snapPx(fieldWorkSpot(fp)));
    window.draw(r);
    if (fp.fallow > 0) return;   // R32-F1: fallow plot is bare soil in its frame
    for (int row = 0; row < 3; row++) {
        float stalkY = 2.0f + 2.0f * row, headY = stalkY - 1.0f;
        if (stage == 0) {                           // staggered 1px sprouts
            r.setSize(Vector2f(1.0f, 1.0f));
            r.setFillColor(fieldSproutColor);
            for (float x = 1.0f + (row % 2); x < 1.0f + rowW; x += 2.0f) {
                r.setPosition(o + Vector2f(x, stalkY));
                window.draw(r);
            }
            continue;
        }
        bool ripe = stage == fieldGrowStages;
        r.setSize(Vector2f(rowW, 1.0f));
        r.setFillColor(ripe ? shade(fieldRipeColor, fieldRipeStalkShade) : fieldStalkColor);
        r.setPosition(o + Vector2f(1.0f, stalkY));
        window.draw(r);
        if (stage == 1) continue;
        r.setFillColor(ripe ? fieldRipeColor : fieldHeadColor);
        r.setPosition(o + Vector2f(1.0f, headY));
        window.draw(r);
    }
    if (stage != fieldGrowStages) return;
    bool up = (frame / fieldBobFrames) % 2 == 0;    // derived from the frame and ripe state only
    r.setSize(Vector2f(1.0f, 1.0f));
    r.setFillColor(fieldBobColor);
    r.setPosition(o + Vector2f(floor(fieldSize.x / 2), -fieldBorderPx - (up ? 3.0f : 2.0f)));
    window.draw(r);
}

// FARM-F2: 10x10 grey-tan tower with a dark inset rim and a dark door, under one sail shape (four 2px blades round a 1px
// hub) that is rotated as a whole and turns only while milling. Drawn with the village structures, under barbs and trees.
void drawMills(RenderWindow& window) {
    RectangleShape body(millBodySize), r;
    body.setFillColor(millBodyColor);
    body.setOutlineThickness(-millOutlinePx);
    body.setOutlineColor(millOutlineColor);
    VertexArray sails(PrimitiveType::Triangles);   // one shape: built once per call around the hub, rotated as a whole
    for (int k = 0; k < 4; k++) {
        float a = 1.5707963f * k;
        Vector2f d(cos(a), sin(a)), n(-d.y, d.x);   // stock direction, and the side the cloth hangs on
        Vector2f p0 = d * millSailInner, p1 = d * millSailLen, p2 = p1 + n * millSailWidth, p3 = p0 + n * millSailWidth;
        for (Vector2f v : {p0, p1, p2, p0, p2, p3}) sails.append(Vertex{v, millSailColor});
    }
    for (int q = 0; q < raceCount; q++) {
        const mill& m = mills[q];
        if (!m.built) continue;
        Vector2f o = snapPx(m.pos);
        body.setPosition(o);
        window.draw(body);
        r.setSize(millDoorSize);
        r.setPosition(o + millDoorPos);
        r.setFillColor(millDoorColor);
        window.draw(r);
        Transform t;
        t.translate(o + millHubOffset + Vector2f(0.5f, 0.5f));   // centre of the hub pixel
        t.rotate(degrees(m.sailAngle));
        window.draw(sails, RenderStates(t));
        r.setSize(Vector2f(1.0f, 1.0f));
        r.setPosition(o + millHubOffset);
        r.setFillColor(millHubColor);
        window.draw(r);
    }
}

// FARM-F2: short faint flour puff at the door after each delivery: a few 1px dots drifting out and up, fading out.
// Drawn above the trees (like the chimney smoke) and under the day/night overlay.
void drawMillPuffs(RenderWindow& window) {
    RectangleShape p(Vector2f(1.0f, 1.0f));
    for (int q = 0; q < raceCount; q++) {
        const mill& m = mills[q];
        if (!m.built || m.puffFrames <= 0) continue;
        float t = 1.0f - (float)m.puffFrames / millPuffFrames;   // 0 -> 1 over the puff
        Color c = millPuffColor;
        c.a = (uint8_t)(millPuffAlpha * (1.0f - t));
        p.setFillColor(c);
        Vector2f door = snapPx(m.pos) + millDoorPos + Vector2f(millDoorSize.x / 2.0f, 0.0f);
        for (int k = 0; k < millPuffDots; k++) {
            float a = 3.14159265f * (0.15f + 0.7f * k / (millPuffDots - 1));   // fanned over the upper half, left to right
            Vector2f at = door + Vector2f(-cos(a), -sin(a) * 0.6f) * (1.0f + millPuffSpreadPx * t);
            p.setPosition(Vector2f(floor(at.x), floor(at.y)));
            window.draw(p);
        }
    }
}

// FARM-F2: per drawn frame (frozen while paused): sails turn while milling, easing to a stop; the puff runs out
void millFrameTick() {
    for (auto& m : mills) {
        if (m.turnFrames > 0) {
            m.sailAngle = fmod(m.sailAngle + millSailDegPerFrame * min(1.0f, (float)m.turnFrames / millTurnEaseFrames), 360.0f);
            m.turnFrames--;
        }
        if (m.puffFrames > 0) m.puffFrames--;
    }
}

struct birthRing {Vector2f pos; int age = 0; int soulRace = -1;};   // R50-F1b: race of the drawn soul, -1 = untinted

int nextBarbId = 0;

// The only place a barb is created: keeps barbs/squares in sync and owns nextBarbId
void makeBarb(Vector2i pos, vector<barb>& barbs, vector<RectangleShape>& squares, vector<birthRing>& rings, int homeHouse, int race) {
    barb b;
    b.id = nextBarbId++;
    b.homeHouse = homeHouse;
    b.x = pos.x;
    b.y = pos.y;
    // R28-F1: race passed in by the caller; -1 (or out of range) rolls one at random
    b.race = (race >= 0 && race < raceCount) ? (barbRace)race
                                             : (barbRace)uniform_int_distribution<int>(0, raceCount - 1)(gen);
    b.hasSpear = false;   // R20-F3: every new barb starts unarmed with no craft in progress
    b.spearTimer = 0;
    b.spearPaid = false;  // POL-F3
    b.spearAtForge = false;   // POL-F5a
    b.weaponTier = 0;         // R47-F8
    b.activity = actNone;   // R53-F4: no shelter, party, raid, forge task, drill, watch or kneel
    b.temperPaid = false;   // R41-F2: no temper in progress, an untempered pick
    b.temperTimer = 0;
    b.toolCharges = 0;
    b.smeltPaid = false;   // R47-F7: no smelt in progress, tier read fresh at the first job
    b.smeltTimer = 0;
    b.jobTier = 0;
    b.drillSlot = -1;      // R47-F6: not drilling, no slot, counter at 0
    b.drillTimer = b.drillHits = b.drillCount = 0;
    b.watchStop = b.watchCooldown = 0;
    b.watchPreyId = -1;
    for (auto& m : b.mem) m = workMem{};   // INBOX-F2: a newborn remembers no spots
    b.memReturn = b.memShared = false;
    b.order = needNone;   // INBOX-F2b: a newborn holds no order
    b.orderTicks = 0;
    b.trait =(barbTrait)uniform_int_distribution<int>(0, traitCount - 1)(gen);   // R25-F1
    if (!sasukeTaken && homeHouse < 0) {   // R25-F2: the first founder is Sasuke (no table name is spent)
        sasukeTaken = true;
        b.shinobi = true;
        b.name = shinobiName;
    } else {   // R25-F1: a random unused name; once all are taken, any name may repeat
        int freeCount = 0;
        for (int k = 0; k < barbNameCount; k++) if (!barbNameUsed[k]) freeCount++;
        int k = 0;
        if (freeCount > 0) {
            int pick = uniform_int_distribution<int>(0, freeCount - 1)(gen);
            for (k = 0; k < barbNameCount; k++) if (!barbNameUsed[k] && pick-- == 0) break;
            barbNameUsed[k] = true;
        } else {
            k = uniform_int_distribution<int>(0, barbNameCount - 1)(gen);
        }
        b.name = barbNames[k];
    }
    barbs.push_back(b);
    Vector2f bodySize = raceOf(b).bodySize;   // hitbox = drawn body, so hover/collision match what you see
    RectangleShape sq;
    sq.setPosition(Vector2f(b.x, b.y));
    sq.setSize(bodySize);
    sq.setFillColor(Color::White);
    squares.push_back(sq);
    rings.push_back({Vector2f(b.x, b.y) + bodySize / 2.0f}); // centred on the body
    printf("BARB BORN - %s the %s (%s)%s\n", b.name, traitOf(b).name, raceOf(b).name, b.shinobi ? " - shinobi" : "");
}

// R25-F2: Sasuke's blink. Puffs live in their own vector, aged per drawn frame and removed in reverse.
struct shinobiPuff {Vector2f pos; int age = 0;};

bool shinobiSpotFree(const FloatRect& r, int self, const vector<RectangleShape>& squares,
                     const vector<RectangleShape>& houseshapes, const vector<CircleShape>& octs) {
    if (r.position.x < 1.0f || r.position.y < hudBarHeight || r.position.x + r.size.x > worldSize - 1.0f || r.position.y + r.size.y > worldSize - 1.0f) return false;
    if (!lakeClear(r) || !forgeClear(r)) return false;
    for (int k = 0; k < (int)squares.size(); k++)
        if (k != self && r.findIntersection(squares[k].getGlobalBounds()).has_value()) return false;
    for (auto& hs : houseshapes) if (r.findIntersection(hs.getGlobalBounds()).has_value()) return false;
    for (auto& o : octs) if (r.findIntersection(o.getGlobalBounds()).has_value()) return false;
    return true;
}

void shinobiTick(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<RectangleShape>& houseshapes,
                 const vector<CircleShape>& octs, vector<shinobiPuff>& puffs) {
    static uniform_int_distribution<int> blinkChance(0, shinobiBlinkOneIn - 1);
    static uniform_int_distribution<int> blinkD(-shinobiBlinkMaxPx, shinobiBlinkMaxPx);
    for (int i = 0; i < (int)barbs.size() && i < (int)squares.size(); i++) {
        barb& b = barbs[i];
        if (!b.shinobi) continue;
        if (!barbAvailable(b) || b.moving || b.chopping || b.building || b.movingbspot || b.deertargeting || b.timer > 0 || b.jobKind != jobNone
            || b.activity == actDrill || b.activity == actWatch || b.activity == actKneel || b.activity == actHerb) continue;   // R47-F6: no blinking off the drill slot (R51-F1: or the watch; R52-F3: or the rite; R53-F2: or the herb bed)
        if (blinkChance(gen) != 0) continue;
        Vector2f from = squares[i].getPosition(), size = squares[i].getSize();
        for (int t = 0; t < shinobiBlinkTries; t++) {
            int dx = blinkD(gen), dy = blinkD(gen);
            if (abs(dx) + abs(dy) < shinobiBlinkMinPx) continue;
            Vector2f to = from + Vector2f((float)dx, (float)dy);
            if (!shinobiSpotFree(FloatRect(to, size), i, squares, houseshapes, octs)) continue;
            puffs.push_back({from + size / 2.0f});
            squares[i].setPosition(to);
            b.stepTimer = 0;
            printf("SHINOBI BLINK - %s %+d,%+d\n", b.name, dx, dy);
            break;
        }
    }
}

void shinobiPuffTick(vector<shinobiPuff>& puffs) {
    for (auto& p : puffs) p.age++;
    for (int i = (int)puffs.size() - 1; i >= 0; i--)
        if (puffs[i].age >= shinobiPuffFrames) swapRemove(puffs, i);
}

void drawShinobiPuffs(RenderWindow& window, const vector<shinobiPuff>& puffs) {
    RectangleShape r(shinobiPuffSize);
    for (auto& p : puffs) {
        Color c = shinobiPuffColor;
        c.a = (uint8_t)(shinobiPuffMaxAlpha * (1.0f - (float)p.age / shinobiPuffFrames));
        r.setFillColor(c);
        r.setPosition(snapPx(p.pos - shinobiPuffSize / 2.0f));
        window.draw(r);
    }
}

void birthRingTick(vector<birthRing>& rings) {
    for (auto& r : rings) r.age++;
    for (int i = (int)rings.size() - 1; i >= 0; i--)
        if (rings[i].age >= birthRingTicks) swapRemove(rings, i);
}

void drawBirthRings(RenderWindow& window, const vector<birthRing>& rings) {
    CircleShape c;
    c.setFillColor(Color::Transparent);
    c.setOutlineThickness(birthRingThickness);
    for (auto& r : rings) {
        float t = (float)r.age / birthRingTicks;
        float rad = birthRingStartRadius + (birthRingEndRadius - birthRingStartRadius) * t;
        Color col = birthRingColor;
        if (r.soulRace >= 0 && r.soulRace < raceCount)   // R50-F1b: a soul-drawn birth leans faintly toward that race
            col = lerpColor(birthRingColor, raceEventColors[r.soulRace], veilRingTint);
        col.a = (uint8_t)(birthRingMaxAlpha * (1.0f - t));
        c.setRadius(rad);
        c.setOrigin({rad, rad});
        c.setPosition(r.pos);
        c.setOutlineColor(col);
        window.draw(c);
    }
}

// R50-F1b: veil motes, world layer under the day/night overlay. One batched draw; 1px, low alpha, a sin fade in/out
// with a soft shimmer, rising and swaying a little. Aged in veilOmenTick (sim ticks), so they hold still while paused.
// R51-F2: shrine faith motes share the same look and batch, rising higher (faithMoteRisePx) and a little brighter.
void drawVeilMotes(RenderWindow& window, const FloatRect& camRect) {
    if (veilMotes.empty() && faithMotes.empty()) return;
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    auto add = [&](const vector<veilMote>& motes, int life, float rise, uint8_t maxAlpha) {
        for (auto& m : motes) {
            float t = (float)m.age / life;
            Vector2f p = m.pos + Vector2f(veilMoteSwayPx * sin(m.phase + m.age * 0.05f), -rise * t);
            if (!camRect.contains(p)) continue;
            float shimmer = 0.65f + 0.35f * sin(m.phase + m.age * 0.3f);
            Color c = lerpColor(veilMoteColor, raceEventColors[clampRace(m.race)], veilMoteRaceTint);
            c.a = (uint8_t)(maxAlpha * sin(3.14159f * t) * shimmer);
            pushQuad(va, snapPx(p), veilMoteSize, c);
        }
    };
    add(veilMotes, veilMoteTicks, veilMoteRisePx, veilMoteMaxAlpha);
    add(faithMotes, faithMoteTicks, faithMoteRisePx, faithMoteMaxAlpha);
    if (va.getVertexCount() > 0) window.draw(va);
}

// Catch rings reuse the birthRing record: one expanding ripple where the line meets the water, per fish landed
void catchRingTick(vector<birthRing>& rings) {
    for (auto& r : rings) r.age++;
    for (int i = (int)rings.size() - 1; i >= 0; i--)
        if (rings[i].age >= fishRingTicks) swapRemove(rings, i);
}

void drawCatchRings(RenderWindow& window, const vector<birthRing>& rings) {
    CircleShape c;
    c.setFillColor(Color::Transparent);
    c.setOutlineThickness(fishRingThickness);
    c.setPointCount(20);
    for (auto& r : rings) {
        float t = (float)r.age / fishRingTicks;
        float rad = fishRingStartRadius + (fishRingEndRadius - fishRingStartRadius) * t;
        Color col = fishRingColor;
        col.a = (uint8_t)(fishRingMaxAlpha * (1.0f - t));
        c.setRadius(rad);
        c.setOrigin({rad, rad});
        c.setPosition(r.pos);
        c.setOutlineColor(col);
        window.draw(c);
    }
}

// R47-F10: once per sim tick. A race whose living population earns a higher step tries exactly one step up; it is
// taken only if the new reach disc passes layoutDiscOk and clears every neighbour's current reach disc. Steps never
// go down (high-water mark); a rejected step leaves the mark as it was and caps that race (nothing ever loosens).
void quarterReachTick(const vector<barb>& barbs) {
    int pop[raceCount] = {};
    for (auto& b : barbs) if (b.race >= 0 && b.race < raceCount) pop[b.race]++;
    for (int r = 0; r < raceCount; r++) {
        if (quarterStepCapped[r] || quarterStep[r] >= quarterStepMax) continue;
        int earned = pop[r] < quarterStepBasePop ? 0 : 1 + (pop[r] - quarterStepBasePop) / quarterStepPopEach;
        if (min(earned, quarterStepMax) <= quarterStep[r]) continue;
        int next = quarterStep[r] + 1;
        Vector2f qc = quarterCentre(r);
        float reach = quarterReachAt(next);
        bool ok = layoutDiscOk(qc, reach);
        for (int q = 0; ok && q < raceCount; q++) {
            if (q == r) continue;
            Vector2f d = qc - quarterCentre(q);
            if (hypot(d.x, d.y) < reach + quarterReachFor(q)) ok = false;
        }
        if (!ok) {
            quarterStepCapped[r] = true;   // quarterStep[r] is not touched
            printf("QUARTER REACH - %s: step %d rejected (reach %.0f overlaps a neighbour or the map/village edge), stays at %d\n",
                   raceDefs[r].name, next, reach, quarterStep[r]);
            continue;
        }
        float oldClear = quarterClearRadiusFor(r);
        quarterStep[r] = next;
        quarterPulses.push_back(quarterPulse{r, oldClear, quarterClearRadiusFor(r) + quarterPulseOvershoot});
        buildQuarterRings();
        printf("QUARTER REACH - %s: step %d at %d living, house disc %.0f, homesteads to %.0f\n", raceDefs[r].name, next, pop[r],
               quarterRadiusFor(r), houseSpreadFarFor(r));
        logRaceEvent(string(racePlurals[r]) + " build further out (reach " + to_string(next) + "/" + to_string(quarterStepMax) + ")", r);
    }
    for (auto& p : quarterPulses) p.age++;
    for (int i = (int)quarterPulses.size() - 1; i >= 0; i--)
        if (quarterPulses[i].age >= quarterPulseTicks) swapRemove(quarterPulses, i);
}

// R47-F10: the step's dust ring, eased out from the old clearing edge to just past the new one, fading as it goes
void drawQuarterPulses(RenderWindow& window) {
    CircleShape c;
    c.setFillColor(Color::Transparent);
    c.setOutlineThickness(quarterPulseThickness);
    c.setPointCount(quarterReachRingPoints);
    for (auto& p : quarterPulses) {
        float t = (float)p.age / quarterPulseTicks;
        float e = 1.0f - (1.0f - t) * (1.0f - t);
        float rad = p.from + (p.to - p.from) * e;
        Color col = quarterPulseColor;
        col.a = (uint8_t)(quarterPulseMaxAlpha * (1.0f - t));
        c.setRadius(rad);
        c.setOrigin({rad, rad});
        c.setPosition(quarterCentre(p.race));
        c.setOutlineColor(col);
        window.draw(c);
    }
}

// R47-F10: hovered crate: a faint 1-screen-px ring at the race's outer build reach
void drawQuarterReachRing(RenderWindow& window, const View& worldView, int r, float zoom) {
    if (r < 0 || r >= raceCount) return;
    window.setView(worldView);
    float rad = quarterReachFor(r);
    CircleShape c(rad, quarterReachRingPoints);
    c.setOrigin({rad, rad});
    c.setPosition(quarterCentre(r));
    c.setFillColor(Color::Transparent);
    c.setOutlineThickness(1.0f / zoom);
    Color col = lensRaceColors[r];
    col.a = (uint8_t)quarterReachRingAlpha;
    c.setOutlineColor(col);
    window.draw(c);
}

settlementEra currentEra = eraCamp;
int eraCheckTimer = 0;

settlementEra computeEra(int barbCount, int houseCount) {
    if (barbCount >= eraTownBarbs && houseCount >= eraTownHouses) return eraTown;
    if (barbCount >= eraHamletBarbs && houseCount >= eraHamletHouses) return eraHamlet;
    return eraCamp;
}

// Promotions are announced; a drop (barbs dying) just updates the rank quietly
void eraTick(const vector<barb>& barbs, const vector<house>& houses) {
    if (++eraCheckTimer < eraCheckIntervalTicks) return;
    eraCheckTimer = 0;
    settlementEra prev = currentEra;
    currentEra = computeEra((int)barbs.size(), (int)houses.size());
    if (currentEra > prev) {
        printf("SETTLEMENT PROMOTED - %s -> %s (pop %d, houses %d)\n", eraNames[prev], eraNames[currentEra], (int)barbs.size(), (int)houses.size());
        logEvent(string("The settlement grows into a ") + eraNames[currentEra]);
    }
}

// R20-F2: every exit from a paving job (finished, unaffordable, interrupted) clears the same barb fields and the claim
void clearPave(barb& b) {
    b.jobKind = jobNone;
    b.moving = b.movingbspot = b.building = false; clearRoute(b);
    b.timer = 0;
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    paveClaim = -1;
    dropCarry(b);   // R29-F1
}

// Runs after eraTick(). One paver at a time, holding the one global paveClaim. With nobody paving, every paveScanTicks
// it picks the unpaved road-tier cell nearest the campfire (within paveRadius) and the nearest idle barb. Stone is
// checked on claim and paid on completion. The claim is re-derived each tick: no barb with jobPave -> released.
void paveTick(vector<barb>& barbs, vector<RectangleShape>& squares) {
    static int scanTimer = 0;
    int paver = -1;
    for (int i = 0; i < (int)barbs.size(); i++) if (barbs[i].jobKind == jobPave) paver = i;
    if (paver < 0) paveClaim = -1;                                        // paver gone: release
    else if (paveClaim < 0) { clearPave(barbs[paver]); paver = -1; }      // job without a cell: drop it
    if (paver >= 0) {
        barb& b = barbs[paver];
        auto& sq = squares[paver];
        if (!b.movingbspot) { printf("PAVING ABANDONED - barb %d taken off the job\n", b.id); clearPave(b); return; }
        Vector2f pos  = sq.getPosition();
        Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
        float dSq = diff.x * diff.x + diff.y * diff.y;
        if (dSq >= 1.0f) {
            walkStep(b, sq);   // R39-F1
            return;
        }
        b.moving = false; clearRoute(b);   // on the cell: no walking bob, just the generic build tool swing
        b.building = true;
        if (workTick(b)) b.timer++;
        if (b.timer < paveTicks) return;
        raceStock& rs = stockOf(b);   // RACE-F1: the paver's race pays
        if (!canAfford(rs, 0, paveStoneCost)) {
            printf("PAVING ABANDONED - barb %d, no stone\n", b.id);
            clearPave(b);
            return;
        }
        int c = paveClaim;
        spend(rs, 0, paveStoneCost);
        trailPaved[c] = 1;
        trailWear[c] = max(trailWear[c], trailRoadThreshold);   // locked at road tier (keeps the road speed bonus)
        trailSetTier(c);
        trailDirty = true;   // R38-F1: paved cells switch to the fixed road underlay
        pavedCount++;
        paveDirty = true;
        printf("---CELL PAVED--- barb %d at %d,%d (%d/%d) stone: %d\n", b.id, (int)((c % trailGridN) * trailCellSize),
               (int)((c / trailGridN) * trailCellSize), pavedCount, paveMaxCells, rs.stone);
        if (pavedCount == 1) logEvent("First cobbles laid by the fire");
        clearPave(b);
        return;
    }
    if (++scanTimer < paveScanTicks) return;
    scanTimer = 0;
    if (currentEra < eraHamlet || pavedCount >= paveMaxCells) return;
    bool anyAfford = false;   // RACE-F1: some race must have the stone; the paver is picked from those races below
    for (int r = 0; r < raceCount; r++) if (canAfford(stock[r], 0, paveStoneCost + houseStoneCost)) anyAfford = true;
    if (!anyAfford) return;
    int best = -1;
    float bestDSq = paveRadius * paveRadius;
    Vector2f bestCentre;
    for (int c = 0; c < (int)trailTier.size(); c++) {
        if (trailTier[c] != 2 || trailPaved[c]) continue;
        Vector2f centre(((c % trailGridN) + 0.5f) * trailCellSize, ((c / trailGridN) + 0.5f) * trailCellSize);
        Vector2f d = centre - villageCenter;
        float dSq = d.x * d.x + d.y * d.y;
        if (dSq <= bestDSq) { bestDSq = dSq; best = c; bestCentre = centre; }
    }
    if (best < 0) return;
    int who = -1;
    float whoDSq = 0.0f;
    for (int i = 0; i < (int)barbs.size(); i++) {
        auto& b = barbs[i];
        if (!barbAvailable(b) || b.jobKind != jobNone || b.deertargeting || b.chopping || b.timer > 0
            || b.moving || b.movingbspot || b.building) continue;
        if (!canAfford(stockOf(b), 0, paveStoneCost + houseStoneCost)) continue;
        Vector2f d = squares[i].getPosition() - bestCentre;
        float dSq = d.x * d.x + d.y * d.y;
        if (who < 0 || dSq < whoDSq) { who = i; whoDSq = dSq; }
    }
    if (who < 0) return;
    barb& b = barbs[who];
    Vector2f stand = bestCentre - squares[who].getSize() / 2.0f;   // hitbox centred on the cell
    paveClaim = best;
    clearTraining(b);   // R47-F6
    b.jobKind = jobPave;
    b.target = targetNone;
    b.targetx = (int)stand.x;
    b.targety = (int)stand.y;
    b.moving = b.movingbspot = true;
    b.timer = 0;
    printf("PAVING CLAIMED - barb %d, cell %d,%d (%d/%d paved)\n", b.id, (int)(bestCentre.x - trailCellSize / 2),
           (int)(bestCentre.y - trailCellSize / 2), pavedCount, paveMaxCells);
}

councilDecree currentDecree = decreeNone;   // none until the first dawn

// Ballot: race lean plus colony needs (wood short of a house, stone short of a foundation, this barb hungry or no food)
councilDecree castBallot(barb& b) {
    float score[decreeCount] = {};
    score[raceLeans[b.race]] += councilRaceLean;
    const raceStock& rs = stockOf(b);   // RACE-F1: each voter weighs its own race's larder
    score[decreeTimber]  += councilNeedLean * clamp((float)(houseWoodCost - rs.wood) / (float)houseWoodCost, 0.0f, 1.0f);
    score[decreeStone]   += councilNeedLean * clamp((float)(houseStoneCost - rs.stone) / (float)houseStoneCost, 0.0f, 1.0f);
    score[decreeBerries] += (isHungry(b) || rs.food <= 0) ? councilNeedLean : 0.0f;
    int r = clampRace(b.race);   // R52-F3: a high-faith race with a shrine to kneel at leans toward the Rite
    if (veil.faithTier[r] == 2 && veil.faithShrineId[r] >= 0) score[decreeRite] += riteBallotLean * veil.faith[r];
    int best = raceLeans[b.race];               // ties go to the barb's own lean
    for (int d = 0; d < decreeCount; d++) if (score[d] > score[best]) best = d;
    b.lastVote = (councilDecree)best;           // shown overhead by drawBallots
    return b.lastVote;
}

int ballotFramesLeft = 0;                       // >0 while the open-ballot pips are on screen

// Draw-only: one pip per barb in its ballot colour, fading out; skipped when zoomed out too far to read
void drawBallots(RenderWindow& window, const View& worldView, float zoom,
                 const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    if (ballotFramesLeft <= 0 || zoom < ballotMinZoom) return;
    window.setView(worldView);
    float a = min(1.0f, (float)ballotFramesLeft / ballotFadeFrames);
    RectangleShape pip(ballotPipSize);
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        if (barbs[i].lastVote == decreeNone || barbHidden(barbs[i]) || barbUnderground(barbs[i])) continue;   // born after the vote; INBOX-F1: indoors; R40-F1: below
        Color c = ballotPipColors[barbs[i].lastVote];
        c.a = (uint8_t)(255 * a);
        pip.setFillColor(c);
        Vector2f ps = isGentry(barbs[i]) ? gentryBallotPipSize : ballotPipSize;   // R24-F1: double vote, taller pip
        pip.setSize(ps);
        Vector2f top = snapPx(squares[i].getPosition());
        float cx = top.x + round((squares[i].getSize().x - ps.x) / 2.0f);
        pip.setPosition(Vector2f(cx, top.y - householdPipGap - ps.y));
        window.draw(pip);
    }
}

// R28-F2: dawn elections. Each leaderless race with enough members puts up its best-liked member;
// every other member approves with raceTraitLike odds. More than half wins the crown, else retry next dawn.
// R30-F2: the only place a leader is cleared, so the race drops back to global weights the same tick
void loseLeader(int r) {
    printf("LEADER LOST - %s leader id %d is gone, %s back to global weights\n", racePlurals[r], raceLeaderId[r], racePlurals[r]);
    logEvent(string(racePlurals[r]) + " lost their leader");
    raceElected[r] = false;
    raceLeaderId[r] = -1;
    raceNeed[r] = needNone;
}

// R30-F2: every tick, so a dead leader's need never outlives them
void leaderWatchTick(const vector<barb>& barbs) {
    for (int r = 0; r < raceCount; r++)
        if (raceElected[r] && findBarbById(barbs, raceLeaderId[r]) < 0) loseLeader(r);
}

// R31-F1: the one undo for a queued keep: refunds the reserved stone to the race and clears the guard with it, so the
// race can queue again. No-op if the keep stands or nothing is queued.
void cancelKeep(int r) {
    if (r < 0 || r >= raceCount || keeps[r].built || !keepQueued[r]) return;
    stock[r].stone += keepReservedStone[r];
    printf("KEEP CANCELLED - %s, %d stone refunded\n", raceDefs[r].name, keepReservedStone[r]);
    keepReservedStone[r] = 0;
    keepQueued[r] = false;
    keepBuilderId[r] = -1;
}

// R31-F1: runs after buildmills(). From the Town era, each race with a standing crate raises one keep at its reserved
// spot through buildOneOff() by a barb of that race. The claim sets keepQueued[r] (buildOneOff's claimed flag) and moves
// keepStoneCost into keepReservedStone[r] in the same step, and only from stone above keepStoneReserve. If the builder is
// lost (death or shelter -> releaseBarbWork) the stone is refunded via cancelKeep(). Keeps are independent per race.
void buildkeeps(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    for (int r = 0; r < raceCount; r++) {
        keep& kp = keeps[r];
        if (kp.built && !kp.gilded && gold >= keepGildCost) {   // R46-F1b: checked before deducting; gilds once
            gold -= keepGildCost;
            kp.gilded = true;
            printf("---KEEP GILDED--- %s, id %d (%d gold, %d left)\n", raceDefs[r].name, kp.id, keepGildCost, gold);
            logRaceEvent(string(racePlurals[r]) + " gild their keep's gate", r);
        }
        if (kp.built) continue;
        if (keepQueued[r]) {   // builder lost without releaseBarbWork() (should not happen): refund and free the guard
            int i = findBarbById(barbs, keepBuilderId[r]);
            if (i < 0 || barbs[i].jobKind != jobKeep || barbs[i].race != r) cancelKeep(r);
        }
        if (!keepQueued[r] && (currentEra < eraTown || !stores[r].built)) continue;
        bool done = buildOneOff(barbs, squares, keepQueued[r], jobKeep, Vector2f((float)kp.x, (float)kp.y) + keepStandOffset, "KEEP",
            [&](const barb& b) {
                FloatRect z = keepZone(r);
                if (!storehouseClear(z) || !smokehouseClear(z) || !lakeClear(z))
                    return -1;   // fixed offset from the crate: only trips if the layout constants are moved onto it
                if (!canAfford(stock[r], 0, keepStoneCost + keepStoneReserve)) return -1;
                spend(stock[r], 0, keepStoneCost);   // reserved together with keepQueued (set by buildOneOff on return 1)
                keepReservedStone[r] = keepStoneCost;
                keepBuilderId[r] = b.id;
                return 1;
            }, r);
        if (!done) continue;
        kp.built = true;
        kp.id = nextKeepId++;
        keepReservedStone[r] = 0;
        keepQueued[r] = false;
        keepBuilderId[r] = -1;
        printf("---KEEP BUILT--- %s, id %d (%d stone)\n", raceDefs[r].name, kp.id, keepStoneCost);
        logRaceEvent(string(racePlurals[r]) + " raise a keep", r);
    }
}

// R31-F1: 14x12 slate keep: merlons over a solid wall, a 1px dark plinth, a dark gate, and the race's saturated banner
// on a 1px pole above the right merlon. While the race has a leader, one gold pip stands in the centre crenel.
// Drawn just after the houses, under barbs.
// R31-F3: sheltering barbs light pale slit windows on the wall face; the banner flies only while someone shelters.
// R36-F1: ported to drawParts (3 notched merlons, dark arched door, ground shadow); the live marks are batched with it.
void drawKeeps(RenderWindow& window, const vector<barb>& barbs) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (int k = 0; k < raceCount; k++) {
        const keep& kp = keeps[k];
        if (!kp.built) continue;
        int inside = 0;   // R31-F3: hidden barbs in this race's keep
        for (auto& b : barbs) if (barbHidden(b) && b.race == k && b.garrisonHouseId == keepGarrisonId) inside++;
        Vector2f o((float)kp.x, (float)kp.y);
        drawParts(va, keepShadowParts, o, Color::White);
        drawParts(va, keepStoneParts, o, keepStoneColor);
        drawParts(va, keepTrimParts, o, Color::White);
        if (kp.gilded) {   // R46-F1b: 1px gilt frame hugging the door: keystone over the arch, shoulders, full-height jambs
            pushQuad(va, o + keepGatePos + Vector2f(1.0f, -1.0f), Vector2f(keepGateSize.x - 2.0f, 1.0f), keepGildColor);
            pushQuad(va, o + keepGatePos, Vector2f(1.0f, 1.0f), keepGildColor);
            pushQuad(va, o + keepGatePos + Vector2f(keepGateSize.x - 1.0f, 0.0f), Vector2f(1.0f, 1.0f), keepGildColor);
            pushQuad(va, o + keepGatePos + Vector2f(-1.0f, 1.0f), Vector2f(1.0f, keepGateSize.y - 1.0f), keepGildColor);
            pushQuad(va, o + keepGatePos + Vector2f(keepGateSize.x, 1.0f), Vector2f(1.0f, keepGateSize.y - 1.0f), keepGildColor);
        }
        pushQuad(va, o + keepPolePos, Vector2f(1.0f, keepPoleH), keepPoleColor);
        if (inside > 0)     // R31-F3: full height, flying right from the pole top
            pushQuad(va, o + keepPolePos + Vector2f(1.0f, 0.0f), keepFlagSize, keepBannerColors[k]);
        else                // R31-F3: empty keep, the banner hangs limp beside the pole
            pushQuad(va, o + keepPolePos + keepDroopOffset, keepDroopSize, keepBannerColors[k]);
        for (int p = 0; p < min(inside, keepSlotMax); p++)   // R31-F3: one slit window per sheltering barb, top row first
            pushQuad(va, o + Vector2f(keepPipCols[p % 4], keepPipRows[p / 4]), keepPipSize, keepPipColor);
        if (raceElected[k]) pushQuad(va, o + keepLeaderPipPos, keepLeaderPipSize, keepLeaderPipColor);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R31-F2: keep centre, the campfire bearing the gate faces, and the 2x2 block top-lefts tracing slot s's arc
Vector2f wallCentre(int r) { return Vector2f((float)keeps[r].x, (float)keeps[r].y) + keepSize / 2.0f; }
void wallArcBlocks(int r, int s, vector<Vector2f>& out) {
    out.clear();
    Vector2f kc = wallCentre(r);
    float gate = atan2(villageCenter.y - kc.y, villageCenter.x - kc.x);
    float mid = gate + 6.2831853f * s / wallSlots;
    float half = (6.2831853f / wallSlots - wallSeamPx / wallRingRadius) / 2.0f;
    int n = max(2, (int)ceil(2.0f * half * wallRingRadius) + 1);   // about one sample per px of arc: no holes in the run
    for (int k = 0; k < n; k++) {
        float a = mid - half + 2.0f * half * k / (n - 1);
        Vector2f p = kc + Vector2f(cos(a), sin(a)) * wallRingRadius;
        out.push_back(snapPx(p - Vector2f(wallThickPx, wallThickPx) / 2.0f));
    }
}

// R31-F2: rise order: slots beside the gate first, alternating sides, ending opposite the gate
int wallOrderSlot(int i) { return i % 2 == 0 ? wallGateSlots / 2 + 1 + i / 2 : wallSlots - wallGateSlots / 2 - 1 - i / 2; }

// R33-F2: called once, when race r's curtain wall completes. Places one tower on each side of the gate slot (slot 0),
// centred on the ring at the neighbouring gate-gap slots (1 and wallSlots-1), nudged away from the gate. A spot is
// dropped for good if its zone is off the map, in another quarter or the village clearing, on the forge/crates/huts/
// keeps (forgeClear), water (lakeClear), plots/mills, the stall, a house, shrine, rock or bush, a paved cell, or on the
// gate slot's arc (the passage stays open and unpaved). Trees in an accepted zone are felled once.
void placeTowers(int r, vector<barb>& barbs, vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps,
                 const vector<CircleShape>& octs, const vector<CircleShape>& bushshapes, const vector<RectangleShape>& houseshapes,
                 const vector<shrine>& shrines, const fieldGrid& fields) {
    static vector<Vector2f> gateBlocks;
    wallArcBlocks(r, 0, gateBlocks);
    Vector2f kc = wallCentre(r);
    float gate = atan2(villageCenter.y - kc.y, villageCenter.x - kc.x);
    for (int k = 0; k < towerCount; k++) {
        float side = k == 0 ? -1.0f : 1.0f;
        float a = gate + side * (6.2831853f / wallSlots + towerArcNudgePx / wallRingRadius);
        Vector2f c = kc + Vector2f(cos(a), sin(a)) * wallRingRadius;
        gatetower t;
        Vector2f tl = snapPx(c - towerSize / 2.0f);
        t.x = (int)tl.x;
        t.y = (int)tl.y;
        FloatRect z = towerZone(t);
        bool ok = z.position.x >= 0.0f && z.position.y >= hudBarHeight
               && z.position.x + z.size.x <= worldSize && z.position.y + z.size.y <= worldSize;
        Vector2f corners[4] = {z.position, z.position + Vector2f(z.size.x, 0.0f), z.position + Vector2f(0.0f, z.size.y), z.position + z.size};
        for (auto& p : corners) {
            for (int q = 0; ok && q < raceCount; q++) {   // never into another race's quarter
                Vector2f d = p - quarterCentre(q);
                if (q != r && d.x * d.x + d.y * d.y < quarterClearRadiusFor(q) * quarterClearRadiusFor(q)) ok = false;   // R47-F10
            }
            Vector2f dv = p - villageCenter;
            if (ok && dv.x * dv.x + dv.y * dv.y < clearRadiusSq) ok = false;
        }
        if (ok && (!forgeClear(z) || !lakeClear(z) || !fieldsClear(fields, z))) ok = false;
        if (ok && z.findIntersection(FloatRect(stallPos, stallSize)).has_value()) ok = false;
        for (int i = 0; ok && i < (int)houseshapes.size(); i++) {
            Vector2f hc = houseshapes[i].getPosition() + houseshapes[i].getSize() / 2.0f;
            if (z.findIntersection(FloatRect(hc - houseSlabSize / 2.0f, houseSlabSize)).has_value()) ok = false;
        }
        for (int i = 0; ok && i < (int)shrines.size(); i++)
            if (z.findIntersection(FloatRect(Vector2f((float)shrines[i].x, (float)shrines[i].y - shrineCapH),
                                             shrineSlabSize + Vector2f(0.0f, shrineCapH))).has_value()) ok = false;
        for (int i = 0; ok && i < (int)octs.size(); i++)
            if (z.findIntersection(octs[i].getGlobalBounds()).has_value()) ok = false;
        for (int i = 0; ok && i < (int)bushshapes.size(); i++)
            if (z.findIntersection(bushshapes[i].getGlobalBounds()).has_value()) ok = false;
        if (ok) {   // any paved cell under the zone (it is inside the map here)
            int cx0 = (int)(z.position.x / trailCellSize), cx1 = (int)((z.position.x + z.size.x - 0.01f) / trailCellSize);
            int cy0 = (int)(z.position.y / trailCellSize), cy1 = (int)((z.position.y + z.size.y - 0.01f) / trailCellSize);
            for (int cy = cy0; cy <= cy1; cy++)
                for (int cx = cx0; cx <= cx1; cx++)
                    if (cx >= 0 && cy >= 0 && cx < trailGridN && cy < trailGridN && trailPaved[cy * trailGridN + cx]) ok = false;
        }
        for (auto& bp : gateBlocks)
            if (ok && z.findIntersection(FloatRect(bp, Vector2f(wallThickPx, wallThickPx))).has_value()) ok = false;
        if (!ok) {
            printf("Gate tower %d skipped - %s (blocked)\n", k, raceDefs[r].name);
            continue;
        }
        for (int i = (int)tris.size() - 1; i >= 0; i--) {   // fell trees in the zone, once
            if (!tris[i].getGlobalBounds().findIntersection(z).has_value()) continue;
            Vector2f p = tris[i].getPosition();
            int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);   // same stand spot the clearing pass in main drops
            for (auto& b : barbs) {
                if (b.activity != actShelter && b.target != targetBush && b.targetx == tix && b.targety == tiy) {
                    b.moving = b.chopping = false; clearRoute(b);
                    b.timer  = 0;
                    b.targetx = b.targety = -1;
                }
            }
            swapRemove(tris, i); swapRemove(stumps, i); swapRemove(trees, i);
        }
        t.race = r;
        towers[r][k] = t;
        printf("---GATE TOWER PLACED--- %s, tower %d at %d,%d\n", raceDefs[r].name, k, t.x, t.y);
    }
}

// R31-F2: once per tick. For each race with a built keep, every wallRiseTicks tries the next slots in rise order until
// one fits (blocked slots are skipped for good). All overlap checks and the one-off tree felling happen only here, at
// placement; nothing scans per tick. Cosmetic: segments never block movement or placement.
void wallTick(vector<barb>& barbs, vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps,
              const vector<CircleShape>& octs, const vector<CircleShape>& bushshapes, const vector<RectangleShape>& houseshapes,
              const vector<shrine>& shrines, const fieldGrid& fields) {
    wallClock++;
    static vector<Vector2f> blocks;
    for (int r = 0; r < raceCount; r++) {
        if (!keeps[r].built || wallNextOrder[r] >= wallSegMax) continue;
        if (++wallTimer[r] < wallRiseTicks) continue;
        wallTimer[r] = 0;
        while (wallNextOrder[r] < wallSegMax) {
            int s = wallOrderSlot(wallNextOrder[r]++);
            wallArcBlocks(r, s, blocks);
            bool ok = true;
            for (auto& bp : blocks) {
                FloatRect br(bp, Vector2f(wallThickPx, wallThickPx));
                Vector2f c = bp + Vector2f(wallThickPx, wallThickPx) / 2.0f;
                if (bp.x < 0.0f || bp.y < hudBarHeight || bp.x + wallThickPx > worldSize || bp.y + wallThickPx > worldSize) ok = false;
                for (int q = 0; ok && q < raceCount; q++) {   // never into another race's quarter
                    Vector2f d = c - quarterCentre(q);
                    if (q != r && d.x * d.x + d.y * d.y < quarterClearRadiusFor(q) * quarterClearRadiusFor(q)) ok = false;   // R47-F10
                }
                Vector2f dv = c - villageCenter;
                if (ok && dv.x * dv.x + dv.y * dv.y < clearRadiusSq) ok = false;
                if (ok && (!forgeClear(br) || !lakeClear(br) || !fieldsClear(fields, br))) ok = false;   // forge, crates, huts, keeps, plots, mills
                if (ok && br.findIntersection(FloatRect(stallPos, stallSize)).has_value()) ok = false;
                for (int i = 0; ok && i < (int)houseshapes.size(); i++) {
                    Vector2f hc = houseshapes[i].getPosition() + houseshapes[i].getSize() / 2.0f;
                    if (br.findIntersection(FloatRect(hc - houseSlabSize / 2.0f, houseSlabSize)).has_value()) ok = false;
                }
                for (int i = 0; ok && i < (int)shrines.size(); i++)
                    if (br.findIntersection(FloatRect(Vector2f((float)shrines[i].x, (float)shrines[i].y - shrineCapH),
                                                      shrineSlabSize + Vector2f(0.0f, shrineCapH))).has_value()) ok = false;
                for (int i = 0; ok && i < (int)octs.size(); i++)
                    if (br.findIntersection(octs[i].getGlobalBounds()).has_value()) ok = false;
                for (int i = 0; ok && i < (int)bushshapes.size(); i++)
                    if (br.findIntersection(bushshapes[i].getGlobalBounds()).has_value()) ok = false;
                for (int k = 0; ok && k < 4; k++) {   // paved cells under any corner
                    int cell = trailCell(bp + Vector2f(k % 2 ? wallThickPx - 0.01f : 0.0f, k / 2 ? wallThickPx - 0.01f : 0.0f));
                    if (cell >= 0 && trailPaved[cell]) ok = false;
                }
                if (!ok) break;
            }
            if (!ok) {
                printf("Wall slot %d skipped - %s (blocked)\n", s, raceDefs[r].name);
                continue;
            }
            for (int i = (int)tris.size() - 1; i >= 0; i--) {   // fell trees on the arc, once
                FloatRect tb = tris[i].getGlobalBounds();
                bool hit = false;
                for (auto& bp : blocks)
                    if (tb.findIntersection(FloatRect(bp, Vector2f(wallThickPx, wallThickPx))).has_value()) { hit = true; break; }
                if (!hit) continue;
                Vector2f p = tris[i].getPosition();
                int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);   // same stand spot the clearing pass in main drops
                for (auto& b : barbs) {
                    if (b.activity != actShelter && b.target != targetBush && b.targetx == tix && b.targety == tiy) {
                        b.moving = b.chopping = false; clearRoute(b);
                        b.timer  = 0;
                        b.targetx = b.targety = -1;
                    }
                }
                swapRemove(tris, i); swapRemove(stumps, i); swapRemove(trees, i);
            }
            Vector2f m = blocks[blocks.size() / 2] + Vector2f(wallThickPx, wallThickPx) / 2.0f;
            wallsegs.push_back(wallseg{nextWallSegId++, r, (int)m.x, (int)m.y, s});
            wallFlashTick[r] = wallClock;
            wallFlashSegId[r] = wallsegs.back().id;
            printf("---WALL SEGMENT--- %s, slot %d at %d,%d (%d tried of %d)\n", raceDefs[r].name, s, (int)m.x, (int)m.y,
                   wallNextOrder[r], wallSegMax);
            break;
        }
        if (wallNextOrder[r] >= wallSegMax) {
            int built = 0;
            for (auto& w : wallsegs) if (w.race == r) built++;
            printf("---CURTAIN WALL DONE--- %s, %d of %d segments\n", raceDefs[r].name, built, wallSegMax);
            if (built > 0) logRaceEvent(string(racePlurals[r]) + " finish a curtain wall", r);
            if (built > 0) placeTowers(r, barbs, trees, tris, stumps, octs, bushshapes, houseshapes, shrines, fields);   // R33-F2
        }
    }
}

// R31-F2: drawn right after the trails, under paving, quarter marks, buildings and barbs. Opaque 2x2 blocks, so
// overlapping samples never stack alpha. Wet tone while raining; the race's newest segment flashes faintly for
// wallFlashTicks after it rises.
void drawWalls(RenderWindow& window, VertexArray& va) {
    va.clear();
    static vector<Vector2f> blocks;
    Color base = raining ? wallRainColor : wallStoneColor;
    for (auto& w : wallsegs) {
        Color col = base;
        int age = wallClock - wallFlashTick[w.race];
        if (w.id == wallFlashSegId[w.race] && age >= 0 && age < wallFlashTicks)
            col = lerpColor(base, wallFlashColor, wallFlashStrength * (1.0f - (float)age / wallFlashTicks));
        wallArcBlocks(w.race, w.slot, blocks);
        for (auto& bp : blocks) pushQuad(va, bp, Vector2f(wallThickPx, wallThickPx), col);
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R33-F2: the one undo for a claimed gate tower: refunds the reserved stone and frees the keep's builder guard
// (keepQueued/keepBuilderId, unused once the keep stands) so the next tower can be claimed. No-op if nothing is claimed.
void cancelTower(int r) {
    if (r < 0 || r >= raceCount || !keeps[r].built || !keepQueued[r]) return;
    stock[r].stone += towerReservedStone[r];
    printf("GATE TOWER CANCELLED - %s, %d stone refunded\n", raceDefs[r].name, towerReservedStone[r]);
    towerReservedStone[r] = 0;
    keepQueued[r] = false;
    keepBuilderId[r] = -1;
}

// R33-F2: runs after wallTick(). For each race with placed, unbuilt towers, raises them one at a time through the keep's
// path: buildOneOff() with keepQueued[r] as the claim, jobKeep and keepBuildTicks of labour, a builder of that race.
// The claim moves towerStoneCost into towerReservedStone[r], only from stone above keepStoneReserve.
void towerTick(vector<barb>& barbs, vector<RectangleShape>& squares) {
    for (int r = 0; r < raceCount; r++) {
        if (!keeps[r].built) continue;
        int k = 0;
        while (k < towerCount && (towers[r][k].race < 0 || towers[r][k].built)) k++;
        if (k == towerCount) continue;
        gatetower& t = towers[r][k];
        if (keepQueued[r]) {   // builder lost without releaseBarbWork() (should not happen): refund and free the guard
            int i = findBarbById(barbs, keepBuilderId[r]);
            if (i < 0 || barbs[i].jobKind != jobKeep || barbs[i].race != r) cancelTower(r);
        }
        bool done = buildOneOff(barbs, squares, keepQueued[r], jobKeep, Vector2f((float)t.x, (float)t.y) + towerStandOffset, "GATE TOWER",
            [&](const barb& b) {
                if (!canAfford(stock[r], 0, towerStoneCost + keepStoneReserve)) return -1;
                spend(stock[r], 0, towerStoneCost);   // reserved together with keepQueued (set by buildOneOff on return 1)
                towerReservedStone[r] = towerStoneCost;
                keepBuilderId[r] = b.id;
                return 1;
            }, r);
        if (!done) continue;
        t.built = true;
        t.id = nextTowerId++;
        towerReservedStone[r] = 0;
        keepQueued[r] = false;
        keepBuilderId[r] = -1;
        printf("---GATE TOWER BUILT--- %s, tower %d id %d (%d stone)\n", raceDefs[r].name, k, t.id, towerStoneCost);
        bool more = false;
        for (auto& o : towers[r]) if (o.race >= 0 && !o.built) more = true;
        if (!more) logRaceEvent(string(racePlurals[r]) + " raise gatehouse towers", r);
    }
}

// R33-F2: 12x16 tower in the wall's stone (wet tone while raining): 4 merlons over a solid face, a dark 2x3 arrow slit,
// a 1px darker plinth, and a tiny race pennant on a 1px pole above the right corner merlon. Drawn with the keeps.
void drawGateTowers(RenderWindow& window) {
    RectangleShape s;
    Color stone = raining ? wallRainColor : wallStoneColor;
    for (int r = 0; r < raceCount; r++) {
        for (auto& t : towers[r]) {
            if (t.race < 0 || !t.built) continue;
            Vector2f o((float)t.x, (float)t.y);
            s.setFillColor(stone);
            s.setSize(Vector2f(towerSize.x, towerSize.y - towerMerlonH));
            s.setPosition(o + Vector2f(0.0f, towerMerlonH));
            window.draw(s);
            for (int m = 0; m < 4; m++) {
                s.setSize(Vector2f(towerMerlonW[m], towerMerlonH));
                s.setPosition(o + Vector2f(towerMerlonX[m], 0.0f));
                window.draw(s);
            }
            s.setFillColor(towerPlinthColor);
            s.setSize(Vector2f(towerSize.x, 1.0f));
            s.setPosition(o + Vector2f(0.0f, towerSize.y - 1.0f));
            window.draw(s);
            s.setFillColor(keepGateColor);
            s.setSize(towerSlitSize);
            s.setPosition(o + towerSlitPos);
            window.draw(s);
            s.setFillColor(keepPoleColor);
            s.setSize(Vector2f(1.0f, towerPoleH));
            s.setPosition(o + towerPolePos);
            window.draw(s);
            s.setFillColor(towerPennantColors[r]);
            s.setSize(towerFlagSize);
            s.setPosition(o + towerPolePos + Vector2f(1.0f, 0.0f));
            window.draw(s);
        }
    }
}

// R33-F3: Bresenham over the trail grid from the keep centre's cell out to the crate centre's cell. Every cell appears
// once (consecutive repeats dropped), ordered keep-first, so the causeway grows outward from the keep.
void causewayLine(int r, vector<int>& out) {
    out.clear();
    Vector2f a = wallCentre(r), b = quarterCentre(r);
    int x0 = (int)(a.x / trailCellSize), y0 = (int)(a.y / trailCellSize);
    int x1 = (int)(b.x / trailCellSize), y1 = (int)(b.y / trailCellSize);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (true) {
        if (x0 >= 0 && y0 >= 0 && x0 < trailGridN && y0 < trailGridN) {
            int c = y0 * trailGridN + x0;
            if (out.empty() || out.back() != c) out.push_back(c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// R33-F3: a causeway cell must show (not entirely under the keep or crate), be dry, and stay off every wall and gate slot
// of the race's ring (all wallSlots, built or not, so the gate gap is never paved)
bool causewayCellOk(int r, int c, vector<Vector2f>& blocks) {
    FloatRect cr(Vector2f((c % trailGridN) * trailCellSize, (c / trailGridN) * trailCellSize), Vector2f(trailCellSize, trailCellSize));
    auto covers = [&](FloatRect o) {
        return o.position.x <= cr.position.x && o.position.y <= cr.position.y
            && o.position.x + o.size.x >= cr.position.x + cr.size.x && o.position.y + o.size.y >= cr.position.y + cr.size.y;
    };
    if (covers(FloatRect(Vector2f((float)keeps[r].x, (float)keeps[r].y), keepSize))) return false;
    if (covers(FloatRect(storehousePos[r], storehouseSize))) return false;
    if (!lakeClear(cr)) return false;
    for (int s = 0; s < wallSlots; s++) {
        wallArcBlocks(r, s, blocks);
        for (auto& bp : blocks)
            if (cr.findIntersection(FloatRect(bp, Vector2f(wallThickPx, wallThickPx))).has_value()) return false;
    }
    return true;
}

// R33-F3: once per tick. For each race with a built keep, every causewayIntervalTicks paves the first unpaved valid cell
// on the keep -> crate line. Stone goes through the keep's reserve rule: canAfford(cost + keepStoneReserve), then spend().
// Short of stone it waits for the next interval; with no cell left it logs completion once and stops.
void causewayTick() {
    static vector<int> line;
    static vector<Vector2f> blocks;
    for (int r = 0; r < raceCount; r++) {
        if (!keeps[r].built || causewayDone[r]) continue;
        if (++causewayTimer[r] < causewayIntervalTicks) continue;
        causewayTimer[r] = 0;
        causewayLine(r, line);
        int next = -1;
        for (int c : line) {
            if (trailPaved[c] || !causewayCellOk(r, c, blocks)) continue;
            next = c;
            break;
        }
        if (next < 0) {
            causewayDone[r] = true;
            printf("---CAUSEWAY DONE--- %s, %d cells\n", raceDefs[r].name, causewayCells[r]);
            if (causewayCells[r] > 0) logRaceEvent(string(racePlurals[r]) + " finish a stone road to their keep", r);
            continue;
        }
        if (!canAfford(stock[r], 0, causewayStoneCost + keepStoneReserve)) continue;
        spend(stock[r], 0, causewayStoneCost);
        trailPaved[next] = causewayPaved;
        trailWear[next] = max(trailWear[next], trailRoadThreshold);   // locked at road tier: road speed bonus
        trailSetTier(next);
        trailDirty = true;   // R38-F1: paved cells switch to the fixed road underlay
        paveDirty = true;
        causewayCells[r]++;
        printf("---CAUSEWAY CELL--- %s at %d,%d (%d laid) stone: %d\n", raceDefs[r].name, (int)((next % trailGridN) * trailCellSize),
               (int)((next / trailGridN) * trailCellSize), causewayCells[r], stock[r].stone);
    }
}

// R47-F9: ground layer, after bushes and before the buildings (a house built later covers its stone). One batched draw.
// Muted race-tinted stone; fades to 40% alpha with age; lifts toward pale stone (and full alpha) while a kinsman is near.
void drawGraves(RenderWindow& window, const FloatRect& camRect) {
    if (graves.empty()) return;
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (auto& g : graves) {
        if (!camRect.findIntersection(FloatRect(g.pos, graveSize))) continue;
        float old = clamp((float)(g.age - graveFadeStartTicks) / graveFadeTicks, 0.0f, 1.0f);
        float lit = (float)g.glow / graveGlowTicks;
        float a = 255.0f + (graveOldAlpha - 255.0f) * old;
        a += (255.0f - a) * lit;
        Color c = lerpColor(lerpColor(graveStoneColor, raceEventColors[clampRace(g.race)], graveRaceTint), graveGlowColor, graveGlowMix * lit);
        c.a = (uint8_t)a;
        Color base = shade(c, graveBaseShade);
        pushQuad(va, g.pos + Vector2f(1.0f, 0.0f), Vector2f(1.0f, 1.0f), c);          // rounded cap
        pushQuad(va, g.pos + Vector2f(0.0f, 1.0f), Vector2f(graveSize.x, 2.0f), c);   // slab
        pushQuad(va, g.pos + Vector2f(0.0f, 3.0f), Vector2f(graveSize.x, 1.0f), base);  // set in the ground
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// INBOX-F1: the release half of removeBarb(), shared with bellTick() so a barb taking shelter frees exactly what a dead one does
void releaseBarbWork(barb& b, smokehouseSet& sh, forge& fg, storehouse (&stores)[raceCount], fieldGrid& fields) {
    if (b.deertargeting) clearHunt(b);
    dropCarry(b);
    if (b.reservedwood) stockOf(b).wood += houseWoodCost;   // house claim walked to but never built: back to its race's stock
    stockOf(b).stone += b.reservedstone;                    // house foundation or shrine stone
    b.reservedwood = false;
    b.reservedstone = 0;
    setWeapon(b, weaponRefund);                  // POL-F3: paid spear craft goes back to its race before the swapRemove (R47-F8: via the helper)
    clearTemper(b);                              // R41-F2: mid-temper stone refunded, charges reset (death and sheltering)
    clearSmelt(b);                               // R47-F7: mid-smelt stone refunded, forge freed
    clearTraining(b);                            // R47-F6: drill slot freed (death and sheltering)
    switch (b.jobKind) {                         // claims paid at claim time stay spent; the next builder pays again
        case jobSmoke: if (b.race >= 0 && b.race < raceCount && !sh[b.race].built) sh[b.race].claimed = false; break;   // INBOX2-F1: own race's hut
        case jobForge: if (!fg.built) fg.claimed = false; break;
        case jobStall: if (!hasStall) stallClaimed = false; break;
        case jobStore: if (b.race >= 0 && b.race < raceCount && !stores[b.race].built) stores[b.race].claimed = false; break;
        case jobField:   // FARM-F1: only the plot this barb sows, in its own race's row
            if (b.race >= 0 && b.race < raceCount)
                for (auto& fp : fields[b.race])
                    if (!fp.built && fp.sowerId == b.id) { fp.claimed = false; fp.sowerId = -1; }
            break;
        case jobMill:    // FARM-F2: only its own race's mill, and only if this barb holds the claim
            if (b.race >= 0 && b.race < raceCount && !mills[b.race].built && mills[b.race].builderId == b.id) {
                mills[b.race].claimed = false;
                mills[b.race].builderId = -1;
            }
            break;
        case jobShaft:   // R34-F1: only its own race's shaft, and only if this barb holds the claim
            if (b.race >= 0 && b.race < raceCount && !mineShafts[b.race].built && mineShafts[b.race].builderId == b.id) {
                mineShafts[b.race].claimed = false;
                mineShafts[b.race].builderId = -1;
            }
            break;
        case jobPave:  clearPave(b); break;
        case jobSpear: clearSpearCraft(b); break;
        case jobKeep:  if (b.race >= 0 && b.race < raceCount && keepBuilderId[b.race] == b.id) {   // R31-F1: refund, can requeue
                           if (keeps[b.race].built) cancelTower(b.race);   // R33-F2: the job is a gate tower once the keep stands
                           else cancelKeep(b.race);
                       }
                       break;
        default: break;
    }
    b.jobKind = jobNone;
    b.moving = b.movingbspot = b.building = b.chopping = false; clearRoute(b);
    b.timer = 0;
    leaveMine(b);   // R40-F1: death and sheltering both pass through here
}

void ungarrison(barb& b);   // R31-F3: defined with bellTick(); also frees the barb's keep slot

// R30-F5: the only place a barb is removed. Releases everything the barb holds or is referenced by (hunt, carried load,
// reserved house/shrine materials, one-off build claims, paving/spear jobs, the crown), then swapRemoves barbs and
// squares at the same index. Callers removing several must go in descending index order.
void removeBarb(vector<barb>& barbs, vector<RectangleShape>& squares, int i,
                smokehouseSet& sh, forge& fg, storehouse (&stores)[raceCount], fieldGrid& fields) {
    barb& b = barbs[i];
    releaseBarbWork(b, sh, fg, stores, fields);   // garrison state (INBOX-F1) lives in the struct and goes with the swapRemove
    ungarrison(b);                                // R31-F3: the shared reset also frees any keep slot before the swapRemove
    expeditionMemberLost(b);                      // R45-F2: invalidates its party id; expeditionTick() dissolves the party
    setWeapon(b, weaponDrop);                     // R47-F8: the unequip half of the helper (craft already refunded above)
    clearWatch(b);                                // R51-F1: explicit, though releaseBarbWork()'s clearTraining() already did it
    raidMemberLost(b);                            // R52-F1: drops its raid id; raidTick() aborts under kRaidMinMembers raiders
    settleLoot(b, false);                         // R53-F1: a dead raider's pack goes back to the robbed race before the swapRemove
    clearRaid(b);
    caravanMemberLost(b);                         // R54-F1: drops its caravan id; caravanTick() aborts the trip next tick
    endActivity(b, actCaravan);
    clearSick(b);                                 // R52-F4
    clearKneel(b);                                // R52-F3: explicit, though releaseBarbWork()'s clearTraining() already did it
    if (isLeader(b)) loseLeader(b.race);
    printf("BARB DIED - %s the %s (%s, %s), id %d - %d left\n", b.name, traitOf(b).name, raceOf(b).name,
           isGentry(b) ? "Gentry" : "Commoner", b.id, (int)barbs.size() - 1);
    addGrave(b, squares[i]);   // R47-F9: copied out by value; nothing in graves points back at barbs
    veilDeposit(b.race, squares[i].getPosition() + squares[i].getSize() / 2.0f, veilSoulPerDeath);   // R50-F1a: by value
    veilAddSite(b.race, squares[i].getPosition() + squares[i].getSize() / 2.0f, b.name);   // R50-F1b: mote site, by value
    b.homeHouse = -1;
    clearRoute(b);   // R39-F1: the route lives in the struct; the swapped-in barb keeps its own route
    swapRemove(barbs, i);
    swapRemove(squares, i);
}

// R30-F5: end of tick. Every barb at 0 hp is collected first, then removed once each, highest index first, so a
// swapRemove only ever pulls in a living barb from the back and the lower dead indices stay valid.
void starveDeathTick(vector<barb>& barbs, vector<RectangleShape>& squares,
                     smokehouseSet& sh, forge& fg, storehouse (&stores)[raceCount], fieldGrid& fields) {
    vector<int> deadIds;
    for (auto& b : barbs) if (b.health <= 0) deadIds.push_back(b.id);
    if (deadIds.empty()) return;
    vector<int> dead;   // ids -> current indices, resolved once before any removal
    for (int id : deadIds) { int i = findBarbById(barbs, id); if (i >= 0) dead.push_back(i); }
    sort(dead.begin(), dead.end(), [](int a, int b) { return a > b; });
    for (int i : dead) {
        if (i >= (int)barbs.size() || i >= (int)squares.size()) continue;
        printf(barbs[i].sick > 0 ? "COUGHED - " : "STARVED - ");   // R52-F4
        removeBarb(barbs, squares, i, sh, fg, stores, fields);
    }
}

// INBOX-F1: town bell. raceBell[r] = ticks left before race r's bell falls silent (0 = silent); refreshed while a wolf is near.
int raceBell[raceCount] = {};
int raceBellAge[raceCount] = {};       // ticks since the bell started; the bell times out at bellMaxTicks
int raceBellCooldown[raceCount] = {};  // ticks before race r may ring again

int findHouseById(const vector<house>& houses, int id) {
    if (id < 0) return -1;
    for (int i = 0; i < (int)houses.size(); i++) if (houses[i].id == id) return i;
    return -1;
}

Vector2f garrisonDoorstep(const RectangleShape& hs) { return snapPx(hs.getPosition()) + garrisonDoorOffset; }

// R31-F3: keep gate (barb hitbox top-left), the keep's builder stand spot just below the gate
Vector2f keepDoorstep(int r) { return Vector2f((float)keeps[r].x, (float)keeps[r].y) + keepStandOffset; }

// The only way out of the garrison state: the barb stands idle on the doorstep (or wherever it got to)
// R31-F3: also the one place a keep slot is freed (bell-off, death via removeBarb(), lost keep, failed validation)
void ungarrison(barb& b) {
    for (auto& slots : keepSlots) slots.erase(std::remove(slots.begin(), slots.end(), b.id), slots.end());
    endActivity(b, actShelter);   // R53-F4: safe on a barb that isn't sheltering
    b.garrisonInside = false;
    b.garrisonHouseId = -1;
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    b.target = targetNone;
}

// R48-F1: the safe zone exists only while the campfire is lit
bool inProwlerSafeZone(Vector2f p, const campfire& fire) {
    Vector2f d = p - villageCenter;
    return fire.lit && d.x * d.x + d.y * d.y < prowlerSafeRadius * prowlerSafeRadius;
}

// R48-F1: a barb a prowler may chase: on the map surface, alive, and outside the lit safe zone
bool prowlerBarbOk(const barb& b, Vector2f centre, const campfire& fire) {
    return b.health > 0 && !barbHidden(b) && !barbUnderground(b) && !inProwlerSafeZone(centre, fire);
}

// R48-F1: the one way a prowler starts to go (dawn or the campfire); removal happens when the fade runs out
void dissolveProwler(prowler& p, const char* why) {
    if (p.fade > 0) return;
    p.fade = prowlerFadeTicks;
    p.targetBarbId = p.targetHouseId = -1;
    printf("PROWLER %d dissolves (%s) at (%.0f, %.0f)\n", p.id, why, p.pos.x, p.pos.y);
}

// R48-F1: runs right after wolfTick(). Spawns up to tonight's era cap at the map edge, chases the nearest barb or house
// outside the safe zone (throttled search, ids re-validated every tick), bites barbs, and dissolves at dawn or in the
// lit safe zone. Deaths are left to starveDeathTick() at the end of the tick (health <= 0).
void prowlerTick(vector<prowler>& prowlers, vector<barb>& barbs, const vector<RectangleShape>& squares,
                 const vector<house>& houses, const vector<RectangleShape>& houseshapes, const campfire& fire) {
    static bool wasNight = false;
    static int spawnedTonight = 0;
    bool night = isNightHour(game_time);
    if (night && !wasNight) spawnedTonight = 0;   // dusk: a fresh night's allowance
    wasNight = night;

    int cap = prowlerCapByEra[min((int)currentEra, 2)];
    if (night && spawnedTonight < cap && uniform_int_distribution<int>(0, prowlerSpawnChance - 1)(gen) == 0) {
        prowler p;
        p.id = nextProwlerId++;
        p.hp = prowlerMaxHp;   // R51-F1: the one spawn site
        p.flashFrames = 0;
        float lo = prowlerEdgeInsetPx, hi = worldSize - prowlerEdgeInsetPx - prowlerSize.x;
        float a = uniform_real_distribution<float>(lo, hi)(gen);
        switch (uniform_int_distribution<int>(0, 3)(gen)) {
            case 0:  p.pos = Vector2f(lo, a); break;
            case 1:  p.pos = Vector2f(hi, a); break;
            case 2:  p.pos = Vector2f(a, lo); break;
            default: p.pos = Vector2f(a, hi); break;
        }
        prowlers.push_back(p);
        const char* dir = compassName(p.pos - villageCenter);
        printf("PROWLER %d arriving from the %s (%.0f, %.0f) - %d of %d tonight\n", p.id, dir, p.pos.x, p.pos.y,
               spawnedTonight + 1, cap);
        if (spawnedTonight == 0) logEvent(string("Something stirs in the dark to the ") + dir, prowlerEventColor);
        spawnedTonight++;
    }

    for (int i = (int)prowlers.size() - 1; i >= 0; i--) {   // backward, so swapRemove only pulls in a visited prowler
        prowler& p = prowlers[i];
        if (p.fade > 0) {
            if (--p.fade <= 0) swapRemove(prowlers, i);
            continue;
        }
        Vector2f c = p.pos + prowlerSize / 2.0f;
        if (!night) { dissolveProwler(p, "dawn"); continue; }
        if (inProwlerSafeZone(c, fire)) { dissolveProwler(p, "campfire"); continue; }
        if (p.biteCooldown > 0) p.biteCooldown--;

        // re-validate the stored target by id every tick
        int bi = findBarbById(barbs, p.targetBarbId);
        if (bi >= 0 && (bi >= (int)squares.size()
                        || !prowlerBarbOk(barbs[bi], squares[bi].getPosition() + squares[bi].getSize() / 2.0f, fire))) bi = -1;
        if (bi < 0) p.targetBarbId = -1;
        int hi = findHouseById(houses, p.targetHouseId);
        if (hi >= 0 && (hi >= (int)houseshapes.size()
                        || inProwlerSafeZone(houseshapes[hi].getPosition() + houseshapes[hi].getSize() / 2.0f, fire))) hi = -1;
        if (hi < 0) p.targetHouseId = -1;

        if (--p.searchTimer <= 0) {   // throttled nearest search over barbs and houses only
            p.searchTimer = prowlerSearchTicks;
            float best = numeric_limits<float>::infinity();
            int nb = -1, nh = -1;
            for (int k = 0; k < (int)barbs.size() && k < (int)squares.size(); k++) {
                Vector2f bc = squares[k].getPosition() + squares[k].getSize() / 2.0f;
                if (!prowlerBarbOk(barbs[k], bc, fire)) continue;
                Vector2f d = bc - c;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < best) { best = dSq; nb = k; }
            }
            for (int k = 0; k < (int)houses.size() && k < (int)houseshapes.size(); k++) {
                Vector2f hc = houseshapes[k].getPosition() + houseshapes[k].getSize() / 2.0f;
                if (houses[k].id == p.sniffedHouseId || inProwlerSafeZone(hc, fire)) continue;
                Vector2f d = hc - c;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < best) { best = dSq; nh = k; nb = -1; }
            }
            bi = nb; hi = nh;
            p.targetBarbId = nb >= 0 ? barbs[nb].id : -1;
            p.targetHouseId = nh >= 0 ? houses[nh].id : -1;
        }

        // no target: drift toward the village (and so into the fire's light, if it is lit)
        Vector2f goal = villageCenter;
        if (bi >= 0) goal = squares[bi].getPosition() + squares[bi].getSize() / 2.0f;
        else if (hi >= 0) goal = houseshapes[hi].getPosition() + houseshapes[hi].getSize() / 2.0f;
        Vector2f diff = goal - c;
        float dSq = diff.x * diff.x + diff.y * diff.y;

        if (bi >= 0 && dSq <= prowlerBiteRangePx * prowlerBiteRangePx && p.biteCooldown == 0) {
            barb& b = barbs[bi];
            b.health = max(0, b.health - prowlerBiteDamage);
            p.biteCooldown = prowlerBiteCooldownTicks;
            printf("PROWLER %d bit %s (id %d) - %d hp\n", p.id, b.name, b.id, b.health);
            if (b.health <= 0) {
                logRaceEvent(string(b.name) + " was taken by a night prowler", b.race);
                clearWatch(b);   // R51-F1: off the watch at once on death, before starveDeathTick() removes the barb
            }
        }
        if (hi >= 0 && bi < 0 && dSq <= prowlerBiteRangePx * prowlerBiteRangePx) {   // sniffed the door: on to the next
            p.sniffedHouseId = p.targetHouseId;
            p.targetHouseId = -1;
            p.searchTimer = 0;
        }
        if (dSq >= 1.0f && ++p.stepTimer >= prowlerStepInterval) {
            p.stepTimer = 0;
            p.pos += diff / sqrt(dSq);
        }
    }
}

// R51-F1: index of the prowler with this id, or -1 (dissolved and removed, or never)
int findProwlerById(const vector<prowler>& prowlers, int id) {
    if (id < 0) return -1;
    for (int i = 0; i < (int)prowlers.size(); i++) if (prowlers[i].id == id) return i;
    return -1;
}

// R51-F1: patrol waypoint k (hitbox centre) on race r's ring
Vector2f watchStopPos(int r, int k) {
    float a = 2.0f * 3.14159265f * (float)(((k % watchRingStops) + watchRingStops) % watchRingStops) / (float)watchRingStops;
    return quarterCentre(clampRace(r)) + Vector2f(cos(a), sin(a)) * watchRingPx;
}

// R51-F1: a stop the barb can stand on: inside the map and off the lake
bool watchStopOk(Vector2f centre, Vector2f size) {
    Vector2f tl = centre - size / 2.0f;
    if (tl.x < prowlerEdgeInsetPx || tl.y < prowlerEdgeInsetPx || tl.x + size.x > worldSize - prowlerEdgeInsetPx
        || tl.y + size.y > worldSize - prowlerEdgeInsetPx) return false;
    return lakeClear(FloatRect(tl, size));
}

// R51-F1: may this barb take the watch tonight? Armed, drilled, hale enough, surface, and idle (standing or on a wander
// walk) or drilling (the watch outranks the dummy).
bool watchEligible(const barb& b) {
    if (b.activity == actWatch || !b.hasSpear || b.drillCount < kDrillPipCount || b.health < watchMinHealth) return false;
    if (inRaid(b)) return false;   // R52-F1: raiders and defenders belong to raidTick()
    if (!barbAvailable(b) || barbHidden(b) || barbUnderground(b) || b.inMine) return false;
    if (b.chopping || b.building || b.movingbspot || b.deertargeting || b.jobKind != jobNone || b.carryKind != carryNone
        || atForgeTask(b) || b.timer > 0 || b.target != targetNone || b.order != needNone) return false;
    bool wandering = b.targetx == b.wanderX && b.targety == b.wanderY;
    return !b.moving || wandering || b.activity == actDrill;
}

// R51-F1: the only place the watch state is set. Ends any drill first (clearTraining() also clears the watch, so the
// flag is set after it), drops any wander walk and starts at the nearest open stop on the ring.
void startWatch(barb& b, const RectangleShape& sq) {
    clearTraining(b);
    beginActivity(b, actWatch);
    b.watchPreyId = -1;
    b.watchCooldown = 0;
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.wanderX = b.wanderY = -1;
    b.targetx = b.targety = -1;
    Vector2f c = sq.getPosition() + sq.getSize() / 2.0f;
    float best = numeric_limits<float>::infinity();
    b.watchStop = 0;
    for (int k = 0; k < watchRingStops; k++) {
        Vector2f d = watchStopPos(b.race, k) - c;
        float dSq = d.x * d.x + d.y * d.y;
        if (dSq < best && watchStopOk(watchStopPos(b.race, k), sq.getSize())) { best = dSq; b.watchStop = k; }
    }
    printf("WATCH STARTED - barb %d (%s, %s), %d drills, stop %d\n", b.id, b.name, racePlurals[clampRace(b.race)],
           b.drillCount, b.watchStop);
}

// R51-F1: off the watch and standing still (dawn, or the barb can no longer walk the ring)
void endWatch(barb& b, const char* why) {
    printf("WATCH ENDED - barb %d (%s)\n", b.id, why);
    clearWatch(b);
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
}

// R51-F1: runs right after prowlerTick(). At night enlists eligible barbs (throttled, capped per race), walks each
// watchman round its ring and fights the nearest live prowler in range (coarse squared distance over watchmen x
// prowlers only; no tree or grid scan). A slain prowler is never removed here: dissolveProwler() starts the shared fade
// and prowlerTick() removes it later, so no prowler index moves during this tick. Dawn ends every watch.
void watchTick(vector<barb>& barbs, vector<RectangleShape>& squares, vector<prowler>& prowlers) {
    static int enlistTimer = 0;
    int n = min((int)barbs.size(), (int)squares.size());
    if (!isNightHour(game_time)) {
        for (int i = 0; i < n; i++) if (barbs[i].activity == actWatch) endWatch(barbs[i], "dawn");
        enlistTimer = 0;   // first scan on the first night tick
        return;
    }

    if (--enlistTimer <= 0) {
        enlistTimer = watchEnlistTicks;
        int on[raceCount] = {};
        for (int i = 0; i < n; i++) if (barbs[i].activity == actWatch) on[clampRace(barbs[i].race)]++;
        for (int i = 0; i < n; i++) {
            int r = clampRace(barbs[i].race);
            if (on[r] >= watchMaxPerRace || !watchEligible(barbs[i])) continue;
            startWatch(barbs[i], squares[i]);
            on[r]++;
        }
    }

    for (int i = 0; i < n; i++) {
        barb& b = barbs[i];
        RectangleShape& sq = squares[i];
        if (b.activity != actWatch) continue;
        if (b.health <= 0) { endWatch(b, "fell"); continue; }   // death: off the watch before starveDeathTick()
        if (!barbAvailable(b) || barbHidden(b) || barbUnderground(b) || !b.hasSpear) { endWatch(b, "called away"); continue; }
        if (b.watchCooldown > 0) b.watchCooldown--;
        Vector2f c = sq.getPosition() + sq.getSize() / 2.0f;

        // re-resolve the prey by id; a dissolving or far-off prowler is let go
        int pi = findProwlerById(prowlers, b.watchPreyId);
        if (pi >= 0) {
            Vector2f d = prowlers[pi].pos + prowlerSize / 2.0f - c;
            if (prowlers[pi].fade > 0 || d.x * d.x + d.y * d.y > watchLeashPx * watchLeashPx) pi = -1;
        }
        if (pi < 0) {
            b.watchPreyId = -1;
            float best = watchEngagePx * watchEngagePx;
            for (int k = 0; k < (int)prowlers.size(); k++) {
                if (prowlers[k].fade > 0) continue;
                Vector2f d = prowlers[k].pos + prowlerSize / 2.0f - c;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < best) { best = dSq; pi = k; }
            }
            if (pi >= 0) {
                b.watchPreyId = prowlers[pi].id;
                printf("WATCH ENGAGES - barb %d -> prowler %d\n", b.id, b.watchPreyId);
            }
        }

        if (pi >= 0) {   // fight: close in straight (short range), strike on cooldown
            prowler& p = prowlers[pi];
            Vector2f pc = p.pos + prowlerSize / 2.0f;
            Vector2f d = pc - c;
            if (d.x * d.x + d.y * d.y <= watchStrikeRangePx * watchStrikeRangePx) {
                b.moving = false; clearRoute(b);
                b.stepTimer = 0;
                if (b.watchCooldown == 0) {
                    b.watchCooldown = watchStrikeCooldownTicks;
                    int dmg = weaponDamage(b);
                    p.hp = max(0, p.hp - dmg);
                    p.flashFrames = prowlerFlashFrames;
                    printf("WATCH HIT - %s (id %d) hits prowler %d for %d, %d hp left\n", b.name, b.id, p.id, dmg, p.hp);
                    if (p.hp <= 0) {
                        logRaceEvent(string(b.name) + " " + weaponOf(b).killVerb + " a night prowler", b.race);
                        dissolveProwler(p, "slain");   // removal stays in prowlerTick(): indices here stay valid
                        b.watchPreyId = -1;
                    }
                }
            } else {
                Vector2f goal = pc - sq.getSize() / 2.0f;
                b.targetx = (int)goal.x;
                b.targety = (int)goal.y;
                b.moving = true;
                walkStep(b, sq, true);
            }
            continue;
        }

        // patrol: walk to the current stop, then on to the next open one round the ring
        Vector2f stop = watchStopPos(b.race, b.watchStop);
        if (!watchStopOk(stop, sq.getSize())) {
            int k = 1;
            while (k < watchRingStops && !watchStopOk(watchStopPos(b.race, b.watchStop + k), sq.getSize())) k++;
            if (k >= watchRingStops) { endWatch(b, "no ring to walk"); continue; }
            b.watchStop = (b.watchStop + k) % watchRingStops;
            stop = watchStopPos(b.race, b.watchStop);
        }
        Vector2f goal = stop - sq.getSize() / 2.0f;
        b.targetx = (int)goal.x;
        b.targety = (int)goal.y;
        Vector2f diff = Vector2f((float)b.targetx, (float)b.targety) - sq.getPosition();
        if (diff.x * diff.x + diff.y * diff.y < 1.0f) {
            b.watchStop = (b.watchStop + 1) % watchRingStops;
            b.moving = false; clearRoute(b);
            b.stepTimer = 0;
        } else {
            b.moving = true;
            walkStep(b, sq);
        }
    }
}

// R52-F3: the evening's rite. shrineId[r] is the race's shrine (by id, re-found every tick); peak[r] is the most of
// that race kneeling at their slots at once, reported in the end-of-rite ticker line.
struct riteState {
    bool active = false;
    int day = -1;                          // game_day of the last rite started, so each Rite day holds one rite
    int shrineId[raceCount] = {-1, -1, -1};
    int peak[raceCount] = {};
};
static_assert(raceCount == 3, "riteState::shrineId initialiser lists one -1 per race");
riteState rite;

// R52-F3 / R53-F3: the omen left by a race's rite. Written only by rollRiteOmen() and riteOmenTick(); everything else reads
// it through omenMult() (prowlers are a later phase).
struct riteOmen {
    bool active = false;
    bool blessed = false;                  // blessing or curse
    int kind = omenBirths;                 // the one effect it touches (omenKind)
    int kneelers = 0;                      // the rite's peak kneelers for this race
    int expiresDay = -1;                   // game_day the omen lapses at (checked every tick)
};
riteOmen riteOmens[raceCount];

// R53-F3: the single accessor. 1.0 with no omen or for the other kind; else the clamped blessing/curse multiplier.
float omenMult(int race, int kind) {
    if (race < 0 || race >= raceCount) return 1.0f;
    const riteOmen& o = riteOmens[race];
    if (!o.active || o.kind != kind) return 1.0f;
    float m = 1.0f;
    if (kind == omenBirths) m = o.blessed ? omenBlessBirthMult : omenCurseBirthMult;
    else if (kind == omenCough) m = o.blessed ? omenBlessCoughMult : omenCurseCoughMult;
    return min(omenMultMax, max(omenMultMin, m));
}

// R53-F3: is this race's omen good for it? Read through omenMult only (a blessing lifts births or lowers the cough).
int omenSign(int race) {
    float b = omenMult(race, omenBirths), c = omenMult(race, omenCough);
    if (b > 1.0f || c < 1.0f) return 1;
    if (b < 1.0f || c > 1.0f) return -1;
    return 0;
}

// R53-F3: roll a race's omen at the rite's end, weighted by its kneelers; a small race is never cursed
void rollRiteOmen(int r, int kneelers, int members) {
    float pBless = min(omenBlessMax, max(omenBlessMin, omenBlessBase + omenBlessPerKneeler * (float)(kneelers - 1)));
    bool blessed = members <= omenCurseFloor || uniform_real_distribution<float>(0.0f, 1.0f)(gen) < pBless;
    int kind = uniform_int_distribution<int>(0, omenKindCount - 1)(gen);
    riteOmen& o = riteOmens[r];
    o.active = true;
    o.blessed = blessed;
    o.kind = kind;
    o.kneelers = kneelers;
    o.expiresDay = game_day + riteOmenDays + 1;   // the rite ends at 23:00, so this lapses at the midnight 2 full days on
    const char* effect = kind == omenBirths ? (blessed ? "more births" : "fewer births")
                                            : (blessed ? "the cough keeps away" : "the cough spreads faster");
    printf("RITE OMEN - %s: %d knelt, p(bless) %.2f, %s (%s, x%.2f) until day %d\n", racePlurals[r], kneelers, pBless,
           blessed ? "blessing" : "curse", effect, omenMult(r, kind), o.expiresDay + 1);
    logRaceEvent(string("Omen for the ") + racePlurals[r] + ": " + (blessed ? "a blessing - " : "a curse - ") + effect +
                 " for " + to_string(riteOmenDays) + " days", r);
}

// R53-F3: lapse at the expiry midnight, with one "glow fades" ticker line
void riteOmenTick() {
    for (int r = 0; r < raceCount; r++) {
        riteOmen& o = riteOmens[r];
        if (!o.active || game_day < o.expiresDay) continue;
        printf("RITE OMEN LAPSED - %s (%s)\n", racePlurals[r], o.blessed ? "blessing" : "curse");
        logEvent(string("The ") + racePlurals[r] + "' shrine glow fades - the " + (o.blessed ? "blessing" : "curse") + " has passed",
                 riteTickerTint);
        o = riteOmen();
    }
}

// R52-F3: centre of a shrine's slab, the kneel ring's centre
Vector2f riteRingCentre(const shrine& s) { return Vector2f((float)s.x, (float)s.y) + shrineSlabSize / 2.0f; }

// R52-F3: top-left a barb of this body size stands at in slot k of the ring (first slot a little off the top, clear of the cap)
Vector2f riteSlotPos(const shrine& s, int k, Vector2f size) {
    float a = 6.2831853f * ((float)k + 0.5f) / (float)riteSlots - 1.5707963f;
    Vector2f p = riteRingCentre(s) + Vector2f(cos(a), sin(a)) * riteRingPx;
    return snapPx(p - size / 2.0f);
}

// R52-F3: idle on the surface (standing or on a wander walk), with no task, haul, hunt, order, drill, watch or raid
bool riteEligible(const barb& b) {
    if (b.activity == actKneel || b.health <= 0 || !barbAvailable(b) || barbHidden(b) || barbUnderground(b) || b.inMine) return false;
    if (b.activity == actDrill || b.activity == actWatch || b.chopping || b.building || b.movingbspot || b.deertargeting || b.jobKind != jobNone
        || b.carryKind != carryNone || atForgeTask(b) || b.timer > 0 || b.target != targetNone || b.order != needNone) return false;
    bool wandering = b.targetx == b.wanderX && b.targety == b.wanderY;
    return !b.moving || wandering;
}

// R52-F3: the only place the kneeling flag is set. clearTraining() first (it also clears the flag, so it is set after),
// then the walk to the slot.
void startKneel(barb& b, Vector2f slot) {
    clearTraining(b);
    beginActivity(b, actKneel);
    b.wanderX = b.wanderY = -1;
    b.targetx = (int)slot.x;
    b.targety = (int)slot.y;
    b.moving = true; clearRoute(b);
    b.stepTimer = 0;
}

// R52-F3: up from the ring and standing still (rite over, dawn, shrine gone, or the barb was called away)
void endKneel(barb& b, const char* why) {
    printf("KNEEL ENDED - barb %d (%s)\n", b.id, why);
    clearKneel(b);
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
}

// R52-F3: one ticker line naming who knelt; R53-F3: then a rolled omen for each race that did
void endRite(vector<barb>& barbs) {
    for (auto& b : barbs) if (b.activity == actKneel) endKneel(b, "rite over");
    rite.active = false;
    string who;
    int races = 0, total = 0;
    for (int r = 0; r < raceCount; r++) total += rite.peak[r] > 0 ? 1 : 0;
    for (int r = 0; r < raceCount; r++) {
        int k = rite.peak[r];
        if (k <= 0) continue;
        races++;
        who += races == 1 ? "" : races == total ? " and " : ", ";
        who += to_string(k) + " " + (k == 1 ? raceDefs[r].name : racePlurals[r]);
    }
    string line = total > 0 ? "The Rite ends: " + who + " knelt at the shrines" : string("The Rite ends: none knelt at the shrines");
    printf("RITE ENDED - %s\n", line.c_str());
    logEvent(line, riteTickerTint);
    // R53-F3: then one omen line per race that knelt (after the tally line, so the ticker reads in order)
    int members[raceCount] = {};
    for (auto& b : barbs) members[clampRace(b.race)]++;
    for (int r = 0; r < raceCount; r++) if (rite.peak[r] > 0) rollRiteOmen(r, rite.peak[r], members[r]);
}

// R52-F3: runs after barbmove(). On a Rite day from dusk to riteEndHour, idle barbs walk to a free slot on the ring round
// their race's shrine and kneel there; riteTick() owns their walk. The shrine is re-found by id every tick and a lost one
// sends its kneelers home. Dawn clears every kneel (backstop: the rite has normally ended at riteEndHour).
void riteTick(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<shrine>& shrines) {
    int n = min((int)barbs.size(), (int)squares.size());
    riteOmenTick();   // R53-F3: lapse first, so a rite ending this tick can roll a fresh omen

    bool inWindow = currentDecree == decreeRite && game_time >= nightStartHour && game_time < riteEndHour;
    if (!inWindow) {
        if (rite.active) endRite(barbs);
        if (game_time == nightEndHour)
            for (int i = 0; i < n; i++) if (barbs[i].activity == actKneel) endKneel(barbs[i], "dawn");
        return;
    }
    if (!rite.active) {
        if (rite.day == game_day) return;   // this Rite day's rite already ran
        rite.active = true;
        rite.day = game_day;
        for (int r = 0; r < raceCount; r++) {
            rite.peak[r] = 0;
            rite.shrineId[r] = findShrineById(shrines, veil.faithShrineId[r]) >= 0 ? veil.faithShrineId[r] : -1;
            for (int o = 0; o < r; o++)   // one race per ring: faithShrineTick() keeps them apart, this makes sure
                if (rite.shrineId[r] >= 0 && rite.shrineId[o] == rite.shrineId[r]) rite.shrineId[r] = -1;
        }
        printf("RITE BEGINS - shrines %d / %d / %d\n", rite.shrineId[0], rite.shrineId[1], rite.shrineId[2]);
    }

    int si[raceCount];
    for (int r = 0; r < raceCount; r++) {
        si[r] = findShrineById(shrines, rite.shrineId[r]);
        if (si[r] < 0 && rite.shrineId[r] >= 0) {
            printf("RITE - %s shrine %d is gone\n", racePlurals[r], rite.shrineId[r]);
            rite.shrineId[r] = -1;
        }
    }

    // walk the kneelers; any that can no longer kneel get up
    int knelt[raceCount] = {};
    bool taken[raceCount][riteSlots] = {};
    for (int i = 0; i < n; i++) {
        barb& b = barbs[i];
        if (b.activity != actKneel) continue;
        int r = clampRace(b.race);
        if (si[r] < 0) { endKneel(b, "shrine gone"); continue; }
        if (b.health <= 0 || !barbAvailable(b) || barbHidden(b) || barbUnderground(b) || b.inMine) { endKneel(b, "called away"); continue; }
        for (int k = 0; k < riteSlots; k++) {
            Vector2f p = riteSlotPos(shrines[si[r]], k, squares[i].getSize());
            if ((int)p.x == b.targetx && (int)p.y == b.targety) taken[r][k] = true;
        }
        Vector2f diff = Vector2f((float)b.targetx, (float)b.targety) - squares[i].getPosition();
        if (diff.x * diff.x + diff.y * diff.y < 1.0f) {
            b.moving = false; clearRoute(b);
            b.stepTimer = 0;
            knelt[r]++;
        } else {
            b.moving = true;
            walkStep(b, squares[i]);
        }
    }

    // bring idle barbs to free slots (every tick: a barb is idle for only the tick between one job and the next)
    for (int i = 0; i < n; i++) {
        barb& b = barbs[i];
        int r = clampRace(b.race);
        if (si[r] < 0 || !riteEligible(b)) continue;
        for (int k = 0; k < riteSlots; k++) {
            if (taken[r][k]) continue;
            Vector2f p = riteSlotPos(shrines[si[r]], k, squares[i].getSize());
            Vector2f from = squares[i].getPosition();
            if (!lakeClear(FloatRect(p, squares[i].getSize()), &from)) continue;
            taken[r][k] = true;
            startKneel(b, p);
            printf("KNEEL - %s (id %d, %s) to shrine %d, slot %d\n", b.name, b.id, racePlurals[r], rite.shrineId[r], k);
            break;
        }
    }
    for (int r = 0; r < raceCount; r++) rite.peak[r] = max(rite.peak[r], knelt[r]);
}

// R52-F3: one faint 1px ring round each rite shrine, in the world layer under barbs (so kneelers sit on it)
void drawRiteRings(RenderWindow& window, const vector<shrine>& shrines) {
    if (!rite.active) return;
    CircleShape ring(riteRingPx, 32);
    ring.setOrigin(Vector2f(riteRingPx, riteRingPx));
    ring.setFillColor(Color::Transparent);
    ring.setOutlineColor(riteRingColor);
    ring.setOutlineThickness(-1.0f);   // inward: a 1px line just inside the 10px radius, under the kneelers' bodies
    for (int r = 0; r < raceCount; r++) {   // riteTick() gives each race its own shrine, so no ring is drawn twice
        int si = findShrineById(shrines, rite.shrineId[r]);
        if (si < 0) continue;
        ring.setPosition(riteRingCentre(shrines[si]));   // slab centre sits on a pixel corner, as the 20px circle needs
        window.draw(ring);
    }
}

// R53-F3: while a race's omen holds, one thin faint ring round its shrine (gold blessing, ashen curse). One cached shape.
void drawOmenAuras(RenderWindow& window, const vector<shrine>& shrines) {
    static CircleShape aura = [] {
        CircleShape c(omenAuraPx, 40);
        c.setOrigin(Vector2f(omenAuraPx, omenAuraPx));
        c.setFillColor(Color::Transparent);
        c.setOutlineThickness(-1.0f);   // 1px just inside the radius
        return c;
    }();
    for (int r = 0; r < raceCount; r++) {
        int sign = omenSign(r);
        if (sign == 0) continue;
        int si = findShrineById(shrines, veil.faithShrineId[r]);
        if (si < 0) continue;
        aura.setOutlineColor(sign > 0 ? omenAuraBlessColor : omenAuraCurseColor);
        aura.setPosition(riteRingCentre(shrines[si]));
        window.draw(aura);
    }
}

// R48-F1: bodies, in the world layer before the night overlay; alpha follows the dissolve fade
// (R51-F1: pure white while a hit flash lasts)
void drawProwlers(RenderWindow& window, const vector<prowler>& prowlers) {
    RectangleShape body(prowlerSize - Vector2f(2.0f, 2.0f));   // 1px outline drawn outward keeps the 5x4 footprint
    for (auto& p : prowlers) {
        float a = p.fade > 0 ? (float)p.fade / prowlerFadeTicks : 1.0f;
        Color fill = p.flashFrames > 0 ? prowlerFlashColor : prowlerColor, rim = prowlerOutlineColor;
        if (p.flashFrames > 0) rim = prowlerFlashColor;
        fill.a = (uint8_t)(fill.a * a);
        rim.a = (uint8_t)(rim.a * a);
        body.setFillColor(fill);
        body.setOutlineThickness(1.0f);
        body.setOutlineColor(rim);
        body.setPosition(snapPx(p.pos) + Vector2f(1.0f, 1.0f));
        window.draw(body);
    }
}

// R48-F1: after the night overlay (and the lamps), so the eyes glow; plus the faint safe-zone ring while the fire is lit
void drawProwlerOverlay(RenderWindow& window, const View& worldView, const vector<prowler>& prowlers, const campfire& fire) {
    window.setView(worldView);
    if (isNightHour(game_time) && fire.lit) {
        CircleShape ring(prowlerSafeRadius, 96);
        ring.setOrigin({prowlerSafeRadius, prowlerSafeRadius});
        ring.setPosition(villageCenter);
        ring.setFillColor(Color::Transparent);
        ring.setOutlineThickness(1.0f);
        ring.setOutlineColor(prowlerSafeRingColor);
        window.draw(ring);
    }
    RectangleShape eye({1.0f, 1.0f});
    for (auto& p : prowlers) {
        Color col = prowlerEyeColor;
        if (p.fade > 0) col.a = (uint8_t)(255.0f * p.fade / prowlerFadeTicks);
        eye.setFillColor(col);
        Vector2f o = snapPx(p.pos);
        eye.setPosition(o + Vector2f(1.0f, 1.0f));
        window.draw(eye);
        eye.setPosition(o + Vector2f(3.0f, 1.0f));
        window.draw(eye);
    }
    // R51-F1: 1px hp bar, 1 row of grass above the body, only on a damaged prowler that is still fighting
    RectangleShape bar;
    for (auto& p : prowlers) {
        if (p.fade > 0 || p.hp >= prowlerMaxHp) continue;
        Vector2f o = snapPx(p.pos) + Vector2f(0.0f, -2.0f);
        float fillW = min(prowlerSize.x, ceil(prowlerSize.x * max(p.hp, 0) / prowlerMaxHp));   // any hp left shows >= 1px
        bar.setSize(Vector2f(prowlerSize.x, 1.0f));
        bar.setFillColor(healthBarBackColor);
        bar.setPosition(o);
        window.draw(bar);
        if (fillW > 0.0f) {
            bar.setSize(Vector2f(fillW, 1.0f));
            bar.setFillColor(healthBarFillColor);
            window.draw(bar);
        }
    }
}

// R51-F1: torch pixels collected by drawBarbs() (so they bob with the body), drawn here above the night overlay
void drawWatchTorches(RenderWindow& window, const View& worldView, int frame) {
    if (watchTorchPts.empty()) return;
    window.setView(worldView);
    bool night = isNightHour(game_time);
    RectangleShape glow({3.0f, 3.0f}), px({1.0f, 1.0f});
    glow.setFillColor(watchTorchGlowColor);
    for (int k = 0; k < (int)watchTorchPts.size(); k++) {
        Vector2f p = watchTorchPts[k];
        if (night) { glow.setPosition(p - Vector2f(1.0f, 1.0f)); window.draw(glow); }
        px.setFillColor(((frame + k * 3) / watchTorchFlickerFrames) % 3 == 0 ? watchTorchFlickerColor : watchTorchColor);
        px.setPosition(p);
        window.draw(px);
    }
}

// R51-F1: the faint patrol ring, only while the hovered or selected barb is on watch
void drawWatchRing(RenderWindow& window, const View& worldView, const vector<barb>& barbs, int selectedId, int hoveredId, float zoom) {
    window.setView(worldView);
    bool drawn[raceCount] = {};
    for (int id : {selectedId, hoveredId}) {
        int i = findBarbById(barbs, id);
        if (i < 0 || barbs[i].activity != actWatch) continue;
        int r = clampRace(barbs[i].race);
        if (drawn[r]) continue;
        drawn[r] = true;
        CircleShape ring(watchRingPx, watchRingPoints);
        ring.setOrigin({watchRingPx, watchRingPx});
        ring.setPosition(quarterCentre(r));
        ring.setFillColor(Color::Transparent);
        ring.setOutlineThickness(1.0f / zoom);
        ring.setOutlineColor(watchRingColor);
        window.draw(ring);
    }
}

// Runs right after wolfTick(). Rings/silences each race's bell, then walks garrisoned barbs to their doorstep and hides them.
// Garrisoning drops the barb's work through releaseBarbWork() (and frees the tree/rock/bush it was working); haulers finish
// their trip first. Every garrisoned barb's house is re-resolved by id each tick; a missing house releases the barb.
void bellTick(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, const vector<RectangleShape>& houseshapes,
              const vector<wolf>& wolves, storehouse (&stores)[raceCount], smokehouseSet& sh, forge& fg, fieldGrid& fields,
              vector<tree>& trees, const vector<CircleShape>& tris, vector<rock>& rocks, const vector<CircleShape>& octs,
              vector<bush>& bushes) {
    for (int r = 0; r < raceCount; r++) {
        if (raceBellCooldown[r] > 0) raceBellCooldown[r]--;
        bool wolfNear = false;
        Vector2f qc = quarterCentre(r);
        for (auto& w : wolves) {
            Vector2f d = w.shape.getPosition() + wolfSize / 2.0f - qc;
            if (d.x * d.x + d.y * d.y < bellWolfRadius * bellWolfRadius) wolfNear = true;
        }
        bool raidNear = raidThreat[r];            // R52-F1: raiders at the crate ring the same bell
        wolfNear = wolfNear || raidNear;
        if (!stores[r].built) wolfNear = false;   // no crate, no bell
        if (raceBell[r] > 0) {
            raceBellAge[r]++;
            if (wolfNear) raceBell[r] = bellQuietTicks;
            else raceBell[r]--;
            bool timedOut = raceBellAge[r] >= bellMaxTicks;
            if (raceBell[r] <= 0 || timedOut || !stores[r].built) {
                raceBell[r] = 0;
                raceBellCooldown[r] = bellCooldownTicks;
                int sheltered = 0;
                for (auto& b : barbs) if (b.activity == actShelter && b.race == r) sheltered++;
                printf("BELL SILENT - %s%s, %d leave shelter\n", racePlurals[r], timedOut ? " (timeout)" : "", sheltered);
                logRaceEvent(string(racePlurals[r]) + " leave shelter, the bell falls silent", r);
            }
        } else if (wolfNear && raceBellCooldown[r] == 0) {
            raceBell[r] = bellQuietTicks;
            raceBellAge[r] = 0;
            printf("BELL RUNG - %s, %s within %.0fpx of the crate\n", racePlurals[r], raidNear ? "raiders" : "wolf",
                   raidNear ? kRaidAlarmPx : bellWolfRadius);
            logRaceEvent(string(racePlurals[r]) + (raidNear ? " ring the bell: raiders at the crate!" : " ring the bell: wolf at the crate!"), r);
        }
    }
    for (int r = 0; r < raceCount; r++) {   // R31-F3: every slot must name a live barb of race r garrisoned in this keep
        auto& slots = keepSlots[r];
        for (int s = (int)slots.size() - 1; s >= 0; s--) {
            int j = findBarbById(barbs, slots[s]);
            bool dup = find(slots.begin(), slots.begin() + s, slots[s]) != slots.begin() + s;
            if (dup || j < 0 || barbs[j].activity != actShelter || barbs[j].garrisonHouseId != keepGarrisonId || barbs[j].race != r) {
                printf("KEEP SLOT DROPPED - %s, barb %d\n", racePlurals[r], slots[s]);
                slots.erase(slots.begin() + s);
            }
        }
        while ((int)slots.size() > keepSlotMax) slots.pop_back();   // should not happen: assignment checks the cap
    }
    for (int i = 0; i < (int)barbs.size() && i < (int)squares.size(); i++) {
        barb& b = barbs[i];
        auto& sq = squares[i];
        if (b.race < 0 || b.race >= raceCount) continue;
        if (b.activity == actShelter) {
            Vector2f spot;
            if (b.garrisonHouseId == keepGarrisonId) {   // R31-F3: keep refuge, validated against keepSlots by id
                const auto& slots = keepSlots[b.race];
                if (!keeps[b.race].built || find(slots.begin(), slots.end(), b.id) == slots.end()) {
                    printf("GARRISON RELEASED - barb %d, no keep slot\n", b.id);
                    ungarrison(b);
                    continue;
                }
                spot = keepDoorstep(b.race);
            } else {
                int h = findHouseById(houses, b.garrisonHouseId);
                if (h < 0 || h >= (int)houseshapes.size()) {
                    printf("GARRISON RELEASED - barb %d, house %d is gone\n", b.id, b.garrisonHouseId);
                    ungarrison(b);
                    continue;
                }
                spot = garrisonDoorstep(houseshapes[h]);
            }
            if (raceBell[b.race] == 0) { ungarrison(b); continue; }
            if (b.garrisonInside) continue;
            b.targetx = (int)spot.x;
            b.targety = (int)spot.y;
            b.moving = true;
            Vector2f pos = sq.getPosition();
            Vector2f diff = spot - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;
            if (dSq < 1.0f) {
                sq.setPosition(spot);
                b.garrisonInside = true;
                b.moving = false; clearRoute(b);
                b.stepTimer = 0;
            } else walkStep(b, sq);   // R39-F1
            continue;
        }
        if (!barbAvailable(b)) continue;   // R45-F2: a party member is out at the ruins, not at the crate
        if (raceBell[b.race] == 0 || b.carryKind != carryNone) continue;   // haulers deposit first, then shelter
        Vector2f pos = sq.getPosition();
        Vector2f dq = pos - quarterCentre(b.race);
        if (dq.x * dq.x + dq.y * dq.y >= bellGatherRadius * bellGatherRadius) continue;
        // R31-F3: the race's built keep first while it has a free slot; otherwise (full or not built) the house path
        bool toKeep = keeps[b.race].built && (int)keepSlots[b.race].size() < keepSlotMax;
        int best = -1;
        float bestDSq = 0.0f;
        if (!toKeep)
            for (int k = 0; k < (int)houses.size() && k < (int)houseshapes.size(); k++) {
                if (houses[k].race != b.race) continue;
                Vector2f d = garrisonDoorstep(houseshapes[k]) - pos;
                float dSq = d.x * d.x + d.y * d.y;
                if (best < 0 || dSq < bestDSq) { best = k; bestDSq = dSq; }
            }
        if (!toKeep && best < 0) continue;   // no free keep slot and no own-race house to hide in
        if (b.chopping && b.target == targetNone)   // the tree/rock/bush it was working is free for others again
            for (int k = 0; k < (int)trees.size() && k < (int)tris.size(); k++) {
                Vector2f p = tris[k].getPosition() + treeWorkOffset;
                if ((int)p.x == b.targetx && (int)p.y == b.targety) { trees[k].dying = false; break; }
            }
        if (b.chopping && b.target == targetRock)
            for (int k = 0; k < (int)rocks.size() && k < (int)octs.size(); k++) {
                Vector2f p = octs[k].getPosition() + rockWorkOffset;
                if ((int)p.x == b.targetx && (int)p.y == b.targety) { rocks[k].dying = false; break; }
            }
        if (b.chopping && b.target == targetBush && b.targetBush >= 0 && b.targetBush < (int)bushes.size())
            bushes[b.targetBush].picking = false;
        releaseBarbWork(b, sh, fg, stores, fields);
        b.target = targetNone;
        b.targetBush = b.targetShore = -1;
        b.wanderX = b.wanderY = -1;
        b.stepTimer = 0;
        beginActivity(b, actShelter);   // R53-F4: releaseBarbWork() above ended any forge task, drill, watch or kneel
        b.garrisonInside = false;
        Vector2f spot;
        if (toKeep) {   // R31-F3: the slot is held from now on, while running and while inside
            b.garrisonHouseId = keepGarrisonId;
            keepSlots[b.race].push_back(b.id);
            spot = keepDoorstep(b.race);
        } else {
            b.garrisonHouseId = houses[best].id;
            spot = garrisonDoorstep(houseshapes[best]);
        }
        b.targetx = (int)spot.x;
        b.targety = (int)spot.y;
        b.moving = true;
        if (toKeep) printf("GARRISON - barb %d (%s) heads for the keep (%d/%d)\n", b.id, racePlurals[b.race],
                           (int)keepSlots[b.race].size(), keepSlotMax);
        else printf("GARRISON - barb %d (%s) heads for house %d\n", b.id, racePlurals[b.race], houses[best].id + 1);
    }
}

// INBOX-F1: world layer, after the houses and crates. A static bell marker by each ringing race's crate; houses with
// barbs sheltering get a roof eased toward the bell gold plus one 1px pip per occupant above the roof (capped).
void drawGarrison(RenderWindow& window, const vector<barb>& barbs, const vector<house>& houses,
                  const vector<RectangleShape>& houseshapes, const storehouse (&stores)[raceCount]) {
    RectangleShape r(bellMarkerSize);
    r.setFillColor(bellMarkerColor);
    for (int k = 0; k < raceCount; k++) {
        if (raceBell[k] <= 0 || !stores[k].built) continue;
        r.setPosition(storehousePos[k] + bellMarkerOffset);
        window.draw(r);
    }
    for (int i = 0; i < (int)houses.size() && i < (int)houseshapes.size(); i++) {
        int n = 0;
        for (auto& b : barbs) if (barbHidden(b) && b.garrisonHouseId == houses[i].id) n++;
        if (n == 0) continue;
        const houseStyle& st = houseStyles[clampRace(houses[i].race)];
        Vector2f o = snapPx(houseshapes[i].getPosition());
        r.setFillColor(lerpColor(st.roof, garrisonRoofTint, garrisonRoofTintAmount));
        for (int k = 0; k < st.rows; k++) {   // same rows as drawHouses(), retinted on top
            r.setSize(Vector2f(st.width[k], 1.0f));
            r.setPosition(o + Vector2f(st.left[k], st.top + k));
            window.draw(r);
        }
        float top = st.top;
        if (st.chimneySize.x > 0.0f) top = min(top, st.chimneyPos.y);
        int pips = min(n, garrisonPipMax);
        float step = 1.0f + garrisonPipGap;
        float x0 = o.x + 2.0f - floor((pips - 1) * step / 2.0f);   // centred on the 4px house (x + 2)
        r.setSize(Vector2f(1.0f, 1.0f));
        r.setFillColor(bellMarkerColor);
        for (int k = 0; k < pips; k++) {
            r.setPosition(Vector2f(x0 + k * step, o.y + top - garrisonPipGap - 1.0f));
            window.draw(r);
        }
        r.setSize(bellMarkerSize);
        r.setFillColor(bellMarkerColor);
    }
}

// R30-F2: leader ranks its race's wood/stone/food shortage (0..1 each); worst wins, none if nothing is short
void rankRaceNeed(const vector<barb>& barbs, int r) {
    int members = 0;
    for (auto& b : barbs) members += b.race == r ? 1 : 0;
    const raceStock& rs = stockOfRace(r);   // RACE-F1
    float s[needCount];
    s[needWood]  = clamp((float)(houseWoodCost - rs.wood) / (float)houseWoodCost, 0.0f, 1.0f);
    s[needStone] = clamp((float)(houseStoneCost - rs.stone) / (float)houseStoneCost, 0.0f, 1.0f);
    s[needFood]  = clamp((float)(members - rs.food) / (float)max(1, members), 0.0f, 1.0f);   // one food per member
    int best = needNone;
    for (int n = 0; n < needCount; n++) if (s[n] > 0.0f && (best == needNone || s[n] > s[best])) best = n;
    raceNeed[r] = (leaderNeed)best;
    printf("LEADER NEED - %s: wood %.2f, stone %.2f, food %.2f -> %s\n", racePlurals[r], s[needWood], s[needStone], s[needFood],
           best == needNone ? "none" : leaderNeedNames[best]);
}

// INBOX-F2b: idle = no work, build, hunt, haul or job claim. Garrisoned barbs are never idle, so an order never touches shelter
bool orderIdle(const barb& b) {
    return barbAvailable(b) && !b.chopping && !b.building && !b.movingbspot && !b.deertargeting && b.jobKind == jobNone && b.carryKind == carryNone && !atForgeTask(b)   // R47-F7
        && b.activity != actDrill     // R47-F6: drillers take no orders and join no party (expedition preference deferred)
        && b.activity != actWatch     // R51-F1: nor do watchmen (and so they never start a drill)
        && b.activity != actHerb;     // R53-F2: nor do herb tenders (the bed visit is short)
}

// INBOX-F2b: right after the leader names its need, it hands that need as an individual order to idle members (not
// itself), at most orderRaceShare of the race. The race's old orders are cleared first, so no order outlives a day.
void issueOrders(vector<barb>& barbs, int r) {
    int members = 0;
    vector<int> idle;
    for (int i = 0; i < (int)barbs.size(); i++) {
        barb& b = barbs[i];
        if (b.race != r) continue;
        members++;
        b.order = needNone;
        b.orderTicks = 0;
        if (!isLeader(b) && orderIdle(b)) idle.push_back(i);
    }
    if (raceNeed[r] == needNone || idle.empty()) return;
    int cap = min((int)idle.size(), (int)(members * orderRaceShare));
    if (cap <= 0) return;
    shuffle(idle.begin(), idle.end(), gen);
    for (int k = 0; k < cap; k++) {
        barbs[idle[k]].order = raceNeed[r];
        barbs[idle[k]].orderTicks = orderDays * 24 * ticksPerGameHour;
    }
    printf("LEADER ORDERS - %s leader orders %d of %d (%d idle) to gather %s\n", racePlurals[r], cap, members, (int)idle.size(),
           leaderNeedNames[raceNeed[r]]);
    logEvent(string(racePlurals[r]) + " leader sends " + to_string(cap) + " for " + leaderNeedNames[raceNeed[r]]);
}

// INBOX-F2b: every tick. Orders run out after orderDays, and lapse at once if the race has no leader any more
void orderTick(vector<barb>& barbs) {
    for (auto& b : barbs) {
        if (b.order == needNone) continue;
        bool leaderless = b.race < 0 || b.race >= raceCount || !raceElected[b.race];
        if (--b.orderTicks <= 0 || leaderless) {
            printf("ORDER ENDED - barb %d (%s) %s order for %s\n", b.id, racePlurals[clampRace(b.race)],
                   leaderless ? "lost its leader's" : "finished its", leaderNeedNames[b.order]);
            b.order = needNone;
            b.orderTicks = 0;
        }
    }
}

void leaderTick(vector<barb>& barbs) {
    for (int r = 0; r < raceCount; r++) {
        if (raceElected[r] && findBarbById(barbs, raceLeaderId[r]) < 0) loseLeader(r);   // leader gone: no leader, may elect again
        if (raceElected[r]) { rankRaceNeed(barbs, r); issueOrders(barbs, r); continue; }   // INBOX-F2b
        vector<int> members;
        for (int i = 0; i < (int)barbs.size(); i++) if (barbs[i].race == r) members.push_back(i);
        if ((int)members.size() < electionThreshold) continue;
        float best = -1.0f;
        vector<int> picks;   // members whose trait this race likes most; one is drawn at random
        for (int i : members) {
            float like = raceTraitLike[r][barbs[i].trait];
            if (like > best) { best = like; picks.clear(); }
            if (like == best) picks.push_back(i);
        }
        const barb& cand = barbs[picks[uniform_int_distribution<int>(0, (int)picks.size() - 1)(gen)]];
        bernoulli_distribution approve(best);
        int yes = 0;
        for (int i : members) yes += (barbs[i].id == cand.id || approve(gen)) ? 1 : 0;
        int n = (int)members.size();
        string who = string(cand.name) + " the " + traitOf(cand).name;
        string tally = to_string(yes) + " of " + to_string(n);
        if (yes * 2 > n) {
            raceElected[r] = true;
            raceLeaderId[r] = cand.id;
            printf("ELECTION - %s crown %s, barb %d (%s approve)\n", racePlurals[r], who.c_str(), cand.id, tally.c_str());
            logEvent(string(racePlurals[r]) + " crown " + who + " (" + tally + " approve)");
            rankRaceNeed(barbs, r);   // R30-F2: the new leader names a need this same dawn
            issueOrders(barbs, r);    // INBOX-F2b: and hands out orders for it
        } else {
            printf("ELECTION - %s: no consensus on %s, barb %d (%s approve)\n", racePlurals[r], who.c_str(), cand.id, tally.c_str());
            logEvent(string(racePlurals[r]) + ": no consensus (" + tally + ")");
        }
    }
}

void councilTick(vector<barb>& barbs) {
    static int lastHour = game_time;
    if (game_time == lastHour) return;
    lastHour = game_time;
    if (game_time != councilDawnHour || barbs.empty()) return;
    int votes[decreeCount] = {};   // sized and zeroed from decreeCount, so a new decree needs no edit here
    int gentry = 0;
    for (auto& b : barbs) {
        bool g = isGentry(b);
        votes[castBallot(b)] += g ? gentryVoteWeight : 1;   // R24-F1: founders' ballots count double
        gentry += g ? 1 : 0;
    }
    ballotFramesLeft = ballotShowFrames;
    int top = *max_element(votes, votes + decreeCount);
    vector<int> tied;
    for (int d = 0; d < decreeCount; d++) if (votes[d] == top) tied.push_back(d);
    currentDecree = (councilDecree)tied[uniform_int_distribution<int>(0, (int)tied.size() - 1)(gen)];
    string tally, counts;   // "Timber 3, Stone 1, ..." for the log, "3-1-..." for the ticker, both in decree order
    for (int d = 0; d < decreeCount; d++) {
        tally += string(d ? ", " : "") + decreeNames[d] + " " + to_string(votes[d]);
        counts += (d ? "-" : "") + to_string(votes[d]);
    }
    printf("DAWN COUNCIL - %s -> %s (Gentry %d x%d, Commoners %d)\n", tally.c_str(),
           decreeNames[currentDecree], gentry, gentryVoteWeight, (int)barbs.size() - gentry);
    logEvent(string("Council decrees ") + decreeNames[currentDecree] + " (" + counts + ")");
    if (gentry > 0)   // R24-F1 ticker line: who carried the weight this dawn
        logEvent(to_string(gentry) + " Gentry cast " + to_string(gentry * gentryVoteWeight) + " of " +
                 to_string(gentry * gentryVoteWeight + (int)barbs.size() - gentry) + " votes");
    leaderTick(barbs);   // R28-F2: elections ride the same dawn
}

int popCap(const vector<house>& houses) {
    return (int)houses.size() * popPerHouse;
}

// R45-F2: the one walk/work reset, on entry to and exit from an expedition. Eligible barbs hold no job, haul, hunt or
// temper, so this only drops wander walks, stale targets and the route.
void expeditionReset(barb& b) {
    if (b.deertargeting) clearHunt(b);
    b.deertargeting = false;
    b.moving = b.chopping = b.building = b.movingbspot = false;
    b.timer = 0;
    clearRoute(b);
    leaveMine(b);
    clearSmelt(b);   // R47-F7: eligibility (orderIdle) already rules smelters out; this keeps the forge claim honest regardless
    clearTraining(b);   // R47-F6: likewise for drillers and their slot
    b.stepTimer = 0;
    b.target = targetNone;
    b.targetBush = b.targetShore = -1;
    b.targetx = b.targety = -1;
    b.wanderX = b.wanderY = -1;
}

// R45-F2: may b join a party of race r? Idle (orderIdle), not walking to work, fed, healthy, and not the leader.
bool expeditionEligible(const barb& b, int r) {
    bool wandering = !b.moving || (b.targetx == b.wanderX && b.targety == b.wanderY);
    return b.race == r && orderIdle(b) && wandering && b.target == targetNone && b.timer == 0 && !b.inMine
        && !b.shinobi && !isLeader(b) && !isHungry(b) && b.health >= expMinHealth;
}

// R45-F2: where member k of the party stands at the end of the current leg (hitbox top-left)
Vector2f expeditionGoal(int k) {
    int r = clampRace(party.race);
    Vector2f along(-expMemberGapPx * k, 0.0f);
    if (party.phase == expeditionParty::Out && party.poiId < 0 && !party.hollow) return campSpot[r] + campStandOffset + along;   // R47-F5: the Hollow walks straight to the mouth
    if (party.phase != expeditionParty::Back) return party.delveSpot[k];   // R47-F4: a site trip walks straight to the site
    return storehousePos[r] + storehouseStandOffset + along;
}

// R45-F2: the one way a party ends. Every member still standing gets the reset and its flag cleared; idx[k] < 0 = gone.
void endExpedition(vector<barb>& barbs, const int (&idx)[expPartySize]) {
    for (int k = 0; k < expPartySize; k++) {
        if (idx[k] < 0 || idx[k] >= (int)barbs.size() || barbs[idx[k]].activity != actExpedition) continue;
        expeditionReset(barbs[idx[k]]);
        endActivity(barbs[idx[k]], actExpedition);
    }
    party.active = false;
    party.race = -1;
    for (auto& id : party.memberIds) id = -1;
    party.rations = party.timer = party.woundCooldown = 0;
    party.relic = false;
    party.lostName = nullptr;
    party.poiId = -1;
    party.rewardFood = party.rewardStone = 0;
    party.hollow = false;   // R47-F5
    party.lootIdx = -1;
    expeditionCooldown = expCooldownTicks;
}

// R47-F4: the one way a site is found (a passing barb or an arriving party): flag, printf and one ticker line
void discoverPoi(int i, int r, const char* who) {
    if (i < 0 || i >= (int)pois.size() || pois[i].found) return;
    pois[i].found = true;
    Vector2f c = poiCentre(i);
    printf("---SITE FOUND--- %s %d at (%.0f, %.0f) by %s (%s)\n", poiNames[pois[i].kind], pois[i].id, c.x, c.y, who, racePlurals[clampRace(r)]);
    logRaceEvent(string(who) + " comes upon a " + poiNames[pois[i].kind], r);
}

// R47-F4: where a party of race r goes. The nearest undiscovered unspent site competes with the ruins (if ruinsOk) by
// distance from the crate; then the ruins; then the nearest found but unspent site. Spent sites are never picked.
// Returns a pois index (valid only until the next removal: the caller stores the id), -1 = the ruins, -2 = nowhere,
// expSiteHollow = the Hollow. R47-F5: the Hollow (hollowOk) sits at the ruins, so it competes at the ruins' distance;
// when both are open one is picked at hollowPickChance.
const int expSiteHollow = -3;
int pickExpeditionSite(int r, bool ruinsOk, bool hollowOk) {
    Vector2f from = storehousePos[clampRace(r)];
    float far = 2.0f * worldSize, uDSq = 0.0f;
    int n = (int)pois.size();
    int u = findNearest(from, far, n, [&](int i) { return poiCentre(i); },
                        [&](int i) { return pois[i].spent || pois[i].found; }, &uDSq);
    if (ruinsOk || hollowOk) {
        Vector2f d = ruinCentre - from;
        if (u < 0 || d.x * d.x + d.y * d.y < uDSq) {
            if (!ruinsOk) return expSiteHollow;
            if (!hollowOk) return -1;
            return uniform_real_distribution<float>(0.0f, 1.0f)(gen) < hollowPickChance ? expSiteHollow : -1;
        }
    }
    if (u >= 0) return u;
    int f = findNearest(from, far, n, [&](int i) { return poiCentre(i); }, [&](int i) { return pois[i].spent; });
    return f >= 0 ? f : -2;
}

// R45-F2: tries one race (random start, fair) with a lean-to, a crate, a silent bell, relics under the cap and food for
// the rations plus expFoodReserve. Members: the nearest eligible barbs to the crate via findNearest, each skipping the
// ones already picked; delve stones the same way from the camp.
void tryLaunchExpedition(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    if ((!hasRuins() && pois.empty()) || uniform_int_distribution<int>(0, expLaunchOneIn - 1)(gen) != 0) return;
    int start = uniform_int_distribution<int>(0, raceCount - 1)(gen);
    int n = min((int)barbs.size(), (int)squares.size());
    for (int o = 0; o < raceCount; o++) {
        int r = (start + o) % raceCount;
        if (!stores[r].built || raceBell[r] > 0) continue;
        if (stock[r].food < expRationFood + expFoodReserve) continue;
        // R47-F4: the ruins still need the race's lean-to and relics under the cap; sites need neither
        // R47-F5: the Hollow needs the lean-to too (the race's foothold at the ruins), but not the relic cap
        int site = pickExpeditionSite(r, hasRuins() && campBuilt(r) && raceRelics[r] < expRelicCap, hollowOpen() && campBuilt(r));
        if (site == -2) continue;
        bool hollowTrip = site == expSiteHollow;
        int pick[expPartySize];
        bool ok = true;
        for (int k = 0; k < expPartySize && ok; k++) {
            pick[k] = findNearest(storehousePos[r], expRecruitRadius, n,
                                  [&](int i) { return squares[i].getPosition(); },
                                  [&](int i) {
                                      for (int q = 0; q < k; q++) if (pick[q] == i) return true;
                                      return !barbAvailable(barbs[i]) || !expeditionEligible(barbs[i], r);
                                  });
            ok = pick[k] >= 0;
        }
        if (!ok) continue;
        Vector2f camp = campSpot[r] + campSize / 2.0f;
        int stone[expPartySize] = {};
        for (int k = 0; k < expPartySize && site == -1; k++) {   // R47-F4: ruin trips only (R47-F5: not the Hollow)
            stone[k] = findNearest(camp, worldSize, (int)ruinStones.size(),
                                   [&](int i) { return ruinStones[i].position + ruinStones[i].size / 2.0f; },
                                   [&](int i) { for (int q = 0; q < k; q++) if (stone[q] == i) return true; return false; });
            if (stone[k] < 0) stone[k] = 0;   // fewer stones than members (not with the current layout): share one
        }
        party = expeditionParty{};
        party.active = true;
        party.race = r;
        party.phase = expeditionParty::Out;
        spend(stock[r], 0, 0, expRationFood);
        party.rations = expRationFood;
        if (site >= 0) { party.poiId = pois[site].id; party.siteKind = pois[site].kind; }   // R47-F4: the id, never the index
        party.hollow = hollowTrip;   // R47-F5
        string names;
        for (int k = 0; k < expPartySize; k++) {
            barb& b = barbs[pick[k]];
            expeditionReset(b);
            beginActivity(b, actExpedition);
            b.order = needNone;
            b.orderTicks = 0;
            party.memberIds[k] = b.id;
            if (site >= 0 || hollowTrip) {   // R47-F4: members flank the site at its foot, even k on the left, odd on the right (R47-F5: or the mouth)
                Vector2f p = hollowTrip ? hollowPos : poiShapes[site].getPosition(), s = hollowTrip ? hollowSize : poiShapes[site].getSize();
                float row = p.y + s.y - squares[pick[k]].getSize().y;
                float out = (k / 2) * expMemberGapPx;
                party.delveSpot[k] = snapPx(k % 2 == 0 ? Vector2f(p.x - squares[pick[k]].getSize().x - 1.0f - out, row)
                                                       : Vector2f(p.x + s.x + 1.0f + out, row));
            } else {
                const FloatRect& s = ruinStones[stone[k]];   // stand at the stone's foot, centred under it
                party.delveSpot[k] = snapPx(Vector2f(s.position.x + s.size.x / 2.0f - 1.0f, s.position.y + s.size.y));
            }
            names += string(k ? " and " : "") + b.name;
        }
        string dest = site >= 0 ? string(pois[site].found ? "the " : "an unexplored ") + poiNames[party.siteKind]
                    : hollowTrip ? string("the Hollow") : string("the ruins");
        printf("---EXPEDITION OUT--- %s: %s to %s, %d rations (food %d left)\n", racePlurals[r], names.c_str(), dest.c_str(), party.rations, stock[r].food);
        logRaceEvent(string(racePlurals[r]) + " send " + names + " to " + dest, r);
        return;
    }
}

// R45-F2: runs after bellTick() (wolves have moved) and before hungerTick(). Re-resolves every member by id each tick;
// a missing, dead or released member dissolves the party. Feeds hungry members from the rations, lets wolves on the road
// wound (never below expWolfMinHp), walks each leg and advances the phase once every member has arrived.
void expeditionTick(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<wolf>& wolves, const storehouse (&stores)[raceCount]) {
    if (hollowFlash > 0) hollowFlash--;   // R47-F5: lintel flash after surfacing
    if (!party.active) {
        if (expeditionCooldown > 0) expeditionCooldown--;
        else tryLaunchExpedition(barbs, squares, stores);
        return;
    }
    int r = clampRace(party.race);
    int idx[expPartySize];
    const char* lost = party.lostName;
    bool broken = false;
    for (int k = 0; k < expPartySize; k++) {
        idx[k] = findBarbById(barbs, party.memberIds[k]);
        bool bad = idx[k] < 0 || idx[k] >= (int)squares.size() || barbs[idx[k]].activity != actExpedition || barbs[idx[k]].race != r;
        if (!bad && barbs[idx[k]].health <= 0) { bad = true; if (!lost) lost = barbs[idx[k]].name; }
        if (bad) { broken = true; if (idx[k] >= (int)squares.size()) idx[k] = -1; }
    }
    if (broken) {
        if (party.hollow) {   // R47-F5: released at the mouth (endExpedition clears the flag, so nobody stays hidden), rations refunded, no haul taken
            bool below = party.phase == expeditionParty::Delve;
            stock[r].food += party.rations;
            printf("---EXPEDITION LOST--- %s: %s %s the Hollow, party released (%d rations returned, %d hauls left)\n", racePlurals[r],
                   lost ? lost : "a member", below ? "below" : "on the way to", party.rations, hollowLootLeft);
            logRaceEvent(string(racePlurals[r]) + "' Hollow party breaks up: " + (lost ? lost : "a member") + " is lost"
                         + (below ? ", the rest climb out" : "") + "; rations returned", r);
            endExpedition(barbs, idx);
            return;
        }
        printf("---EXPEDITION LOST--- %s: %s, party dissolved (%d rations lost)\n", racePlurals[r], lost ? lost : "a member", party.rations);
        logRaceEvent(string(racePlurals[r]) + "' expedition breaks up: " + (lost ? lost : "a member") + " is lost", r);
        endExpedition(barbs, idx);
        return;
    }
    if (party.woundCooldown > 0) party.woundCooldown--;
    int site = -1;   // R47-F4: re-looked up by id every tick, used only within this tick
    if (party.poiId >= 0 && party.phase != expeditionParty::Back) {
        site = findPoiById(party.poiId);
        if (site < 0 || pois[site].spent) {   // gone or already searched: turn for home empty-handed
            printf("EXPEDITION SITE GONE - %s turn home from the %s\n", racePlurals[r], poiNames[party.siteKind]);
            party.phase = expeditionParty::Back;
            party.relic = false;
            party.rewardFood = party.rewardStone = 0;
            site = -1;
        }
    }
    for (int k = 0; k < expPartySize; k++) {   // rations: a member eats on the road instead of from the larder
        barb& b = barbs[idx[k]];
        if (!isHungry(b) || party.rations <= 0) continue;
        party.rations--;
        b.hunger = 0;
        b.slowTick = 0;
        b.starveTicks = 0;
        b.health = min(barbMaxHealth, b.health + eatHealHp);
        printf("EXPEDITION RATION - %s ate, %d left\n", b.name, party.rations);
    }
    if (party.phase != expeditionParty::Delve && party.woundCooldown == 0)   // wolves only meet the party on the road
        for (int k = 0; k < expPartySize && party.woundCooldown == 0; k++) {
            barb& b = barbs[idx[k]];
            Vector2f c = squares[idx[k]].getPosition() + squares[idx[k]].getSize() / 2.0f;
            for (auto& w : wolves) {
                Vector2f d = w.shape.getPosition() + wolfSize / 2.0f - c;
                if (d.x * d.x + d.y * d.y >= expWolfRadius * expWolfRadius) continue;
                b.health = max(min(b.health, expWolfMinHp), b.health - expWolfWoundHp);   // a wound, never the kill
                party.woundCooldown = expWolfWoundCooldownTicks;
                printf("EXPEDITION WOUND - %s (%s) bitten by a wolf, %d hp\n", b.name, racePlurals[r], b.health);
                logRaceEvent(string(b.name) + " is bitten by a wolf on the road", r);
                break;
            }
        }
    bool allThere = true;
    for (int k = 0; k < expPartySize; k++) {
        barb& b = barbs[idx[k]];
        auto& sq = squares[idx[k]];
        Vector2f goal = expeditionGoal(k);
        b.targetx = (int)goal.x;
        b.targety = (int)goal.y;
        Vector2f diff = Vector2f((float)b.targetx, (float)b.targety) - sq.getPosition();
        if (diff.x * diff.x + diff.y * diff.y < 1.0f) {
            sq.setPosition(Vector2f((float)b.targetx, (float)b.targety));
            if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
        } else {
            allThere = false;
            b.moving = true;
            walkStep(b, sq);
        }
    }
    if (!allThere) return;
    if (party.phase == expeditionParty::Out) {
        party.phase = expeditionParty::Delve;
        party.timer = 0;
        if (site >= 0) {   // R47-F4: arriving finds the site, through the same path as a passing barb
            discoverPoi(site, r, barbs[idx[0]].name);
            printf("EXPEDITION AT SITE - %s, searching the %s\n", racePlurals[r], poiNames[party.siteKind]);
        } else if (party.hollow) {   // R47-F5: down the stairs; barbInHollow() hides them from now until they surface
            printf("EXPEDITION DESCENDS - %s into the Hollow (%d hauls left)\n", racePlurals[r], hollowLootLeft);
            logRaceEvent(string(barbs[idx[0]].name) + " and " + barbs[idx[expPartySize - 1]].name + " descend into the Hollow", r);
        } else printf("EXPEDITION AT CAMP - %s, to the stones\n", racePlurals[r]);
    } else if (party.phase == expeditionParty::Delve) {
        if (++party.timer < (site >= 0 ? poiSearchTicks : expDelveTicks)) return;   // R47-F5: the one room is abstracted as expDelveTicks
        uniform_real_distribution<float> roll01(0.0f, 1.0f);
        if (party.hollow) {   // R47-F5: one weighted roll of the capped table; an empty Hollow (another party took the last) gives nothing
            party.lootIdx = hollowLootCount - 1;   // "nothing but dust"
            if (hollowLootLeft > 0) {
                float total = 0.0f;
                for (auto& l : hollowLoot) total += l.weight;
                float roll = roll01(gen) * total;
                for (int i = 0; i < hollowLootCount; i++) {
                    if (roll < hollowLoot[i].weight) { party.lootIdx = i; break; }
                    roll -= hollowLoot[i].weight;
                }
                hollowLootLeft--;
            }
            const hollowLootDef& l = hollowLoot[party.lootIdx];
            party.rewardFood = min(l.food, hollowLootMaxFood);
            party.rewardStone = min(l.stone, hollowLootMaxStone);
            hollowFlash = hollowFlashTicks;
            printf("EXPEDITION SURFACES - %s from the Hollow with %s (+%d food, +%d stone), %d hauls left\n",
                   racePlurals[r], l.what, party.rewardFood, party.rewardStone, hollowLootLeft);
        } else if (site >= 0) {   // R47-F4: one small reward per site, then it is spent
            const poiRewardDef& w = poiRewards[party.siteKind];
            float roll = roll01(gen);
            if (roll < w.foodChance) party.rewardFood = uniform_int_distribution<int>(poiFoodMin, poiFoodMax)(gen);
            else if (roll < w.foodChance + w.stoneChance || raceRelics[r] >= expRelicCap)
                party.rewardStone = uniform_int_distribution<int>(poiStoneMin, poiStoneMax)(gen);
            else party.relic = true;
            pois[site].spent = true;
            pois[site].spentTicks = 0;
            printf("EXPEDITION SEARCHED - %s, %s: %s\n", racePlurals[r], poiNames[party.siteKind],
                   party.relic ? "relic" : party.rewardFood > 0 ? "food" : "stone");
        } else {
            party.relic = raceRelics[r] < expRelicCap && roll01(gen) < expRelicChance;
            printf("EXPEDITION DELVED - %s, %s\n", racePlurals[r], party.relic ? "relic found" : "nothing found");
        }
        party.phase = expeditionParty::Back;
    } else {
        stock[r].food += party.rations;   // leftovers back to the larder
        string outcome;
        string from = party.poiId >= 0 ? string(" from the ") + poiNames[party.siteKind] : string();   // R47-F4
        if (party.hollow) {   // R47-F5: food and stone may both come up in one haul
            stock[r].food += party.rewardFood;
            stock[r].stone += party.rewardStone;
            const char* what = party.lootIdx >= 0 ? hollowLoot[party.lootIdx].what : hollowLoot[hollowLootCount - 1].what;
            outcome = "return from the Hollow with " + string(what);
            if (party.rewardFood > 0 || party.rewardStone > 0) {
                outcome += " (";
                if (party.rewardFood > 0) outcome += "+" + to_string(party.rewardFood) + " food";
                if (party.rewardFood > 0 && party.rewardStone > 0) outcome += ", ";
                if (party.rewardStone > 0) outcome += "+" + to_string(party.rewardStone) + " stone";
                outcome += ")";
            }
            if (hollowLootLeft == 0) outcome += "; it is picked clean";
        } else if (party.relic && raceRelics[r] < expRelicCap) {
            raceRelics[r] = min(expRelicCap, raceRelics[r] + 1);
            stock[r].stone += expRelicStone;
            outcome = "return with a relic" + from + " (+" + to_string(expRelicStone) + " stone, " + to_string(raceRelics[r]) + "/" + to_string(expRelicCap) + ")";
        } else if (party.relic || party.rewardStone > 0) {   // a relic found after the cap filled counts as stone
            int st = party.rewardStone > 0 ? party.rewardStone : expRelicStone;
            stock[r].stone += st;
            outcome = "return with " + to_string(st) + " stone" + from;
        } else if (party.rewardFood > 0) {
            stock[r].food += party.rewardFood;
            outcome = "return with " + to_string(party.rewardFood) + " food" + from;
        } else outcome = "return empty-handed" + from;
        printf("---EXPEDITION HOME--- %s %s, %d rations returned\n", racePlurals[r], outcome.c_str(), party.rations);
        logRaceEvent(string(racePlurals[r]) + "' expedition " + outcome, r);
        endExpedition(barbs, idx);
    }
}

// R52-F1: free to be called up for a raid: available (not sheltering, away or already in the raid), on the surface, no
// job, haul, hunt, forge task or claim, not on watch, not kneeling, and standing, wandering or drilling
bool raidIdle(const barb& b) {
    if (!barbAvailable(b) || barbHidden(b) || barbUnderground(b) || b.inMine || b.activity == actKneel || b.activity == actWatch) return false;
    if (b.chopping || b.building || b.movingbspot || b.deertargeting || b.jobKind != jobNone || b.carryKind != carryNone
        || atForgeTask(b) || b.timer > 0 || b.target != targetNone) return false;
    bool wandering = !b.moving || (b.targetx == b.wanderX && b.targety == b.wanderY);
    return wandering || b.activity == actDrill;
}
bool raidRaiderOk(const barb& b, int r) {
    return b.race == r && b.hasSpear && raidIdle(b) && b.health >= kRaidMinHealth && !isHungry(b) && !isLeader(b) && !b.shinobi;
}
bool raidDefenderOk(const barb& b, int r) {
    return b.race == r && b.hasSpear && raidIdle(b) && b.health >= kRaidDefendMinHealth;
}

// R52-F1: the only place the raid state is set. The shared walk/work reset ends any drill or wander first.
void joinRaid(barb& b, raidRole side) {
    expeditionReset(b);
    clearRaid(b);
    beginActivity(b, side == raidAttacker ? actRaidAttack : actRaidDefend);
    if (side == raidAttacker) { b.order = needNone; b.orderTicks = 0; }
}
// R52-F1: hand a barb back to the other systems, standing where it is (every exit goes through here)
// R53-F1: home = released at its own crate after the walk back (the pack is deposited); any other exit drops it
void releaseRaidBarb(barb& b, bool home = false) {
    settleLoot(b, home);
    if (!inRaid(b)) return;
    expeditionReset(b);
    clearRaid(b);
}

// R52-F1: the one way a raid ends: every member still in the raid is released, the bell threat drops
void endRaid(vector<barb>& barbs, bool home = false) {
    for (int id : raid.raiderIds) { int i = findBarbById(barbs, id); if (i >= 0) releaseRaidBarb(barbs[i], home); }
    for (int id : raid.defenderIds) { int i = findBarbById(barbs, id); if (i >= 0) releaseRaidBarb(barbs[i]); }
    raid = raidParty{};
    for (auto& t : raidThreat) t = false;
}

Vector2f raidCentre(const RectangleShape& sq) { return sq.getPosition() + sq.getSize() / 2.0f; }

// R52-F1: raider k's stand spot at race r's crate (hitbox top-left): in a row left of the crate, like the expedition
Vector2f raidCrateSpot(int r, int k) { return storehousePos[clampRace(r)] + storehouseStandOffset + Vector2f(-kRaidColumnGapPx * k, 0.0f); }

// R52-F1: a cold pair whose cooldown has run out, by daylight, both crates built. The aggressor is the side with more
// eligible armed barbs (at least kRaidPartySize, its bell silent); ties are rolled. The column is the nearest eligible
// barbs to its crate. The pair's cooldown restarts at launch, so a broken-up raid still costs the pair a day.
void tryLaunchRaid(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    if (isNightHour(game_time)) return;
    int n = min((int)barbs.size(), (int)squares.size());
    int start = uniform_int_distribution<int>(0, raceCount - 1)(gen);
    for (int o = 0; o < raceCount; o++) {
        int a = (start + o) % raceCount, b = (a + 1) % raceCount;   // each pair once: (0,1) (1,2) (2,0)
        if (raidPairCooldown[a][b] > 0 || relationTierOf(raceRelation[a][b]) != relCold) continue;
        if (pairFriendly(a, b)) continue;   // R54-F1: friendly pairs never raid
        if (!stores[a].built || !stores[b].built) continue;
        int ready[2] = {0, 0}, side[2] = {a, b};
        for (int s = 0; s < 2; s++) {
            if (raceBell[side[s]] > 0) continue;
            for (int i = 0; i < n; i++) {
                Vector2f d = raidCentre(squares[i]) - quarterCentre(side[s]);
                if (d.x * d.x + d.y * d.y < kRaidRecruitRadius * kRaidRecruitRadius && raidRaiderOk(barbs[i], side[s])) ready[s]++;
            }
        }
        if (ready[0] < kRaidPartySize && ready[1] < kRaidPartySize) continue;
        int s = ready[0] == ready[1] ? uniform_int_distribution<int>(0, 1)(gen) : ready[1] > ready[0] ? 1 : 0;
        int att = side[s], def = side[1 - s];
        int pick[kRaidPartySize];
        bool ok = true;
        for (int k = 0; k < kRaidPartySize && ok; k++) {
            pick[k] = findNearest(quarterCentre(att), kRaidRecruitRadius, n,
                                  [&](int i) { return raidCentre(squares[i]); },
                                  [&](int i) {
                                      for (int q = 0; q < k; q++) if (pick[q] == i) return true;
                                      return !raidRaiderOk(barbs[i], att);
                                  });
            ok = pick[k] >= 0;
        }
        if (!ok) continue;
        raid = raidParty{};
        raid.active = true;
        raid.att = att;
        raid.def = def;
        raid.phase = raidParty::March;
        raidPairCooldown[att][def] = raidPairCooldown[def][att] = kRaidPairCooldownTicks;
        string names;
        for (int k = 0; k < kRaidPartySize; k++) {
            barb& m = barbs[pick[k]];
            joinRaid(m, raidAttacker);
            raid.raiderIds[k] = m.id;
            names += string(k == 0 ? "" : k == kRaidPartySize - 1 ? " and " : ", ") + m.name;
        }
        printf("---RAID OUT--- %s march on the %s (%s), relation %.2f\n", racePlurals[att], racePlurals[def], names.c_str(),
               raceRelation[att][def]);
        logEvent(string(racePlurals[att]) + " march on the " + racePlurals[def] + ": " + names, kRaidTickerTint);
        return;
    }
}

// R52-F1: the column walk. The head routes to goal and waits while any gap in the column is wider than
// kRaidColumnStretchPx; each follower walks straight to a spot kRaidColumnGapPx behind the barb ahead.
// Returns true once the head stands on goal and the column is closed up.
// R54-F1: sized by the array, so the two-barb caravan walks the same way
template <int N>
bool raidColumnStep(vector<barb>& barbs, vector<RectangleShape>& squares, const int (&ri)[N], Vector2f goal) {
    bool stretched = false;
    int prev = -1;
    for (int k = 0; k < N; k++) {
        if (ri[k] < 0) continue;
        if (prev >= 0) {
            Vector2f d = squares[ri[k]].getPosition() - squares[prev].getPosition();
            if (d.x * d.x + d.y * d.y > kRaidColumnStretchPx * kRaidColumnStretchPx) stretched = true;
        }
        prev = ri[k];
    }
    bool headThere = false;
    prev = -1;
    for (int k = 0; k < N; k++) {
        int i = ri[k];
        if (i < 0) continue;
        barb& b = barbs[i];
        auto& sq = squares[i];
        Vector2f target = goal;
        bool straight = false;
        if (prev >= 0) {
            Vector2f d = squares[prev].getPosition() - sq.getPosition();
            float len = sqrt(d.x * d.x + d.y * d.y);
            prev = i;
            if (len <= kRaidColumnGapPx) {   // close enough behind: stand
                if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
                continue;
            }
            target = sq.getPosition() + d - d / len * kRaidColumnGapPx;   // on the line to the barb ahead, gap short of it
            straight = true;
        } else prev = i;
        b.targetx = (int)target.x;
        b.targety = (int)target.y;
        Vector2f diff = Vector2f((float)b.targetx, (float)b.targety) - sq.getPosition();
        if (diff.x * diff.x + diff.y * diff.y < 1.0f) {
            if (!straight) { sq.setPosition(Vector2f((float)b.targetx, (float)b.targety)); headThere = true; }
            if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
        } else if (!straight && stretched) {
            b.stepTimer = 0;   // the head waits for its tail (route kept)
        } else {
            b.moving = true;
            walkStep(b, sq, straight);
        }
    }
    return headThere && !stretched;
}

// R52-F1: one fighter's tick. Keeps its foe (re-resolved by id among foes[], unbeaten only) or takes the nearest within
// engagePx, closes in straight and strikes on cooldown. A blow never takes the foe below kRaidHealthFloor: at the floor
// it is beaten. Returns false when there is no foe to fight.
bool raidFightStep(vector<barb>& barbs, vector<RectangleShape>& squares, int i, const int* foes, int nFoes, float engagePx) {
    barb& b = barbs[i];
    auto& sq = squares[i];
    if (b.raidCooldown > 0) b.raidCooldown--;
    Vector2f c = raidCentre(sq);
    int fi = -1;
    for (int k = 0; k < nFoes; k++)
        if (foes[k] >= 0 && barbs[foes[k]].id == b.raidPreyId && !barbs[foes[k]].raidBeaten) fi = foes[k];
    if (fi < 0) {
        b.raidPreyId = -1;
        float best = engagePx * engagePx;
        for (int k = 0; k < nFoes; k++) {
            int j = foes[k];
            if (j < 0 || barbs[j].raidBeaten) continue;
            Vector2f d = raidCentre(squares[j]) - c;
            float dSq = d.x * d.x + d.y * d.y;
            if (dSq < best) { best = dSq; fi = j; }
        }
        if (fi < 0) return false;
        b.raidPreyId = barbs[fi].id;
        printf("RAID ENGAGES - %s (id %d) -> %s (id %d)\n", b.name, b.id, barbs[fi].name, barbs[fi].id);
    }
    barb& f = barbs[fi];
    Vector2f d = raidCentre(squares[fi]) - c;
    if (d.x * d.x + d.y * d.y <= kRaidStrikeRangePx * kRaidStrikeRangePx) {
        if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
        if (b.raidCooldown == 0) {
            b.raidCooldown = kRaidStrikeCooldownTicks;
            int dmg = weaponDamage(b);
            f.health = max(min(f.health, kRaidHealthFloor), f.health - dmg);   // a wound, never the kill
            f.raidFlash = kRaidFlashFrames;
            printf("RAID HIT - %s (id %d) hits %s (id %d) for %d, %d hp left\n", b.name, b.id, f.name, f.id, dmg, f.health);
            if (f.health <= kRaidHealthFloor) {
                f.raidBeaten = true;
                f.raidPreyId = -1;
                b.raidPreyId = -1;
                printf("RAID BEATEN - %s (id %d, %s)\n", f.name, f.id, racePlurals[clampRace(f.race)]);
            }
        }
    } else {
        Vector2f goal = raidCentre(squares[fi]) - sq.getSize() / 2.0f;
        b.targetx = (int)goal.x;
        b.targety = (int)goal.y;
        b.moving = true;
        walkStep(b, sq, true);
    }
    return true;
}

// R53-F1: the pickup, on the tick a raid is won. Each unbeaten raider lifts one pack of the kind the rival can spare
// most, deducted from the rival stock at once. The raid takes at most kLootRaidCap units, at most kLootStockFrac of any
// one stock and never below kLootRivalFloor. One ticker line names the race and the amount; the pair's relation drops
// once if anything was taken.
void raidPlunder(vector<barb>& barbs, const int (&ri)[kRaidPartySize]) {
    int att = clampRace(raid.att), def = clampRace(raid.def);
    raceStock& rs = stockOfRace(def);
    const int have[3] = {rs.food, rs.wood, rs.stone};
    const int floorOf[3] = {kLootRivalFloor.food, kLootRivalFloor.wood, kLootRivalFloor.stone};
    const int pack[3] = {kLootPackFood, kLootPackWood, kLootPackStone};
    int avail[3];
    for (int k = 0; k < 3; k++) avail[k] = max(0, min((int)(have[k] * kLootStockFrac), have[k] - floorOf[k]));
    int capLeft = kLootRaidCap;
    raceStock total{0, 0, 0};
    for (int k = 0; k < kRaidPartySize && capLeft > 0; k++) {
        int i = ri[k];
        if (i < 0 || barbs[i].raidBeaten || barbs[i].activity != actRaidAttack) continue;
        int kind = 0;
        for (int q = 1; q < 3; q++) if (avail[q] > avail[kind]) kind = q;
        int take = min(min(pack[kind], avail[kind]), capLeft);
        if (take <= 0) break;
        avail[kind] -= take;
        capLeft -= take;
        barb& b = barbs[i];
        settleLoot(b, false);   // never stack on a stale pack
        raceStock got{0, 0, 0};
        (kind == 0 ? got.food : kind == 1 ? got.wood : got.stone) = take;
        spend(rs, got.wood, got.stone, got.food);
        b.loot = got;
        b.lootFrom = def;
        total.food += got.food; total.wood += got.wood; total.stone += got.stone;
        printf("LOOT LIFTED - %s (id %d) lifts %s from the %s\n", b.name, b.id, lootText(got).c_str(), racePlurals[def]);
    }
    if (total.food + total.wood + total.stone == 0) {
        printf("LOOT NONE - the %s stock is at its floor\n", racePlurals[def]);
        logEvent(string(racePlurals[att]) + " find the " + racePlurals[def] + "' stores too bare to plunder", kRaidTickerTint);
        return;
    }
    string line = string(racePlurals[att]) + " plunder " + lootText(total) + " from the " + racePlurals[def];
    printf("---RAID PLUNDER--- %s\n", line.c_str());
    logEvent(line, kRaidTickerTint);
    setRelation(att, def, raceRelation[att][def] - kLootRelationDelta);
}

// R52-F1: runs after watchTick() and before bellTick() (which reads raidThreat[]). Owns every raider's and defender's
// movement. Re-resolves every id each tick and drops a missing or dead member (its flags restored if it is still
// there); under kRaidMinMembers raiders the raid breaks up. March: the column walks to the rival crate. Fight: inside
// kRaidAlarmPx the rival bell rings, armed defenders are called up and the resolver runs; fewer than kRaidMinMembers
// unbeaten raiders (or the fight running out) is a loss, holding the crate unopposed for kRaidHoldTicks is a win.
// Home: the column walks back and is released at its crate. The outcome is one ticker line and nothing else.
void raidTick(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    static int checkTimer = 0;
    for (auto& t : raidThreat) t = false;
    for (int a = 0; a < raceCount; a++)
        for (int b = a + 1; b < raceCount; b++)
            if (raidPairCooldown[a][b] > 0) raidPairCooldown[a][b] = --raidPairCooldown[b][a];
    int n = min((int)barbs.size(), (int)squares.size());

    for (int i = 0; i < n; i++) {   // backstop: a raid flag with no raid entry behind it is cleared
        barb& b = barbs[i];
        if (!inRaid(b)) {
            if (lootLaden(b)) { printf("LOOT BACKSTOP - barb %d has a pack outside the raid\n", b.id); settleLoot(b, false); }   // R53-F1
            continue;
        }
        bool listed = false;
        if (raid.active) {
            if (b.activity == actRaidAttack) for (int id : raid.raiderIds) listed = listed || id == b.id;
            if (b.activity == actRaidDefend) for (int id : raid.defenderIds) listed = listed || id == b.id;
        }
        if (!listed) { printf("RAID FLAG CLEARED - barb %d (no raid entry)\n", b.id); releaseRaidBarb(b); }
    }

    if (!raid.active) {
        if (--checkTimer <= 0) { checkTimer = kRaidCheckTicks; tryLaunchRaid(barbs, squares, stores); }
        return;
    }
    int att = clampRace(raid.att), def = clampRace(raid.def);
    raid.ticks++;

    int ri[kRaidPartySize], di[kRaidDefenderMax];
    int alive = 0, standing = 0;
    for (int k = 0; k < kRaidPartySize; k++) {
        ri[k] = findBarbById(barbs, raid.raiderIds[k]);
        bool bad = ri[k] < 0 || ri[k] >= n || barbs[ri[k]].activity != actRaidAttack || barbs[ri[k]].race != att || barbs[ri[k]].health <= 0;
        if (bad) {
            if (raid.raiderIds[k] >= 0) printf("RAIDER DROPPED - barb %d\n", raid.raiderIds[k]);
            if (ri[k] >= 0 && ri[k] < n && barbs[ri[k]].activity == actRaidAttack) releaseRaidBarb(barbs[ri[k]]);
            raid.raiderIds[k] = ri[k] = -1;
            continue;
        }
        alive++;
        if (!barbs[ri[k]].raidBeaten) standing++;
    }
    for (int k = 0; k < kRaidDefenderMax; k++) {
        di[k] = findBarbById(barbs, raid.defenderIds[k]);
        bool bad = di[k] < 0 || di[k] >= n || barbs[di[k]].activity != actRaidDefend || barbs[di[k]].race != def || barbs[di[k]].health <= 0;
        if (bad) {
            if (raid.defenderIds[k] >= 0) printf("DEFENDER DROPPED - barb %d\n", raid.defenderIds[k]);
            if (di[k] >= 0 && di[k] < n && barbs[di[k]].activity == actRaidDefend) releaseRaidBarb(barbs[di[k]]);
            raid.defenderIds[k] = di[k] = -1;
        }
    }
    string pairText = string(racePlurals[att]) + "' march on the " + racePlurals[def];
    if (alive < kRaidMinMembers) {
        printf("---RAID BROKEN--- %s: %d raider(s) left\n", pairText.c_str(), alive);
        logEvent(pairText + " breaks up", kRaidTickerTint);
        endRaid(barbs);
        return;
    }
    if (raid.ticks > kRaidMaxTicks || !stores[att].built || (raid.phase != raidParty::Home && !stores[def].built)) {
        printf("---RAID ABANDONED--- %s (%s)\n", pairText.c_str(), raid.ticks > kRaidMaxTicks ? "timeout" : "crate gone");
        logEvent(pairText + " is abandoned", kRaidTickerTint);
        endRaid(barbs);
        return;
    }

    if (raid.phase == raidParty::March) {
        raidColumnStep(barbs, squares, ri, raidCrateSpot(def, 0));
        for (int k = 0; k < kRaidPartySize; k++) {
            if (ri[k] < 0) continue;
            Vector2f d = raidCentre(squares[ri[k]]) - quarterCentre(def);
            if (d.x * d.x + d.y * d.y < kRaidAlarmPx * kRaidAlarmPx) {
                raid.phase = raidParty::Fight;
                raid.fightTicks = raid.holdTicks = raid.scanTimer = 0;
                printf("RAID AT THE GATES - %s reach the %s crate\n", racePlurals[att], racePlurals[def]);
                break;
            }
        }
        if (raid.phase == raidParty::March) return;
    }

    if (raid.phase == raidParty::Fight) {
        raidThreat[def] = true;
        raid.fightTicks++;
        if (--raid.scanTimer <= 0) {   // call up armed defenders into empty slots, nearest to the crate first
            raid.scanTimer = kRaidDefendScanTicks;
            for (int k = 0; k < kRaidDefenderMax; k++) {
                if (di[k] >= 0) continue;
                int j = findNearest(quarterCentre(def), kRaidDefendRadius, n,
                                    [&](int i) { return raidCentre(squares[i]); },
                                    [&](int i) { return !raidDefenderOk(barbs[i], def); });
                if (j < 0) break;
                joinRaid(barbs[j], raidDefender);
                raid.defenderIds[k] = barbs[j].id;
                di[k] = j;
                printf("DEFENDER CALLED - %s (id %d, %s), %d hp\n", barbs[j].name, barbs[j].id, racePlurals[def], barbs[j].health);
            }
        }
        for (int k = 0; k < kRaidPartySize; k++) {   // raiders: fight a defender in reach, else make for the crate
            int i = ri[k];
            if (i < 0) continue;
            barb& b = barbs[i];
            if (b.raidBeaten) { if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; } continue; }
            if (raidFightStep(barbs, squares, i, di, kRaidDefenderMax, kRaidEngagePx)) continue;
            Vector2f spot = raidCrateSpot(def, k);
            b.targetx = (int)spot.x;
            b.targety = (int)spot.y;
            Vector2f diff = spot - squares[i].getPosition();
            if (diff.x * diff.x + diff.y * diff.y < 1.0f) {
                squares[i].setPosition(spot);
                if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
            } else { b.moving = true; walkStep(b, squares[i]); }
        }
        for (int k = 0; k < kRaidDefenderMax; k++) {   // defenders: go for the nearest unbeaten raider, else stand
            int i = di[k];
            if (i < 0) continue;
            if (!raidFightStep(barbs, squares, i, ri, kRaidPartySize, 2.0f * worldSize)) {
                barb& b = barbs[i];
                if (b.moving) { b.moving = false; clearRoute(b); b.stepTimer = 0; }
            }
        }
        standing = 0;
        int defenders = 0;
        bool atCrate = false;
        for (int k = 0; k < kRaidDefenderMax; k++) {   // a beaten defender leaves the fight at once (the bell may shelter it)
            int i = di[k];
            if (i < 0) continue;
            if (barbs[i].raidBeaten) {
                raid.defendersBeaten++;
                releaseRaidBarb(barbs[i]);
                raid.defenderIds[k] = di[k] = -1;
            } else defenders++;
        }
        for (int k = 0; k < kRaidPartySize; k++) {
            int i = ri[k];
            if (i < 0 || barbs[i].raidBeaten) continue;
            standing++;
            Vector2f diff = raidCrateSpot(def, k) - squares[i].getPosition();
            if (diff.x * diff.x + diff.y * diff.y < 4.0f) atCrate = true;
        }
        raid.holdTicks = defenders == 0 && atCrate ? raid.holdTicks + 1 : 0;
        bool lost = standing < kRaidMinMembers || raid.fightTicks > kRaidFightMaxTicks;
        bool won = !lost && raid.holdTicks >= kRaidHoldTicks;
        if (!lost && !won) return;
        raid.won = won;
        raid.phase = raidParty::Home;
        raidThreat[def] = false;
        for (int k = 0; k < kRaidDefenderMax; k++) {
            if (di[k] >= 0) releaseRaidBarb(barbs[di[k]]);
            raid.defenderIds[k] = -1;
        }
        for (int k = 0; k < kRaidPartySize; k++) if (ri[k] >= 0) { barbs[ri[k]].raidPreyId = -1; barbs[ri[k]].raidCooldown = 0; }
        string line;
        if (won) line = string(racePlurals[att]) + " win their raid on the " + racePlurals[def]
                      + (raid.defendersBeaten > 0 ? " (" + to_string(raid.defendersBeaten) + " defender" + (raid.defendersBeaten == 1 ? "" : "s") + " beaten)"
                                                  : " (unopposed)");
        else line = string(racePlurals[def]) + " drive off the " + racePlurals[att] + "' raid"
                  + (standing < kRaidMinMembers ? " (" + to_string(alive - standing) + " raider" + (alive - standing == 1 ? "" : "s") + " beaten)"
                                                : " (the raiders tire)");
        printf("---RAID %s--- %s after %d fight ticks\n", won ? "WON" : "LOST", line.c_str(), raid.fightTicks);
        logEvent(line, kRaidTickerTint);
        if (won) raidPlunder(barbs, ri);   // R53-F1
        return;
    }

    // Home: beaten raiders walk too; released on reaching their own crate
    if (raidColumnStep(barbs, squares, ri, raidCrateSpot(att, 0))) {
        printf("RAID HOME - %s back at their crate (%s)\n", racePlurals[att], raid.won ? "won" : "lost");
        endRaid(barbs, true);   // R53-F1: packs are deposited here
    }
}

// R52-F1: world layer, beside the bell marker: a pulsing red marker on the target crate while a raid marches or fights
void drawRaidMarker(RenderWindow& window, int frame) {
    if (!raid.active || raid.phase == raidParty::Home || raid.def < 0 || raid.def >= raceCount) return;
    float pulse = 0.5f + 0.5f * sin(frame * kRaidMarkerPulseRate);
    Color c = kRaidMarkerColor;
    c.a = (uint8_t)(kRaidMarkerAlphaMin + (255 - kRaidMarkerAlphaMin) * pulse);
    RectangleShape m(kRaidMarkerSize);
    m.setFillColor(c);
    m.setPosition(snapPx(storehousePos[raid.def]) + kRaidMarkerOffset);
    window.draw(m);
}

// R54-F1: one stock kind by index (0 food, 1 wood, 2 stone, like the plunder packs)
int& stockKind(raceStock& s, int kind) { return kind == 0 ? s.food : kind == 1 ? s.wood : s.stone; }
int tradeKeepOf(int kind) { return kind == 0 ? kTradeKeep.food : kind == 1 ? kTradeKeep.wood : kTradeKeep.stone; }
// the kind race r can best spare (a full kTradeLot above kTradeKeep), skipping `exclude`; -1 = nothing to spare
int tradeSpareKind(int r, int exclude) {
    raceStock& s = stockOfRace(r);
    int best = -1, bestSpare = kTradeLot - 1;
    for (int k = 0; k < 3; k++) {
        if (k == exclude) continue;
        int spare = stockKind(s, k) - tradeKeepOf(k);
        if (spare > bestSpare) { bestSpare = spare; best = k; }
    }
    return best;
}
raceStock tradeLot(int kind) { raceStock s{0, 0, 0}; stockKind(s, kind) = kTradeLot; return s; }

bool caravanRecruitOk(const barb& b, int r) {   // raidIdle(): free, on the surface, no job/haul/hunt/claim, not watching or kneeling
    return b.race == r && raidIdle(b) && b.health >= kTradeMinHealth && !isHungry(b) && !isLeader(b) && !b.shinobi && b.order == needNone;
}
// hand a member back to the other systems, standing where it is (every exit goes through here)
void releaseCaravanBarb(barb& b) {
    if (b.activity != actCaravan) return;
    expeditionReset(b);
    endActivity(b, actCaravan);
}
// the one way a trip ends. home = the cart reached the sender's crate (goods go to the sender); otherwise the goods go
// back to cargoOwner. Every member still there is released.
void endCaravan(vector<barb>& barbs, bool home) {
    for (int id : caravan.memberIds) { int i = findBarbById(barbs, id); if (i >= 0) releaseCaravanBarb(barbs[i]); }
    int to = clampRace(home ? caravan.from : caravan.cargoOwner);
    raceStock& s = stockOfRace(to);
    s.food += caravan.cargo.food; s.wood += caravan.cargo.wood; s.stone += caravan.cargo.stone;
    if (caravanLaden()) printf("CARAVAN GOODS - %s to the %s (%s)\n", lootText(caravan.cargo).c_str(), racePlurals[to], home ? "home" : "returned");
    caravan = caravanTrip{};
}
void abortCaravan(vector<barb>& barbs, const char* why) {
    int from = clampRace(caravan.from), to = clampRace(caravan.to), owner = clampRace(caravan.cargoOwner);
    string line = string(racePlurals[from]) + "' caravan to the " + racePlurals[to] + " is called off";
    if (caravanLaden()) line += " (" + lootText(caravan.cargo) + " back to the " + racePlurals[owner] + ")";
    printf("---CARAVAN ABORTED--- %s: %s\n", line.c_str(), why);
    logEvent(line, kTradeTickerTint);
    endCaravan(barbs, false);
}

// R54-F1: by daylight, no raid on, a pair that is not cold with its cooldown run out, both crates built and both bells
// silent. The sender has a spare kind and the rival a different one; two eligible barbs nearest the sender's crate go.
// The sender's lot is paid at launch and rides on the cart. The pair's cooldown restarts at launch.
void tryLaunchCaravan(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    if (isNightHour(game_time) || raid.active) return;
    int n = min((int)barbs.size(), (int)squares.size());
    int start = uniform_int_distribution<int>(0, raceCount - 1)(gen);
    for (int o = 0; o < raceCount; o++) {
        int a = (start + o) % raceCount, b = (a + 1) % raceCount;   // each pair once: (0,1) (1,2) (2,0)
        if (tradePairCooldown[a][b] > 0 || relationTierOf(raceRelation[a][b]) == relCold) continue;
        if (!stores[a].built || !stores[b].built || raceBell[a] > 0 || raceBell[b] > 0) continue;
        int first = uniform_int_distribution<int>(0, 1)(gen);
        for (int d = 0; d < 2; d++) {
            int from = (d == first) ? a : b, to = from == a ? b : a;
            int give = tradeSpareKind(from, -1);
            if (give < 0 || tradeSpareKind(to, give) < 0) continue;
            int pick[kTradeMembers];
            bool ok = true;
            for (int k = 0; k < kTradeMembers && ok; k++) {
                pick[k] = findNearest(quarterCentre(from), kTradeRecruitRadius, n,
                                      [&](int i) { return raidCentre(squares[i]); },
                                      [&](int i) {
                                          for (int q = 0; q < k; q++) if (pick[q] == i) return true;
                                          return !caravanRecruitOk(barbs[i], from);
                                      });
                ok = pick[k] >= 0;
            }
            if (!ok) continue;
            caravan = caravanTrip{};
            caravan.active = true;
            caravan.from = from;
            caravan.to = to;
            caravan.phase = caravanTrip::Out;
            caravan.giveKind = give;
            caravan.cargo = tradeLot(give);
            caravan.cargoOwner = from;
            raceStock& fs = stockOfRace(from);
            spend(fs, caravan.cargo.wood, caravan.cargo.stone, caravan.cargo.food);
            tradePairCooldown[a][b] = tradePairCooldown[b][a] = kTradePairCooldownTicks;
            string names;
            for (int k = 0; k < kTradeMembers; k++) {
                barb& m = barbs[pick[k]];
                expeditionReset(m);   // ends any drill or wander walk first
                beginActivity(m, actCaravan);
                caravan.memberIds[k] = m.id;
                names += string(k == 0 ? "" : k == kTradeMembers - 1 ? " and " : ", ") + m.name;
            }
            string line = string(racePlurals[from]) + " send a caravan to the " + racePlurals[to] + " with " + lootText(caravan.cargo) + ": " + names;
            printf("---CARAVAN OUT--- %s, relation %.2f\n", line.c_str(), raceRelation[from][to]);
            logEvent(line, kTradeTickerTint);
            return;
        }
    }
}

// R54-F1: at the rival crate. The sender's lot goes into the rival stock and the rival's best other spare kind comes
// back on the cart; the relation warms and, at or above kRelFriendlyMinRelation, the friendly timer is (re)set.
// A rival with nothing left to spare sends the cart home with its own goods.
void caravanSwap() {
    int a = clampRace(caravan.from), b = clampRace(caravan.to);
    int get = tradeSpareKind(b, caravan.giveKind);
    if (get < 0) {
        string line = string(racePlurals[b]) + " have nothing to spare: the " + racePlurals[a] + "' caravan heads home";
        printf("---CARAVAN NO TRADE--- %s\n", line.c_str());
        logEvent(line, kTradeTickerTint);
        return;
    }
    raceStock& rb = stockOfRace(b);
    rb.food += caravan.cargo.food; rb.wood += caravan.cargo.wood; rb.stone += caravan.cargo.stone;
    raceStock sent = caravan.cargo, got = tradeLot(get);
    spend(rb, got.wood, got.stone, got.food);
    caravan.cargo = got;
    caravan.cargoOwner = b;
    setRelation(a, b, raceRelation[a][b] + kTradeRelationDelta);
    string line = string(racePlurals[a]) + " and " + racePlurals[b] + " trade " + lootText(sent) + " for " + lootText(got);
    if (raceRelation[a][b] >= kRelFriendlyMinRelation) {
        bool renewed = pairFriendly(a, b);
        relFriendlyTicks[a][b] = relFriendlyTicks[b][a] = kRelFriendlyTicks;
        line += string(renewed ? ": friendship renewed for " : ": now friendly for ") + to_string(kRelFriendlyDays) + " days";
    }
    printf("---CARAVAN TRADE--- %s (relation %.2f)\n", line.c_str(), raceRelation[a][b]);
    logEvent(line, kTradeTickerTint);
}

// R54-F1: runs in the fixed-step sim loop after raidTick(), before bellTick(). Counts the pair cooldowns and the
// friendly timers down once per sim tick (a pair slipping to uneasy loses the tier at once), clears a stale caravan
// flag, launches on a throttle, then owns the members' walk. Every member id is re-resolved each tick; a missing member
// or one whose flags conflict (chopping, building, raiding, any other activity) aborts the trip.
void caravanTick(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount]) {
    static int checkTimer = kTradeCheckTicks;
    for (int a = 0; a < raceCount; a++)
        for (int b = a + 1; b < raceCount; b++) {
            if (tradePairCooldown[a][b] > 0) tradePairCooldown[a][b] = --tradePairCooldown[b][a];
            if (relFriendlyTicks[a][b] <= 0) continue;
            bool soured = relationTierOf(raceRelation[a][b]) != relNeutral;
            relFriendlyTicks[a][b] = relFriendlyTicks[b][a] = soured ? 0 : relFriendlyTicks[a][b] - 1;
            if (relFriendlyTicks[a][b] > 0) continue;
            string line = string(racePlurals[a]) + " and " + racePlurals[b] + (soured ? ": the friendship sours" : " drift apart: no longer friendly");
            printf("RELATION - %s (%.2f)\n", line.c_str(), raceRelation[a][b]);
            logEvent(line, kTradeTickerTint);
        }
    int n = min((int)barbs.size(), (int)squares.size());

    for (int i = 0; i < n; i++) {   // backstop: a caravan flag with no caravan entry behind it is cleared
        barb& b = barbs[i];
        if (b.activity != actCaravan) continue;
        bool listed = false;
        if (caravan.active) for (int id : caravan.memberIds) listed = listed || id == b.id;
        if (!listed) { printf("CARAVAN FLAG CLEARED - barb %d (no caravan entry)\n", b.id); releaseCaravanBarb(b); }
    }

    if (!caravan.active) {
        if (--checkTimer <= 0) { checkTimer = kTradeCheckTicks; tryLaunchCaravan(barbs, squares, stores); }
        return;
    }
    int from = clampRace(caravan.from), to = clampRace(caravan.to);
    caravan.ticks++;
    int ci[kTradeMembers];
    for (int k = 0; k < kTradeMembers; k++) {
        ci[k] = findBarbById(barbs, caravan.memberIds[k]);
        if (ci[k] < 0 || ci[k] >= n) { abortCaravan(barbs, "member gone"); return; }
        const barb& m = barbs[ci[k]];
        if (m.activity != actCaravan || inRaid(m) || m.chopping || m.building || m.movingbspot || m.deertargeting
            || m.race != from || m.health <= 0) {
            printf("CARAVAN CONFLICT - barb %d (%s)\n", m.id, activityDefs[m.activity].name);
            abortCaravan(barbs, "member state conflict");
            return;
        }
    }
    if (caravan.ticks > kTradeMaxTicks || !stores[from].built || (caravan.phase != caravanTrip::Home && !stores[to].built)) {
        abortCaravan(barbs, caravan.ticks > kTradeMaxTicks ? "timeout" : "crate gone");
        return;
    }

    if (caravan.phase == caravanTrip::Out) {
        if (raidColumnStep(barbs, squares, ci, raidCrateSpot(to, 0))) {
            caravan.phase = caravanTrip::Trade;
            caravan.stayTicks = 0;
            printf("CARAVAN ARRIVES - %s at the %s crate\n", racePlurals[from], racePlurals[to]);
        }
        return;
    }
    if (caravan.phase == caravanTrip::Trade) {
        for (int k = 0; k < kTradeMembers; k++) {   // stand at the crate
            barb& m = barbs[ci[k]];
            if (m.moving) { m.moving = false; clearRoute(m); m.stepTimer = 0; }
        }
        if (++caravan.stayTicks < kTradeStayTicks) return;
        caravanSwap();
        caravan.phase = caravanTrip::Home;
        return;
    }
    // Home: released on reaching their own crate; the cart's goods go into the sender's stock
    if (raidColumnStep(barbs, squares, ci, raidCrateSpot(from, 0))) {
        printf("CARAVAN HOME - %s back at their crate\n", racePlurals[from]);
        endCaravan(barbs, true);
    }
}

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, const vector<house>& houses, vector<RectangleShape>& houseshapes, vector<birthRing>& rings) {
    static int deferCooldown = 0;
    if (deferCooldown > 0) deferCooldown--;
    int cap = popCap(houses);
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        // R53-F3: a blessing on births gives a failed spawn roll a second try (rate ~x omenMult); a curse holds some back below
        int r = clampRace(houses[i].race);
        float omenBirth = omenMult(r, omenBirths);
        bool spawnHit = spawnChance(gen) == 0;
        if (!spawnHit && omenBirth > 1.0f && uniform_real_distribution<float>(0.0f, 1.0f)(gen) < omenBirth - 1.0f) spawnHit = spawnChance(gen) == 0;
        if (!spawnHit) continue;
        if ((int)barbs.size() >= cap) continue;
        // RACE-F1: a birth draws on the house race's larder, gated on that race's own head count
        raceStock& rs = stock[r];
        int members = 0;
        for (auto& b : barbs) members += clampRace(b.race) == r ? 1 : 0;
        bool fed = members < birthFreeFloor || rs.food >= members;
        if (!fed) {
            if (deferCooldown == 0) {
                printf("Birth deferred: %s larder low (food %d, %s %d)\n", racePlurals[r], rs.food, racePlurals[r], members);
                logEvent(string("Birth deferred: ") + racePlurals[r] + " larder low");
                deferCooldown = birthDeferCooldownTicks;
            }
            continue;
        }
        Vector2f pos = houseshapes[i].getPosition();
        Vector2f birthSpot(pos.x + 4, pos.y + 13);
        // R50-F1a: the veil's mild factor (veilBirthFloor..1) after the granary rule; exactly 1 while the veil is empty
        float veilFactor = veilBirthFactor(birthSpot);
        if (veilFactor < 1.0f) {
            uniform_real_distribution<float> veilRoll(0.0f, 1.0f);
            if (veilRoll(gen) >= veilFactor) {
                veil.slowedBirths++;
                veil.lastSlowedRace = r;
                printf("VEIL - %s birth held back (factor %.2f, %d held)\n", racePlurals[r], veilFactor, veil.slowedBirths);
                veilOmen(string(racePlurals[r]) + ": the air is still here");   // R50-F1b: rare, shared cooldown
                continue;   // nothing paid: the next roll may succeed
            }
        }
        // R53-F3: a curse holds a share of births back, never while the race is under the free-birth floor
        if (omenBirth < 1.0f && members >= birthFreeFloor && uniform_real_distribution<float>(0.0f, 1.0f)(gen) >= omenBirth) {
            printf("OMEN - %s birth held back by the curse (x%.2f)\n", racePlurals[r], omenBirth);
            continue;   // nothing paid
        }
        int paid = min(rs.food, birthFoodCost);
        spend(rs, 0, 0, birthFoodCost);
        makeBarb(Vector2i((int)birthSpot.x, (int)birthSpot.y), barbs, squares, rings, houses[i].id, houses[i].race);   // R28-F1: born into the house's clan
        veil.lastDrawnRace = veilDraw(birthSpot, veilBirthDraw);   // R50-F1a: -1 if no soul was near
        if (veil.lastDrawnRace >= 0 && !rings.empty()) {   // R50-F1b: makeBarb just pushed this birth's ring
            rings.back().soulRace = veil.lastDrawnRace;
            if (veil.lastDrawnRace != r)
                veilOmen(string(barbs.back().name) + " is born with old eyes (" + racePlurals[veil.lastDrawnRace] + ")");
        }
        logEvent(string(barbs.back().name) + " the " + traitOf(barbs.back()).name + " is born (pop " + to_string(barbs.size()) + ")" +
                 (paid > 0 ? " -" + to_string(paid) + " food" : ""));
        if (paid > 0) foodFlashFrames = birthFlashFrames;
    }
}

// R47-F6: a free drill slot in race r's built barracks, or -1
int freeDrillSlot(int r) {
    if (r < 0 || r >= raceCount || !halls[r].built) return -1;
    for (int s = 0; s < kDrillSlots; s++) if (halls[r].slotId[s] < 0) return s;
    return -1;
}

// R47-F6: may this idle, standing barb (top-left pos) join its race's drill? Called from barbmove() only, which has
// already ruled out work, builds, hauls and forge tasks; orderIdle() adds hunts and job claims, and no live order.
bool drillWanted(const barb& b, Vector2f pos) {
    if (b.activity == actDrill || b.moving || b.timer > 0 || b.inMine || b.target != targetNone || b.order != needNone || !orderIdle(b)) return false;
    if (freeDrillSlot(b.race) < 0) return false;
    Vector2f d = barracksPos(b.race) + barracksSize / 2.0f - pos;
    return d.x * d.x + d.y * d.y < kDrillJoinRadius * kDrillJoinRadius;
}

// R47-F6: the only place the drill state is set. The slot is held by barb id from here until clearTraining().
void startTraining(barb& b, int s) {
    halls[b.race].slotId[s] = b.id;
    beginActivity(b, actDrill);
    b.drillSlot = s;
    b.drillTimer = b.drillHits = 0;
    Vector2f spot = drillSlotSpot(b.race, s);
    b.target = targetNone;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.wanderX = b.wanderY = -1;
    b.moving = true;
    b.stepTimer = 0;
    printf("DRILL STARTED - barb %d (%s), slot %d, counter %d\n", b.id, racePlurals[clampRace(b.race)], s, b.drillCount);
}

// R47-F6: one drill step, called from minematerials() for a training barb: walk to the slot, then a hit on the dummy
// every kDrillHitTicks work ticks (drill counter +1, dummy shakes). After kDrillSessionHits the barb is released to look
// for work again. A lost barracks or slot ends the session through clearTraining().
void drillTick(barb& b, RectangleShape& sq) {
    auto release = [&]() {
        b.moving = false; clearRoute(b);
        b.stepTimer = 0;
        b.targetx = b.targety = -1;
    };
    int r = b.race;
    if (r < 0 || r >= raceCount || !halls[r].built || b.drillSlot < 0 || b.drillSlot >= kDrillSlots || halls[r].slotId[b.drillSlot] != b.id) {
        printf("DRILL ABORTED - barb %d, slot lost\n", b.id);
        clearTraining(b);
        release();
        return;
    }
    Vector2f spot = drillSlotSpot(r, b.drillSlot);
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    Vector2f diff = spot - sq.getPosition();
    if (diff.x * diff.x + diff.y * diff.y >= 1.0f) {
        b.moving = true;
        walkStep(b, sq);   // R39-F1
        return;
    }
    b.moving = false; clearRoute(b);   // squared up to the dummy
    if (!workTick(b)) return;
    if (++b.drillTimer < kDrillHitTicks) return;
    b.drillTimer = 0;
    b.drillHits++;
    b.drillCount++;
    halls[r].shakeTicks = kDummyShakeTicks;
    if (b.drillCount == kDrillPipCount) {
        printf("---DRILLED--- barb %d (%s) reaches %d drills\n", b.id, racePlurals[r], b.drillCount);
        logRaceEvent(string(b.name) + " is drilled (" + to_string(b.drillCount) + " hits)", r);
    }
    if (b.drillHits < kDrillSessionHits) return;
    printf("DRILL DONE - barb %d (%s), %d hits, counter %d\n", b.id, racePlurals[r], b.drillHits, b.drillCount);
    clearTraining(b);
    release();
}

// R53-F2: may this idle, standing barb (top-left pos) come and tend its race's herb bed? Called from barbmove() only,
// like drillWanted(). Reads the ripe count herbTick() cached this tick, so no barb ever scans the bed. Daylight only.
bool herbWanted(const barb& b, Vector2f pos) {
    if (b.activity != actNone || b.moving || b.timer > 0 || b.inMine || b.target != targetNone || b.order != needNone || !orderIdle(b)) return false;
    if (!herbHutBuilt(b.race) || isNightHour(game_time)) return false;
    const herbGarden& g = herbGardens[b.race];
    if (g.tenderId >= 0 || g.ripe <= 0 || raceHerbs[b.race] >= kHerbStockMax) return false;
    Vector2f d = herbBedPos(b.race) + herbBedSize / 2.0f - pos;
    return d.x * d.x + d.y * d.y < kHerbJoinRadius * kHerbJoinRadius;
}

// R53-F2: the only place the tend state is set. The bed is held by barb id from here until endActivity().
void startHerbTend(barb& b) {
    herbGardens[b.race].tenderId = b.id;
    beginActivity(b, actHerb);
    b.herbTimer = 0;
    Vector2f spot = herbTendSpot(b.race);
    b.target = targetNone;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.wanderX = b.wanderY = -1;
    b.moving = true;
    b.stepTimer = 0;
    printf("HERBS TENDING - barb %d (%s), %d ripe\n", b.id, racePlurals[clampRace(b.race)], herbGardens[b.race].ripe);
}

// R53-F2: one tend step, called from minematerials() for a tending barb: walk to the spot under the bed, work
// kHerbTendTicks, then pick every ripe plant into the race's herb stock (bloom plants give more) and go back to work.
void herbTendTick(barb& b, RectangleShape& sq) {
    auto release = [&]() {
        b.moving = false; clearRoute(b);
        b.stepTimer = 0;
        b.targetx = b.targety = -1;
    };
    int r = b.race;
    if (!herbHutBuilt(r) || herbGardens[r].tenderId != b.id) {
        printf("HERBS ABORTED - barb %d, bed lost\n", b.id);
        endActivity(b, actHerb);
        release();
        return;
    }
    Vector2f spot = herbTendSpot(r);
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    Vector2f diff = spot - sq.getPosition();
    if (diff.x * diff.x + diff.y * diff.y >= 1.0f) {
        b.moving = true;
        walkStep(b, sq);   // R39-F1
        return;
    }
    b.moving = false; clearRoute(b);
    if (!workTick(b)) return;
    if (++b.herbTimer < kHerbTendTicks) return;
    herbGarden& g = herbGardens[r];
    int gained = 0;
    for (auto& p : g.plants) {
        if (p.growth < 1.0f) continue;
        gained += p.bloom ? kHerbBloomYield : kHerbYield;
        p.growth = 0.0f;
        p.bloom = false;
    }
    int before = raceHerbs[r];
    raceHerbs[r] = min(kHerbStockMax, raceHerbs[r] + gained);
    g.ripe = 0;
    if (raceHerbs[r] > 0) g.bareSaid = false;
    printf("HERBS TENDED - barb %d (%s) +%d herbs (stock %d)\n", b.id, racePlurals[r], raceHerbs[r] - before, raceHerbs[r]);
    // the stock fills quietly through summer; from autumn one line a day says it is being laid by
    if (raceHerbs[r] > before && seasonOf(game_day) == seasonAutumn && g.storedLineDay != game_day) {
        g.storedLineDay = game_day;
        logRaceEvent(to_string(raceHerbs[r]) + " herbs stored for winter", r);
    }
    endActivity(b, actHerb);
    release();
}

void barbmove(vector<RectangleShape>& squares, vector<barb>& barbs) {
    static uniform_int_distribution<int> drillJoin(0, kDrillJoinOneIn - 1);   // R47-F6
    static uniform_int_distribution<int> herbJoin(0, kHerbJoinOneIn - 1);     // R53-F2
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (!barbAvailable(b) || b.chopping || b.timer > 0 || b.movingbspot || b.building || b.carryKind != carryNone || atForgeTask(b)) continue;   // R29-F1: haulTick() walks haulers; INBOX-F1: bellTick() walks garrisoned; R41-F2: temperTick() walks temperers (R47-F7: smeltTick() smelters); R45-F2: expeditionTick() walks parties
        if (b.activity == actDrill || b.activity == actWatch || b.activity == actKneel || b.activity == actHerb) continue;   // R47-F6: drillTick() walks drillers; R51-F1: watchTick() walks watchmen; R52-F3: riteTick() kneelers; R53-F2: herbTendTick() tenders
        Vector2f pos = sq.getPosition();
        if (!b.moving) {
            if (herbWanted(b, pos) && herbJoin(gen) == 0) {   // R53-F2: an idle elf near ripe herbs tends them (before the drill: it fills a stock)
                startHerbTend(b);
                continue;
            }
            if (drillWanted(b, pos) && drillJoin(gen) == 0) {   // R47-F6: an idle barb near its barracks may drill instead
                startTraining(b, freeDrillSlot(b.race));
                continue;
            }
            if (wanderChance(gen) != 0) continue;
            int tx = (int)pos.x + wanderDX(gen);
            int ty = (int)pos.y + wanderDY(gen);
            if (tx > 0 && tx < worldSize && ty > 0 && ty < worldSize && lakeClear(FloatRect(Vector2f(tx, ty), sq.getSize()), &pos)) {
                b.targetx = b.wanderX = tx;
                b.targety = b.wanderY = ty;
                b.moving  = true;
            }
        } else {
            Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;
            bool wandering = b.targetx == b.wanderX && b.targety == b.wanderY;
            if (dSq < 1.0f) {
                b.moving    = false; clearRoute(b);
                b.stepTimer = 0;
                b.wanderX = b.wanderY = -1;
            } else walkStep(b, sq, wandering);   // R39-F1: wander walks stay straight, others follow a route
        }
    }
}

Vector2f bushWorkSpot(const CircleShape& bs) {
    Vector2f p = bs.getPosition() + bushWorkOffset;
    return Vector2f((float)(int)p.x, (float)(int)p.y);
}

// Claimed targets: claim[i] = index of the barb headed for / working tree (or rock) i, -1 = free.
// Rebuilt from barbs' current targets every tick rather than tracked, so death, depletion and
// abandonment release claims automatically and swapRemove index shifts can't leave stale claims.
long long workSpotKey(int x, int y) { return ((long long)x << 32) ^ (unsigned int)y; }

void buildWorkClaims(const vector<barb>& barbs, const vector<CircleShape>& tris, const vector<CircleShape>& octs,
                     vector<int>& treeClaim, vector<int>& rockClaim) {
    unordered_map<long long, int> treeSpots, rockSpots;
    for (int j = 0; j < (int)barbs.size(); j++) {
        const barb& b = barbs[j];
        if (!barbAvailable(b) || b.targetx == -1 || b.target == targetBush || b.target == targetLake || b.target == targetField || b.target == targetDepot || b.deertargeting || b.movingbspot || b.building
            || b.activity == actDrill || b.activity == actWatch || b.activity == actKneel || b.activity == actHerb) continue;   // R47-F6: a drill slot is not a tree spot (R51-F1: nor a patrol stop; R52-F3: nor a kneel slot; R53-F2: nor the herb bed)
        (b.target == targetRock ? rockSpots : treeSpots).emplace(workSpotKey(b.targetx, b.targety), j); // first claimant kept
    }
    treeClaim.assign(tris.size(), -1);
    for (int i = 0; i < (int)tris.size(); i++) {
        Vector2f p = tris[i].getPosition() + treeWorkOffset;
        auto it = treeSpots.find(workSpotKey((int)p.x, (int)p.y));
        if (it != treeSpots.end()) treeClaim[i] = it->second;
    }
    rockClaim.assign(octs.size(), -1);
    for (int i = 0; i < (int)octs.size(); i++) {
        Vector2f p = octs[i].getPosition() + rockWorkOffset;
        auto it = rockSpots.find(workSpotKey((int)p.x, (int)p.y));
        if (it != rockSpots.end()) rockClaim[i] = it->second;
    }
}

// INBOX-F2: drop mem[k], shifting the older spots up so [0] stays the newest
void memDrop(barb& b, int k) {
    for (int i = k; i + 1 < memSpotCount; i++) b.mem[i] = b.mem[i + 1];
    b.mem[memSpotCount - 1] = workMem{};
}

// INBOX-F2: remember spot p as the newest; a same-kind spot within memMergeRadius is refreshed (moved to [0]), else the oldest falls off
void memRecord(barb& b, memKind kind, Vector2f p) {
    for (int k = 0; k < memSpotCount; k++) {
        Vector2f d = b.mem[k].pos - p;
        if (b.mem[k].kind == kind && d.x * d.x + d.y * d.y < memMergeRadius * memMergeRadius) { memDrop(b, k); break; }
    }
    for (int i = memSpotCount - 1; i > 0; i--) b.mem[i] = b.mem[i - 1];
    b.mem[0] = workMem{p, kind};
}

// INBOX-F2: a trip ends (load deposited or bush picked): the next search heads back to mem[0] and may share again
void memEndTrip(barb& b) {
    b.memReturn = true;
    b.memShared = false;
}

// INBOX-F2: hand fresh spot p to the nearest same-race barbs within memShareRadius of barbs[j], at most memShareMax of
// them. Sheltering barbs are skipped. Marks the sharer so it shares once per trip.
void memShare(vector<barb>& barbs, const vector<RectangleShape>& squares, int j, memKind kind, Vector2f p) {
    barb& b = barbs[j];
    b.memShared = true;
    Vector2f from = squares[j].getPosition();
    for (int n = 0; n < memShareMax; n++) {
        int k = findNearest(from, memShareRadius, (int)barbs.size(),
                            [&](int i) { return squares[i].getPosition(); },
                            [&](int i) { const barb& o = barbs[i];
                                         return i == j || o.race != b.race || !barbAvailable(o) || o.health <= 0
                                             || (o.mem[0].kind == kind && o.mem[0].pos == p); });   // already told
        if (k == -1) break;
        memRecord(barbs[k], kind, p);
        printf("SPOT SHARED - %s tells %s of a %s\n", b.name, barbs[k].name, kind == memTree ? "grove" : kind == memRock ? "rock" : "bush");
    }
}

// Every way out of a crop harvest (berry bush or wheat field) clears the same barb fields
void clearHarvest(barb& b) {
    b.moving = b.chopping = false; clearRoute(b);
    b.timer = 0;
    b.targetx = b.targety = -1;
    b.target = targetNone;
    b.targetBush = -1;
    dropCarry(b);   // R29-F1
}

// Shared crop gathering for bushes and the field: once the barb stands within 2px of spot it works the crop.
// On the finishing tick it adds yield food, clears the barb and returns true; the caller resets its own crop.
bool harvestTick(barb& b, Vector2f spot, Vector2f bpos, int ticks, int yield) {
    float dx = spot.x - bpos.x, dy = spot.y - bpos.y;
    if (dx * dx + dy * dy >= 4.0f) return false;
    b.chopping = true;
    if (workTick(b)) b.timer++;
    if (b.timer < ticks) return false;
    stockOf(b).food += yield;   // RACE-F1: the picker's race
    clearHarvest(b);
    return true;
}

// R29-F1: where a hauler drops its load: its own race's storehouse if standing, else the campfire. A lookup, no search.
// FARM-F2: true if b carries a sheaf and its own race's mill is standing (the mill is the depot for wheat)
bool millDepot(const barb& b) {
    return b.carryKind == carryWheat && b.race >= 0 && b.race < raceCount && mills[b.race].built;
}

Vector2f depotSpot(const barb& b, const storehouse (&stores)[raceCount]) {
    if (millDepot(b)) return millWorkSpot(mills[b.race]);   // FARM-F2: sheaves go to the mill while it stands
    if (b.race >= 0 && b.race < raceCount && stores[b.race].built) return storehousePos[b.race] + storehouseStandOffset;
    return villageCenter + campfireDepotOffset;
}

// R29-F1: a felled tree / mined rock becomes a load; the barb walks it to the depot (haulTick) before it counts.
// FARM-F2: a reaped sheaf too (carryWheat).
void startHaul(barb& b, carryLoad load, const storehouse (&stores)[raceCount], int amount = 1) {
    b.carryKind = load;   // first: the depot depends on the load (wheat -> mill)
    b.carryAmount = max(1, amount);   // R34-F1: kShaftYield stone from a shaft
    Vector2f spot = depotSpot(b, stores);
    b.chopping = false;
    b.timer = 0;
    b.target = targetDepot;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    b.stepTimer = 0;
}

// R41-F2: started by haulTick() right after a dwarf's stone deposit; the stone is paid here (temperPaid) and refunded by
// clearTemper() on any abort
void startTemper(barb& b) {
    spend(stockOf(b), 0, kTemperStoneCost);
    beginActivity(b, actTemper);
    b.temperPaid = true;
    b.temperTimer = 0;
    Vector2f spot = forgePos + forgeStandOffset;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    b.stepTimer = 0;
    printf("TEMPER STARTED - barb %d, %s -%d stone (stone: %d)\n", b.id, racePlurals[clampRace(b.race)], kTemperStoneCost, stockOf(b).stone);
}

// R41-F2: one temper step: walk to the forge stand spot, then kTemperTicks work ticks with a spark burst on each.
// The forge going away (forgeActive false) refunds via clearTemper() and frees the barb.
void temperTick(barb& b, RectangleShape& sq, const forge& fg) {
    auto release = [&]() {
        b.moving = false; clearRoute(b);
        b.stepTimer = 0;
        b.targetx = b.targety = -1;
    };
    if (!forgeActive(fg, b.race)) {
        printf("TEMPER ABORTED - barb %d, forge lost\n", b.id);
        clearTemper(b);
        release();
        return;
    }
    Vector2f spot = forgePos + forgeStandOffset;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    Vector2f diff = spot - sq.getPosition();
    if (diff.x * diff.x + diff.y * diff.y >= 1.0f) {
        b.moving = true;
        walkStep(b, sq);   // R39-F1
        return;
    }
    b.moving = false; clearRoute(b);   // standing at the anvil
    if (!workTick(b)) return;
    b.temperTimer++;
    spawnForgeSparks();
    if (b.temperTimer < kTemperTicks) return;
    b.temperPaid = false;   // finishing consumes the stone, so clearTemper refunds nothing
    clearTemper(b);
    b.toolCharges = kTemperCharges;
    release();
    memEndTrip(b);   // INBOX-F2: head back to the remembered rock
    printf("---PICK TEMPERED--- barb %d, %s, %d charges\n", b.id, racePlurals[clampRace(b.race)], kTemperCharges);
    logRaceEvent(string(b.name) + " tempered pick (" + to_string(kTemperCharges) + ")", b.race);
}

// R47-F7: may b smelt after this stone deposit? Forge built and free, its race below the ingot cap, stone above the reserve
bool smeltWanted(const barb& b, const forge& fg) {
    return forgeSmeltOpen(fg) && forgeSmelterId < 0 && b.race >= 0 && b.race < raceCount && raceIngots[b.race] < kIngotStockMax
        && canAfford(stockOf(b), 0, kSmeltStoneCost + kSmeltStoneReserve);
}
// R47-F7: started by haulTick() right after a stone deposit; paid here (smeltPaid), refunded by clearSmelt() on any abort
void startSmelt(barb& b) {
    spend(stockOf(b), 0, kSmeltStoneCost);
    beginActivity(b, actSmelt);
    b.smeltPaid = true;
    b.smeltTimer = 0;
    forgeSmelterId = b.id;
    Vector2f spot = forgePos + smeltStandOffset;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    b.stepTimer = 0;
    printf("SMELT STARTED - barb %d, %s -%d stone (stone: %d)\n", b.id, racePlurals[clampRace(b.race)], kSmeltStoneCost, stockOf(b).stone);
}
// R47-F7: smelting right now (standing at the forge, work begun): drives the smoke wisp
bool forgeSmelting(const vector<barb>& barbs) {
    for (auto& b : barbs) if (b.activity == actSmelt && b.smeltTimer > 0 && b.id == forgeSmelterId) return true;
    return false;
}
// R47-F7: one smelt step: walk to the smelt spot, kSmeltTicks work ticks, then the ingots and one spark burst.
// The forge going away refunds via clearSmelt() and frees the barb.
void smeltTick(barb& b, RectangleShape& sq, const forge& fg) {
    auto release = [&]() {
        b.moving = false; clearRoute(b);
        b.stepTimer = 0;
        b.targetx = b.targety = -1;
    };
    if (!forgeSmeltOpen(fg) || forgeSmelterId != b.id) {
        printf("SMELT ABORTED - barb %d, %s\n", b.id, forgeSmeltOpen(fg) ? "forge taken" : "forge lost");
        clearSmelt(b);
        release();
        return;
    }
    Vector2f spot = forgePos + smeltStandOffset;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    Vector2f diff = spot - sq.getPosition();
    if (diff.x * diff.x + diff.y * diff.y >= 1.0f) {
        b.moving = true;
        walkStep(b, sq);   // R39-F1
        return;
    }
    b.moving = false; clearRoute(b);   // standing at the anvil
    if (!workTick(b)) return;
    b.smeltTimer++;
    if (b.smeltTimer < kSmeltTicks) return;
    int r = clampRace(b.race);
    int before = toolTierOf(r);
    raceIngots[r] = min(kIngotStockMax, raceIngots[r] + kSmeltIngots);   // R49-F1: buffer above the top tier
    int after = toolTierOf(r);
    toolTierSeen = true;
    b.smeltPaid = false;   // finishing consumes the stone, so clearSmelt refunds nothing
    clearSmelt(b);
    release();
    memEndTrip(b);   // INBOX-F2: head back to the remembered rock
    spawnForgeSparks();   // one burst per finished smelt
    forgeEmberFrames = forgeEmberBoostFrames;
    printf("---INGOT SMELTED--- barb %d, %s ingots %d, tool tier %d\n", b.id, racePlurals[r], raceIngots[r], after);
    if (after > before) {
        bool regained = after <= raceTierBest[r];   // R49-F1: a tier lost to wear and smelted back
        raceTierBest[r] = max(raceTierBest[r], after);
        raceTierDimFrames[r] = 0;                   // back up: the drop cue ends at once
        printf("---TOOL TIER %d--- %s (%d ingots, %.0f%% faster work)%s\n", after, racePlurals[r], raceIngots[r],
               (toolTierSpeed[after] - 1.0f) * 100.0f, regained ? " re-forged" : "");
        logRaceEvent(string(racePlurals[r]) + (regained ? " re-forge tier " : " forge tier ") + to_string(after) + " tools", r);
    }
}

// R49-F1: the one wear path, called once at each chop/mine completion (tree, rock, shaft), never at job start. Only a
// race holding ingots wears; at kWearJobs it spends kWearIngots (floored at 0). Tier is compared before/after, so the
// amber ticker line and the HUD/steel-mark dim fire only on a real drop. at = the barb's top-left, for the grey spark.
void toolWearOnJobDone(const barb& b, Vector2f at) {
    if (b.race < 0 || b.race >= raceCount) return;
    int r = b.race;
    if (raceIngots[r] <= 0) { raceWear[r] = 0; return; }   // plain tools: nothing to wear
    if (wearSparkCooldown[r] <= 0) {   // one 1px chip, at most once per kWearSparkCooldownFrames per race
        wearSparkPos[r] = at + wearSparkOffset;
        wearSparkAge[r] = 0;
        wearSparkCooldown[r] = kWearSparkCooldownFrames;
    }
    if (++raceWear[r] < kWearJobs) return;
    raceWear[r] = 0;
    int before = toolTierOf(r);
    raceIngots[r] = max(0, raceIngots[r] - kWearIngots);
    int after = toolTierOf(r);
    printf("TOOL WEAR - %s spent %d ingot after %d jobs (ingots %d, tool tier %d)\n", racePlurals[r], kWearIngots, kWearJobs, raceIngots[r], after);
    if (after < before) {
        raceTierDimFrames[r] = tierDropDimFrames;
        raceTierLost[r] = before;
        printf("---TOOL TIER DROP--- %s tier %d -> %d (%d ingots)\n", racePlurals[r], before, after, raceIngots[r]);
        logEvent(string(racePlurals[r]) + "' tools wear down to tier " + to_string(after), toolWearColor);
    }
}

// R49-F1: each race's single wear spark: 1px grey, rising and fading; timers aged per drawn frame (not while paused)
void drawWearSparks(RenderWindow& window, bool paused) {
    RectangleShape s(Vector2f(1.0f, 1.0f));
    for (int r = 0; r < raceCount; r++) {
        if (!paused) {
            if (wearSparkCooldown[r] > 0) wearSparkCooldown[r]--;
            if (raceTierDimFrames[r] > 0) raceTierDimFrames[r]--;
        }
        if (wearSparkAge[r] >= wearSparkFrames) continue;
        float t = (float)wearSparkAge[r] / wearSparkFrames;
        Color c = wearSparkColor;
        c.a = (uint8_t)(wearSparkMaxAlpha * (1.0f - t));
        s.setFillColor(c);
        s.setPosition(snapPx(wearSparkPos[r] - Vector2f(0.0f, wearSparkRisePx * t)));
        window.draw(s);
        if (!paused) wearSparkAge[r]++;
    }
}

// R29-F1: one hauler step. The depot is re-resolved every tick, so a storehouse that is gone sends the load to the
// campfire. On arrival the load is added to the stock, the storehouse flashes, and the barb is idle again.
void haulTick(barb& b, RectangleShape& sq, storehouse (&stores)[raceCount], const forge& fg) {
    Vector2f spot = depotSpot(b, stores);
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    Vector2f pos  = sq.getPosition();
    Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq >= 1.0f) {
        walkStep(b, sq);   // R39-F1
        return;
    }
    if (b.carryKind == carryWheat) {   // FARM-F2: mill revalidated on arrival; gone/unbuilt -> the sheaf goes to the storehouse
        bool milled = millDepot(b);
        Vector2f ms = milled ? millWorkSpot(mills[b.race]) : Vector2f();
        if (milled && (b.targetx != (int)ms.x || b.targety != (int)ms.y)) milled = false;   // stood at another depot instead
        storehouse* st = (!milled && b.race >= 0 && b.race < raceCount && stores[b.race].built) ? &stores[b.race] : nullptr;
        int yield = (milled ? millFoodYield : fieldFoodYield) + forageBonus(b);   // R25-F1: Forager +1
        raceStock& rs = stockOf(b);   // RACE-F1: the reaper's race
        rs.food += yield;
        if (milled) {
            mill& m = mills[b.race];
            m.turnFrames = millTurnFrames;
            m.puffFrames = millPuffFrames;
        } else if (st) { st->stored++; st->flashFrames = storehouseFlashFrames; }
        printf("WHEAT %s at %s - %s food: %d\n", milled ? "MILLED" : "STORED", milled ? "mill" : st ? "storehouse" : "campfire",
               racePlurals[clampRace(b.race)], rs.food);
        logEvent(string(milled ? "Wheat milled (+" : "Wheat stored (+") + to_string(yield) + " food, " + to_string(rs.food) + ")");
    } else {
        bool isWood = b.carryKind == carryWood;
        raceStock& rs = stockOf(b);   // RACE-F1: the hauler's race
        bool tithe = false;           // every campfireTitheEvery-th wood deposit feeds the shared campfire pool instead
        if (isWood && campfireWood < campfireWoodCap && ++campfireTitheCount >= campfireTitheEvery) {
            campfireTitheCount = 0;
            campfireWood++;
            tithe = true;
        } else (isWood ? rs.wood : rs.stone) += b.carryAmount;   // R34-F1: shaft stone is a kShaftYield load
        bool forged = !isWood && forgeActive(fg, b.race);   // POL-F5b: forge stone bonus, checked at deposit
        if (forged) {
            rs.stone += forgeStoneBonus;
            spawnForgeSparks();
        }
        storehouse* st = (b.race >= 0 && b.race < raceCount && stores[b.race].built) ? &stores[b.race] : nullptr;
        if (st) { st->stored++; st->flashFrames = storehouseFlashFrames; }
        printf("%s x%d DEPOSITED at %s%s%s - %s wood: %d stone: %d, campfire wood: %d\n", isWood ? "WOOD" : "STONE", b.carryAmount,
               st ? "storehouse" : "campfire", tithe ? " (campfire tithe)" : "", forged ? " (+forge bonus)" : "",
               racePlurals[clampRace(b.race)], rs.wood, rs.stone, campfireWood);
    }
    // R41-F2: a dwarf's stone haul ends at the forge to temper an untempered pick, if the stone is there to pay for it
    bool temper = b.carryKind == carryStone && forgeActive(fg, b.race) && b.toolCharges == 0
                  && canAfford(stockOf(b), 0, kTemperStoneCost);
    bool smelt = !temper && b.carryKind == carryStone && smeltWanted(b, fg);   // R47-F7: any race; temper goes first for a dwarf
    b.carryKind = carryNone;
    b.carryAmount = 1;   // R34-F1
    b.target = targetNone;
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    memEndTrip(b);   // INBOX-F2
    if (temper) startTemper(b);
    else if (smelt) startSmelt(b);
}

void minematerials(vector<RectangleShape>& squares, vector<barb>& barbs, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<tree>& trees, vector<CircleShape>& octs, vector<rock>& rocks, vector<char>& rockGold,
                   vector<bush>& bushes, vector<CircleShape>& bushshapes, vector<birthRing>& catchRings, fieldGrid& fields, storehouse (&stores)[raceCount],
                   const forge& fg) {
    vector<int> treeClaim, rockClaim;   // parallel to trees/tris and rocks/octs for this tick
    buildWorkClaims(barbs, tris, octs, treeClaim, rockClaim);
    treeGridRebuild(tris);   // R47-F1: picks up every tree add/remove since last tick; chops below mark it stale
    vector<int> shoreClaim(shorePoints.size(), -1);   // rebuilt every tick like the tree/rock claims, so no stale anglers
    for (int j = 0; j < (int)barbs.size(); j++) {
        int k = barbs[j].targetShore;
        if (barbs[j].target == targetLake && k >= 0 && k < (int)shoreClaim.size() && shoreClaim[k] == -1) shoreClaim[k] = j;
    }
    bool fieldTaken[raceCount][fieldsPerRace] = {};   // one reaper per plot, derived from targets every tick (no stored claim)
    for (auto& o : barbs) {
        if (o.target != targetField || o.race < 0 || o.race >= raceCount) continue;
        int k = fieldIndexAt(fields[o.race], o.targetx, o.targety);
        if (k >= 0) fieldTaken[o.race][k] = true;
    }
    int shaftMiner[raceCount] = {-1, -1, -1};   // R34-F1: one miner per shaft, derived from targets every tick (no stored claim)
    for (int r = 0; r < raceCount; r++) shaftMiner[r] = shaftMinerOf(barbs, r);   // R40-F1: same rule as the tooltip and bar
    for (int j = 0; j < (int)barbs.size(); j++)   // R40-F1: redirected, garrisoned or shaft gone/reset: out of the pit
        if (barbs[j].inMine && (!barbUnderground(barbs[j]) || shaftMiner[barbs[j].race] != j)) leaveMine(barbs[j]);
    leaderNeed liveNeed[raceCount];   // R30-F2: a race's need counts only while its leader is alive (checked once per tick)
    for (int r = 0; r < raceCount; r++) liveNeed[r] = findBarbById(barbs, raceLeaderId[r]) >= 0 ? raceNeed[r] : needNone;
    // R51-F4: barb j just claimed the tree/rock whose work spot is spot. Inside any race's frontier, a tree or rock within
    // kRelationClashRadius already claimed this tick by another race's barb is a claim clash between the two races.
    // Claims are this tick's index tables, read and range-checked here only; nothing is stored.
    auto noteClaimClash = [&](int j, Vector2f spot) {
        int r = barbs[j].race;
        if (r < 0 || r >= raceCount) return;
        int owner = -1;
        for (int k = 0; k < raceCount && owner < 0; k++) {
            Vector2f d = spot - quarterCentre(k);
            if (d.x * d.x + d.y * d.y < frontierRadius * frontierRadius) owner = k;
        }
        if (owner < 0) return;
        auto foreign = [&](int c) {
            return c >= 0 && c < (int)barbs.size() && c != j && barbs[c].race >= 0 && barbs[c].race < raceCount && barbs[c].race != r;
        };
        int t = findNearestTree(tris, spot, kRelationClashRadius, [&](int i) { return i >= (int)treeClaim.size() || !foreign(treeClaim[i]); });
        int other = t >= 0 ? barbs[treeClaim[t]].race : -1;
        if (other < 0) {
            int k = findNearest(spot, kRelationClashRadius, (int)octs.size(),
                                [&](int i) { return octs[i].getPosition() + rockWorkOffset; },
                                [&](int i) { return i >= (int)rockClaim.size() || !foreign(rockClaim[i]); });
            if (k >= 0) other = barbs[rockClaim[k]].race;
        }
        if (other >= 0) relationClash(r, other, owner);
    };
    for (int j = 0; j < (int)squares.size(); j++) {
        auto& sq = squares[j];
        auto& b  = barbs[j];
        if (!barbAvailable(b)) continue;   // INBOX-F1: no work or tree/rock/bush/shore search while sheltering; R45-F2: or away
        if (b.toolCharges > 0 && b.activity != actTemper && !forgeActive(fg, b.race)) {   // R41-F2: forge gone: the tempered edge goes with it
            printf("PICK UNTEMPERED - barb %d, forge lost\n", b.id);
            clearTemper(b);
        }
        if (b.activity == actTemper) {   // R41-F2: any other state taken over the temper refunds it; temperTick() handles the forge going
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.jobKind != jobNone || b.carryKind != carryNone) {
                printf("TEMPER INTERRUPTED - barb %d\n", b.id);
                clearTemper(b);
            } else { temperTick(b, sq, fg); continue; }
        }
        if (b.activity == actSmelt) {   // R47-F7: same rule as the temper: any other state taken over the smelt refunds it
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.jobKind != jobNone || b.carryKind != carryNone || b.activity == actTemper) {
                printf("SMELT INTERRUPTED - barb %d\n", b.id);
                clearSmelt(b);
            } else { smeltTick(b, sq, fg); continue; }
        }
        if (b.activity == actDrill) {   // R47-F6: backstop: any other state set on a driller without clearTraining() ends the session
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.timer > 0 || b.jobKind != jobNone
                || b.carryKind != carryNone || atForgeTask(b) || b.target != targetNone) {
                printf("DRILL INTERRUPTED - barb %d\n", b.id);
                clearTraining(b);
            } else { drillTick(b, sq); continue; }   // no work search while drilling: the session is short
        }
        if (b.activity == actWatch) {   // R51-F1: backstop: watching is exclusive with work, builds, timers, hunts, hauls and the drill
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.timer > 0 || b.jobKind != jobNone
                || b.carryKind != carryNone || atForgeTask(b) || b.activity == actDrill || b.target != targetNone) {
                printf("WATCH INTERRUPTED - barb %d\n", b.id);
                clearWatch(b);
            } else continue;   // watchTick() walks and fights; no work search on watch
        }
        if (b.activity == actKneel) {   // R52-F3: backstop: kneeling is exclusive with work, builds, timers, hunts, hauls, the drill and the watch
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.timer > 0 || b.jobKind != jobNone
                || b.carryKind != carryNone || atForgeTask(b) || b.activity == actDrill || b.activity == actWatch || b.target != targetNone) {
                printf("KNEEL INTERRUPTED - barb %d\n", b.id);
                clearKneel(b);
            } else continue;   // riteTick() walks the kneeler to its slot; no work search while kneeling
        }
        if (b.activity == actHerb) {   // R53-F2: backstop: tending is exclusive with work, builds, timers, hunts and hauls
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.timer > 0 || b.jobKind != jobNone
                || b.carryKind != carryNone || atForgeTask(b) || b.target != targetNone || b.inMine) {
                printf("HERBS INTERRUPTED - barb %d\n", b.id);
                endActivity(b, actHerb);
            } else { herbTendTick(b, sq); continue; }   // no work search at the bed: the visit is short
        }
        if (b.carryKind != carryNone) {   // R29-F1: any other state taken over the haul drops the load
            if (b.target != targetDepot || b.movingbspot || b.building || b.deertargeting || b.chopping || b.jobKind != jobNone) dropCarry(b);
            else { haulTick(b, sq, stores, fg); continue; }
        }
        if (b.movingbspot || b.building) continue;
        Vector2f bpos = sq.getPosition();
        bool bestIsRock = false;
        if (b.timer == 0 && !b.chopping) {
            float treeDSq, rockDSq, bushDSq;
            int bestTree = findNearestTree(tris, bpos, barbWorkSearchRadius,   // R47-F1: grid, nearby cells only
                                       [&](int i) { return trees[i].dying || (treeClaim[i] != -1 && treeClaim[i] != j); }, &treeDSq);
            int bestRock = findNearest(bpos, barbWorkSearchRadius, (int)rocks.size(),
                                       [&](int k) { return octs[k].getPosition() + rockWorkOffset; },
                                       [&](int k) { return rocks[k].dying || (rockClaim[k] != -1 && rockClaim[k] != j); }, &rockDSq);
            // R34-F1: the own race's open shaft is a rock candidate, by its stand spot; it replaces a farther rock
            bool shaftPick = false;
            if (shaftOpen(b.race) && (shaftMiner[b.race] == -1 || shaftMiner[b.race] == j)) {
                Vector2f d = shaftWorkSpot(b.race) - bpos;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < kShaftSearchRadius * kShaftSearchRadius && (bestRock == -1 || dSq < rockDSq)) {
                    shaftPick = true;
                    rockDSq = dSq;
                }
            }
            bool anyRock = bestRock != -1 || shaftPick;
            int bestBush = findNearest(bpos, barbWorkSearchRadius, (int)bushes.size(),
                                       [&](int k) { return bushWorkSpot(bushshapes[k]); },
                                       [&](int k) { return !bushes[k].ripe || bushes[k].picking; }, &bushDSq);
            // need weighting: wood short of a house makes trees count as nearer. Scores are squared
            // distances divided by weight^2; not-found stays infinite so a boosted miss never wins.
            float woodShort = clamp((float)(houseWoodCost - stockOf(b).wood) / (float)houseWoodCost, 0.0f, 1.0f);   // RACE-F1: own race
            float treeW = clamp(1.0f + treeNeedBoost * woodShort, needWeightMin, needWeightMax);
            float rockW = clamp(1.0f, needWeightMin, needWeightMax);
            float bushW = clamp(1.0f, needWeightMin, needWeightMax);
            leaderNeed ln = (b.race >= 0 && b.race < raceCount) ? liveNeed[b.race] : needNone;   // R30-F2: re-clamped so no job starves
            if (ln == needWood)  treeW = clamp(treeW * leaderNeedMult, needWeightMin, needWeightMax);
            if (ln == needStone) rockW = clamp(rockW * leaderNeedMult, needWeightMin, needWeightMax);
            if (ln == needFood)  bushW = clamp(bushW * leaderNeedMult, needWeightMin, needWeightMax);
            if (currentDecree == decreeTimber)  treeW *= councilDecreeMult;
            if (currentDecree == decreeStone)   rockW *= councilDecreeMult;
            if (currentDecree == decreeBerries) bushW *= councilDecreeMult;
            const float notFound = numeric_limits<float>::infinity();
            float treeScore = bestTree != -1 ? treeDSq / (treeW * treeW) : notFound;
            float rockScore = anyRock ? rockDSq / (rockW * rockW) : notFound;   // R34-F1: rock or shaft
            float bushScore = bestBush != -1 ? bushDSq / (bushW * bushW) : notFound;
            // INBOX-F2b: an ordered barb takes its ordered resource whenever one is in reach (-1 beats any real score);
            // with none in reach it falls back to the normal weighted pick, so an order never strands a barb
            bool orderHit = false;
            if (b.order == needWood  && bestTree != -1) { treeScore = -1.0f; orderHit = true; }
            if (b.order == needStone && anyRock) { rockScore = -1.0f; orderHit = true; }
            if (b.order == needFood  && bestBush != -1) { bushScore = -1.0f; orderHit = true; }
            // a rock wins only when strictly better, as when trees were scanned first; a bush likewise
            bestIsRock = anyRock && rockScore < treeScore;
            bool bestIsBush = bestBush != -1 && bushScore < (bestIsRock ? rockScore : treeScore);
            int best = bestIsRock ? bestRock : bestTree;
            // ripe wheat: weighted like berries (same food decree), wins only when strictly better than everything else
            // FARM-F1: only the barb's own race's plots
            float fieldScore = notFound;
            int bestField = -1;
            for (int k = 0; b.race >= 0 && b.race < raceCount && k < fieldsPerRace; k++) {
                const plot& fp = fields[b.race][k];
                if (!fieldRipe(fp) || fieldTaken[b.race][k]) continue;
                Vector2f d = fieldWorkSpot(fp) - bpos;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < fieldSearchRadius * fieldSearchRadius && dSq / (bushW * bushW) < fieldScore) {
                    fieldScore = dSq / (bushW * bushW);
                    bestField = k;
                }
            }
            if (b.order == needFood && bestField >= 0 && bestBush == -1) { fieldScore = -1.0f; orderHit = true; }   // INBOX-F2b: wheat is food too
            if (bestField >= 0 && fieldScore < min(min(treeScore, rockScore), bushScore) && b.moving==false) {
                Vector2f wp = fieldWorkSpot(fields[b.race][bestField]);
                b.targetx = (int)wp.x;
                b.targety = (int)wp.y;
                b.moving  = true;   // later branches all need !moving, so nothing else claims this barb this tick
                b.target = targetField;
                fieldTaken[b.race][bestField] = true;
            }
            // INBOX-F2: a trip just ended, so head back to the newest remembered spot that still holds a free resource of
            // its kind (looked up by position and live state, never a stored index). Stale spots are dropped; with none
            // left the normal search below runs as before.
            bool memStale = false;
            memKind freshKind = memNone;   // the spot this barb heads for this tick, shared if a stale one was just dropped
            Vector2f freshPos;
            if (orderHit) b.memReturn = false;   // INBOX-F2b: a live order outranks spot memory
            if (b.memReturn && !b.moving && !b.deertargeting) {
                b.memReturn = false;
                for (int k = 0; k < memSpotCount && b.mem[k].kind != memNone && !b.moving; ) {
                    const workMem m = b.mem[k];
                    int hit = -1;
                    if (m.kind == memTree)
                        hit = findNearestTree(tris, m.pos, memSpotRadius,
                                          [&](int i) { return trees[i].dying || (treeClaim[i] != -1 && treeClaim[i] != j); });
                    else if (m.kind == memRock)
                        hit = findNearest(m.pos, memSpotRadius, (int)rocks.size(),
                                          [&](int i) { return octs[i].getPosition() + rockWorkOffset; },
                                          [&](int i) { return rocks[i].dying || (rockClaim[i] != -1 && rockClaim[i] != j); });
                    else
                        hit = findNearest(m.pos, memSpotRadius, (int)bushes.size(),
                                          [&](int i) { return bushWorkSpot(bushshapes[i]); },
                                          [&](int i) { return !bushes[i].ripe || bushes[i].picking; });
                    if (hit == -1) { memDrop(b, k); memStale = true; continue; }   // stale: cleared, [k] now holds the next
                    if (m.kind == memTree) {
                        Vector2f tp = tris[hit].getPosition();
                        b.targetx = (int)(tp.x + 8);
                        b.targety = (int)(tp.y + 15);
                        b.target = targetNone;
                        treeClaim[hit] = j;
                        noteClaimClash(j, tp + treeWorkOffset);   // R51-F4
                    } else if (m.kind == memRock) {
                        Vector2f rp = octs[hit].getPosition();
                        b.targetx = (int)(rp.x + 6);
                        b.targety = (int)(rp.y + 10);
                        b.target = targetRock;
                        rockClaim[hit] = j;
                        noteClaimClash(j, rp + rockWorkOffset);   // R51-F4
                    } else {
                        Vector2f wp = bushWorkSpot(bushshapes[hit]);
                        b.targetx = (int)wp.x;
                        b.targety = (int)wp.y;
                        b.target = targetBush;
                        b.targetBush = hit;
                    }
                    b.moving = true;   // every branch below needs !moving, so the remembered spot holds
                    freshKind = m.kind;
                    freshPos = Vector2f(b.targetx, b.targety);
                }
            }
            if (bestIsBush && b.moving==false) {
                Vector2f wp = bushWorkSpot(bushshapes[bestBush]);
                b.targetx = (int)wp.x;
                b.targety = (int)wp.y;
                b.moving  = true;
                b.target = targetBush;
                b.targetBush = bestBush;
                freshKind = memBush;   // INBOX-F2
                freshPos = wp;
            }
            if (best != -1 && !bestIsBush && !bestIsRock && b.moving==false) {
                Vector2f tp = tris[best].getPosition();
                b.targetx = (int)(tp.x + 8);
                b.targety = (int)(tp.y + 15);
                b.moving  = true;
                b.target = targetNone;
                treeClaim[best] = j;
                noteClaimClash(j, tp + treeWorkOffset);   // R51-F4
                freshKind = memTree;   // INBOX-F2
                freshPos = Vector2f(b.targetx, b.targety);
            }
            if (shaftPick && !bestIsBush && bestIsRock && b.moving==false) {   // R34-F1: the shaft beat every rock in reach
                Vector2f sp = shaftWorkSpot(b.race);
                b.targetx = (int)sp.x;
                b.targety = (int)sp.y;
                b.moving = true;   // the rock branch below needs !moving
                b.target = targetRock;
                shaftMiner[b.race] = j;
            }
	    if (best != -1 && !bestIsBush && bestIsRock && b.moving==false) {
		    Vector2f rp = octs[best].getPosition();
		    b.targetx = (int)(rp.x + 6);
		    b.targety = (int)(rp.y + 10);
		    b.moving = true;
		    b.target = targetRock;
		    rockClaim[best] = j;
		    noteClaimClash(j, rp + rockWorkOffset);   // R51-F4
		    freshKind = memRock;   // INBOX-F2
		    freshPos = Vector2f(b.targetx, b.targety);
        }
            if (memStale && freshKind != memNone && !b.memShared) memShare(barbs, squares, j, freshKind, freshPos);   // INBOX-F2
            // Nothing to chop, mine or pick in reach: walk to the nearest free shore point and fish
            if (best == -1 && !bestIsBush && !b.moving && !b.deertargeting) {
                Vector2f size = sq.getSize();
                int k = findNearest(bpos, fishSearchRadius, (int)shorePoints.size(),
                                    [&](int s) { return shoreStandSpot(s, size); },
                                    [&](int s) { return shoreClaim[s] != -1 && shoreClaim[s] != j; });
                if (k != -1) {
                    Vector2f sp = shoreStandSpot(k, size);
                    b.targetx = (int)sp.x;
                    b.targety = (int)sp.y;
                    b.moving = true;
                    b.target = targetLake;
                    b.targetShore = k;
                    shoreClaim[k] = j;
                }
            }
            // R45-F1a: still nothing (no work in reach, no free shore): one wider look with the same weights, so barbs on
            // the bigger map head for the nearest tree, rock or ripe bush instead of wandering empty ground
            if (best == -1 && !bestIsBush && !b.moving && !b.deertargeting) {
                float wTreeDSq, wRockDSq, wBushDSq;
                // R47-F3: race-aware: the wide look never reaches into another race's frontier
                int wTree = findNearestTree(tris, bpos, barbWorkFallbackRadius,
                                        [&](int i) { return trees[i].dying || (treeClaim[i] != -1 && treeClaim[i] != j)
                                                            || inOtherFrontier(tris[i].getPosition() + treeWorkOffset, b.race); }, &wTreeDSq);
                int wRock = findNearest(bpos, barbWorkFallbackRadius, (int)rocks.size(),
                                        [&](int k) { return octs[k].getPosition() + rockWorkOffset; },
                                        [&](int k) { return rocks[k].dying || (rockClaim[k] != -1 && rockClaim[k] != j)
                                                            || inOtherFrontier(octs[k].getPosition() + rockWorkOffset, b.race); }, &wRockDSq);
                int wBush = findNearest(bpos, barbWorkFallbackRadius, (int)bushes.size(),
                                        [&](int k) { return bushWorkSpot(bushshapes[k]); },
                                        [&](int k) { return !bushes[k].ripe || bushes[k].picking
                                                            || inOtherFrontier(bushWorkSpot(bushshapes[k]), b.race); }, &wBushDSq);
                float wTreeScore = wTree != -1 ? wTreeDSq / (treeW * treeW) : notFound;
                float wRockScore = wRock != -1 ? wRockDSq / (rockW * rockW) : notFound;
                float wBushScore = wBush != -1 ? wBushDSq / (bushW * bushW) : notFound;
                if (wBush != -1 && wBushScore < wTreeScore && wBushScore < wRockScore) {
                    Vector2f wp = bushWorkSpot(bushshapes[wBush]);
                    b.targetx = (int)wp.x;
                    b.targety = (int)wp.y;
                    b.moving = true;
                    b.target = targetBush;
                    b.targetBush = wBush;
                } else if (wRock != -1 && wRockScore < wTreeScore) {
                    Vector2f rp = octs[wRock].getPosition();
                    b.targetx = (int)(rp.x + 6);
                    b.targety = (int)(rp.y + 10);
                    b.moving = true;
                    b.target = targetRock;
                    rockClaim[wRock] = j;
                    noteClaimClash(j, rp + rockWorkOffset);   // R51-F4
                } else if (wTree != -1) {
                    Vector2f tp = tris[wTree].getPosition();
                    b.targetx = (int)(tp.x + 8);
                    b.targety = (int)(tp.y + 15);
                    b.moving = true;
                    b.target = targetNone;
                    treeClaim[wTree] = j;
                    noteClaimClash(j, tp + treeWorkOffset);   // R51-F4
                }
            }
        if (b.targetx == -1) continue;
	}
	if (b.target == targetLake) {
            int k = b.targetShore;
            auto quit = [&]() {   // every way out of fishing clears the same fields
                b.moving = b.chopping = false; clearRoute(b);
                b.timer = 0;
                b.targetx = b.targety = -1;
                b.target = targetNone;
                b.targetShore = -1;
            };
            if (k < 0 || k >= (int)shorePoints.size()) { quit(); continue; }
            Vector2f sp = shoreStandSpot(k, sq.getSize());
            if (b.targetx != (int)sp.x || b.targety != (int)sp.y) { quit(); continue; } // retargeted elsewhere
            float dx = sp.x - bpos.x, dy = sp.y - bpos.y;
            if (dx * dx + dy * dy >= 4.0f) continue;   // still walking (barbmove)
            b.chopping = true;
            if (workTick(b)) b.timer++;
            if (b.timer >= fishCatchTicks) {
                stockOf(b).food += fishFoodYield;   // RACE-F1: the angler's race
                catchRings.push_back({fishPoseOf(b, sq).lineEnd + Vector2f(0.5f, 0.5f)});
                quit();
                printf("FISH CAUGHT - %s food: %d\n", racePlurals[clampRace(b.race)], stockOf(b).food);
            }
            continue;
	}
	if (b.target == targetField) {
            int k = (b.race >= 0 && b.race < raceCount) ? fieldIndexAt(fields[b.race], b.targetx, b.targety) : -1;
            if (k < 0) { clearHarvest(b); continue; }   // retargeted, or not a plot of its own race
            plot& fp = fields[b.race][k];
            Vector2f wp = fieldWorkSpot(fp);
            if (!fp.built || (!fieldRipe(fp) && !b.chopping)) {
                clearHarvest(b);   // nothing to reap
                continue;
            }
            // FARM-F2: the reaped sheaf adds no food here; it is carried (carryWheat) to the race's mill, or to the
            // storehouse/campfire if no mill stands, and counted on arrival in haulTick() (Forager bonus applied there)
            if (harvestTick(b, wp, bpos, fieldHarvestTicks, 0)) {
                replantField(fields[b.race], k);   // replanted at once (R32-F1: maybe fallow first, to stay staggered)
                startHaul(b, carryWheat, stores);
                printf("WHEAT REAPED - barb %d carries the sheaf to the %s\n", b.id, millDepot(b) ? "mill" : "storehouse");
                practice(techSeedSelection);
            }
            continue;
	}
	if (b.target == targetBush) {
            int i = b.targetBush;
            if (i < 0 || i >= (int)bushes.size()) continue;
            Vector2f wp = bushWorkSpot(bushshapes[i]);
            if (b.targetx != (int)wp.x || b.targety != (int)wp.y) continue; // retargeted (e.g. a hunt)
            if ((!bushes[i].ripe || bushes[i].picking) && !b.chopping) {   // someone else got there first
                clearHarvest(b);
                continue;
            }
            bool done = harvestTick(b, wp, bpos, bushPickTicks, bushFoodYield + forageBonus(b));   // R25-F1: Forager +1
            if (b.chopping) bushes[i].picking = true;
            if (done) {
                bushes[i].ripe = bushes[i].picking = false;
                bushes[i].regrow = 0;
                bushshapes[i].setFillColor(bushBareColor);
                memRecord(b, memBush, wp);   // INBOX-F2: picking food ends the trip on the spot
                memEndTrip(b);
                printf("BERRIES PICKED - %s food: %d\n", racePlurals[clampRace(b.race)], stockOf(b).food);
            }
            continue;
	}
	if (atShaftTarget(b)) {   // R34-F1: own race's shaft; checked before the rock scan (its spot is never a rock's)
            mineShaft& ms = mineShafts[b.race];
            if (!ms.built || ms.seam <= 0 || shaftMiner[b.race] != j) {   // dry, gone or taken: drop it and search again next tick
                if (b.chopping || b.timer > 0) printf("SHAFT LEFT - barb %d, %s seam %d\n", b.id, racePlurals[b.race], ms.seam);
                leaveMine(b);   // R40-F1
                b.moving = b.chopping = false; clearRoute(b);
                b.timer = 0;
                b.targetx = b.targety = -1;
                b.target = targetNone;
                continue;
            }
            Vector2f sp = shaftWorkSpot(b.race);
            float dx = sp.x - bpos.x, dy = sp.y - bpos.y;
            if (dx * dx + dy * dy >= 4.0f) continue;   // still walking (barbmove)
            startToolJob(b);   // R47-F7: tier read once, on the first work tick
            b.chopping = true;
            if (!b.inMine) {   // R40-F1: at the mouth: down into the pit
                b.inMine = true;
                printf("SHAFT DESCENT - barb %d, %s seam %d\n", b.id, racePlurals[b.race], ms.seam);
            }
            if (workTick(b)) b.timer++;
            if (b.timer >= mineTicksFor(b)) {
                int yield = min(kShaftYield, ms.seam);
                ms.seam = max(0, ms.seam - yield);
                leaveMine(b, true);   // R40-F1: climbs out with the stone; lintel flash
                practice(techPickwork);
                toolWearOnJobDone(b, bpos);   // R49-F1
                if (b.race == raceDwarf) forgeEmberFrames = forgeEmberBoostFrames;   // same flare as a mined rock
                startHaul(b, carryStone, stores, yield);
                printf("SHAFT MINED - barb %d hauls %d stone, %s seam %d/%d\n", b.id, yield, racePlurals[b.race], ms.seam, kShaftCapacity);
                if (ms.seam == 0) {
                    printf("---SHAFT EXHAUSTED--- %s\n", racePlurals[b.race]);
                    logEvent(string(racePlurals[b.race]) + "' mine shaft runs dry");
                }
            }
            continue;
	}
	if (b.target != targetRock) {
        int i = treeAtWorkSpot(tris, b.targetx, b.targety);   // R47-F1: grid lookup instead of a scan of every tree
        if (i >= 0) {
            Vector2f tp = tris[i].getPosition();
            float tix = tp.x + 8, tiy = tp.y + 15;
            float dx = tix - bpos.x, dy = tiy - bpos.y;
            if (trees[i].dying && !b.chopping) {
                b.moving  = false; clearRoute(b);
                b.targetx = b.targety = -1;
            } else if (dx * dx + dy * dy < 4.0f) {
                startToolJob(b);   // R47-F7: tier read once, on the first work tick
                b.chopping = true;
                trees[i].dying = true;
                if (workTick(b)) b.timer++;
                if (b.timer >= chopTicks(b)) {
                    practice(techHandAxe);
                    toolWearOnJobDone(b, bpos);   // R49-F1
                    memRecord(b, memTree, Vector2f((int)tix, (int)tiy));   // INBOX-F2: by position; the tree index dies with the swapRemove
                    swapRemove(trees,  i);
                    swapRemove(tris,   i);
                    swapRemove(stumps, i);
                    swapRemove(treeClaim, i);
                    treeGrid.stale = true;   // R47-F1: indices moved; the next lookup rebuilds
                    startHaul(b, carryWood, stores);   // R29-F1: wood counts once it reaches the depot
                    printf("TREE DESTROYED - hauling wood (stock %d)\n", stockOf(b).wood);
                }
            }
        }
	    }
	else
		for(int i = 0; i < (int)rocks.size(); i++) {
			Vector2f rp = octs[i].getPosition();
			float rix = rp.x + 6, riy = rp.y + 10;
			if (b.targetx != (int)rix || b.targety != (int)riy) continue;
			if (rocks[i].dying && !b.chopping) {
				b.moving = false; clearRoute(b);
				b.targetx = b.targety = -1;
				break;
			}
			float dx = rix - bpos.x, dy = riy - bpos.y;
			if (dx * dx + dy * dy >= 4.0f) break;
			startToolJob(b);   // R47-F7: tier read once, on the first work tick
			b.chopping = true;
			rocks[i].dying = true;
			if (workTick(b)) b.timer++;
			if (b.timer >= mineTicksFor(b)) {        // dwarves finish rocks faster; R41-F2: a tempered pick faster still
				practice(techPickwork);
				toolWearOnJobDone(b, bpos);   // R49-F1
				if (b.toolCharges > 0) {             // R41-F2: one charge per completed rock, after the scaled check above
					b.toolCharges--;
					printf("TEMPERED MINE - barb %d, %d charges left\n", b.id, b.toolCharges);
				}
				if (b.race == raceDwarf) forgeEmberFrames = forgeEmberBoostFrames; // forge ember flares (drawn only once built)
				memRecord(b, memRock, Vector2f((int)rix, (int)riy));   // INBOX-F2
				if (i < (int)rockGold.size() && rockGold[i]) {   // R46-F1a: nuggets go straight to the global count; stone still hauls
					gold += goldPerRock;
					goldFound = true;
					printf("GOLD ROCK MINED - barb %d, +%d gold (total %d)\n", b.id, goldPerRock, gold);
				}
				swapRemove(rocks, i);
				swapRemove(octs, i);
				swapRemove(rockGold, i);
				swapRemove(rockClaim, i);
				startHaul(b, carryStone, stores);  // R29-F1: stone counts once it reaches the depot
				printf("ROCK DESTROYED - hauling stone (stock %d)\n", stockOf(b).stone);
			}
			break;
		}
	}
}



// The one tree logic-shape builder (startup and treespawn); draw kind is derived later in drawTrees
CircleShape makeTreeTri(int x, int y) {
    CircleShape tri;
    tri.setPointCount(3);
    tri.setPosition(Vector2f(x, y));
    tri.setRadius(8);
    tri.setFillColor(treeLogicColor);
    return tri;
}

void treespawn(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, const fieldGrid& fields,
               const vector<house>& houses, const vector<barb>& barbs) {
    if (treespawnChance(gen) != 0) return;
    // R47-F1: bucket by band, weighted by each band's startup tree share; a core at zoneCoreTreeCap takes no more
    // (counts from the tree grid, rebuilt in minematerials() earlier this tick)
    double w[zoneCount];
    for (int z = 0; z < zoneCount; z++) w[z] = zoneTreeTarget[z];
    if (treeGrid.zoneTrees[zoneCore] >= zoneCoreTreeCap) w[zoneCore] = 0.0;
    if (w[zoneCore] + w[zoneWild] + w[zoneEdge] <= 0.0) return;
    int z = discrete_distribution<int>(w, w + zoneCount)(gen);
    Vector2i p;
    if (!rollInZone(z, treeFootprint, p)) return;
    tree t{p.x, p.y, false};
    // R44-F3: reroll toward the frontier (inside the core only, the band the clearing is in); the last roll stands
    uniform_real_distribution<float> groveRoll(0.0f, 1.0f);
    for (int i = 0; z == zoneCore && i < groveMaxRetries; i++) {
        float d = hypot(t.x - villageCenter.x, t.y - villageCenter.y);
        float accept = clamp((d - groveRampStart) / groveRamp, groveMinAccept, 1.0f);
        if (groveRoll(gen) < accept) break;
        if (rollInZone(zoneCore, treeFootprint, p)) t = tree{p.x, p.y, false};
    }
    if (!lakeClear(FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return; // no trees in the water
    if (!fieldsClear(fields, FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return; // nor on a wheat plot
    if (!towerClear(FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return;  // R33-F2: nor on a gate tower
    {   // R44-F4a: nor on a homestead (or its pending spot) out past the quarter clearing
        FloatRect tr(Vector2f(t.x - homesteadClutterPad, t.y - homesteadClutterPad),
                     Vector2f(treeFootprint + 2 * homesteadClutterPad, treeFootprint + 2 * homesteadClutterPad));
        for (auto& h : houses)
            if (tr.findIntersection(FloatRect(Vector2f(h.x - 3.0f, h.y - 3.0f), houseSlabSize))) return;
        for (auto& o : barbs)
            if (o.movingbspot && o.jobKind == jobNone
                && tr.findIntersection(FloatRect(Vector2f(o.targetx - 3.0f, o.targety - 3.0f), houseSlabSize))) return;
    }
    if (!ruinsClear(FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return;  // R45-F1b: nor in the ruin site
    if (!poiClear(FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return;    // R47-F4: nor on a hut or well
    trees.push_back(t);

    tris.push_back(makeTreeTri(t.x, t.y));

    RectangleShape stump(Vector2f(4, 4));
    stump.setFillColor(stumpColor);
    stump.setPosition(Vector2f(t.x + 6, t.y + 12));
    stumps.push_back(stump);
}

// lakeCountMin-Max ponds (2-3 per 800x800 of map) away from the village, apart from each other. Startup trees (trees/tris/stumps) and rocks (rocks/octs)
// in or touching a lake are removed together. Fixed village spots (stall, forge) are inside the
// lakeVillageMinDist ring, and the race smokehouses sit inside the dry quarters, so they never need a lake check. Shore points are cached here for angling.
void spawnLakes(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<rock>& rocks, vector<CircleShape>& octs, vector<char>& rockGold) {
    uniform_int_distribution<int> count(lakeCountMin, lakeCountMax);
    uniform_int_distribution<int> pickSize(0, lakeSizeCount - 1);
    bernoulli_distribution trimCorner(lakeCornerTrimChance);
    int want = count(gen);
    for (int n = 0; n < want; n++) {
        const lakeSizeDef& def = lakeSizes[pickSize(gen)];
        Vector2f lakeSize(def.cols * lakeTilePx, def.rows * lakeTilePx);
        uniform_real_distribution<float> lx(lakeEdgeMargin, worldSize - lakeSize.x - lakeEdgeMargin);
        uniform_real_distribution<float> ly(hudBarHeight + lakeEdgeMargin, worldSize - lakeSize.y - lakeEdgeMargin);
        for (int k = 0; k < lakePlaceTries; k++) {
            FloatRect r(Vector2f(round(lx(gen)), round(ly(gen))), lakeSize);
            Vector2f c = r.position + lakeSize / 2.0f;
            if (hypot(c.x - villageCenter.x, c.y - villageCenter.y) < lakeVillageMinDist) continue;
            bool ok = true;
            for (auto& l : lakes) {
                FloatRect gap(l.position - Vector2f(lakeMinGap, lakeMinGap), l.size + Vector2f(2 * lakeMinGap, 2 * lakeMinGap));
                if (r.findIntersection(gap).has_value()) ok = false;
            }
            for (int q = 0; q < raceCount; q++) {   // R30-F1: race quarters stay dry (clearing + lakeMinGap)
                float m = quarterClearRadiusFor(q) + lakeMinGap;   // R47-F10: step 0 at startup
                if (r.findIntersection(FloatRect(quarterCentre(q) - Vector2f(m, m), Vector2f(2 * m, 2 * m))).has_value()) ok = false;
            }
            if (!ok) continue;
            // Tile mask: full grid, then at most one tile trimmed per corner. A corner is only trimmed while its row
            // keeps at least two tiles, so trimming never leaves an empty row.
            int cols = def.cols, rows = def.rows;
            vector<char> wet(cols * rows, 1);
            auto rowCount = [&](int y) { int m = 0; for (int x = 0; x < cols; x++) m += wet[y * cols + x]; return m; };
            const int cornerX[4] = {0, cols - 1, 0, cols - 1}, cornerY[4] = {0, 0, rows - 1, rows - 1};
            for (int q = 0; q < 4; q++)
                if (trimCorner(gen) && rowCount(cornerY[q]) > 2) wet[cornerY[q] * cols + cornerX[q]] = 0;
            // Bank quads first (each tile grown by lakeBankPx, so the bank follows the blocky outline), then water on top.
            VertexArray va(PrimitiveType::Triangles);
            auto addQuad = [&va](Vector2f p, Vector2f s, Color col) {
                Vector2f a = p, b = p + Vector2f(s.x, 0.0f), c2 = p + s, d = p + Vector2f(0.0f, s.y);
                va.append(Vertex{a, col}); va.append(Vertex{b, col}); va.append(Vertex{c2, col});
                va.append(Vertex{a, col}); va.append(Vertex{c2, col}); va.append(Vertex{d, col});
            };
            Vector2f tile(lakeTilePx, lakeTilePx), bank(lakeBankPx, lakeBankPx);
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < cols; x++)
                    if (wet[y * cols + x])
                        addQuad(r.position + Vector2f(x * lakeTilePx, y * lakeTilePx) - bank, tile + 2.0f * bank, lakeBankColor);
            for (int y = 0; y < rows; y++)
                for (int x = 0; x < cols; x++) {
                    if (!wet[y * cols + x]) continue;
                    bool ring = def.shallows && (x == 0 || y == 0 || x == cols - 1 || y == rows - 1);
                    Color col = ring ? lakeShallowsColor : ((x + y) % 2 == 0 ? lakeDeepColorA : lakeDeepColorB);
                    addQuad(r.position + Vector2f(x * lakeTilePx, y * lakeTilePx), tile, col);
                }
            lakes.push_back(r);
            lakeTiles.push_back(va);
            // Shore points (top, bottom, left, right; see shoreWaterDir) from the real wet spans of the centre column and row.
            int cc = cols / 2, cr = rows / 2;
            int y0 = 0, y1 = rows - 1, x0 = 0, x1 = cols - 1;
            while (y0 < y1 && !wet[y0 * cols + cc]) y0++;
            while (y1 > y0 && !wet[y1 * cols + cc]) y1--;
            while (x0 < x1 && !wet[cr * cols + x0]) x0++;
            while (x1 > x0 && !wet[cr * cols + x1]) x1--;
            float edge = lakeBankPx + lakeShorePx; // just past the bank
            float midX = round(r.position.x + (cc + 0.5f) * lakeTilePx), midY = round(r.position.y + (cr + 0.5f) * lakeTilePx);
            shorePoints.push_back(Vector2f(midX, r.position.y + y0 * lakeTilePx - edge));
            shorePoints.push_back(Vector2f(midX, r.position.y + (y1 + 1) * lakeTilePx + edge - 1.0f));
            shorePoints.push_back(Vector2f(r.position.x + x0 * lakeTilePx - edge, midY));
            shorePoints.push_back(Vector2f(r.position.x + (x1 + 1) * lakeTilePx + edge - 1.0f, midY));
            break;
        }
    }
    for (int i = (int)tris.size() - 1; i >= 0; i--) {
        if (lakeClear(FloatRect(tris[i].getPosition(), Vector2f(treeFootprint, treeFootprint)))) continue;
        swapRemove(trees, i); swapRemove(tris, i); swapRemove(stumps, i);
    }
    for (int i = (int)octs.size() - 1; i >= 0; i--) {
        if (lakeClear(octs[i].getGlobalBounds())) continue;
        swapRemove(rocks, i); swapRemove(octs, i); swapRemove(rockGold, i);
    }
    printf("Spawned %zu lakes (%zu shore points)\n", lakes.size(), shorePoints.size());
}

// Blocky pixel lakes (R23-F1): one prebuilt vertex array per lake (bank, then shallows/checkered deep tiles).
// Ground layer: after trails, before rocks/trees.
void drawLakes(RenderWindow& window) {
    for (auto& va : lakeTiles) window.draw(va);
}

// R30-F1: true if p is within dist of any race quarter's centre
bool nearQuarter(Vector2f p, float dist) {
    for (int q = 0; q < raceCount; q++) {
        Vector2f d = p - quarterCentre(q);
        if (d.x * d.x + d.y * d.y < dist * dist) return true;
    }
    return false;
}

// A few clumps of bushes, away from the village center. Startup trees overlapping a bush are
// cleared (trees, tris and stumps together) so the clumps sit in small clearings and stay visible.
void spawnBushes(vector<bush>& bushes, vector<CircleShape>& bushshapes, vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps) {
    uniform_int_distribution<int> centre((int)bushClumpEdgeMargin, (int)(worldSize - bushClumpEdgeMargin));
    uniform_real_distribution<float> spread(-bushClumpSpread, bushClumpSpread);
    for (int c = 0; c < numBushClumps; c++) {
        Vector2f cp;
        do cp = Vector2f((float)centre(gen), (float)centre(gen));
        while (hypot(cp.x - villageCenter.x, cp.y - villageCenter.y) < bushClumpMinCenterDist
               || nearQuarter(cp, quarterClearRadiusFor(0) + bushClumpSpread + 2 * bushRadius)   // R30-F1: house space stays free (R47-F10: step 0, same for every race at startup)
               || !lakeClear(FloatRect(cp - Vector2f(bushClumpSpread, bushClumpSpread),     // whole clump + work spots off the water
                                       Vector2f(2 * (bushClumpSpread + bushRadius), 2 * (bushClumpSpread + bushRadius) + bushWorkOffset.y))));
        for (int k = 0; k < bushesPerClump; k++) {
            bush bu{(int)(cp.x + spread(gen)), (int)(cp.y + spread(gen))};
            bushes.push_back(bu);
            CircleShape bs(bushRadius);
            bs.setPosition(Vector2f(bu.x, bu.y));
            bs.setFillColor(bushRipeColor);
            bushshapes.push_back(bs);
        }
    }
    for (int i = (int)tris.size() - 1; i >= 0; i--) {
        FloatRect tb = tris[i].getGlobalBounds();
        for (auto& bs : bushshapes) {
            FloatRect bb = bs.getGlobalBounds();
            bb.position -= Vector2f(bushTreeClearance, bushTreeClearance);
            bb.size += Vector2f(2 * bushTreeClearance, 2 * bushTreeClearance + bushWorkOffset.y); // keep the work spot clear too
            if (!tb.findIntersection(bb)) continue;
            swapRemove(trees, i); swapRemove(tris, i); swapRemove(stumps, i);
            break;
        }
    }
    printf("Spawned %zu bushes in %d clumps\n", bushes.size(), numBushClumps);
}

// R30-F1 startup check, after trees, rocks, lakes and bushes: each quarter must be dry and have at least quarterMinTrees
// trees and quarterMinRocks rocks between its clearing and quarterResourceRadius. A short quarter is topped up from a
// fixed-seed generator (deterministic re-seed), keeping every parallel vector in step.
void checkQuarters(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps,
                   vector<rock>& rocks, vector<CircleShape>& octs, vector<char>& rockGold, const vector<CircleShape>& bushshapes) {
    const Vector2f treeSz(treeFootprint, treeFootprint), rockSz(12.0f, 12.0f);   // rock: 6px-radius pentagon
    auto nearby = [](Vector2f topLeft, Vector2f size, int q) {
        Vector2f d = topLeft + size / 2.0f - quarterCentre(q);
        return d.x * d.x + d.y * d.y <= quarterResourceRadius * quarterResourceRadius && !inClearing(topLeft);
    };
    auto spotOk = [&](Vector2f p, Vector2f size) {
        if (p.x < 0.0f || p.y < hudBarHeight || p.x + size.x > worldSize || p.y + size.y > worldSize) return false;
        FloatRect r(p, size);
        if (inClearing(p) || !lakeClear(r)) return false;
        for (auto& bs : bushshapes) {
            FloatRect bb = bs.getGlobalBounds();
            bb.position -= Vector2f(bushTreeClearance, bushTreeClearance);
            bb.size += Vector2f(2 * bushTreeClearance, 2 * bushTreeClearance + bushWorkOffset.y);
            if (r.findIntersection(bb)) return false;
        }
        return true;
    };
    for (int q = 0; q < raceCount; q++) {
        float m = quarterRadiusFor(q) + houseSlabSize.x;   // R47-F10: step 0 at startup
        if (!lakeClear(FloatRect(quarterCentre(q) - Vector2f(m, m), Vector2f(2 * m, 2 * m))))
            printf("Race quarter %s: WARNING water inside the house area\n", raceDefs[q].name);
        int nt = 0, nr = 0;
        for (auto& t : tris) if (nearby(t.getPosition(), treeSz, q)) nt++;
        for (auto& o : octs) if (nearby(o.getPosition(), rockSz, q)) nr++;
        int addT = 0, addR = 0;
        mt19937 reseed(quarterReseedSeed + q);
        uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(quarterClearRadiusFor(q) + treeFootprint, quarterResourceRadius - treeFootprint);   // R47-F10: step 0
        for (int k = 0; k < quarterTopUpTries && (nt + addT < quarterMinTrees || nr + addR < quarterMinRocks); k++) {
            float a = ang(reseed), r = rad(reseed);
            Vector2f c = quarterCentre(q) + Vector2f(cos(a), sin(a)) * r;
            if (nt + addT < quarterMinTrees) {
                Vector2f p = snapPx(c - treeSz / 2.0f);
                if (!spotOk(p, treeSz)) continue;
                trees.push_back(tree{(int)p.x, (int)p.y, false});
                tris.push_back(makeTreeTri((int)p.x, (int)p.y));
                RectangleShape stump(Vector2f(4, 4));
                stump.setFillColor(stumpColor);
                stump.setPosition(Vector2f(p.x + 6, p.y + 12));
                stumps.push_back(stump);
                addT++;
            } else {
                Vector2f p = snapPx(c - rockSz / 2.0f);
                if (!spotOk(p, rockSz)) continue;
                rocks.push_back(rock{(int)p.x, (int)p.y, false});
                CircleShape oct;
                oct.setPointCount(5);
                oct.setPosition(p);
                oct.setRadius(6);
                oct.setFillColor(Color(125, 125, 125));
                octs.push_back(oct);
                rockGold.push_back(rollGoldRock());   // R46-F1a: same index as rocks/octs
                addR++;
            }
        }
        bool shortNow = nt + addT < quarterMinTrees || nr + addR < quarterMinRocks;
        printf("Race quarter %s: %d trees, %d rocks nearby (+%d/+%d re-seeded)%s\n", raceDefs[q].name, nt, nr, addT, addR,
               shortNow ? " - STILL SHORT" : "");
    }
}

// R45-F1b: one ruin site, after the quarters, lakes, bushes and the quarter top-up. The site centre is at least ruinMinDist
// from the campfire (second pass: ruinFallbackDist; R47-F2: never under ruinCoreMinDist, so the site stays outside the
// safe core), outside every quarter's startup resource ring, inside the map below
// the HUD, and dry over its whole square. No spot = no ruins (and so no ridge or camps). Startup trees (trees/tris/stumps),
// rocks (rocks/octs) and bushes (bushes/bushshapes) touching the site disc are removed together; then the ridge's rocks
// are added on the far side, and each race's lean-to spot is fixed on the village-facing side.
// R47-F4: the one site placer (ruins, huts, wells): rolls up to `tries` whole-pixel centres at least `margin` inside the
// map (below the HUD strip) and keeps the first that ok(c) accepts. Places nothing itself.
template<typename OkFn>
bool placePoi(float margin, int tries, OkFn ok, Vector2f& out) {
    uniform_real_distribution<float> sx(margin, worldSize - margin), sy(hudBarHeight + margin, worldSize - margin);
    for (int k = 0; k < tries; k++) {
        Vector2f c(round(sx(gen)), round(sy(gen)));
        if (!ok(c)) continue;
        out = c;
        return true;
    }
    return false;
}

void spawnRuins(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<rock>& rocks,
                vector<CircleShape>& octs, vector<char>& rockGold, vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    ruinStones.clear();
    camps.clear();
    hollowPlaced = false;   // R47-F5
    hollowLootLeft = hollowLootCap;
    float lo = ruinSiteRadius + ruinEdgeMargin;
    bool found = false;
    for (float minDist : {ruinMinDist, ruinFallbackDist}) {
        minDist = max(minDist, ruinCoreMinDist);   // R47-F2: both passes keep the site disc outside the safe core
        found = placePoi(lo, ruinPlaceTries, [&](Vector2f c) {
            if (hypot(c.x - villageCenter.x, c.y - villageCenter.y) < minDist) return false;
            if (nearQuarter(c, ruinQuarterGap)) return false;
            return lakeClear(FloatRect(c - Vector2f(ruinSiteRadius, ruinSiteRadius), Vector2f(2 * ruinSiteRadius, 2 * ruinSiteRadius)));
        }, ruinCentre);
        if (found) break;
    }
    if (!found) { printf("Ruins: no spot found, none placed\n"); return; }

    // Stones: a rough ring (feet on the ring), two of them broken on opposite sides, one fallen stone in the middle
    uniform_real_distribution<float> twist(-ruinRingJitter, ruinRingJitter), startAng(0.0f, 6.2831853f);
    uniform_int_distribution<int> pickBroken(0, ruinStoneCount - 1);
    int brokenA = pickBroken(gen), brokenB = (brokenA + ruinStoneCount / 2) % ruinStoneCount;
    float a0 = startAng(gen);
    for (int i = 0; i < ruinStoneCount; i++) {
        float a = a0 + 6.2831853f * i / ruinStoneCount + twist(gen);
        Vector2f foot = ruinCentre + Vector2f(cos(a), sin(a)) * ruinRingRadius;
        float h = i == brokenA ? ruinBrokenH[0] : i == brokenB ? ruinBrokenH[1] : ruinStoneSize.y;
        ruinStones.push_back(FloatRect(snapPx(foot - Vector2f(ruinStoneSize.x / 2, h)), Vector2f(ruinStoneSize.x, h)));
    }
    ruinStones.push_back(FloatRect(snapPx(ruinCentre - ruinFallenSize / 2.0f), ruinFallenSize));
    sort(ruinStones.begin(), ruinStones.end(), [](const FloatRect& p, const FloatRect& q) {   // nearer (lower) stones drawn last
        return p.position.y + p.size.y < q.position.y + q.size.y;
    });

    int clearedT = 0, clearedR = 0, clearedB = 0;
    for (int i = (int)tris.size() - 1; i >= 0; i--) {
        if (ruinsClear(FloatRect(tris[i].getPosition(), Vector2f(treeFootprint, treeFootprint)))) continue;
        swapRemove(trees, i); swapRemove(tris, i); swapRemove(stumps, i);
        clearedT++;
    }
    for (int i = (int)octs.size() - 1; i >= 0; i--) {
        if (ruinsClear(octs[i].getGlobalBounds())) continue;
        swapRemove(rocks, i); swapRemove(octs, i); swapRemove(rockGold, i);
        clearedR++;
    }
    for (int i = (int)bushshapes.size() - 1; i >= 0; i--) {
        if (ruinsClear(bushshapes[i].getGlobalBounds())) continue;
        swapRemove(bushes, i); swapRemove(bushshapes, i);
        clearedB++;
    }

    // Camps: fanned by race round the direction back to the campfire
    Vector2f toVillage = villageCenter - ruinCentre;
    float homeAng = atan2(toVillage.y, toVillage.x);
    for (int r = 0; r < raceCount; r++) {
        float a = homeAng + (r - (raceCount - 1) / 2.0f) * campSpreadRad;
        campSpot[r] = snapPx(ruinCentre + Vector2f(cos(a), sin(a)) * campRuinDist - campSize / 2.0f);
    }
    // R47-F5: the Hollow, a quarter turn off the village direction: between the camp fan (+-0.6 rad) and the ridge
    // (from pi - 1.2 rad), past the stone ring, inside the cleared disc so nothing grows on it
    {
        float a = homeAng + 1.5707963f;
        hollowPos = snapPx(ruinCentre + Vector2f(cos(a), sin(a)) * hollowRuinDist - hollowSize / 2.0f);
        hollowPlaced = true;
        printf("Hollow: stair mouth at (%.0f, %.0f), %d hauls\n", hollowPos.x, hollowPos.y, hollowLootLeft);
    }

    // Ridge: ordinary rocks on an arc away from the village; trees under a ridge rock are removed so it reads
    const Vector2f rockSz(12.0f, 12.0f);   // 6px-radius pentagon, as everywhere else
    uniform_real_distribution<float> jit(-ridgeJitterPx, ridgeJitterPx);
    int steps = max(1, (int)(2 * ridgeHalfSpan * ridgeRadius / ridgeStepPx));
    int ridgeFirst = (int)octs.size();
    for (int i = 0; i <= steps; i++) {
        float a = homeAng + 3.1415927f - ridgeHalfSpan + 2 * ridgeHalfSpan * i / steps;
        Vector2f p = snapPx(ruinCentre + Vector2f(cos(a), sin(a)) * (ridgeRadius + jit(gen)) - rockSz / 2.0f);
        FloatRect rr(p, rockSz);
        if (p.x < 0.0f || p.y < hudBarHeight || p.x + rockSz.x > worldSize || p.y + rockSz.y > worldSize) continue;
        if (inClearing(p) || !lakeClear(rr) || !ruinsClear(rr)) continue;
        rocks.push_back(rock{(int)p.x, (int)p.y, false});
        rockGold.push_back(rollGoldRock());   // R46-F1a: same index as rocks/octs
        CircleShape oct;
        oct.setPointCount(5);
        oct.setPosition(p);
        oct.setRadius(6);
        oct.setFillColor(Color(125, 125, 125));
        octs.push_back(oct);
    }
    for (int i = (int)tris.size() - 1; i >= 0; i--) {
        FloatRect tb(tris[i].getPosition(), Vector2f(treeFootprint, treeFootprint));
        for (int k = ridgeFirst; k < (int)octs.size(); k++) {
            if (!tb.findIntersection(octs[k].getGlobalBounds())) continue;
            swapRemove(trees, i); swapRemove(tris, i); swapRemove(stumps, i);
            break;
        }
    }
    printf("Ruins at (%.0f, %.0f), %.0f px out: %zu stones, %d ridge rocks (cleared %d trees, %d rocks, %d bushes)\n",
           ruinCentre.x, ruinCentre.y, hypot(ruinCentre.x - villageCenter.x, ruinCentre.y - villageCenter.y), ruinStones.size(),
           (int)octs.size() - ridgeFirst, clearedT, clearedR, clearedB);
}

// R47-F4: huts and wells, poiBandCounts per band, after the ruins. Each goes through placePoi: in its band, clear of
// the quarters, the ruin site and ridge, every other site and the water. Then startup trees (trees/tris/stumps), rocks
// (rocks/octs/rockGold) and bushes (bushes/bushshapes) on or beside a footprint are removed together.
void spawnPois(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<rock>& rocks,
               vector<CircleShape>& octs, vector<char>& rockGold, vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    pois.clear();
    poiShapes.clear();
    int placed[poiKindCount] = {}, missed = 0;
    for (int z = 0; z < zoneCount; z++)
        for (int kind = 0; kind < poiKindCount; kind++)
            for (int n = 0; n < poiBandCounts[z][kind]; n++) {
                Vector2f sz = poiSizes[kind], c;
                bool ok = placePoi(poiEdgeMargin, poiPlaceTries, [&](Vector2f p) {
                    if (zoneAt(p) != z || nearQuarter(p, poiQuarterGap)) return false;
                    if (hasRuins() && hypot(p.x - ruinCentre.x, p.y - ruinCentre.y) < poiRuinGap) return false;
                    for (int i = 0; i < (int)pois.size(); i++) {
                        Vector2f d = poiCentre(i) - p;
                        if (d.x * d.x + d.y * d.y < poiMinGap * poiMinGap) return false;
                    }
                    return lakeClear(FloatRect(p - sz / 2.0f - Vector2f(poiClearPad, poiClearPad), sz + Vector2f(2 * poiClearPad, 2 * poiClearPad)));
                }, c);
                if (!ok) { missed++; continue; }
                poi s;
                s.id = nextPoiId++;
                s.kind = (poiKind)kind;
                pois.push_back(s);
                RectangleShape shape(sz);
                shape.setPosition(snapPx(c - sz / 2.0f));
                poiShapes.push_back(shape);
                placed[kind]++;
            }
    int clearedT = 0, clearedR = 0, clearedB = 0;
    for (int i = (int)tris.size() - 1; i >= 0; i--) {
        if (poiClear(FloatRect(tris[i].getPosition(), Vector2f(treeFootprint, treeFootprint)))) continue;
        swapRemove(trees, i); swapRemove(tris, i); swapRemove(stumps, i);
        clearedT++;
    }
    for (int i = (int)octs.size() - 1; i >= 0; i--) {
        if (poiClear(octs[i].getGlobalBounds())) continue;
        swapRemove(rocks, i); swapRemove(octs, i); swapRemove(rockGold, i);
        clearedR++;
    }
    for (int i = (int)bushshapes.size() - 1; i >= 0; i--) {
        if (poiClear(bushshapes[i].getGlobalBounds())) continue;
        swapRemove(bushes, i); swapRemove(bushshapes, i);
        clearedB++;
    }
    printf("Sites: %d hermit huts, %d sunken wells (%d unplaced; cleared %d trees, %d rocks, %d bushes)\n",
           placed[poiHut], placed[poiWell], missed, clearedT, clearedR, clearedB);
}

// R47-F4: per tick, after expeditionTick(). Spent sites age and crumble (removePoi, iterating down so no index is reused);
// every poiDiscoverEveryTicks the first visible barb near an unfound site finds it.
void poiTick(const vector<barb>& barbs, const vector<RectangleShape>& squares) {
    for (int i = (int)pois.size() - 1; i >= 0; i--)
        if (pois[i].spent && ++pois[i].spentTicks >= poiSpentLingerTicks) removePoi(i);
    static int discoverTimer = 0;
    if (++discoverTimer < poiDiscoverEveryTicks) return;
    discoverTimer = 0;
    int n = min((int)barbs.size(), (int)squares.size());
    for (int i = 0; i < (int)pois.size(); i++) {
        if (pois[i].found) continue;
        Vector2f c = poiCentre(i);
        for (int j = 0; j < n; j++) {
            if (barbHidden(barbs[j])) continue;
            Vector2f d = squares[j].getPosition() + squares[j].getSize() / 2.0f - c;
            if (d.x * d.x + d.y * d.y >= poiDiscoverRadius * poiDiscoverRadius) continue;
            discoverPoi(i, barbs[j].race, barbs[j].name);
            break;
        }
    }
}

// R45-F1b: one lean-to per race at the ruins, through buildOneOff (jobCamp) by a builder of that race, once its crate
// stands and it has campMinHouses houses of its own; wood is paid at claim (houseWoodCost kept back). The claim is not
// stored: it is "a barb of this race holds jobCamp", so a lost builder frees it through releaseBarbWork()'s reset.
void buildcamps(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, const storehouse (&stores)[raceCount]) {
    if (!hasRuins()) return;
    for (int r = 0; r < raceCount; r++) {
        if (campBuilt(r) || !stores[r].built) continue;
        bool claimed = false;
        for (auto& b : barbs) if (b.jobKind == jobCamp && b.race == r) claimed = true;
        bool done = buildOneOff(barbs, squares, claimed, jobCamp, campSpot[r] + campStandOffset, "LEAN-TO",
            [&](const barb&) {
                int raceHouses = 0;
                for (auto& h : houses) if (h.race == r) raceHouses++;
                if (raceHouses < campMinHouses) return -1;
                if (!canAfford(stock[r], campWoodCost + houseWoodCost, 0)) return -1;
                spend(stock[r], campWoodCost, 0);
                return 1;
            }, r);
        if (!done) continue;
        camps.push_back(leanTo{r, campSpot[r]});
        printf("---LEAN-TO BUILT--- %s camp at the ruins (%.0f, %.0f)\n", raceDefs[r].name, campSpot[r].x, campSpot[r].y);
        logRaceEvent(string(racePlurals[r]) + " pitch a lean-to by the ruins", r);
    }
}

// R47-F6: runs after buildkeeps(). Each race whose keep stands builds one barracks at its fixed spot left of the crate,
// through buildOneOff (jobBarracks) by a barb of that race, paid at claim with a house's wood and stone kept back. The
// claim is not stored ("a barb of this race holds jobBarracks"), so a lost builder frees it through releaseBarbWork().
// Also, every tick: each drill slot must name a live training barb of that race in that slot, and the dummy shake counts down.
void buildbarracks(vector<barb>& barbs, vector<RectangleShape>& squares, const storehouse (&stores)[raceCount],
                   const vector<CircleShape>& bushshapes) {
    for (int r = 0; r < raceCount; r++) {
        barracksHall& hall = halls[r];
        for (int s = 0; s < kDrillSlots; s++) {
            int id = hall.slotId[s];
            if (id < 0) continue;
            int j = findBarbById(barbs, id);
            if (j < 0 || barbs[j].activity != actDrill || barbs[j].drillSlot != s || barbs[j].race != r) {
                printf("DRILL SLOT FREED - %s slot %d, barb %d\n", racePlurals[r], s, id);
                hall.slotId[s] = -1;
            }
        }
        if (hall.shakeTicks > 0) hall.shakeTicks--;
        if (hall.built || !stores[r].built || !keeps[r].built) continue;
        bool claimed = false;
        for (auto& b : barbs) if (b.jobKind == jobBarracks && b.race == r) claimed = true;
        bool done = buildOneOff(barbs, squares, claimed, jobBarracks, barracksPos(r) + barracksStandOffset, "BARRACKS",
            [&](const barb&) {
                FloatRect z = barracksZone(r);
                if (!lakeClear(z)) return -1;   // fixed spot: a lake or bush on it means no barracks for this race
                for (auto& bs : bushshapes) if (z.findIntersection(bs.getGlobalBounds())) return -1;
                if (!canAfford(stock[r], barracksWoodCost + houseWoodCost, barracksStoneCost + houseStoneCost)) return -1;
                spend(stock[r], barracksWoodCost, barracksStoneCost);
                return 1;
            }, r);
        if (!done) continue;
        hall.built = true;
        printf("---BARRACKS BUILT--- %s (%d wood, %d stone), %d drill slots\n", raceDefs[r].name, barracksWoodCost, barracksStoneCost, kDrillSlots);
        logRaceEvent(string(racePlurals[r]) + " raise a barracks", r);
    }
}

// R47-F6: 10x8 hall (inset grey roof row, full roof row, darker eave, timber walls, 2x3 door, two 1px windows) with a
// 1px ground shadow, and the straw dummy in front: fixed post and torso, head and arms swaying 1px after each hit.
// World layer with the other buildings, under barbs and trees.
void drawBarracks(RenderWindow& window) {
    RectangleShape p;
    auto rect = [&](Vector2f pos, Vector2f size, Color c) {
        p.setPosition(pos);
        p.setSize(size);
        p.setFillColor(c);
        window.draw(p);
    };
    for (int r = 0; r < raceCount; r++) {
        const barracksHall& hall = halls[r];
        if (!hall.built) continue;
        Vector2f o = snapPx(barracksPos(r));
        rect(o + Vector2f(1.0f, barracksSize.y), Vector2f(barracksSize.x, 1.0f), buildingShadowColor);
        rect(o + Vector2f(0.0f, barracksRoofH), Vector2f(barracksSize.x, barracksSize.y - barracksRoofH), barracksWallColor);
        rect(o + Vector2f(1.0f, 0.0f), Vector2f(barracksSize.x - 2.0f, 1.0f), barracksRoofColor);
        rect(o + Vector2f(0.0f, 1.0f), Vector2f(barracksSize.x, barracksRoofH - 2.0f), barracksRoofColor);
        rect(o + Vector2f(0.0f, barracksRoofH - 1.0f), Vector2f(barracksSize.x, 1.0f), barracksEaveColor);
        rect(o + barracksDoorPos, barracksDoorSize, barracksDoorColor);
        for (auto& w : barracksWindowPos) rect(o + w, Vector2f(1.0f, 1.0f), barracksWindowColor);
        Vector2f d = o + dummyOffset;
        float sway = hall.shakeTicks > 0 ? ((hall.shakeTicks / kDummyShakeStepTicks) % 2 == 0 ? 1.0f : -1.0f) : 0.0f;
        rect(d + Vector2f(1.0f, 3.0f), Vector2f(1.0f, dummySize.y - 3.0f), dummyPostColor);   // post, planted
        rect(d + Vector2f(1.0f, 2.0f), Vector2f(1.0f, 1.0f), dummyStrawColor);                // torso, planted
        rect(d + Vector2f(sway, 1.0f), Vector2f(dummySize.x, 1.0f), dummyStrawColor);         // arms
        rect(d + Vector2f(1.0f + sway, 0.0f), Vector2f(1.0f, 1.0f), dummyHeadColor);          // sack head
    }
}

// R53-F2: runs after buildbarracks(). Once the herb race's crate stands and it has kHerbHutMinHouses houses, a barb of
// that race builds the herb hut at its fixed spot through buildOneOff (jobHerbHut), paid at claim with a house's wood
// kept back. The claim is not stored ("a barb of this race holds jobHerbHut"), so a lost builder frees it through
// releaseBarbWork(). The bed is sown with the hut, its plants part-grown (bare in winter).
void buildherbhut(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses,
                  const storehouse (&stores)[raceCount], const vector<CircleShape>& bushshapes) {
    int r = kHerbRace;
    herbGarden& g = herbGardens[r];
    if (g.built || !stores[r].built) return;
    bool claimed = false;
    for (auto& b : barbs) if (b.jobKind == jobHerbHut && b.race == r) claimed = true;
    bool done = buildOneOff(barbs, squares, claimed, jobHerbHut, herbHutPos(r) + herbHutStandOffset, "HERB HUT",
        [&](const barb&) {
            int raceHouses = 0;
            for (auto& h : houses) if (h.race == r) raceHouses++;
            if (raceHouses < kHerbHutMinHouses) return -1;
            FloatRect z = herbZone(r);
            if (!lakeClear(z)) return -1;   // fixed spot: a lake or bush on it means no herb hut
            for (auto& bs : bushshapes) if (z.findIntersection(bs.getGlobalBounds())) return -1;
            if (!canAfford(stock[r], herbHutWoodCost + houseWoodCost, 0)) return -1;
            spend(stock[r], herbHutWoodCost, 0);
            return 1;
        }, r);
    if (!done) return;
    g = herbGarden{};
    g.built = true;
    g.lastSeason = seasonOf(game_day);
    uniform_real_distribution<float> seedGrowth(0.0f, 0.6f);
    for (auto& p : g.plants) p.growth = isWinter() ? 0.0f : seedGrowth(gen);
    printf("---HERB HUT BUILT--- %s (%d wood), %d plants sown\n", raceDefs[r].name, herbHutWoodCost, kHerbPlants);
    logRaceEvent(string(racePlurals[r]) + " raise a herb hut and sow a herb patch", r);
}

// R53-F2: once per tick. Validates each bed's tender by id, turns the seasons (browns the bed at the first winter
// midnight with a "stored for winter" line, greens it in spring), grows the plants, makes harvest day (the last day of
// summer, from dawn) a full bloom, caches the ripe count for herbWanted(), and spoils the stock slowly in winter.
// kHerbPlants per built bed: no barb loop beyond one id lookup.
void herbTick(const vector<barb>& barbs) {
    int s = seasonOf(game_day);
    bool harvestDay = s == seasonSummer && dayInSeason(game_day) == daysPerSeason - 1 && game_time >= nightEndHour;
    for (int r = 0; r < raceCount; r++) {
        herbGarden& g = herbGardens[r];
        if (!g.built) continue;
        if (g.tenderId >= 0) {
            int j = findBarbById(barbs, g.tenderId);
            if (j < 0 || barbs[j].activity != actHerb || barbs[j].race != r) {
                printf("HERB BED FREED - %s, barb %d\n", racePlurals[r], g.tenderId);
                g.tenderId = -1;
            }
        }
        if (s != g.lastSeason) {
            if (s == seasonWinter) {
                for (auto& p : g.plants) p = herbPlant{};
                printf("HERBS BROWNED - %s, stock %d\n", racePlurals[r], raceHerbs[r]);
                logRaceEvent(raceHerbs[r] > 0 ? "Frost browns the herb patch - " + to_string(raceHerbs[r]) + " herbs stored for winter"
                                              : "Frost browns the herb patch - no herbs stored for winter", r);
            } else if (g.lastSeason == seasonWinter) logRaceEvent(string(racePlurals[r]) + "' herb patch greens again", r);
            g.lastSeason = s;
            g.spoilTicks = 0;
        }
        if (harvestDay && g.bloomDay != game_day) {
            g.bloomDay = game_day;
            for (auto& p : g.plants) { p.growth = 1.0f; p.bloom = true; }
            printf("HERBS BLOOM - %s harvest day\n", racePlurals[r]);
            logRaceEvent("Harvest day: the " + string(racePlurals[r]) + "' herb patch is in bloom", r);
        }
        float grow = herbSeasonGrowth[s] / kHerbRipenTicks;
        g.ripe = 0;
        for (auto& p : g.plants) {
            if (p.bloom && g.bloomDay != game_day) p.bloom = false;   // the bloom lasts the day; the plant stays ripe
            p.growth = min(1.0f, p.growth + grow);
            if (p.growth >= 1.0f) g.ripe++;
        }
        if (s != seasonWinter || raceHerbs[r] <= 0) continue;
        g.spoilTicks += raining ? kHerbRainSpoilMult : 1;
        if (g.spoilTicks < kHerbSpoilTicks) continue;
        g.spoilTicks = 0;
        raceHerbs[r]--;
        printf("HERB SPOILED - %s stock %d\n", racePlurals[r], raceHerbs[r]);
        if (raceHerbs[r] == 0 && !g.bareSaid) {
            g.bareSaid = true;
            logRaceEvent(string(racePlurals[r]) + "' herb store is bare", r);
        }
    }
}

// R53-F2: thatched hut (inset sage roof row, full roof row, darker eave, timber walls, 1x2 door, two dried bundles under
// the eave) with a 1px ground shadow, and the bed beside it: tilled soil, darker bottom row, six 2x2 plants (leaf row
// under a flower row): bare leaves, a pale bud from half-grown, two light-purple pixels when ripe, a brighter bloom with
// a third pixel above on harvest day, and browned leaves in winter. One vertex array; world layer under barbs and trees.
void drawHerbHut(RenderWindow& window) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    bool winter = isWinter();
    for (int r = 0; r < raceCount; r++) {
        const herbGarden& g = herbGardens[r];
        if (!g.built) continue;
        Vector2f o = snapPx(herbHutPos(r));
        pushQuad(va, o + Vector2f(1.0f, herbHutSize.y), Vector2f(herbHutSize.x, 1.0f), buildingShadowColor);
        pushQuad(va, o + Vector2f(0.0f, herbHutRoofH), Vector2f(herbHutSize.x, herbHutSize.y - herbHutRoofH), herbHutWallColor);
        pushQuad(va, o + Vector2f(1.0f, 0.0f), Vector2f(herbHutSize.x - 2.0f, 1.0f), herbHutRoofColor);
        pushQuad(va, o + Vector2f(0.0f, 1.0f), Vector2f(herbHutSize.x, herbHutRoofH - 2.0f), herbHutRoofColor);
        pushQuad(va, o + Vector2f(0.0f, herbHutRoofH - 1.0f), Vector2f(herbHutSize.x, 1.0f), herbHutEaveColor);
        pushQuad(va, o + herbHutDoorPos, herbHutDoorSize, herbHutDoorColor);
        for (auto& w : herbHutBundlePos) pushQuad(va, o + w, Vector2f(1.0f, 1.0f), herbBundleColor);
        Vector2f bed = snapPx(herbBedPos(r));
        pushQuad(va, bed, Vector2f(herbBedSize.x, herbBedSize.y - 1.0f), herbBedColor);
        pushQuad(va, bed + Vector2f(0.0f, herbBedSize.y - 1.0f), Vector2f(herbBedSize.x, 1.0f), herbBedEdgeColor);
        for (int k = 0; k < kHerbPlants; k++) {
            Vector2f p = snapPx(herbPlantPos(r, k));
            const herbPlant& hp = g.plants[k];
            if (winter) { pushQuad(va, p + Vector2f(0.0f, 1.0f), Vector2f(2.0f, 1.0f), herbBrownColor); continue; }
            pushQuad(va, p + Vector2f(0.0f, 1.0f), Vector2f(2.0f, 1.0f), herbLeafColor);
            if (hp.growth >= 1.0f) {
                Color c = hp.bloom ? herbBloomColor : herbFlowerColor;
                pushQuad(va, p, Vector2f(2.0f, 1.0f), c);
                if (hp.bloom) pushQuad(va, p + Vector2f(0.0f, -1.0f), Vector2f(1.0f, 1.0f), c);
            } else if (hp.growth >= 0.5f) pushQuad(va, p, Vector2f(1.0f, 1.0f), herbBudColor);
        }
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R45-F1b: pale stones, one vertex array: body, 1px weathered cap (broken stones: a 4px stub, the corner knocked off),
// 1px shaded right column, moss at the foot of every other stone. Drawn with the buildings, under barbs and trees.
void drawRuins(RenderWindow& window) {
    if (!hasRuins()) return;
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (int i = 0; i < (int)ruinStones.size(); i++) {
        Vector2f p = ruinStones[i].position, s = ruinStones[i].size;
        bool broken = s.x == ruinStoneSize.x && s.y < ruinStoneSize.y;
        pushQuad(va, p + Vector2f(0.0f, 1.0f), Vector2f(s.x, s.y - 1.0f), ruinStoneColor);
        pushQuad(va, p, Vector2f(broken ? s.x - 2.0f : s.x, 1.0f), ruinStoneTopColor);
        pushQuad(va, p + Vector2f(s.x - 1.0f, 1.0f), Vector2f(1.0f, s.y - 1.0f), ruinStoneShadeColor);
        if (i % 2 == 0) pushQuad(va, p + Vector2f(0.0f, s.y - 1.0f), Vector2f(1.0f, 1.0f), ruinMossColor);
    }
    window.draw(va);
}

// R47-F5: the Hollow's 6x6 stair mouth. Row 0 is a pale lintel (flashing toward cream when a party surfaces), the
// columns at each side are jambs (lit left, shaded right), and inside is a dark 4x5 opening with three 1px steps on
// rows 1, 3 and 5, each darker and the last narrower, so the stairs fall away into the dark. Picked clean: flat dull steps.
void drawHollow(RenderWindow& window) {
    if (!hollowPlaced) return;
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    Vector2f o = snapPx(hollowPos);
    bool clean = hollowLootLeft <= 0;
    pushQuad(va, o + Vector2f(1.0f, 1.0f), Vector2f(4.0f, 5.0f), hollowDarkColor);   // the opening
    for (int s = 0; s < 3; s++) {
        float inset = s == 2 ? 1.0f : 0.0f;
        pushQuad(va, o + Vector2f(1.0f + inset, 1.0f + 2.0f * s), Vector2f(4.0f - 2.0f * inset, 1.0f),
                 clean ? hollowCleanStepColor : hollowStepColors[s]);
    }
    pushQuad(va, o + Vector2f(0.0f, 1.0f), Vector2f(1.0f, 5.0f), hollowJambColor);
    pushQuad(va, o + Vector2f(5.0f, 1.0f), Vector2f(1.0f, 5.0f), hollowJambShadeColor);
    Color lc = hollowLintelColor;
    if (hollowFlash > 0) lc = lerpColor(lc, hollowFlashColor, hollowFlashMix * hollowFlash / hollowFlashTicks);
    pushQuad(va, o, Vector2f(hollowSize.x, 1.0f), lc);
    window.draw(va);
}

// R45-F1b: 5x4 lean-to, roof sloping down to the right in the race's crate colour (lit top rows, darker eave), a timber
// pole at each end and a dark open front under the roof.
void drawCamps(RenderWindow& window) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (auto& c : camps) {
        Color roof = storehouseColors[clampRace(c.race)];
        Color lit = lerpColor(roof, Color::White, 0.15f);
        Vector2f o = snapPx(c.pos);
        pushQuad(va, o + Vector2f(1.0f, 2.0f), Vector2f(3.0f, 2.0f), campInsideColor);   // open front
        pushQuad(va, o + Vector2f(0.0f, 1.0f), Vector2f(1.0f, 3.0f), campPoleColor);     // back pole, under the roof edge
        pushQuad(va, o + Vector2f(4.0f, 3.0f), Vector2f(1.0f, 1.0f), campPoleColor);     // short front pole
        pushQuad(va, o, Vector2f(2.0f, 1.0f), lit);                                      // roof, top row
        pushQuad(va, o + Vector2f(1.0f, 1.0f), Vector2f(3.0f, 1.0f), roof);              // roof, middle row
        pushQuad(va, o + Vector2f(3.0f, 2.0f), Vector2f(2.0f, 1.0f), shade(roof, 0.8f)); // eave
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R45-F1b: faint breathing glow over the ruins in the evening hours, after the day/night overlay (additive, like the
// campfire); rain dims it to ruinGlowRainMult.
void drawRuinGlow(RenderWindow& window, const View& worldView, int frame) {
    if (!hasRuins()) return;
    window.setView(worldView);
    float breath = 1.0f + ruinGlowBreathDepth * sin(frame * 6.2831853f / ruinGlowBreathFrames);
    Color col = ruinGlowColor;
    col.a = (uint8_t)(ruinGlowLayerAlpha * breath * (raining ? ruinGlowRainMult : 1.0f));
    if (col.a == 0) return;
    CircleShape c;
    c.setPosition(ruinCentre);
    c.setFillColor(col);
    c.setPointCount(40);
    for (int i = 1; i <= ruinGlowLayers; i++) {
        float r = ruinGlowRadius * i / ruinGlowLayers;
        c.setRadius(r);
        c.setOrigin({r, r});
        window.draw(c, BlendAdd);
    }
}

// R47-F4: huts and wells in one vertex array, with the buildings (under barbs and trees), culled to the view.
// Hut (5x4): mirrored lean-to in neutral thatch sloping down to the left, timber back wall on the right, a stone
// chimney pixel at the high end and, while unspent, a 1px smoke pixel rising above it. Well (6x6): stone ring with
// the corners cut (lit top row, shaded bottom row) round dark water with a shadowed top row and a 1px glint; dry once
// spent. Spent sites draw at poiSpentShade.
void drawPois(RenderWindow& window, const FloatRect& camRect, int frame) {
    static VertexArray va(PrimitiveType::Triangles);
    va.clear();
    for (int i = 0; i < (int)pois.size() && i < (int)poiShapes.size(); i++) {
        Vector2f o = snapPx(poiShapes[i].getPosition()), s = poiShapes[i].getSize();
        if (!camRect.findIntersection(FloatRect(o - Vector2f(0.0f, 4.0f), s + Vector2f(0.0f, 4.0f)))) continue;
        bool spent = pois[i].spent;
        auto col = [&](Color c) { return spent ? shade(c, poiSpentShade) : c; };
        if (pois[i].kind == poiHut) {
            pushQuad(va, o + Vector2f(1.0f, 2.0f), Vector2f(3.0f, 2.0f), col(poiHutInsideColor));   // open front
            pushQuad(va, o + Vector2f(4.0f, 1.0f), Vector2f(1.0f, 3.0f), col(poiHutWallColor));     // back wall, high side
            pushQuad(va, o + Vector2f(0.0f, 3.0f), Vector2f(1.0f, 1.0f), col(poiHutWallColor));     // short front pole
            pushQuad(va, o + Vector2f(2.0f, 0.0f), Vector2f(2.0f, 1.0f), col(poiThatchLitColor));   // thatch, top row
            pushQuad(va, o + Vector2f(4.0f, 0.0f), Vector2f(1.0f, 1.0f), col(poiChimneyColor));     // chimney stone
            pushQuad(va, o + Vector2f(1.0f, 1.0f), Vector2f(3.0f, 1.0f), col(poiThatchColor));      // thatch, middle row
            pushQuad(va, o + Vector2f(0.0f, 2.0f), Vector2f(2.0f, 1.0f), col(poiThatchEaveColor));  // eave
            if (!spent) {   // smoke: rises 1..3 rows over the chimney, fading as it goes
                int step = (frame / poiSmokeFrames) % 3;
                Color sm = poiSmokeColor;
                sm.a = (uint8_t)(poiSmokeColor.a * (3 - step) / 3);
                pushQuad(va, o + Vector2f(4.0f, -1.0f - step), Vector2f(1.0f, 1.0f), sm);
            }
        } else {
            pushQuad(va, o + Vector2f(1.0f, 0.0f), Vector2f(4.0f, 1.0f), col(poiWellStoneLitColor));    // top rim
            pushQuad(va, o + Vector2f(0.0f, 1.0f), Vector2f(1.0f, 4.0f), col(poiWellStoneColor));       // left rim
            pushQuad(va, o + Vector2f(5.0f, 1.0f), Vector2f(1.0f, 4.0f), col(poiWellStoneColor));       // right rim
            pushQuad(va, o + Vector2f(1.0f, 5.0f), Vector2f(4.0f, 1.0f), col(poiWellStoneShadeColor));  // bottom rim
            if (spent) pushQuad(va, o + Vector2f(1.0f, 1.0f), Vector2f(4.0f, 4.0f), poiWellDryColor);  // dry bed
            else {
                pushQuad(va, o + Vector2f(1.0f, 1.0f), Vector2f(4.0f, 1.0f), poiWellWaterShadowColor);  // sunken: rim shadow
                pushQuad(va, o + Vector2f(1.0f, 2.0f), Vector2f(4.0f, 3.0f), poiWellWaterColor);
                pushQuad(va, o + Vector2f(3.0f, 3.0f), Vector2f(1.0f, 1.0f), poiWellGlintColor);
            }
        }
    }
    if (va.getVertexCount() > 0) window.draw(va);
}

// R47-F4: found sites glow soft violet (additive, after the day/night overlay, any hour, dimmed by rain like the ruins);
// spent sites keep one faint small layer; undiscovered sites have none.
void drawPoiGlow(RenderWindow& window, const View& worldView, const FloatRect& camRect, int frame) {
    window.setView(worldView);
    float breath = 1.0f + ruinGlowBreathDepth * sin(frame * 6.2831853f / ruinGlowBreathFrames);
    float rain = raining ? ruinGlowRainMult : 1.0f;
    CircleShape c;
    c.setPointCount(24);
    for (int i = 0; i < (int)pois.size() && i < (int)poiShapes.size(); i++) {
        if (!pois[i].found) continue;
        Vector2f p = poiCentre(i);
        if (!camRect.contains(p)) continue;
        bool spent = pois[i].spent;
        int layers = spent ? 1 : poiGlowLayers;
        float outer = spent ? poiSpentGlowRadius : poiGlowRadius;
        Color col = poiGlowColor;
        col.a = (uint8_t)((spent ? poiSpentGlowAlpha : poiGlowLayerAlpha * breath) * rain);
        if (col.a == 0) continue;
        c.setFillColor(col);
        c.setPosition(p);
        for (int k = 1; k <= layers; k++) {
            float r = outer * k / layers;
            c.setRadius(r);
            c.setOrigin({r, r});
            window.draw(c, BlendAdd);
        }
    }
}

// Bare bushes ripen again after bushRegrowTicks (rainRegrowMult times faster in rain)
void bushTick(vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    if (isWinter()) return;   // R52-F2: the lean season: bare bushes wait for spring (regrow counters hold)
    for (int i = 0; i < (int)bushes.size(); i++) {
        if (bushes[i].ripe) continue;
        bushes[i].regrow += raining ? rainRegrowMult : 1;
        if (bushes[i].regrow < bushRegrowTicks) continue; // >= ripens, so overshoot can't skip it
        bushes[i].ripe = true;
        bushes[i].regrow = 0;
        bushshapes[i].setFillColor(bushRipeColor);
    }
}

// R52-F2: first frost (the tick winter begins) shrivels every ripe bush nobody is picking, with one ticker line; and
// once every larderRefreshTicks the larder bar values are cached per race. Bushes are never removed, so the shrivel
// pass edits bushes[i] and bushshapes[i] in place at the same index (no swaps, both vectors stay in step).
void leanSeasonTick(const vector<barb>& barbs, vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    static bool wasWinter = isWinter();
    bool winter = isWinter();
    if (winter && !wasWinter) {
        int withered = 0;
        for (int i = 0; i < (int)bushes.size() && i < (int)bushshapes.size(); i++) {
            if (!bushes[i].ripe || bushes[i].picking) continue;   // a picker already on it finishes the pick
            bushes[i].ripe = false;
            bushes[i].regrow = 0;
            bushshapes[i].setFillColor(bushBareColor);
            withered++;
        }
        printf("LEAN SEASON - frost withers %d ripe bushes\n", withered);
        logEvent("Frost withers " + to_string(withered) + " berry bushes - no regrowth till spring", seasonDefs[seasonWinter].ticker);
    }
    wasWinter = winter;

    static int refresh = 0;
    if (refresh > 0) { refresh--; return; }
    refresh = larderRefreshTicks - 1;
    int eaters[raceCount] = {};
    for (auto& b : barbs) if (b.activity != actExpedition) eaters[clampRace(b.race)]++;   // away members eat rations, not the larder
    float mealsPerDay = 24.0f * ticksPerGameHour / hungerThreshold * winterHungerMult;   // winter rate: it is the lean days we count
    float need = leanDaysToSpring();
    for (int r = 0; r < raceCount; r++) {
        float days = eaters[r] > 0 ? stock[r].food / (eaters[r] * mealsPerDay) : 0.0f;
        larderCovered[r] = eaters[r] > 0 && days >= need;
        larderFill[r] = need > 0.0f ? clamp(days / need, 0.0f, 1.0f) : 1.0f;
    }
}

int main() {
    RenderWindow window(VideoMode({(unsigned)windowSize, (unsigned)windowSize}), "sim");
    window.setFramerateLimit(60);

    Clock clock;
    Time timesincelastupdate = Time::Zero;
    const Time Timepertick = seconds(1.0f / 60.0f);

    resetKnowHow();   // fresh practice counters and no learned techs for every new game (R20-F1)
    resetTrails();    // wear, tiers and cobbles (R20-F2)
    vector<barb> barbs;
    vector<RectangleShape> squares;
    vector<birthRing> birthRings;
    vector<birthRing> catchRings;   // fishing ripples (R19-F1)
    vector<shinobiPuff> shinobiPuffs; // Sasuke's blink puffs (R25-F2)
    barbs.reserve(numbarbs);
    squares.reserve(numbarbs);
    placeQuarters();  // R30-F1: crates first, so founders, lakes and bushes can use the quarters
    if (quarterFallbackUsed) logEvent("Quarters: random layout failed, fixed layout used");   // INBOX-F3: once per layout
    {
        uniform_int_distribution<int> d(-30, 30);   // R30-F1: founders start around their own race's crate
        int founded[raceCount] = {};
        for (int i = 0; i < numbarbs; i++) {
            Vector2f qc = quarterCentre(i / foundersPerRace);
            int x = (int)qc.x + d(gen), y = (int)qc.y + d(gen);
            makeBarb(Vector2i(x, y), barbs, squares, birthRings, -1, i / foundersPerRace);   // R28-F1: clan by index
            founded[barbs.back().race]++;
        }
        string founders = "Founders:";
        for (int r = 0; r < raceCount; r++)
            founders += string(r ? ", " : " ") + to_string(founded[r]) + " " + raceDefs[r].name;
        printf("%s\n", founders.c_str());
        logEvent(founders);
        printf("%s (%.0f, %.0f)\n", foundingTickerLine, villageCenter.x, villageCenter.y);
        logEvent(foundingTickerLine);   // R47-F2: once per game, at startup
    }

    // R47-F1: startup trees and rocks bucketed per band (zoneTreeTarget/zoneRockTarget), uniform inside each band
    computeZoneTargets();
    buildZoneTint();
    vector<tree> trees;
    vector<rock> rocks;
    for (int z = 0; z < zoneCount; z++) {
        Vector2i p;
        for (int k = 0; k < zoneTreeTarget[z]; k++)
            if (rollInZone(z, treeFootprint, p)) trees.push_back(tree{p.x, p.y, false});
        for (int k = 0; k < zoneRockTarget[z]; k++)
            if (rollInZone(z, 12.0f, p)) rocks.push_back(rock{p.x, p.y, false});   // 6px-radius pentagon
    }

    vector<house> houses;
    vector<RectangleShape> houseshapes;
    vector<shrine> shrines;
    shrines.reserve(maxShrines);
    smokehouseSet smoke;   // INBOX2-F1: one per race, indexed by barbRace
    forge dwarfForge;
    storehouse stores[raceCount];   // R29-F1: one per race, indexed by barbRace
    fieldGrid fields;   // FARM-F1: fields[race][k], each race's wheat plots beside its storehouse

    vector<deer> deers;
    vector<RectangleShape> deershape;

    vector<CircleShape> tris;
    vector<RectangleShape> stumps;
    tris.reserve(trees.size());
    stumps.reserve(trees.size());
    for (auto& t : trees) {
        tris.push_back(makeTreeTri(t.x, t.y));

        RectangleShape stump(Vector2f(4, 4));
        stump.setFillColor(stumpColor);
        stump.setPosition(Vector2f(t.x + 6, t.y + 12));
        stumps.push_back(stump);
    }

    vector<CircleShape> octs;
    octs.reserve(rocks.size());
    for (auto& r : rocks) {
        CircleShape oct;
        oct.setPointCount(5);
        oct.setPosition(Vector2f(r.x, r.y));
        oct.setRadius(6);
        oct.setFillColor(Color(125, 125, 125));
        octs.push_back(oct);
    }
    vector<char> rockGold;   // R46-F1a: 1 = gold-flecked, parallel to rocks/octs (every swapRemove touches all three)
    rockGold.reserve(rocks.size());
    for (size_t i = 0; i < rocks.size(); i++) rockGold.push_back(rollGoldRock());

    spawnLakes(trees, tris, stumps, rocks, octs, rockGold); // before the bushes so clumps can avoid the water

    vector<bush> bushes;
    vector<CircleShape> bushshapes;
    spawnBushes(bushes, bushshapes, trees, tris, stumps);
    checkQuarters(trees, tris, stumps, rocks, octs, rockGold, bushshapes);   // R30-F1: no quarter short of wood/stone
    spawnRuins(trees, tris, stumps, rocks, octs, rockGold, bushes, bushshapes);   // R45-F1b: last, so it clears whatever landed on the site
    spawnPois(trees, tris, stumps, rocks, octs, rockGold, bushes, bushshapes);    // R47-F4: after the ruins, so sites keep clear of them
    for (auto& s : stock) {   // RACE-F1: every race starts with at least startStock
        s.wood = max(s.wood, startStock.wood);
        s.stone = max(s.stone, startStock.stone);
        s.food = max(s.food, startStock.food);
    }

    VertexArray treeVerts(PrimitiveType::Triangles);
    VertexArray rockVerts(PrimitiveType::Triangles), bushVerts(PrimitiveType::Triangles), stumpVerts(PrimitiveType::Triangles);   // rebuilt every frame
    VertexArray rainVerts(PrimitiveType::Triangles); // scratch buffer for drawRain, rebuilt every frame
    VertexArray minimapVerts(PrimitiveType::Triangles); // UIO-F1: scratch buffer for drawMinimap's dots, rebuilt every frame

    RectangleShape dnc({windowSize, windowSize});   // screen-space overlay, drawn in the default view

    Font hudFont;
    bool hudFontOk = hudFont.openFromFile(hudFontPath);
    if (!hudFontOk) printf("HUD font failed to load: %s\n", hudFontPath);
    else printf("HUD font loaded: %s\n", hudFontPath);

    bool paused = false;
    int simSpeed = 1;
    float zoom = minZoom;
    // R45-F1a: open on the village; the view spans windowSize / zoom world px. R47-F2: nudged up by half the HUD strip so
    // the campfire sits centred in the visible area below the HUD
    Vector2f camCenter = villageCenter - Vector2f(0.0f, hudBarHeight / 2.0f / zoom);
    bool panning = false;
    Vector2i lastMouse;
    View worldView(FloatRect({0.0f, 0.0f}, {windowSize, windowSize}));
    optional<trader> activeTrader;
    FloatRect tickerRect;                 // last frame's ticker strip, used to suppress hover over it
    FloatRect hudRect;                    // last frame's HUD strip, same use
    int selectedBarbId = -1;              // UIO-F2: inspected barb (stable id, never an index); -1 = none
    FloatRect inspectRect;                // UIO-F2: last frame's inspect panel; empty when nothing is selected
    Lens lens = Lens::Off;                // UIO-F3: overlay lens, cycled with L
    FloatRect lensRect;                   // UIO-F3: last frame's lens legend; empty when off or hidden
    FloatRect stockRect;                  // RACE-F1: last frame's race stock panel; empty when hidden
    VertexArray lensVerts(PrimitiveType::Triangles);
    VertexArray trailVerts(PrimitiveType::Triangles);
    VertexArray paveVerts(PrimitiveType::Triangles);   // cobbled cells (R20-F2), rebuilt only when a cell is paved
    VertexArray wallVerts(PrimitiveType::Triangles);   // R31-F2: scratch buffer for drawWalls, rebuilt every frame
    vector<bird> birds;
    Vector2f birdHeading(1.0f, 0.0f);
    RectangleShape birdShape;
    int frameCount = 0;
    vector<wolf> wolves;
    vector<prowler> prowlers;   // R48-F1
    campfire fire;

    while (window.isOpen()) {
        while (const optional event = window.pollEvent())
            if (event->is<Event::Closed>()) window.close();
            else if (const auto* kp = event->getIf<Event::KeyPressed>()) {
                if (kp->code == Keyboard::Key::Space) paused = !paused;
                else if (kp->code == Keyboard::Key::Num1) simSpeed = 1;
                else if (kp->code == Keyboard::Key::Num2) simSpeed = 2;
                else if (kp->code == Keyboard::Key::Num3) simSpeed = 4;
                else if (kp->code == Keyboard::Key::Escape) selectedBarbId = -1;   // UIO-F2: deselect
                else if (kp->code == Keyboard::Key::L) {   // UIO-F3: Off -> Hunger -> Race -> Off
                    lens = lens == Lens::Off ? Lens::Hunger : lens == Lens::Hunger ? Lens::Race : Lens::Off;
                    printf("Lens: %s\n", lensNames[(int)lens]);
                }
                else if (kDebugBuild && kp->code == kDebugActivityKey) {   // R53-F4: debug builds only
                    debugActivityTip = !debugActivityTip;
                    printf("Debug activity tooltip: %s\n", debugActivityTip ? "on" : "off");
                }
            }
            else if (const auto* ms = event->getIf<Event::MouseWheelScrolled>()) {
                float nz = clamp(zoom * (ms->delta > 0 ? zoomStep : 1.0f / zoomStep), minZoom, maxZoom);
                Vector2f before = window.mapPixelToCoords(ms->position, worldView);
                worldView.setSize({windowSize / nz, windowSize / nz});
                Vector2f after = window.mapPixelToCoords(ms->position, worldView);
                camCenter = worldView.getCenter() + (before - after);
                zoom = nz;
            }
            else if (const auto* mp = event->getIf<Event::MouseButtonPressed>()) {
                // UIO-F1: minimap first; any press on it is consumed so nothing reaches the world.
                // Left-click recentres; the per-frame clamp below keeps camCenter inside the map bounds.
                Vector2f mui = window.mapPixelToCoords(mp->position, window.getDefaultView());
                if (minimapRect(window).contains(mui)) {
                    if (mp->button == Mouse::Button::Left) camCenter = minimapToWorld(window, mui);
                }
                // UIO-F2: then the inspect panel (consumes the press), then the world pick: barbs only, empty click deselects.
                else if (inspectRect.size.x > 0 && inspectRect.contains(mui)) {}
                else if (lensRect.size.x > 0 && lensRect.contains(mui)) {}   // UIO-F3: legend swallows clicks too
                else if (stockRect.size.x > 0 && stockRect.contains(mui)) {} // RACE-F1: so does the stock panel
                else if (mp->button == Mouse::Button::Left) {
                    if (!hudRect.contains(mui) && !tickerRect.contains(mui))
                        selectedBarbId = pickBarbAt(window.mapPixelToCoords(mp->position, worldView), tooltipPickRadius / zoom, barbs, squares);
                }
                else if (mp->button == Mouse::Button::Middle) { panning = true; lastMouse = mp->position; }
            }
            else if (const auto* mr = event->getIf<Event::MouseButtonReleased>()) {
                if (mr->button == Mouse::Button::Middle) panning = false;
            }
            else if (const auto* mm = event->getIf<Event::MouseMoved>()) {
                if (panning) {
                    camCenter -= Vector2f(mm->position - lastMouse) / zoom;
                    lastMouse = mm->position;
                }
            }

        {
            float half = windowSize / zoom / 2;
            camCenter.x = clamp(camCenter.x, half, worldSize - half);
            camCenter.y = clamp(camCenter.y, half, worldSize - half);
            worldView.setSize({windowSize / zoom, windowSize / zoom});
            worldView.setCenter(camCenter);
        }

        timesincelastupdate += clock.restart();
        if (paused) timesincelastupdate = Time::Zero;
        int ticksThisFrame = 0;
        while (timesincelastupdate > Timepertick) {
            timesincelastupdate -= Timepertick;
            if (ticksThisFrame >= maxTicksPerFrame) { timesincelastupdate = Time::Zero; break; }
            ticksThisFrame++;
            for (int rep = 0; rep < simSpeed; rep++) {
                buildhouse(barbs, squares, houses, houseshapes, fields, stores, trees, rocks, bushshapes);
                buildshrine(barbs, squares, houses, shrines, fields);
                buildsmokehouse(barbs, squares, houses, stores, smoke);
                buildstall(barbs, squares);
                buildforge(barbs, squares, houses, dwarfForge);
                buildstorehouses(barbs, squares, houses, stores, fields);
                buildshafts(barbs, squares, stores);   // R34-F1
                buildcamps(barbs, squares, houses, stores);   // R45-F1b: lean-tos at the ruins
                buildfield(barbs, squares, houses, shrines, stores, fields);
                buildmills(barbs, squares, fields);   // FARM-F2
                buildkeeps(barbs, squares, stores);   // R31-F1: Town era
                buildbarracks(barbs, squares, stores, bushshapes);   // R47-F6: once the keep stands; drill slot upkeep
                buildherbhut(barbs, squares, houses, stores, bushshapes);   // R53-F2: elf herb hut and bed
                wallTick(barbs, trees, tris, stumps, octs, bushshapes, houseshapes, shrines, fields);   // R31-F2: cosmetic curtain walls
                towerTick(barbs, squares);   // R33-F2: gatehouse towers, after the wall completes
                causewayTick();   // R33-F3: keep -> crate stone road
                craftspear(barbs, squares, dwarfForge);
                minematerials(squares, barbs, tris, stumps, trees, octs, rocks, rockGold, bushes, bushshapes, catchRings, fields, stores, dwarfForge);
                barbmove(squares, barbs);
                riteTick(barbs, squares, shrines);   // R52-F3: after the work search, so a barb between jobs is caught idle
                shaftTick(barbs);   // R40-F1: dust puffs and lintel flash
                shinobiTick(barbs, squares, houseshapes, octs, shinobiPuffs);
                treespawn(trees, tris, stumps, fields, houses, barbs);
                barbspawner(squares, barbs, houses, houseshapes, birthRings);
                quarterReachTick(barbs);   // R47-F10: after births, so a new barb can earn a step the same tick
                birthRingTick(birthRings);
                catchRingTick(catchRings);
                deerspawn(deers, deershape);
                deerwander(deershape, deers, squares, barbs);
                deerkilling(deers, deershape, barbs, squares, smoke);
                wolfTick(wolves, deers, deershape, barbs);
                prowlerTick(prowlers, barbs, squares, houses, houseshapes, fire);   // R48-F1: bites land before starveDeathTick()
                watchTick(barbs, squares, prowlers);   // R51-F1: watchmen patrol and fight; slain prowlers fade, prowlerTick() removes them
                raidTick(barbs, squares, stores);   // R52-F1: raid march and fight; sets raidThreat[] for bellTick() just below
                caravanTick(barbs, squares, stores);   // R54-F1: caravans, swaps and the friendly timers, once per sim tick
                bellTick(barbs, squares, houses, houseshapes, wolves, stores, smoke, dwarfForge, fields,
                         trees, tris, rocks, octs, bushes);   // INBOX-F1: after the wolves move
                expeditionTick(barbs, squares, wolves, stores);   // R45-F2: after the wolves and the bell, before hungerTick()
                poiTick(barbs, squares);   // R47-F4: site discovery and spent-site crumbling, after the party's tick
                clockk();
                weatherTick();
                relationTick();   // R51-F4: drift to neutral and clash cooldowns, before the trader prices a visit
                traderTick(activeTrader, barbs, stores);
                graveTick(barbs, squares);   // R47-F9: morale term per race, before hungerTick() reads it
                veilTick();   // R50-F1a: throttled diffusion/decay of the hidden soul-ether
                veilOmenTick();   // R50-F1b: death-site motes and the omen cooldown
                faithShrineTick(shrines);   // R51-F2: shrine motes at a faith-scaled rate (faith itself eases in veilTick)
                hungerTick(barbs);
                sickTick(barbs);   // R52-F4: after meals heal, before starveDeathTick() removes the fallen
                trailTick();
                birdTick(birds, birdHeading, squares, barbs);
                bushTick(bushes, bushshapes);
                herbTick(barbs);   // R53-F2: seasons, growth, harvest-day bloom, ripe cache and winter spoilage
                leanSeasonTick(barbs, bushes, bushshapes);   // R52-F2: first-frost shrivel + cached larder bars
                fieldTick(fields);
                campfireTick(fire);
                eraTick(barbs, houses);
                paveTick(barbs, squares);
                leaderWatchTick(barbs);   // R30-F2: leader death -> global weights + ticker line, same tick
                councilTick(barbs);
                orderTick(barbs);   // INBOX-F2b: after leaderWatchTick, so a dead leader's orders lapse the same tick
                starveDeathTick(barbs, squares, smoke, dwarfForge, stores, fields);   // R30-F5: last, once per tick
                stockShortageTick(barbs);   // RACE-F1: after deaths, so an emptied race never announces
            }
        }

        for (int i = (int)tris.size() - 1; i >= 0; i--) {
            Vector2f p = tris[i].getPosition();
            if (inClearing(p)) {   // village clearing or a race quarter (R30-F1)
                int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);
                for (auto& b : barbs) {
                    if (b.activity != actShelter && b.target != targetBush && b.targetx == tix && b.targety == tiy) {   // INBOX-F1: doorstep walks untouched
                        b.moving = b.chopping = false; clearRoute(b);
                        b.timer  = 0;
                        b.targetx = b.targety = -1;
                    }
                }
                swapRemove(tris, i); swapRemove(stumps, i); swapRemove(trees, i);
            }
        }
        for (int i = (int)octs.size() - 1; i >= 0; i--) {
            Vector2f p = octs[i].getPosition();
            if (inClearing(p)) {   // village clearing or a race quarter (R30-F1)
                int rix = (int)(p.x + 6), riy = (int)(p.y + 10);   // same stand spot minematerials targets
                for (auto& b : barbs) {
                    if (b.activity != actShelter && b.target == targetRock && b.targetx == rix && b.targety == riy) {
                        b.moving = b.chopping = false; clearRoute(b);
                        b.timer  = 0;
                        b.targetx = b.targety = -1;
                        printf("Rock cleared near the village - miner %d dropped its target\n", b.id);
                    }
                }
                swapRemove(octs, i); swapRemove(rocks, i); swapRemove(rockGold, i);
            }
        }

        window.clear(seasonGroundColor(game_day, game_time)); //bg color, R51-F3: subtle per-season ground
        window.setView(worldView);
        FloatRect camRect(worldView.getCenter() - worldView.getSize() / 2.0f, worldView.getSize());   // R47-F1: view culling
        drawZoneTint(window, camRect);   // R47-F1: faint band wash, first on the ground
        drawTrails(window, trailVerts);
        drawWalls(window, wallVerts);    // R31-F2: under paving, quarter marks, buildings and barbs
        drawPaving(window, paveVerts);   // over the trail tint, under everything else
        drawQuarters(window, zoom);   // R30-F1: ground marks, under lakes, houses and crates
        drawLakes(window);
        drawLens(window, lensVerts, worldView, lens, barbs, squares, houses, houseshapes, stores);   // UIO-F3: under rocks, buildings and barbs

	drawRocks(window, octs, rockGold, rockVerts, camRect);   // R46-F1a: gold rocks get one pale-gold fleck pixel
	drawBushes(window, bushshapes, bushVerts, camRect);
	drawGraves(window, camRect);   // R47-F9: ancestor stones, on the ground under the buildings

	{
		RectangleShape slab(houseSlabSize); // foundations under houses, drawn first so every house sits on top
		slab.setFillColor(houseSlabColor);
		for (int i = 0; i < (int)houses.size(); i++) {
			if (!houses[i].hasFoundation) continue;
			Vector2f centre = houseshapes[i].getPosition() + houseshapes[i].getSize() / 2.0f;
			slab.setPosition(centre - houseSlabSize / 2.0f);
			window.draw(slab);
		}
	}
	drawHouses(window, houses, houseshapes);
	drawKeeps(window, barbs);   // R31-F1; R31-F3 refuge windows + banner
	drawGateTowers(window);     // R33-F2: with the keeps, over the wall ends and ground marks, under barbs
	drawShrines(window, shrines);
	drawSmokehouse(window, smoke, stores);
	drawRuins(window);   // R45-F1b: with the buildings, under barbs and trees
	drawHollow(window);  // R47-F5: the stair mouth, beside the stones
	drawCamps(window);
	drawPois(window, camRect, frameCount);   // R47-F4: huts and wells, with the buildings
	drawStall(window, activeTrader);
	drawForge(window, dwarfForge, frameCount, forgeBonusPending(barbs, dwarfForge), forgeSmelting(barbs));   // R47-F7: smoke wisp
	if (forgeEmberFrames > 0 && !paused) forgeEmberFrames--;
	drawStorehouses(window, stores);
	drawMineShafts(window, barbs, squares);   // R34-F1: beside the crates, under barbs
	drawBarracks(window);   // R47-F6: hall + straw dummy, left of the crates, under barbs
	drawHerbHut(window);    // R53-F2: herb hut + bed, above-right of the elf crate, under barbs
	drawGarrison(window, barbs, houses, houseshapes, stores);   // INBOX-F1: bell marker, sheltering roof tint + pips
	drawRaidMarker(window, frameCount);   // R52-F1: pulsing red marker on the raided crate, opposite corner to the bell
	for (auto& st : stores) if (st.flashFrames > 0 && !paused) st.flashFrames--;
	for (int r = 0; r < raceCount; r++)
	    for (auto& fp : fields[r]) drawField(window, fp, r, frameCount);
	drawMills(window);   // FARM-F2: beside each race's plot
	if (!paused) millFrameTick();

	drawCampfire(window, fire, frameCount);

	drawDeer(window, deers, deershape, frameCount);

	for (auto& w : wolves)
		drawSnapped(window, w.shape);
	drawProwlers(window, prowlers);   // R48-F1: bodies under the night overlay; eyes after it
	for (auto& p : prowlers) if (p.flashFrames > 0 && !paused) p.flashFrames--;   // R51-F1: the hit flash lasts 2 drawn frames

        drawStumps(window, stumps, stumpVerts, camRect);   // R45-F1a: ~12k stumps (R47-F1), only those touching the view

        drawOmenAuras(window, shrines);   // R53-F3: faint aura round a shrine while its race's omen holds
        drawRiteRings(window, shrines);   // R52-F3: faint ring round each rite shrine, under the kneelers
        drawBarbs(window, barbs, squares, frameCount, zoom);
        for (auto& b : barbs) if (b.raidFlash > 0 && !paused) b.raidFlash--;   // R52-F1: the hit flash lasts 2 drawn frames
	if (activeTrader) drawSnapped(window, activeTrader->shape);
	drawTrees(window, tris, treeVerts, camRect);
	drawSmoke(window, smoke, paused);   // INBOX2-F1: per-race puffs, only after own smoked kills
	drawMillPuffs(window);   // FARM-F2: above the trees, under the overlay
	drawForgeSparks(window, paused);   // POL-F5b: above barbs and trees so the sparks read, under the overlay
	drawWearSparks(window, paused);    // R49-F1: one grey tool-wear chip per race; also ages the tier-drop dim
	drawBirds(window, birds, birdShape, frameCount);
	if (raining) drawRain(window, rainVerts, frameCount);
	for (int q = 0; q < (int)squares.size(); ++q) {
            auto& b = barbs[q];
            if (b.chopping && b.timer > 0 && b.target == targetNone)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 20), (float)b.timer / chopTicks(b), pal.barChop);
            if (b.chopping && b.timer > 0 && b.target == targetBush)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 14), (float)b.timer / bushPickTicks, pal.barPick);
            if (b.chopping && b.timer > 0 && b.target == targetField)   // centred over the plot, above the bobbing ear
                drawBar(window, Vector2f(b.targetx - fieldStandOffset.x + fieldSize.x / 2 - barSize.x / 2, b.targety - fieldStandOffset.y - 11),
                        (float)b.timer / fieldHarvestTicks, fieldBarColor);
            if (b.building &&  b.timer > 0)
                drawBar(window, Vector2f(b.targetx - 9, b.targety - 10), b.timer / (float)buildJobTicks[b.jobKind], pal.barBuild);
	    if (b.chopping && b.timer > 0 && atShaftTarget(b)) {   // R34-F1: centred over the shaft, clear of its seam bar
                if (shaftMinerOf(barbs, b.race) == q)   // R40-F1: only the shaft's own miner, as the tooltip resolves it
                drawBar(window, shaftPos(b.race) + Vector2f(shaftSize.x / 2 - barSize.x / 2,
                        -shaftSeamGap - shaftSeamH - 2.0f - barFramePx - barSize.y), (float)b.timer / mineTicksFor(b), pal.barMine);
	    } else if (b.chopping && b.timer > 0 && b.target == targetRock)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 16), (float)b.timer / mineTicksFor(b), pal.barMine);   // R41-F2: tempered scale
	}
	drawBirthRings(window, birthRings);
	drawVeilMotes(window, camRect);   // R50-F1b: faint shimmer over recent death sites, under the overlay
	drawCatchRings(window, catchRings);
	drawQuarterPulses(window);   // R47-F10: step dust ring, world layer under the overlay
	drawShinobiPuffs(window, shinobiPuffs);   // world layer, under the day/night overlay
	if (!paused) shinobiPuffTick(shinobiPuffs);

        int t = game_time;
        Color dncColor;
        if (isNightHour(t))             dncColor = Color(4, 7, 32, 150);      // Night
        else if (t < middayStartHour)   dncColor = Color(250, 231, 190, 30);  // Morning
        else if (t < eveningStartHour)  dncColor = Color(233, 255, 253, 30);  // Midday
        else                            dncColor = Color(255, 178, 127, 50);  // Evening
        drawOverlay(window, dnc, dncColor);
        if (raining) drawOverlay(window, dnc, rainOverlayColor);
        if (t >= eveningStartHour && t < nightStartHour) drawRuinGlow(window, worldView, frameCount);   // R45-F1b: dusk only
        drawPoiGlow(window, worldView, camRect, frameCount);   // R47-F4: found sites, any hour
        if (isNightHour(t)) drawCampfireGlow(window, worldView, fire, frameCount);
        if (isNightHour(t)) drawHouseLights(window, worldView, houses, houseshapes);
        drawProwlerOverlay(window, worldView, prowlers, fire);   // R48-F1: red eyes + safe-zone ring, above the night overlay
        drawWatchTorches(window, worldView, frameCount);   // R51-F1: torches glow above the overlay (prowler hp bars are drawn in the call above)
        drawBallots(window, worldView, zoom, barbs, squares); // above the overlays so the pips read true; under hover marks
        if (ballotFramesLeft > 0 && !paused) ballotFramesLeft--;
        if (window.hasFocus() && Keyboard::isKeyPressed(Keyboard::Key::H))   // R44-F2: held, not toggled; under select/hover marks
            drawBeacons(window, worldView, zoom, frameCount, barbs, squares, fields, bushes, bushshapes);
        for (int k = 0; k < techCount; k++)
            if (know.pulseFrames[k] > 0 && !paused) know.pulseFrames[k]--;   // know-how pip flash (R20-F1)

        // UIO-F2: resolve the selection once per frame; a vanished barb (died/removed) clears it
        int selIdx = findBarbById(barbs, selectedBarbId);
        if (selIdx < 0) { selectedBarbId = -1; inspectRect = FloatRect(); }
        if (selIdx >= 0 && selIdx < (int)squares.size() && !barbHidden(barbs[selIdx]))   // R40-F1: a miner below is ringed in its pit
            drawSelectRing(window, worldView, barbInHollow(barbs[selIdx]) ? hollowShape()   // R47-F5: ringed at the mouth
                           : barbUnderground(barbs[selIdx]) ? shaftPitRect(barbs[selIdx].race) : squares[selIdx], zoom);
        if (selIdx >= 0 && selIdx < (int)squares.size() && !barbHidden(barbs[selIdx]) && !barbUnderground(barbs[selIdx]))   // R44-F1
            if (auto tp = barbTargetPos(barbs[selIdx], squares[selIdx], deers, deershape))
                drawTargetLine(window, worldView, squares[selIdx].getPosition() + squares[selIdx].getSize() / 2.0f, *tp, zoom, frameCount);

        optional<hoverHit> hover;
        bool overHud = false;
        {
            Vector2i mpx = Mouse::getPosition(window);
            Vector2u wsz = window.getSize();
            Vector2f mscreen = window.mapPixelToCoords(mpx, window.getDefaultView());
            overHud = hudFontOk && window.hasFocus() && !panning && hudRect.contains(mscreen);
            bool overUi = overHud || tickerRect.contains(mscreen) || minimapRect(window).contains(mscreen)
                       || (inspectRect.size.x > 0 && inspectRect.contains(mscreen))
                       || (lensRect.size.x > 0 && lensRect.contains(mscreen))
                       || (stockRect.size.x > 0 && stockRect.contains(mscreen));
            if (window.hasFocus() && !panning && !overUi && mpx.x >= 0 && mpx.y >= 0 && mpx.x < (int)wsz.x && mpx.y < (int)wsz.y) {
                Vector2f mworld = window.mapPixelToCoords(mpx, worldView);
                hover = pickHover(mworld, tooltipPickRadius / zoom, barbs, squares, activeTrader, wolves, deers, deershape, rocks, octs, bushes, bushshapes, houses, houseshapes, fields, stores);
                if (hover && hover->targetPos && hover->rect && hover->hoveredBarb != selectedBarbId)   // R44-F1: selected one drawn above
                    drawTargetLine(window, worldView, hover->rect->getPosition() + hover->rect->getSize() / 2.0f, *hover->targetPos, zoom, frameCount);
                if (hover && hover->hoveredCrate >= 0) drawQuarterReachRing(window, worldView, hover->hoveredCrate, zoom);   // R47-F10: under the outline
                if (hover) drawHoverOutline(window, worldView, *hover, zoom);
                if (hover) drawHousehold(window, worldView, *hover, zoom, barbs, squares, houses, houseshapes);
            }
            drawOrderPips(window, worldView, zoom, hover ? hover->hoveredBarb : -1, barbs, squares);   // INBOX-F2b: before the HUD
            drawWatchRing(window, worldView, barbs, selectedBarbId, hover ? hover->hoveredBarb : -1, zoom);   // R51-F1: hover/selection only
        }

        if (hudFontOk) {
            window.setView(window.getDefaultView());
            hudValues hv{};
            for (int r = 0; r < raceCount; r++) { hv.stock[r] = stock[r]; hv.members[r] = 0; }   // RACE-F1
            for (auto& b : barbs) hv.members[clampRace(b.race)]++;
            hv.campfireWood = campfireWood;
            for (auto& b : barbs) if (isHungry(b)) hv.hungry++;
            for (auto& b : barbs) (isGentry(b) ? hv.gentry : hv.commoners)++;
            hv.people = (int)barbs.size();
            hv.houses = (int)houses.size();
            hv.hour = t;
            hv.speed = simSpeed;
            hv.night = isNightHour(t);
            hv.paused = paused;
            hv.raining = raining;
            hv.trader = !activeTrader ? "Away"
                : activeTrader->phase == trader::Arriving ? "Arriving"
                : activeTrader->phase == trader::Trading ? "Trading" : "Leaving";
            hv.tradeLine = tradeMsgTimer > 0 ? tradeMsg : "";
            hv.era = eraNames[currentEra];
            hv.gold = gold;              // R46-F1a
            hv.goldFound = goldFound;
            hv.day = game_day;           // R51-F3
            for (int k = 0; k < techCount; k++) { hv.learned[k] = know.learned[k]; hv.pulse[k] = know.pulseFrames[k]; }
            hudRect = drawHud(window, hudFont, hv);
            tickerRect = drawEvents(window, hudFont);
            drawMinimap(window, minimapVerts, worldView, barbs, squares, houses, houseshapes, stores);   // UIO-F1: after the HUD, under tooltips
            inspectRect = selIdx >= 0 ? drawInspectPanel(window, hudFont, barbs[selIdx], minimapRect(window)) : FloatRect();   // UIO-F2
            stockRect = drawStockPanel(window, hudFont, hv, inspectRect, minimapRect(window), tickerRect);   // RACE-F1: top-left, before the legend
            lensRect = drawLensLegend(window, hudFont, lens, inspectRect, minimapRect(window), tickerRect, stockRect);   // UIO-F3: after both, so it can dodge them
            Vector2f mouse = window.mapPixelToCoords(Mouse::getPosition(window), window.getDefaultView());
            if (overHud) // full details hang just under the strip, beside the cursor
                drawTooltip(window, hudFont, hudDetailText(hv),
                            Vector2f(mouse.x, hudRect.position.y + hudRect.size.y + tooltipEdgeMargin - tooltipOffset.y), hudRect, tickerRect);
            else if (hover) drawTooltip(window, hudFont, hover->label, mouse, hudRect, tickerRect, hover->tint.value_or(tooltipTextColor));
        }
        else drawMinimap(window, minimapVerts, worldView, barbs, squares, houses, houseshapes, stores);   // no font: still show the map
        eventTick();
        window.display();
        frameCount++;
    }
    return 0;
}
