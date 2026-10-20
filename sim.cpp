#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include <cstdio>
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
enum councilDecree { decreeTimber, decreeStone, decreeBerries, decreeCount, decreeNone = decreeCount };
const char* const decreeNames[decreeCount] = {"Timber", "Stone", "Berries"};
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
};

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
const Vector2f forgePos(375.0f, 393.0f);   // forge top-left: west of the campfire
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
// Lakes are placed after the quarters and keep quarterClearRadius + lakeMinGap off each one.
const float raceMinSeparation = 260.0f;      // min distance between two races' crates (startup check; R47-F3: was 185)
const bool quarterForceFallback = false;     // R47-F3 test switch: true skips the random layout so the fixed fallback runs
// R47-F3 Frontier Spacing: each race's frontier is the disc of frontierRadius round its crate centre. Crates are at least
// raceMinSeparation apart, so frontiers never touch and the land between them is a gap the trader walks through. The wide
// work look (barbWorkFallbackRadius) skips targets inside another race's frontier. Shown only as a faint dotted circle
// on the minimap; nothing is drawn in the main view.
const float frontierRadius = raceMinSeparation / 2.0f;   // 130 = houseSpreadFar, so homesteads stay inside their frontier
const float frontierMinimapDotGap = 3.0f;    // minimap px between border dots (1px each)
const uint8_t frontierMinimapAlpha = 80;     // faint: under the house/crate/barb dots on the dark backing
const float quarterMinRadius = 210.0f;       // crate centre distance from the campfire (R44-F4b: was 165)
const float quarterMaxRadius = 250.0f;       // (R44-F4b: was 200)
const float traderGapRadius = quarterMaxRadius;   // R47-F3: trader's waypoint on a gap's bisector, this far from the campfire
const int quarterPlaceTries = 50;            // random layouts tried before the fixed fallback layout
const int quarterSweepSteps = 360;           // R47-F2: startup check of quarterLayoutOk's rules at every 1 deg rotation
const float quarterHouseRadius = 56.0f;      // house spot within this of the crate centre (+25% over R30-F1's 45)
const float quarterClearMargin = 7.0f;       // clearing reaches past the house disc: 5px slab half + 2
const float quarterClearRadius = quarterHouseRadius + quarterClearMargin; // trees/rocks cleared within this
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
// Quarter radii (quarterMin/MaxRadius, quarterHouseRadius) are unchanged in this phase.
const float houseSpreadNear = 90.0f;         // homestead distance from the crate centre
const float houseSpreadFar = 130.0f;
const int homesteadEvery = 4;                // every 4th house of a race is tried as a homestead
const float homesteadArc = 1.0471976f;       // +-60 deg around the crate's outward direction
const int homesteadTries = 12;               // spots rolled per claim before falling back to the quarter disc
const float homesteadHouseGap = 30.0f;       // no homestead within this of any house (or pending house spot), any race
const float homesteadClutterPad = 2.0f;      // grass kept between the slab and a tree/rock/bush footprint

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

// Build jobs: which structure a barb's movingbspot/building belongs to. jobNone = no job, or a house (buildhouse()).
// jobSpear is a craft, not a structure: it uses movingbspot (so no other system grabs the barb) but never building/timer.
// jobPave (R20-F2) is run by paveTick(): movingbspot while walking, building/timer while laying the cobbles.
enum buildJob {jobNone, jobShrine, jobSmoke, jobStall, jobForge, jobField, jobSpear, jobPave, jobStore, jobMill, jobKeep, jobShaft, jobCamp, buildJobCount};
const int buildJobTicks[buildJobCount] = {houseBuildTicks, shrineBuildTicks, smokehouseBuildTicks, stallBuildTicks, forgeBuildTicks, fieldBuildTicks, spearCraftTicks, paveTicks, storehouseBuildTicks, millBuildTicks, keepBuildTicks, shaftBuildTicks, campBuildTicks};
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
enum hudIconId { iconWood, iconStone, iconFood, iconPeople, iconHouse, iconSun, iconMoon, iconSpeed, iconPause, iconRain, iconCount };
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
};

mt19937 gen(random_device{}());
uniform_int_distribution<int> distrib(0, (int)worldSize - 1);
uniform_int_distribution<int> wanderChance(0, 149);
uniform_int_distribution<int> wanderDX(-50, 50);
uniform_int_distribution<int> wanderDY(-50, 50);
uniform_int_distribution<int> spawnChance(0, 7500);
uniform_int_distribution<int> treespawnChance(0, treeSpawnOneIn - 1);
uniform_int_distribution<int> quarterSpotD(-(int)quarterHouseRadius, (int)quarterHouseRadius); // R30-F1: house offset per axis, disc-rejected
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
int gold = 0;              // R46-F1a: global nuggets, from gold rocks only
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
        && shaftClear(r);   // R34-F1
}

// R30-F1: race quarters
Vector2f quarterCentre(int r) { return storehousePos[r] + storehouseSize / 2.0f; }
VertexArray quarterRingVerts(PrimitiveType::Triangles);   // dotted rings, built once in placeQuarters()

// True if a tree/rock at top-left p is inside the village clearing or any race quarter's clearing (removed every frame)
bool inClearing(Vector2f p) {
    float dx = p.x - villageCenter.x, dy = p.y - villageCenter.y;
    if (dx * dx + dy * dy < clearRadiusSq) return true;
    for (int r = 0; r < raceCount; r++) {
        Vector2f d = p - quarterCentre(r);
        if (d.x * d.x + d.y * d.y < quarterClearRadius * quarterClearRadius) return true;
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
        if (!layoutDiscOk(L.crate[a] + storehouseSize / 2.0f, quarterClearRadius)) return false;
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
void placeQuarters() {
    quarterFallbackUsed = false;
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
    quarterRingVerts.clear();
    for (int k = 0; k < raceCount; k++) {
        Color col = storehouseColors[k];
        col.a = quarterRingAlpha;
        int dots = (int)round(6.2831853f * quarterClearRadius / quarterRingDotGap);
        for (int i = 0; i < dots; i++) {
            float a = 6.2831853f * i / dots;
            appendDot(quarterRingVerts, quarterCentre(k) + Vector2f(cos(a), sin(a)) * quarterClearRadius, col);
        }
    }
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
    int delay = 0;
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

struct barb {
    int id = -1;                // stable, unique; assigned only by makeBarb()
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
    const char* name = "";       // R25-F1: points into barbNames (no heap strings); set in makeBarb()
    barbTrait trait = traitSwift; // R25-F1: rolled once in makeBarb()
    bool shinobi = false;        // R25-F2: Sasuke; set once in makeBarb()
    carryLoad carryKind = carryNone; // R29-F1: load being hauled to a storehouse/the campfire; dropped with the barb, cleared by dropCarry()
    int carryAmount = 1;         // R34-F1: units in the load (kShaftYield stone from a shaft); back to 1 whenever carryKind clears
    int health = barbMaxHealth;  // R30-F5: 0 = dies at the end of the tick (removeBarb)
    int starveTicks = 0;         // R30-F5: ticks spent hungry since the last meal; starving past starveGraceTicks
    bool garrisoned = false;     // INBOX-F1: exclusive state owned by bellTick(); every other per-tick loop skips the barb
    bool garrisonInside = false; // INBOX-F1: reached the doorstep and hidden inside (not drawn, not pickable)
    int garrisonHouseId = -1;    // INBOX-F1: house.id (stable, never an index); validated every tick
    workMem mem[memSpotCount];   // INBOX-F2: recent work spots, [0] newest; reset in makeBarb(), moves with the struct on swapRemove
    bool memReturn = false;      // INBOX-F2: a trip just ended (deposit or pick); the next search tries mem[] first
    bool memShared = false;      // INBOX-F2: already shared a fresh spot this trip
    leaderNeed order = needNone; // INBOX-F2b: leader's individual order; needNone = no order. Never touches garrison state
    int orderTicks = 0;          // INBOX-F2b: ticks until the order expires; reset in makeBarb()
    barbRoute route;             // R39-F1: moves with the struct on swapRemove; reset only through clearRoute()
    bool inMine = false;         // R40-F1: down its race's shaft (not drawn); set at the stand spot, cleared only by leaveMine()
    bool tempering = false;      // R41-F2: exclusive temper state owned by temperTick(); never set with chopping/building; cleared by clearTemper()
    int temperTimer = 0;         // R41-F2: work ticks at the forge; separate from timer; cleared by clearTemper()
    bool temperPaid = false;     // R41-F2: kTemperStoneCost already taken; refunded by clearTemper() unless the temper finished
    int toolCharges = 0;         // R41-F2: tempered rock mines left; set on a finished temper, reset by clearTemper() and makeBarb()
    bool onExpedition = false;   // R45-F2: the one expedition flag; exclusive state owned by expeditionTick(), set/cleared only there
};
// R39-F1: the one route reset, called wherever moving/movingbspot is cleared and before a barb is swapRemoved
void clearRoute(barb& b) {
    b.route.len = b.route.idx = 0;
    b.route.goalX = b.route.goalY = -1;
    b.route.planned = false;
}
bool barbHidden(const barb& b) { return b.garrisoned && b.garrisonInside; }   // INBOX-F1
// R45-F2: the one eligibility gate every job selector checks: false while bellTick() (garrison) or expeditionTick() owns the barb
bool barbAvailable(const barb& b) { return !b.garrisoned && !b.onExpedition; }

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
bool barbInHollow(const barb& b) { return b.onExpedition && party.active && party.hollow && party.phase == expeditionParty::Delve; }
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
    if (!party.active || !b.onExpedition) return;
    for (auto& id : party.memberIds) if (id == b.id) { id = -1; party.lostName = b.name; }
}
// R45-F2: the relic carrier on the walk home, for drawBarbs(); -1 = nobody
int relicCarrierId() {
    return party.active && party.relic && party.phase == expeditionParty::Back ? party.memberIds[0] : -1;
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
int mineTicks(const barb& b) { return (int)(rockMineTicks / raceOf(b).mineSpeed / (know.learned[techPickwork] ? pickworkMineSpeed : 1.0f)); }

// R29-F1: every way out of a haul other than the deposit itself; the load is lost. Safe on a barb that isn't hauling.
void dropCarry(barb& b) {
    if (b.carryKind != carryNone) printf("LOAD DROPPED - barb %d (%s)\n", b.id, carryName(b.carryKind));
    b.carryKind = carryNone;
    b.carryAmount = 1;   // R34-F1
    if (b.target == targetDepot) b.target = targetNone;
}

// R41-F2: every way out of a temper (finished, interrupted, reassigned, forge gone, sheltering, death) goes through here.
// Refunds the stone if it is still owed and resets toolCharges; leaves movement alone so a new assignment is not undone
// (temperTick() clears movement on its own exits). The finish path clears temperPaid first and sets the charges after.
void clearTemper(barb& b) {
    if (b.temperPaid) {
        stockOf(b).stone += kTemperStoneCost;
        printf("TEMPER REFUNDED - barb %d, %s +%d stone\n", b.id, racePlurals[clampRace(b.race)], kTemperStoneCost);
    }
    b.temperPaid = false;
    b.tempering = false;
    b.temperTimer = 0;
    b.toolCharges = 0;
}

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
        if (barbs[j].race == r && !barbs[j].garrisoned && atShaftTarget(barbs[j])) return j;
    return -1;
}
// R40-F1: down the shaft right now. Separate from barbHidden() (garrison only); re-checks the job so a stale flag never hides a barb
bool barbUnderground(const barb& b) {
    return (b.inMine && !b.garrisoned && atShaftTarget(b) && mineShafts[b.race].built) || barbInHollow(b);   // R47-F5: or down the Hollow
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
bool barbIdle(const barb& b) { return !b.deertargeting && !b.building && !b.tempering && !b.chopping; }

// Priority when flags overlap: hunting > building > picking > harvesting > fishing > mining > chopping > idle
Color barbColor(const barb& b) {
    if (barbIdle(b)) return barbIdleColor;
    if (b.deertargeting) return barbHuntColor;
    if (b.building) return barbBuildColor;
    if (b.tempering) return barbMineColor;   // R41-F2: pick work at the forge; never overlaps chopping/building
    if (b.chopping && b.target == targetBush) return barbPickColor;
    if (b.chopping && b.target == targetField) return barbHarvestColor;
    if (b.chopping && b.target == targetLake) return barbFishColor;
    if (b.chopping && b.target == targetRock) return barbMineColor;
    return barbChopColor;   // only chopping is left once barbIdle() is false
}

// Same priority as barbColor() so the tooltip always matches the colour on screen
const char* barbTaskName(const barb& b) {
    if (b.garrisoned) return b.garrisonInside ? "sheltering" : "running to shelter";   // INBOX-F1
    if (b.onExpedition)   // R45-F2
        return party.phase == expeditionParty::Out ? "expedition: setting out"
             : party.phase == expeditionParty::Delve ? (party.hollow ? "expedition: below the Hollow" : "expedition: delving")   // R47-F5
             : "expedition: heading home";
    if (b.deertargeting) return b.hasSpear ? "hunting (spear)" : "hunting";
    if (b.building) return "building";
    if (b.tempering) return "tempering pick";   // R41-F2
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
			if (!barbAvailable(b) || b.building || b.movingbspot || b.chopping || b.timer > 0 || b.carryKind != carryNone || b.tempering) continue;   // R29-F1: haulers finish the trip; R41-F2: so do temperers; R45-F2
			if (!b.deertargeting) {
				Vector2f bpos = sq.getPosition();
				float dx = deerpos.x - bpos.x, dy = deerpos.y - bpos.y;
				if (dx * dx + dy * dy >= deerInfluenceDSq) continue;
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
				d.health -= speared ? spearHitDamage : animalHitDamage;
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
					printf("%s KILLED%s - %s food: %d (+%d, +%d smoked)\n", name, speared ? " (spear)" : "", racePlurals[clampRace(b.race)], rs.food, yield, smoked);
					logRaceEvent(string(b.name) + (speared ? " speared a " : " killed a ") + name + " (+" + to_string(yield) + " food"
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
    return speed > 1.0f ? max(1, (int)(lim / speed)) : lim;
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
    for (auto& b : barbs) {
        if (!isHungry(b)) {
            b.hunger++;
            if (isHungry(b) && stockOf(b).food <= 0 && hungryLogCooldown == 0) {
                logRaceEvent(string(b.name) + " is hungry - no food (" + racePlurals[clampRace(b.race)] + ")", b.race);
                hungryLogCooldown = eventHungryCooldownTicks;
            }
        }
        if (isHungry(b) && !isGentry(b) && !b.onExpedition) hungryCommoners[clampRace(b.race)]++;   // R45-F2: away members eat rations
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
        if (isGentry(b) && isHungry(b) && !b.onExpedition && stockOf(b).food > gentryKeep) eat(b, gentryKeep);
    }
    for (auto& b : barbs)
        if (!isGentry(b) && isHungry(b) && !b.onExpedition && stockOf(b).food > 0) eat(b, 0);   // R45-F2: rations only, in expeditionTick()
    // Pass 3 (R30-F5): whoever is still hungry after the meal goes on starving; damage only past the grace period.
    // Health is only lowered here; deaths are removed by starveDeathTick() at the end of the tick.
    for (auto& b : barbs) {
        if (!isHungry(b) || b.health <= 0) continue;
        b.starveTicks++;
        int over = b.starveTicks - starveGraceTicks;
        if (over > 0 && over % starveDamageTicks == 0) {
            b.health--;
            if (b.health % 25 == 0) printf("STARVING - %s %d hp\n", b.name, b.health);
        }
    }
}

bool isStarving(const barb& b) { return isHungry(b) && b.starveTicks >= starveGraceTicks; }

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
    static VertexArray lookVerts(PrimitiveType::Triangles);   // R35-F1: every race mark this frame, one draw after the loop
    lookVerts.clear();
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
        Color bodyColor = b.shinobi ? shinobiBodyColor : barbBodyColor;
        body.setSize(Vector2f(bodySize.x, bodyH));
        body.setFillColor(hungry ? hungryTint(bodyColor) : bodyColor);
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
            if (lk.axeGlint && raised && (phase / (barbToolFrames * 2)) % dwarfGlintEvery == 0)   // R35-F1: up-right of the head, clear of the horn
                addLookQuad(Vector2f(side + 1.0f, bodyTop - barbToolSize.y - 1.0f), Vector2f(1.0f, 1.0f), dwarfGlintColor);
        } else if (b.jobKind == jobSpear && b.spearTimer > 0) {   // R20-F3: knapping flint at the fire, hand taps the stone
            bool raised = (phase / spearKnapFrames) % 2 == 0;
            tool.setFillColor(spearTipColor);
            tool.setPosition(Vector2f(side, raised ? bodyTop + 1.0f : bodyTop + 2.0f));
            window.draw(tool);
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
        }
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
        bool pays = food ? stock[r].food >= tradeFoodReserve + tradeFoodCost : stock[r].stone >= tradeStoneCost;
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
            int cost = t.buysFood ? tradeFoodCost : tradeStoneCost;
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

int spearsOfRace(const vector<barb>& barbs, int r);   // POL-F3, defined with craftspear

// POL-F1: ledger string cached on the hover target (race index, raceCount = campfire) and the numbers it shows;
// the string is only rebuilt when either changes
struct ledgerCacheT { int target = -1; vector<int> key; string text; };
ledgerCacheT ledgerCache;

string ledgerLine(int target, const vector<barb>& barbs, const storehouse (&stores)[raceCount]) {
    int hungry[raceCount] = {}, spears[raceCount] = {};
    for (auto& b : barbs) if (isHungry(b)) hungry[clampRace(b.race)]++;
    for (int r = 0; r < raceCount; r++) spears[r] = spearsOfRace(barbs, r);
    vector<int> races;   // storehouse: its race; campfire: every race still hauling there (no crate yet)
    if (target < raceCount) races.push_back(target);
    else for (int r = 0; r < raceCount; r++) if (!stores[r].built) races.push_back(r);
    vector<int> key{target < raceCount ? 0 : campfireWood};
    for (int r : races) key.insert(key.end(), {r, stock[r].wood, stock[r].stone, stock[r].food, hungry[r], spears[r]});
    if (ledgerCache.target == target && ledgerCache.key == key) return ledgerCache.text;
    string s = target < raceCount ? "" : "Campfire: fire wood " + to_string(campfireWood);
    bool full = races.size() == 1;   // several races at the campfire: stock only, so the single line stays short
    for (int r : races) {
        if (!s.empty()) s += "; ";
        s += string(racePlurals[r]) + (full ? ": " : " ") + to_string(stock[r].wood) + "w " + to_string(stock[r].stone) + "s "
           + to_string(stock[r].food) + "f";
        if (full) s += ", " + to_string(hungry[r]) + " hungry, " + to_string(spears[r]) + (spears[r] == 1 ? " spear" : " spears");
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
            if (b.homeHouse >= 0) {   // kin counted here, on hover only
                int kin = 0;
                for (auto& o : barbs) if (o.id != b.id && o.homeHouse == b.homeHouse) kin++;
                label += ", of house " + to_string(b.homeHouse + 1) + ", " + to_string(kin) + " kin";
            }
            if (isLeader(b) && raceNeed[b.race] != needNone) label += string("\nNeeds: ") + leaderNeedNames[raceNeed[b.race]];   // R30-F2
            if (b.order != needNone)   // INBOX-F2b: hours rounded up so a live order never reads 0h
                label += string("\nOrder: ") + leaderNeedNames[b.order] + " (" +
                         to_string((b.orderTicks + ticksPerGameHour - 1) / ticksPerGameHour) + "h left)";
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
        h.tint = lensRaceColors[r];
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
};

// Draws one table icon at pos (top-left, screen px), one quad per filled cell
void drawIcon(RenderWindow& window, hudIconId id, Vector2f pos, float px = hudIconPx) {   // px: screen px per icon cell
    const iconDef& d = hudIcons[id];
    VertexArray va(PrimitiveType::Triangles);
    pos = snapPx(pos);
    for (int y = 0; y < hudIconGrid; y++)
        for (int x = 0; x < hudIconGrid && d.rows[y][x]; x++) {
            char ch = d.rows[y][x];
            if (ch < 'a' || ch > 'd') continue;
            pushQuad(va, pos + Vector2f(x * px, y * px), Vector2f(px, px), d.key[ch - 'a']);
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
    s += "\nStockpiles (wood/stone/food):";   // RACE-F1
    for (int r = 0; r < raceCount; r++)
        s += string("\n  ") + racePlurals[r] + ": " + to_string(v.stock[r].wood) + " / " + to_string(v.stock[r].stone) + " / " + to_string(v.stock[r].food);
    s += "\n  Campfire wood: " + to_string(v.campfireWood);
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
    struct cell { hudIconId icon; string text; Color color; };
    vector<cell> cells = {   // RACE-F1: wood/stone/food moved to the per-race stock panel under the strip
        {iconPeople, to_string(v.people), pal.hudText},
        {iconHouse,  to_string(v.houses), pal.hudText},
        {v.night ? iconMoon : iconSun, hour, pal.hudText},
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
            drawIcon(window, c.icon, {x, iconY});
            x += hudIconSize + hudIconGap;
        }
        Text t(font, c.text, hudFontSize);
        FloatRect tb = t.getLocalBounds();
        t.setFillColor(c.color);
        t.setPosition({round(x - tb.position.x), textY});
        window.draw(t);
        x += max(tb.size.x, rb.size.x) + hudCellGap;
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
    Vector2f size(round(innerW + 2 * stockPad.x), round(raceCount * rowH + (raceCount - 1) * stockRowGap + 2 * stockPad.y));
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
        float y = pos.y + stockPad.y + r * (rowH + stockRowGap);
        float textY = round(y + (rowH - rb.size.y) / 2.0f - rb.position.y);
        bool empty = v.members[r] <= 0;
        float x = pos.x + stockPad.x;
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
    return d.x * d.x + d.y * d.y > quarterClearRadius * quarterClearRadius;
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
        float m = quarterClearRadius + homesteadHouseGap;
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
                uniform_real_distribution<float> arc(-homesteadArc, homesteadArc), spread(houseSpreadNear, houseSpreadFar);
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
                int dx = quarterSpotD(gen), dy = quarterSpotD(gen);
                if (dx * dx + dy * dy > quarterHouseRadius * quarterHouseRadius) continue; // R30-F1: inside the own quarter's disc
                tx = (int)qc.x + dx; ty = (int)qc.y + dy;
                if (!houseSpotOk(barbs, houses, fields, b.race, qc, tx, ty)) continue;     // reroll next tick
                if (triedHomestead)   // logged once, on the fallback claim itself
                    printf("HOMESTEAD - %s: no spot in %d tries, built in the quarter instead\n", raceDefs[b.race].name, homesteadTries);
            } else {
                printf("HOMESTEAD CLAIMED - %s at (%d, %d), %.0fpx from the crate\n", raceDefs[b.race].name, tx, ty,
                       hypot(tx + 2.0f - qc.x, ty + 2.0f - qc.y));
            }
            raceStock& rs = stockOf(b);
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
void drawForge(RenderWindow& window, const forge& fg, int frame, bool bonusPending) {
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
        if (d.x * d.x + d.y * d.y > quarterClearRadius * quarterClearRadius) return false;
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
void refundSpear(barb& b) {
    if (!b.spearPaid) return;
    b.spearPaid = false;
    raceStock& rs = stockOf(b);
    rs.wood += spearRefundWood;
    rs.stone += spearRefundStone;
    printf("SPEAR REFUNDED - barb %d, %s +%d wood +%d stone\n", b.id, racePlurals[clampRace(b.race)], spearRefundWood, spearRefundStone);
    logRaceEvent(string(b.name) + "'s spear refunded", b.race);
}

// POL-F3: spears held or being made by race r (the per-race cap counts both)
int spearsOfRace(const vector<barb>& barbs, int r) {
    int n = 0;
    for (auto& b : barbs)
        if (b.race == r && (b.hasSpear || b.jobKind == jobSpear)) n++;
    return n;
}

// R20-F3: every way out of a spear craft (finished, interrupted, removed) clears the same barb fields.
// POL-F3: refunds via refundSpear() first; the finish path clears spearPaid beforehand so nothing is returned.
void clearSpearCraft(barb& b) {
    refundSpear(b);
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
        else if (barbs[i].spearPaid) refundSpear(barbs[i]);   // reassigned without going through clearSpearCraft
    }
    Vector2f stand = villageCenter + spearStandOffset;
    if (crafter < 0) {
        int members[raceCount] = {}, spears[raceCount] = {};
        for (auto& b : barbs) members[clampRace(b.race)]++;
        for (int r = 0; r < raceCount; r++) spears[r] = spearsOfRace(barbs, r);
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
                spend(stockOfRace(pick), spearWoodCost, spearStoneCost);   // POL-F3: paid up front by the crafter's race
                b.spearPaid = true;
                b.jobKind = jobSpear;
                b.target = targetNone;
                b.spearAtForge = forgeActive(fg, pick);   // POL-F5a
                Vector2f spot = b.spearAtForge ? forgePos + forgeStandOffset : stand;
                b.targetx = (int)spot.x;
                b.targety = (int)spot.y;
                b.moving = b.movingbspot = true;
                b.spearTimer = 0;
                printf("SPEAR CLAIMED - barb %d, %s %d/%d spears (paid %d wood, %d stone)%s\n", b.id, racePlurals[pick],
                       spears[pick] + 1, members[pick] / spearBarbsPerSpear, spearWoodCost, spearStoneCost,
                       b.spearAtForge ? " at the forge" : "");
                logRaceEvent(string(b.name) + " begins a spear", b.race);
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
    b.spearPaid = false;   // POL-F3: cost was taken at claim; finishing consumes it, so clearSpearCraft refunds nothing
    b.hasSpear = true;
    clearSpearCraft(b);
    printf("---SPEAR MADE--- barb %d, %s\n", b.id, racePlurals[clampRace(b.race)]);
    logRaceEvent(string(b.name) + " made a flint spear", b.race);
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

struct birthRing {Vector2f pos; int age = 0;};

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
    b.tempering = b.temperPaid = false;   // R41-F2: no temper in progress, an untempered pick
    b.temperTimer = 0;
    b.toolCharges = 0;
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
        if (!barbAvailable(b) || b.moving || b.chopping || b.building || b.movingbspot || b.deertargeting || b.timer > 0 || b.jobKind != jobNone) continue;
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
        col.a = (uint8_t)(birthRingMaxAlpha * (1.0f - t));
        c.setRadius(rad);
        c.setOrigin({rad, rad});
        c.setPosition(r.pos);
        c.setOutlineColor(col);
        window.draw(c);
    }
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
    float score[decreeCount] = {0.0f, 0.0f, 0.0f};
    score[raceLeans[b.race]] += councilRaceLean;
    const raceStock& rs = stockOf(b);   // RACE-F1: each voter weighs its own race's larder
    score[decreeTimber]  += councilNeedLean * clamp((float)(houseWoodCost - rs.wood) / (float)houseWoodCost, 0.0f, 1.0f);
    score[decreeStone]   += councilNeedLean * clamp((float)(houseStoneCost - rs.stone) / (float)houseStoneCost, 0.0f, 1.0f);
    score[decreeBerries] += (isHungry(b) || rs.food <= 0) ? councilNeedLean : 0.0f;
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
                if (q != r && d.x * d.x + d.y * d.y < quarterClearRadius * quarterClearRadius) ok = false;
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
                if (!b.garrisoned && b.target != targetBush && b.targetx == tix && b.targety == tiy) {
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
                    if (q != r && d.x * d.x + d.y * d.y < quarterClearRadius * quarterClearRadius) ok = false;
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
                    if (!b.garrisoned && b.target != targetBush && b.targetx == tix && b.targety == tiy) {
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

// INBOX-F1: the release half of removeBarb(), shared with bellTick() so a barb taking shelter frees exactly what a dead one does
void releaseBarbWork(barb& b, smokehouseSet& sh, forge& fg, storehouse (&stores)[raceCount], fieldGrid& fields) {
    if (b.deertargeting) clearHunt(b);
    dropCarry(b);
    if (b.reservedwood) stockOf(b).wood += houseWoodCost;   // house claim walked to but never built: back to its race's stock
    stockOf(b).stone += b.reservedstone;                    // house foundation or shrine stone
    b.reservedwood = false;
    b.reservedstone = 0;
    refundSpear(b);                              // POL-F3: paid spear craft goes back to its race before the swapRemove
    clearTemper(b);                              // R41-F2: mid-temper stone refunded, charges reset (death and sheltering)
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
    if (isLeader(b)) loseLeader(b.race);
    printf("BARB DIED - %s the %s (%s, %s), id %d - %d left\n", b.name, traitOf(b).name, raceOf(b).name,
           isGentry(b) ? "Gentry" : "Commoner", b.id, (int)barbs.size() - 1);
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
        printf("STARVED - ");
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
    b.garrisoned = b.garrisonInside = false;
    b.garrisonHouseId = -1;
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    b.target = targetNone;
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
                for (auto& b : barbs) if (b.garrisoned && b.race == r) sheltered++;
                printf("BELL SILENT - %s%s, %d leave shelter\n", racePlurals[r], timedOut ? " (timeout)" : "", sheltered);
                logRaceEvent(string(racePlurals[r]) + " leave shelter, the bell falls silent", r);
            }
        } else if (wolfNear && raceBellCooldown[r] == 0) {
            raceBell[r] = bellQuietTicks;
            raceBellAge[r] = 0;
            printf("BELL RUNG - %s, wolf within %.0fpx of the crate\n", racePlurals[r], bellWolfRadius);
            logRaceEvent(string(racePlurals[r]) + " ring the bell: wolf at the crate!", r);
        }
    }
    for (int r = 0; r < raceCount; r++) {   // R31-F3: every slot must name a live barb of race r garrisoned in this keep
        auto& slots = keepSlots[r];
        for (int s = (int)slots.size() - 1; s >= 0; s--) {
            int j = findBarbById(barbs, slots[s]);
            bool dup = find(slots.begin(), slots.begin() + s, slots[s]) != slots.begin() + s;
            if (dup || j < 0 || !barbs[j].garrisoned || barbs[j].garrisonHouseId != keepGarrisonId || barbs[j].race != r) {
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
        if (b.garrisoned) {
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
        b.garrisoned = true;
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
    return barbAvailable(b) && !b.chopping && !b.building && !b.movingbspot && !b.deertargeting && b.jobKind == jobNone && b.carryKind == carryNone && !b.tempering;
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
    int votes[decreeCount] = {0, 0, 0};
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
    printf("DAWN COUNCIL - Timber %d, Stone %d, Berries %d -> %s (Gentry %d x%d, Commoners %d)\n", votes[decreeTimber], votes[decreeStone],
           votes[decreeBerries], decreeNames[currentDecree], gentry, gentryVoteWeight, (int)barbs.size() - gentry);
    logEvent(string("Council decrees ") + decreeNames[currentDecree] + " (" + to_string(votes[decreeTimber]) + "-" +
             to_string(votes[decreeStone]) + "-" + to_string(votes[decreeBerries]) + ")");
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
        if (idx[k] < 0 || idx[k] >= (int)barbs.size() || !barbs[idx[k]].onExpedition) continue;
        expeditionReset(barbs[idx[k]]);
        barbs[idx[k]].onExpedition = false;
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
            b.onExpedition = true;
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
        bool bad = idx[k] < 0 || idx[k] >= (int)squares.size() || !barbs[idx[k]].onExpedition || barbs[idx[k]].race != r;
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

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, const vector<house>& houses, vector<RectangleShape>& houseshapes, vector<birthRing>& rings) {
    static int deferCooldown = 0;
    if (deferCooldown > 0) deferCooldown--;
    int cap = popCap(houses);
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        if (spawnChance(gen) != 0) continue;
        if ((int)barbs.size() >= cap) continue;
        // RACE-F1: a birth draws on the house race's larder, gated on that race's own head count
        int r = clampRace(houses[i].race);
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
        int paid = min(rs.food, birthFoodCost);
        spend(rs, 0, 0, birthFoodCost);
        Vector2f pos = houseshapes[i].getPosition();
        makeBarb(Vector2i((int)(pos.x + 4), (int)(pos.y + 13)), barbs, squares, rings, houses[i].id, houses[i].race);   // R28-F1: born into the house's clan
        logEvent(string(barbs.back().name) + " the " + traitOf(barbs.back()).name + " is born (pop " + to_string(barbs.size()) + ")" +
                 (paid > 0 ? " -" + to_string(paid) + " food" : ""));
        if (paid > 0) foodFlashFrames = birthFlashFrames;
    }
}

void barbmove(vector<RectangleShape>& squares, vector<barb>& barbs) {
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (!barbAvailable(b) || b.chopping || b.timer > 0 || b.movingbspot || b.building || b.carryKind != carryNone || b.tempering) continue;   // R29-F1: haulTick() walks haulers; INBOX-F1: bellTick() walks garrisoned; R41-F2: temperTick() walks temperers; R45-F2: expeditionTick() walks parties
        Vector2f pos = sq.getPosition();
        if (!b.moving) {
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
        if (!barbAvailable(b) || b.targetx == -1 || b.target == targetBush || b.target == targetLake || b.target == targetField || b.target == targetDepot || b.deertargeting || b.movingbspot || b.building) continue;
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
    b.temperPaid = true;
    b.tempering = true;
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
    b.carryKind = carryNone;
    b.carryAmount = 1;   // R34-F1
    b.target = targetNone;
    b.moving = false; clearRoute(b);
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    memEndTrip(b);   // INBOX-F2
    if (temper) startTemper(b);
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
    for (int j = 0; j < (int)squares.size(); j++) {
        auto& sq = squares[j];
        auto& b  = barbs[j];
        if (!barbAvailable(b)) continue;   // INBOX-F1: no work or tree/rock/bush/shore search while sheltering; R45-F2: or away
        if (b.toolCharges > 0 && !b.tempering && !forgeActive(fg, b.race)) {   // R41-F2: forge gone: the tempered edge goes with it
            printf("PICK UNTEMPERED - barb %d, forge lost\n", b.id);
            clearTemper(b);
        }
        if (b.tempering) {   // R41-F2: any other state taken over the temper refunds it; temperTick() handles the forge going
            if (b.movingbspot || b.building || b.deertargeting || b.chopping || b.jobKind != jobNone || b.carryKind != carryNone) {
                printf("TEMPER INTERRUPTED - barb %d\n", b.id);
                clearTemper(b);
            } else { temperTick(b, sq, fg); continue; }
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
                    } else if (m.kind == memRock) {
                        Vector2f rp = octs[hit].getPosition();
                        b.targetx = (int)(rp.x + 6);
                        b.targety = (int)(rp.y + 10);
                        b.target = targetRock;
                        rockClaim[hit] = j;
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
                } else if (wTree != -1) {
                    Vector2f tp = tris[wTree].getPosition();
                    b.targetx = (int)(tp.x + 8);
                    b.targety = (int)(tp.y + 15);
                    b.moving = true;
                    b.target = targetNone;
                    treeClaim[wTree] = j;
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
                b.chopping = true;
                trees[i].dying = true;
                if (workTick(b)) b.timer++;
                if (b.timer >= chopTicks()) {
                    practice(techHandAxe);
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
			b.chopping = true;
			rocks[i].dying = true;
			if (workTick(b)) b.timer++;
			if (b.timer >= mineTicksFor(b)) {        // dwarves finish rocks faster; R41-F2: a tempered pick faster still
				practice(techPickwork);
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
                float m = quarterClearRadius + lakeMinGap;
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
               || nearQuarter(cp, quarterClearRadius + bushClumpSpread + 2 * bushRadius)   // R30-F1: house space stays free
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
        float m = quarterHouseRadius + houseSlabSize.x;
        if (!lakeClear(FloatRect(quarterCentre(q) - Vector2f(m, m), Vector2f(2 * m, 2 * m))))
            printf("Race quarter %s: WARNING water inside the house area\n", raceDefs[q].name);
        int nt = 0, nr = 0;
        for (auto& t : tris) if (nearby(t.getPosition(), treeSz, q)) nt++;
        for (auto& o : octs) if (nearby(o.getPosition(), rockSz, q)) nr++;
        int addT = 0, addR = 0;
        mt19937 reseed(quarterReseedSeed + q);
        uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(quarterClearRadius + treeFootprint, quarterResourceRadius - treeFootprint);
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
    for (int i = 0; i < (int)bushes.size(); i++) {
        if (bushes[i].ripe) continue;
        bushes[i].regrow += raining ? rainRegrowMult : 1;
        if (bushes[i].regrow < bushRegrowTicks) continue; // >= ripens, so overshoot can't skip it
        bushes[i].ripe = true;
        bushes[i].regrow = 0;
        bushshapes[i].setFillColor(bushRipeColor);
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
                wallTick(barbs, trees, tris, stumps, octs, bushshapes, houseshapes, shrines, fields);   // R31-F2: cosmetic curtain walls
                towerTick(barbs, squares);   // R33-F2: gatehouse towers, after the wall completes
                causewayTick();   // R33-F3: keep -> crate stone road
                craftspear(barbs, squares, dwarfForge);
                minematerials(squares, barbs, tris, stumps, trees, octs, rocks, rockGold, bushes, bushshapes, catchRings, fields, stores, dwarfForge);
                barbmove(squares, barbs);
                shaftTick(barbs);   // R40-F1: dust puffs and lintel flash
                shinobiTick(barbs, squares, houseshapes, octs, shinobiPuffs);
                treespawn(trees, tris, stumps, fields, houses, barbs);
                barbspawner(squares, barbs, houses, houseshapes, birthRings);
                birthRingTick(birthRings);
                catchRingTick(catchRings);
                deerspawn(deers, deershape);
                deerwander(deershape, deers, squares, barbs);
                deerkilling(deers, deershape, barbs, squares, smoke);
                wolfTick(wolves, deers, deershape, barbs);
                bellTick(barbs, squares, houses, houseshapes, wolves, stores, smoke, dwarfForge, fields,
                         trees, tris, rocks, octs, bushes);   // INBOX-F1: after the wolves move
                expeditionTick(barbs, squares, wolves, stores);   // R45-F2: after the wolves and the bell, before hungerTick()
                poiTick(barbs, squares);   // R47-F4: site discovery and spent-site crumbling, after the party's tick
                clockk();
                weatherTick();
                traderTick(activeTrader, barbs, stores);
                hungerTick(barbs);
                trailTick();
                birdTick(birds, birdHeading, squares, barbs);
                bushTick(bushes, bushshapes);
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
                    if (!b.garrisoned && b.target != targetBush && b.targetx == tix && b.targety == tiy) {   // INBOX-F1: doorstep walks untouched
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
                    if (!b.garrisoned && b.target == targetRock && b.targetx == rix && b.targety == riy) {
                        b.moving = b.chopping = false; clearRoute(b);
                        b.timer  = 0;
                        b.targetx = b.targety = -1;
                        printf("Rock cleared near the village - miner %d dropped its target\n", b.id);
                    }
                }
                swapRemove(octs, i); swapRemove(rocks, i); swapRemove(rockGold, i);
            }
        }

        window.clear(Color(4, 20, 14)); //bg color
        window.setView(worldView);
        FloatRect camRect(worldView.getCenter() - worldView.getSize() / 2.0f, worldView.getSize());   // R47-F1: view culling
        drawZoneTint(window, camRect);   // R47-F1: faint band wash, first on the ground
        drawTrails(window, trailVerts);
        drawWalls(window, wallVerts);    // R31-F2: under paving, quarter marks, buildings and barbs
        drawPaving(window, paveVerts);   // over the trail tint, under everything else
        drawQuarters(window, zoom);   // R30-F1: ground marks, under lakes, houses and crates
        drawLakes(window);
        drawLens(window, lensVerts, worldView, lens, barbs, squares, houses, houseshapes, stores);   // UIO-F3: under rocks, buildings and barbs

	{
		RectangleShape fleck({1.0f, 1.0f});   // R46-F1a: one pale-gold pixel on each gold rock
		fleck.setFillColor(goldFleckColor);
		for (int i = 0; i < (int)octs.size(); i++) {
			CircleShape d = rockDrawShape(octs[i]);
			bool isGold = i < (int)rockGold.size() && rockGold[i];
			if (isGold) d.setFillColor(goldRockColor);
			window.draw(d);
			if (isGold) { fleck.setPosition(d.getPosition() + goldFleckOffset); window.draw(fleck); }
		}
	}

	for (auto& bs : bushshapes)
		window.draw(bushDrawShape(bs));

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
	drawForge(window, dwarfForge, frameCount, forgeBonusPending(barbs, dwarfForge));
	if (forgeEmberFrames > 0 && !paused) forgeEmberFrames--;
	drawStorehouses(window, stores);
	drawMineShafts(window, barbs, squares);   // R34-F1: beside the crates, under barbs
	drawGarrison(window, barbs, houses, houseshapes, stores);   // INBOX-F1: bell marker, sheltering roof tint + pips
	for (auto& st : stores) if (st.flashFrames > 0 && !paused) st.flashFrames--;
	for (int r = 0; r < raceCount; r++)
	    for (auto& fp : fields[r]) drawField(window, fp, r, frameCount);
	drawMills(window);   // FARM-F2: beside each race's plot
	if (!paused) millFrameTick();

	drawCampfire(window, fire, frameCount);

	drawDeer(window, deers, deershape, frameCount);

	for (auto& w : wolves)
		drawSnapped(window, w.shape);

        {
            // R45-F1a: thousands of stumps (R47-F1: ~12k on the 2400 map), so only the ones in (or touching) the view are drawn
            FloatRect viewRect(worldView.getCenter() - worldView.getSize() / 2.0f, worldView.getSize());
            for (int i = 0; i < (int)tris.size(); i++)
                if (viewRect.findIntersection(stumps[i].getGlobalBounds())) drawSnapped(window, stumps[i]);
        }

        drawBarbs(window, barbs, squares, frameCount, zoom);
	if (activeTrader) drawSnapped(window, activeTrader->shape);
	drawTrees(window, tris, treeVerts, camRect);
	drawSmoke(window, smoke, paused);   // INBOX2-F1: per-race puffs, only after own smoked kills
	drawMillPuffs(window);   // FARM-F2: above the trees, under the overlay
	drawForgeSparks(window, paused);   // POL-F5b: above barbs and trees so the sparks read, under the overlay
	drawBirds(window, birds, birdShape, frameCount);
	if (raining) drawRain(window, rainVerts, frameCount);
	for (int q = 0; q < (int)squares.size(); ++q) {
            auto& b = barbs[q];
            if (b.chopping && b.timer > 0 && b.target == targetNone)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 20), (float)b.timer / chopTicks(), pal.barChop);
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
	drawCatchRings(window, catchRings);
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
                if (hover) drawHoverOutline(window, worldView, *hover, zoom);
                if (hover) drawHousehold(window, worldView, *hover, zoom, barbs, squares, houses, houseshapes);
            }
            drawOrderPips(window, worldView, zoom, hover ? hover->hoveredBarb : -1, barbs, squares);   // INBOX-F2b: before the HUD
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
