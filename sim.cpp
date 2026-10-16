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

using namespace sf;
using namespace std;

const int numtrees = 2000;
const int numrocks = 100;

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
const float worldSize = 800.0f;
const float minZoom = 1.0f;
const float maxZoom = 4.0f;
const float zoomStep = 1.1f;
const int ticksPerMinute = 3600;            // one real minute at 1x (60 ticks/s)
const int realMinutesPerGameHour = 10;      // the one day-length knob: real minutes (at 1x) per in-game hour (4 real hours per day)
const int ticksPerGameHour = realMinutesPerGameHour * ticksPerMinute; // clockk() advances game_time by one hour after this many ticks
const float clearRadiusSq = 50.0f * 50.0f;  // trees/rocks within 50px of the village centre (400,400) are cleared
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

int wood = 0;
int stone = 0;
int food = 0;
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
const int rainStreakCount = 150;           // sparse over the 800x800 map
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
const Vector2f villageCenter(400.0f, 400.0f);

const int hungerThreshold = 15 * ticksPerMinute; // 15 real min between meals; deer are the only food for now
const int hungerWorkSlowdown = 2;         // hungry barbs with no food work and walk this many times slower
const Color hungryTintColor(140, 100, 60);
const float hungryTintAmount = 0.45f;     // 0 = pure task colour, 1 = pure tint

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

const float trailCellSize = 8.0f;          // world px per trail grid cell
const int trailGridN = 100;                // cells per side (800 / 8)
const int trailDecayIntervalTicks = 120;   // every 2s each cell loses 1 wear, so stray steps fade out
const int rainErodeMult = 2;               // trails decay this many times faster while raining (4 needed a barb per cell every 4s to hold a road through a shower)
const int trailMaxWear = 240;             // cap so abandoned roads fade within ~8 min
const int trailPathThreshold = 48;         // wear for the faint "path" tint (~6 recent crossings)
const int trailRoadThreshold = 144;        // wear for "road": darker tint and the speed bonus
const int roadStepBonus = 1;               // ticks shaved off the step limit on road cells (~10% faster)
// R29-F2 path following: each step scores straight and +/-45 deg candidates as alignment (cos, 1.0 straight)
// + pathBias * min(wear, pathWearClamp) / pathWearClamp. pathBias stays below the alignment weight (1.0), so the
// heading term always dominates: trails can bend a step by at most 45 deg, never away from or around the target.
constexpr float pathBias = 0.4f;              // a full road beside bare ground wins a 45 deg detour (0.707 + 0.4 > 1.0)
const int pathWearClamp = trailRoadThreshold; // wear beyond road tier adds no extra pull
const float pathSkipDistSq = 4.0f;         // within 2px of the target barbs step straight in
static_assert(pathBias < 1.0f, "pathBias must stay below the alignment weight");
const Color trailPathColor(150, 125, 85, 26);
const Color trailRoadColor(150, 125, 85, 52);

// Cobbled roads (R20-F2): from the Hamlet era an idle barb paves road-tier trail cells near the campfire; paved cells never decay
const int paveScanTicks = 600;             // look for a new cell to pave this often (only while nobody is paving)
const int paveStoneCost = 1;               // per cell, paid on completion; houseStoneCost is kept back for the next house
const int paveMaxCells = 24;               // total paved cells ever
const float paveRadius = 48.0f;            // cell centre within this of the campfire (inside the 50px tree clearing)
const int paveTicks = 240;                 // ~4s at 1x of work on the cell (barb.timer, shows the build bar)
const Color paveColorA(78, 84, 94, 140);   // slate cobbles: cool blue-grey, darker than rocks (125) and clear of the brown trails;
const Color paveColorB(66, 72, 82, 140);   // the two tones differ by 12 so the 4px checker stays low-contrast

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

const int maxWolves = 2;
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
const float noSearchRadius = numeric_limits<float>::infinity(); // findNearest with no distance limit
const Vector2f treeWorkOffset(8.0f, 15.0f); // where a barb stands to chop a tree, relative to its shape
const Vector2f rockWorkOffset(6.0f, 10.0f); // where a barb stands to mine a rock, relative to its shape
const float treeNeedBoost = 1.0f;          // tree weight rises up to 1 + this as wood falls from houseWoodCost to 0
const float needWeightMin = 0.5f;          // job weights are clamped to [min, max] so no resource is ever starved
const float needWeightMax = 2.0f;          // 2x weight = a tree up to 2x farther still wins (distance / weight)

const int numBushClumps = 4;
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
const int houseStyleMaxRows = 4;
struct houseStyle {
    int rows;
    float left[houseStyleMaxRows];   // x offset of each roof row, top row first
    float width[houseStyleMaxRows];
    float top;                       // y of the first roof row
    Color roof;
    Vector2f chimneyPos, chimneySize; // 0x0 = no chimney
};
const houseStyle houseStyles[raceCount] = {
    // Human: the original 3-row stepped brick roof
    {houseRoofRows, {houseRoofLeft[0], houseRoofLeft[1], houseRoofLeft[2]}, {houseRoofWidth[0], houseRoofWidth[1], houseRoofWidth[2]},
     houseRoofTop, houseRoofColor, {0.0f, 0.0f}, {0.0f, 0.0f}},
    // Dwarf: squat 2-row flat roof with a 1x2 chimney on the right
    {2, {-1.0f, -2.0f}, {6.0f, 8.0f}, -1.0f, houseRoofDwarfColor, {3.0f, -3.0f}, {1.0f, 2.0f}},
    // Elf: tall 4-row pointed roof
    {4, {1.0f, 0.0f, -1.0f, -2.0f}, {2.0f, 4.0f, 6.0f, 8.0f}, -3.0f, houseRoofElfColor, {0.0f, 0.0f}, {0.0f, 0.0f}},
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

// Smokehouse: at most one; once standing, every barb kill of a deer or rabbit yields extra food
const int smokehouseWoodCost = 20;
const int smokehouseStoneCost = 4;
const int smokehouseFoodBonus = 1;         // extra food per deer/rabbit kill (wolf kills still give none)
const int smokehouseMinHouses = 1;         // the first home comes before the smokehouse
const int smokehouseBuildTicks = 600;      // same work as a house
const Vector2f smokehousePos(384.0f, 406.0f); // hut top-left: fixed spot just south-west of the campfire, inside the clearing
const Vector2f smokehouseSize(6.0f, 5.0f);
const float smokehouseRoofH = 2.0f;        // top 2 rows are roof, bottom 3 are wall
const Vector2f smokehouseDoorPos(1.0f, 3.0f);  // 1x2 door, relative to hut top-left
const Vector2f smokehouseDoorSize(1.0f, 2.0f);
const Vector2f smokehouseChimneyPos(4.0f, -2.0f); // 1x2 chimney poking up from the right of the roof
const Vector2f smokehouseChimneySize(1.0f, 2.0f);
const Vector2f smokehouseStandOffset(-3.0f, 1.0f); // builder's spot relative to the hut: stands at its left wall
const float smokehouseClearPx = 4.0f;      // shrines keep this far from the hut (and its smoke)
const Color smokehouseWallColor(88, 70, 54);   // dark smoked timber: darker than house tan, still clear of the ground
const Color smokehouseRoofColor(58, 44, 36);
const Color smokehouseDoorColor(30, 22, 16);
const Color smokehouseChimneyColor(110, 106, 100);
const Color smokeColor(176, 174, 168);     // pale grey puffs
const uint8_t smokeAlpha = 150;            // starting alpha; each puff fades to 0 as it rises
const int smokeCycleFrames = 180;          // one puff in flight per cycle normally (~3s): sparse
const int smokePuffsIdle = 1;
const int smokePuffsBoost = 3;             // a burst right after a bonus kill so the effect reads on screen
const int smokeBoostTicks = 360;           // sim ticks the burst lasts after a bonus kill
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

// Smokehouse roof takes the colour of the village's majority race (by house builders); tie or no houses = default brown
const Color smokehouseRoofRaceColors[raceCount] = {   // human, dwarf, elf (indexed by barbRace)
    smokehouseRoofColor,
    Color(72, 70, 66),                         // dark stone grey
    Color(66, 82, 62),                         // dark sage
};

// Dwarf forge: at most one, built by an idle dwarf once dwarves have built enough houses
const int forgeWoodCost = 10;
const int forgeStoneCost = 8;              // the forge is where stone goes; houses keep houseStoneCost in reserve
const int forgeMinDwarfHouses = 2;
const int forgeBuildTicks = 600;           // same work as a house
const int houseBuildTicks = 600;
const Vector2f forgePos(375.0f, 393.0f);   // forge top-left: west of the campfire, clear of the smokehouse smoke column
const Vector2f forgeStandOffset(-3.0f, 1.0f); // builder's spot relative to the forge: stands at its left wall
const float forgeClearPx = 3.0f;           // houses, shrines and the smokehouse keep this far from the forge footprint
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

// Race storehouses (R29-F1): one per race at a fixed spot, built by a barb of that race. Chopped wood and mined stone
// are carried to the hauler's own race's storehouse (the campfire while it isn't standing) and only counted on arrival.
const int storehouseWoodCost = 10;
const int storehouseMinRaceHouses = 1;     // a race builds its storehouse once it has a house of its own
const int storehouseBuildTicks = 600;      // same work as a house
const Vector2f storehouseSize(10.0f, 10.0f);
Vector2f storehousePos[raceCount];         // crate top-left, indexed by barbRace; set once at startup by placeQuarters() (R30-F1)
const Vector2f storehouseStandOffset(-4.0f, 3.0f); // builder/hauler hitbox top-left relative to the crate: at its left side
const float storehouseClearPx = 3.0f;      // houses, shrines, the field and blinks keep this far from crate + stand spot
const float storehouseOutlinePx = 1.0f;    // drawn inward, so the crate stays exactly storehouseSize
const Color storehouseColors[raceCount] = {
    Color(142, 106, 66),                   // human: muted timber brown
    Color(116, 112, 102),                  // dwarf: stone grey
    Color(108, 134, 94),                   // elf: sage
};
const Color storehouseOutlineColor(34, 26, 18); // dark rim and lid slat
const int storehouseFlashFrames = 18;      // deposit flash, ~0.3s (frames, frozen while paused)
const float storehouseFlashMix = 0.4f;     // peak blend toward white: subtle
const Vector2f campfireDepotOffset(-10.0f, -2.0f); // fallback drop spot (hitbox top-left) just west of the stone ring

// Race quarters (R30-F1): each race's crate sits in its own quarter around the village, at least raceMinSeparation from
// the others; that race's houses go within quarterHouseRadius of it. Trees/rocks are kept off each quarter like the
// village clearing, lakes and bush clumps avoid it, and each quarter is topped up at startup if it is short of wood/stone.
const float raceMinSeparation = 150.0f;      // min distance between two races' crates (startup check)
const float quarterMinRadius = 130.0f;       // crate centre distance from the campfire
const float quarterMaxRadius = 160.0f;
const int quarterPlaceTries = 50;            // random layouts tried before the fixed fallback layout
const float quarterHouseRadius = 45.0f;      // house spot within this of the crate centre
const float quarterClearRadius = 52.0f;      // trees/rocks cleared within this (house spot 45 + 5px slab half + 2)
const float quarterResourceRadius = 140.0f;  // startup resource check: trees/rocks between the clearing and this radius
const int quarterMinTrees = 25;              // fewer than this nearby -> plant more (expected ~140 at 2000 trees)
const int quarterMinRocks = 3;               // fewer than this nearby -> place more (expected ~7 at 100 rocks)
const int quarterTopUpTries = 400;           // placement attempts per quarter for the top-up
const unsigned quarterReseedSeed = 3001;     // fixed seed (+ race) so the top-up is deterministic
const float quarterRingMinZoom = 2.0f;       // ring hidden below this zoom
const float quarterRingDotGap = 6.0f;        // world px between ring dots (1px each)
const uint8_t quarterRingAlpha = 30;         // very faint, in the race's crate colour
const Color quarterRoadColor(150, 125, 85, 56); // 1px crate -> nearest house trail: trail brown, dim

// Lakes (R19-F2): static ponds placed once at startup. Static rect + rim only in v1 (no glint/ripples).
// Only wander paths (barbs, deer) avoid lakes; task paths (chop/mine/build/hunt/flee/wolves) are straight lines and may cross
// one. Nothing is ever placed in a lake, so those crossings are brief, and an animal that ends up inside can always wander out.
const int lakeCountMin = 2;
const int lakeCountMax = 3;
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

// Single-plot farming (R19-F3): one 8x8 wheat plot near the campfire, sown once by an idle barb, then regrows forever.
// Ripe is derived from growth (no stored flag); no scarecrow. The harvest shares harvestTick() with berry picking.
const Vector2f fieldSize(8.0f, 8.0f);
const float fieldBorderPx = 1.0f;          // gold frame drawn outside the soil (10x10 overall)
const int fieldWoodCost = 6;               // cheap: fences and a hoe
const int fieldMinHouses = 1;              // the first home comes before the field
const int fieldBuildTicks = 600;           // same work as a house
const int fieldGrowTicks = 10800;          // ~3 min from sowing to ripe at 1x (3 growing stages of ~1 min each)
const int fieldGrowStages = 3;
const int fieldHarvestTicks = 240;         // ~4s to reap
const int fieldFoodYield = 2;
const float fieldSearchRadius = 60.0f;     // like angling: one plot, so idle barbs look a bit farther for it
const float fieldMinRadius = 20.0f;        // plot centre ring around the campfire, inside the 50px tree clearing
const float fieldMaxRadius = 36.0f;
const int fieldPlaceTries = 20;            // per tick until a spot is found
const float fieldClearPx = 3.0f;           // houses, shrines and trees keep this far from the plot and its stand spot
const Vector2f fieldStandOffset(3.0f, 10.0f); // builder/harvester hitbox top-left relative to the soil: just below the frame
const float fieldZoneBelowPx = 6.0f;       // reserved below the stand spot's top: the tallest barb body (5) + 1
const Color fieldSoilColor(110, 80, 45);
const Color fieldBorderColor(178, 148, 62);   // muted straw gold: darker than the ripe heads and the hover outline
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
const int spearWoodCost = 2;                  // paid on completion, not on claim
const int spearStoneCost = 1;
const int spearCraftTicks = 300;              // ~5s at 1x; counted in barb.spearTimer, never barb.timer
const int spearBarbsPerSpear = 3;             // at most 1 spear (held or being made) per this many barbs
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
enum buildJob {jobNone, jobShrine, jobSmoke, jobStall, jobForge, jobField, jobSpear, jobPave, jobStore, buildJobCount};
const int buildJobTicks[buildJobCount] = {houseBuildTicks, shrineBuildTicks, smokehouseBuildTicks, stallBuildTicks, forgeBuildTicks, fieldBuildTicks, spearCraftTicks, paveTicks, storehouseBuildTicks};
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
    // Deer: 6x3, 2x2 head overhangs the front-top corner, tiny swept-back tine above the back of the head
    {"Deer", {6.0f, 3.0f}, 100, 2, 10, 101, 0, deerStepInterval, deerFleeStepInterval,
     gaitWalk, 6, 4, {0.0f, 1.0f, 4.0f, 5.0f}, 2.0f, 1.0f, 0.0f,
     {5.0f, -1.0f}, {2.0f, 2.0f}, 2, {{5.0f, -2.0f}, {4.0f, -3.0f}}, 0, {0.0f, 0.0f},
     Color(192, 133, 82), Color(176, 120, 74), Color(112, 78, 48), Color(178, 162, 132), Color(0, 0, 0)},
    // Rabbit: 4x2 pale sandy body (much lighter than deer so it pops on dark green), 1x2 ear, white scut.
    // Fragile (3 hits) and only 1 food, but breeds fast up to a larger cap.
    {"Rabbit", {4.0f, 2.0f}, 15, 1, 12, 1800, 1200, 2, 1,
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
uniform_int_distribution<int> treespawnChance(0, 99999);
uniform_int_distribution<int> quarterSpotD(-(int)quarterHouseRadius, (int)quarterHouseRadius); // R30-F1: house offset per axis, disc-rejected
uniform_int_distribution<int> DeerSpawnY(20, 780); 
uniform_int_distribution<int> DeerSpawnRL(0, 1);
uniform_int_distribution<int> DeerSpawnXgotoL(20, 100);
uniform_int_distribution<int> DeerSpawnXgotoR(700, 780);

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
void drawTrees(RenderWindow& window, const vector<CircleShape>& tris, VertexArray& va) {
    va.clear();
    for (auto& tri : tris) {
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
struct smokehouse {bool built = false; bool claimed = false; int boostTicks = 0;}; // the one smokehouse; claimed = a builder is on it
bool hasStall = false;      // market stall standing: traderTick() allows stallExtraLots more lots
bool stallClaimed = false;  // a builder is on the stall
int stallBonusLots = 0;     // extra lots the stall added to the current visit; drawn as crates while the trader is Trading
struct forge {bool built = false; bool claimed = false;}; // the one dwarf forge; claimed = a builder is on it
int forgeEmberFrames = 0;   // >0 = ember glows hot; set when a dwarf mines a rock, counted down per drawn frame
// R29-F1: one per race (index = barbRace), held in a fixed array in main; built is the validity flag haulers check
struct storehouse {bool built = false; bool claimed = false; int stored = 0; int flashFrames = 0;}; // flashFrames: deposit flash, per drawn frame

// False if r comes within storehouseClearPx of any race's crate or its stand spot (reserved from the start, built or not)
bool storehouseClear(const FloatRect& r) {
    for (int k = 0; k < raceCount; k++) {
        Vector2f lo = storehousePos[k] + Vector2f(storehouseStandOffset.x - storehouseClearPx, -storehouseClearPx);
        FloatRect zone(lo, storehouseSize + Vector2f(-storehouseStandOffset.x + 2 * storehouseClearPx, 2 * storehouseClearPx));
        if (r.findIntersection(zone).has_value()) return false;
    }
    return true;
}

// False if r comes within forgeClearPx of the forge footprint (reserved from the start, built or not).
// R29-F1: also keeps the storehouse spots clear, so every placement that avoids the forge avoids them too.
bool forgeClear(const FloatRect& r) {
    FloatRect zone(forgePos - Vector2f(forgeClearPx, forgeClearPx), forgeParts[0].size + Vector2f(2 * forgeClearPx, 2 * forgeClearPx));
    return !r.findIntersection(zone).has_value() && storehouseClear(r);
}

// R30-F1: race quarters
Vector2f quarterCentre(int r) { return storehousePos[r] + storehouseSize / 2.0f; }
VertexArray quarterRingVerts(PrimitiveType::Triangles);   // dotted rings, built once in placeQuarters()
VertexArray quarterRoads[raceCount];                      // crate -> nearest own house, rebuilt only when a house is built

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
bool quarterLayoutOk() {
    for (int a = 0; a < raceCount; a++) {
        Vector2f c = quarterCentre(a);
        if (c.x - quarterClearRadius < 0.0f || c.x + quarterClearRadius > worldSize
            || c.y - quarterClearRadius < hudBarHeight || c.y + quarterClearRadius > worldSize) return false;
        if (hypot(c.x - villageCenter.x, c.y - villageCenter.y) < sqrt(clearRadiusSq) + quarterClearRadius) return false;
        for (int b = a + 1; b < raceCount; b++) {
            Vector2f d = storehousePos[a] - storehousePos[b];
            if (hypot(d.x, d.y) < raceMinSeparation) return false;
        }
    }
    return true;
}

// Places the crates once, before anything else is spawned: a random rotation/radius of an even 3-way split, checked by
// quarterLayoutOk(); if no try passes, the fixed layout (human north, others 120 deg apart) is used. Builds the rings.
void placeQuarters() {
    uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(quarterMinRadius, quarterMaxRadius);
    auto layout = [](float a0, float r) {
        for (int k = 0; k < raceCount; k++) {
            float a = a0 + 6.2831853f * k / raceCount;
            Vector2f c(villageCenter.x + cos(a) * r, villageCenter.y + sin(a) * r);
            storehousePos[k] = snapPx(c - storehouseSize / 2.0f);
        }
    };
    bool ok = false;
    for (int t = 0; t < quarterPlaceTries && !ok; t++) {
        layout(ang(gen), rad(gen));
        ok = quarterLayoutOk();
    }
    if (!ok) {
        layout(-1.5707963f, (quarterMinRadius + quarterMaxRadius) / 2.0f);
        printf("Race quarters: random layout failed the check, using the fixed layout (%s)\n", quarterLayoutOk() ? "ok" : "STILL FAILS");
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

// The one wheat plot (R19-F3). rect/shape are set once the spot is chosen; growth counts up to fieldGrowTicks once sown.
struct plot {
    bool placed = false;    // spot chosen and reserved (houses/shrines/trees avoid it from then on)
    bool claimed = false;   // a barb holds jobField (sowing)
    bool built = false;     // sown: wheat grows
    FloatRect rect;         // soil, 8x8
    RectangleShape shape;   // soil fill + gold outline frame
    int growth = 0;
};

bool fieldRipe(const plot& fp) { return fp.built && fp.growth >= wheatGrowTicks(); }

// 0..2 growing stages, fieldGrowStages = ripe
int fieldStage(const plot& fp) { return fieldRipe(fp) ? fieldGrowStages : fp.growth * fieldGrowStages / wheatGrowTicks(); }

Vector2f fieldWorkSpot(const plot& fp) { return fp.rect.position + fieldStandOffset; }

// Soil + frame + the stand spot below it, padded by fieldClearPx
FloatRect fieldZone(Vector2f pos) {
    float pad = fieldBorderPx + fieldClearPx;
    return FloatRect(pos - Vector2f(pad, pad),
                     Vector2f(fieldSize.x + 2 * pad, fieldStandOffset.y + fieldZoneBelowPx + fieldClearPx + pad));
}

// False if r touches the reserved plot zone (always true before a spot is chosen)
bool fieldClear(const plot& fp, const FloatRect& r) {
    return !fp.placed || !r.findIntersection(fieldZone(fp.rect.position)).has_value();
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

// Work target of a barb. targetNone with a set targetx is a tree (or a hunt/wander/build spot, which own their flags).
enum targetKind {targetNone, targetBush, targetRock, targetLake, targetField, targetDepot}; // targetDepot: hauling a load (R29-F1)
enum carryLoad {carryNone, carryWood, carryStone};   // R29-F1: what a hauler holds; set only while target == targetDepot

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
    const char* name = "";       // R25-F1: points into barbNames (no heap strings); set in makeBarb()
    barbTrait trait = traitSwift; // R25-F1: rolled once in makeBarb()
    bool shinobi = false;        // R25-F2: Sasuke; set once in makeBarb()
    carryLoad carryKind = carryNone; // R29-F1: load being hauled to a storehouse/the campfire; dropped with the barb, cleared by dropCarry()
};

const raceDef& raceOf(const barb& b) { return raceDefs[b.race]; }
const traitDef& traitOf(const barb& b) { return traitDefs[b.trait]; }
int forageBonus(const barb& b) { return b.trait == traitForager ? foragerFoodBonus : 0; }
int hungerThresholdOf(const barb& b) {
    float s = b.trait == traitSturdy ? sturdyHungerScale : b.trait == traitGlutton ? gluttonHungerScale : 1.0f;
    return (int)(hungerThreshold * s);
}
bool isGentry(const barb& b) { return b.homeHouse < 0; }   // R24-F1: founders; derived, no stored flag

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
    if (b.carryKind != carryNone) printf("LOAD DROPPED - barb %d (%s)\n", b.id, b.carryKind == carryWood ? "wood" : "stone");
    b.carryKind = carryNone;
    if (b.target == targetDepot) b.target = targetNone;
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
};

// Wolves own their shape (like the trader), so there is no parallel vector to keep in sync
struct wolf {
    enum Phase {Hunting, Leaving} phase = Hunting;
    Vector2f exit;
    int stepTimer = 0;
    RectangleShape shape;
};

char tradeMsg[64] = "";
int tradeMsgTimer = 0;

struct gameEvent {string text; int age = 0; Color tint = Color::Transparent;};   // tint.a == 0: normal ticker colour
deque<gameEvent> events;
int foodFlashFrames = 0;   // >0: the newest ticker line (a paid birth) is tinted birthFlashColor

void logEvent(const string& text, Color tint = Color::Transparent) {
    foodFlashFrames = 0;   // any newer line ends the flash, so it never lands on the wrong line
    events.push_back({text, 0, tint});
    while ((int)events.size() > eventMaxLines) events.pop_front();
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
    if (e.age <= eventHoldTicks) return full;
    float f = 1.0f - (float)(e.age - eventHoldTicks) / (eventFadeTicks - eventHoldTicks);
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
        Text t(font, e.text, eventFontSize);
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
        Text t(font, events[i].text, eventFontSize);
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

// Priority when flags overlap: hunting > building > picking > harvesting > fishing > mining > chopping > idle
Color barbColor(const barb& b) {
    if (b.deertargeting) return barbHuntColor;
    if (b.building) return barbBuildColor;
    if (b.chopping && b.target == targetBush) return barbPickColor;
    if (b.chopping && b.target == targetField) return barbHarvestColor;
    if (b.chopping && b.target == targetLake) return barbFishColor;
    if (b.chopping && b.target == targetRock) return barbMineColor;
    if (b.chopping) return barbChopColor;
    return barbIdleColor;
}

// Same priority as barbColor() so the tooltip always matches the colour on screen
const char* barbTaskName(const barb& b) {
    if (b.deertargeting) return b.hasSpear ? "hunting (spear)" : "hunting";
    if (b.building) return "building";
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
	b.moving = false;
	b.timer = 0;
	b.targetx = b.targety = -1;
	dropCarry(b);   // R29-F1
}

// Any barb whose target deer no longer exists stops hunting. Safe to call after any deer removal.
void validateDeerTargets(vector<barb>& barbs, const vector<deer>& deers) {
	for (auto& b : barbs)
		if (b.deertargeting && findDeerById(deers, b.targetDeerId) == -1) clearHunt(b);
}

void deerkilling(vector<deer>& deers, vector<RectangleShape>& deershape, vector<barb>& barbs, vector<RectangleShape>& squares, smokehouse& sh) {
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
			if (b.building || b.movingbspot || b.chopping || b.timer > 0 || b.carryKind != carryNone) continue;   // R29-F1: haulers finish the trip
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
					int smoked = sh.built ? smokehouseFoodBonus : 0;
					food += yield + smoked;
					if (smoked > 0) sh.boostTicks = smokeBoostTicks;   // puff burst over the hut
					printf("%s KILLED%s - Food: %d (+%d, +%d smoked)\n", name, speared ? " (spear)" : "", food, yield, smoked);
					logEvent(string(name) + (speared ? " speared (+" : " killed (+") + to_string(yield) + " food"
					         + (smoked > 0 ? ", +" + to_string(smoked) + " smoked" : string()) + ", " + to_string(food) + ")");
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
		wolf w;
		float along = (float)clamp(distrib(gen), 20, 780);
		switch (uniform_int_distribution<int>(0, 3)(gen)) {
			case 0:  w.exit = Vector2f(-10.0f, along); break;
			case 1:  w.exit = Vector2f(worldSize + 10.0f, along); break;
			case 2:  w.exit = Vector2f(along, -10.0f); break;
			default: w.exit = Vector2f(along, worldSize + 10.0f); break;
		}
		w.shape.setSize(wolfSize);
		w.shape.setFillColor(wolfColor);
		w.shape.setPosition(w.exit);
		wolves.push_back(w);
		printf("Wolf arriving\n");
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
				d.x = -20;
				d.targetx = DeerSpawnXgotoL(gen);
			} else {
				d.x = 820;
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

void deerwander(vector<RectangleShape>& deershape, vector<deer>& deers, const vector<RectangleShape>& squares) {
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
            for (const auto& sq : squares) {
                Vector2f diff = pos - sq.getPosition();
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
            if (tx > 0 && tx < 800 && ty > 0 && ty < 800 && lakeClear(FloatRect(Vector2f(tx, ty), ds.getSize() + Vector2f(0.0f, speciesOf(d).legPx)), &pos)) {
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
bool trailDirty = false;
vector<uint8_t> trailPaved(trailGridN * trailGridN, 0);   // R20-F2: 1 = cobbled; same grid as trailWear, skipped by trailTick decay
int pavedCount = 0;
int paveClaim = -1;      // the one cell being paved, -1 = none. Released by paveTick() whenever no barb holds jobPave
                         // (paver removed from barbs) or the paver lost movingbspot (taken off the job), and on finish/abandon.
bool paveDirty = false;

// One reset path for the whole trail grid (wear, tiers, paving); called on (re)start in main
void resetTrails() {
    trailWear.assign(trailGridN * trailGridN, 0);
    trailTier.assign(trailGridN * trailGridN, 0);
    trailPaved.assign(trailGridN * trailGridN, 0);
    pavedCount = 0;
    paveClaim = -1;
    trailDirty = paveDirty = true;
}

int trailCell(Vector2f p) {
    int cx = (int)(p.x / trailCellSize), cy = (int)(p.y / trailCellSize);
    if (p.x < 0 || p.y < 0 || cx >= trailGridN || cy >= trailGridN) return -1;
    return cy * trailGridN + cx;
}

void trailSetTier(int c) {
    uint8_t t = trailWear[c] >= trailRoadThreshold ? 2 : trailWear[c] >= trailPathThreshold ? 1 : 0;
    if (t != trailTier[c]) { trailTier[c] = t; trailDirty = true; }
}

void trailStep(Vector2f p) {
    int c = trailCell(p);
    if (c < 0 || trailWear[c] >= trailMaxWear) return;
    trailWear[c]++;
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

bool onRoad(Vector2f p) {
    int c = trailCell(p);
    return c >= 0 && trailTier[c] == 2;
}

int stepLimit(const barb& b, Vector2f pos) { return max(1, stepLimit(b) - (onRoad(pos) ? roadStepBonus : 0)); }

// R29-F2: one 1px step from pos toward pos + diff, bent up to 45 deg onto worn trail cells. Grid lookups only.
// Every diagonal candidate must strictly shorten the distance to the target and stay on the map, so the
// straight step (always valid) is the fallback and barbs can't orbit. Within 2px it is always straight.
Vector2f pathStep(Vector2f pos, Vector2f diff) {
    float dSq = diff.x * diff.x + diff.y * diff.y;
    Vector2f u = diff / sqrt(dSq);
    if (dSq < pathSkipDistSq) return u;
    const float c45 = 0.70710678f;
    Vector2f cands[3] = { u, Vector2f(u.x * c45 - u.y * c45, u.x * c45 + u.y * c45),
                             Vector2f(u.x * c45 + u.y * c45, -u.x * c45 + u.y * c45) };
    const float align[3] = { 1.0f, c45, c45 };
    Vector2f best = u;
    float bestScore = -1.0f;
    for (int k = 0; k < 3; k++) {
        Vector2f s = cands[k];
        if (k > 0 && 2.0f * (s.x * diff.x + s.y * diff.y) <= 1.0f) continue;   // |diff - s| must be < |diff|
        int c = trailCell(pos + s);                                              // -1 = off the grid
        if (k > 0 && c < 0) continue;
        float wear = c >= 0 ? (float)min(trailWear[c], pathWearClamp) / (float)pathWearClamp : 0.0f;
        float score = align[k] + pathBias * wear;
        if (score > bestScore) { bestScore = score; best = s; }
    }
    return best;
}

// Rebuilds the tinted cell quads only when some cell changed tier
void drawTrails(RenderWindow& window, VertexArray& va) {
    if (trailDirty) {
        va.clear();
        for (int c = 0; c < (int)trailTier.size(); c++) {
            if (trailTier[c] == 0) continue;
            Color col = trailTier[c] == 2 ? trailRoadColor : trailPathColor;
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

void birdTick(vector<bird>& birds, Vector2f& heading, const vector<RectangleShape>& squares) {
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
        for (auto& sq : squares) {
            Vector2f d = b.pos - sq.getPosition();
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
    int hungryCommoners = 0;
    for (auto& b : barbs) {
        if (!isHungry(b)) {
            b.hunger++;
            if (isHungry(b) && food <= 0 && hungryLogCooldown == 0) {
                logEvent("A barb is hungry - no food");
                hungryLogCooldown = eventHungryCooldownTicks;
            }
        }
        if (isHungry(b) && !isGentry(b)) hungryCommoners++;
    }
    // Pass 2 (R24-F1): eating, Gentry first. While a Commoner is hungry, Gentry leave gentryFoodReserve
    // so the Commoners are never shut out of the larder entirely.
    // R25-F1: a Glutton takes gluttonMealFood, but never digs into `keep` (callers guarantee food > keep, so bite >= 1)
    auto eat = [](barb& b, int keep) {
        int bite = min(b.trait == traitGlutton ? gluttonMealFood : 1, food - keep);
        food -= bite;
        b.hunger = 0;
        b.slowTick = 0;
        printf("BARB ATE (%s, %s %d) - Food: %d\n", isGentry(b) ? "Gentry" : "Commoner", b.name, bite, food);
    };
    int gentryKeep = hungryCommoners > 0 ? gentryFoodReserve : 0;
    for (auto& b : barbs)
        if (isGentry(b) && isHungry(b) && food > gentryKeep) eat(b, gentryKeep);
    for (auto& b : barbs)
        if (!isGentry(b) && isHungry(b) && food > 0) eat(b, 0);
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
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        const barb& b = barbs[i];
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
        } else if (b.jobKind == jobSpear && b.spearTimer > 0) {   // R20-F3: knapping flint at the fire, hand taps the stone
            bool raised = (phase / spearKnapFrames) % 2 == 0;
            tool.setFillColor(spearTipColor);
            tool.setPosition(Vector2f(side, raised ? bodyTop + 1.0f : bodyTop + 2.0f));
            window.draw(tool);
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
    }
}

// Roofed houses: slab is drawn separately underneath. Window is dark glass here; drawHouseLights lights it at night.
void drawHouses(RenderWindow& window, const vector<house>& houses, const vector<RectangleShape>& houseshapes) {
    RectangleShape r;
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        auto& hs = houseshapes[i];
        const houseStyle& st = houseStyles[houses[i].race];
        Vector2f o = snapPx(hs.getPosition());
        if (st.chimneySize.x > 0.0f) {                  // behind the roof so its base tucks in
            r.setSize(st.chimneySize);
            r.setPosition(o + st.chimneyPos);
            r.setFillColor(houseChimneyDwarfColor);
            window.draw(r);
        }
        r.setFillColor(st.roof);
        for (int k = 0; k < st.rows; k++) {
            r.setSize(Vector2f(st.width[k], 1.0f));
            r.setPosition(o + Vector2f(st.left[k], st.top + k));
            window.draw(r);
        }
        r.setSize(houseWallSize);
        r.setPosition(o + houseWallPos);
        r.setFillColor(houseWallColor);
        window.draw(r);
        r.setSize(houseDoorSize);
        r.setPosition(o + houseDoorPos);
        r.setFillColor(houseDoorColor);
        window.draw(r);
        r.setSize(houseWindowSize);
        r.setPosition(o + houseWindowPos);
        r.setFillColor(houseWindowDayColor);
        window.draw(r);
    }
}

// Lit windows, drawn after the night overlay (which leaves the default view set) so they read as lamplight
void drawHouseLights(RenderWindow& window, const View& worldView, const vector<RectangleShape>& houseshapes) {
    window.setView(worldView);
    RectangleShape glow(houseWindowSize + Vector2f(2 * houseWindowGlowPx, 2 * houseWindowGlowPx));
    glow.setFillColor(houseWindowGlowColor);
    RectangleShape lamp(houseWindowSize);
    lamp.setFillColor(houseWindowNightColor);
    for (auto& hs : houseshapes) {
        Vector2f w = snapPx(hs.getPosition()) + houseWindowPos;
        glow.setPosition(w - Vector2f(houseWindowGlowPx, houseWindowGlowPx));
        window.draw(glow, BlendAdd);
        lamp.setPosition(w);
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

void traderTick(optional<trader>& tr) {
    static int ttimer = 0;
    if (tradeMsgTimer > 0) tradeMsgTimer--;
    if (!tr) {
        if (++ttimer < traderIntervalTicks) return;
        ttimer = 0;
        trader t;
        float along = (float)clamp(distrib(gen), 20, 780);
        switch (uniform_int_distribution<int>(0, 3)(gen)) {
            case 0:  t.exit = Vector2f(-10.0f, along); break;
            case 1:  t.exit = Vector2f(worldSize + 10.0f, along); break;
            case 2:  t.exit = Vector2f(along, -10.0f); break;
            default: t.exit = Vector2f(along, worldSize + 10.0f); break;
        }
        t.shape.setSize(traderSize);
        t.shape.setFillColor(traderColor);
        t.shape.setPosition(t.exit);
        tr = t;
        printf("Trader arriving\n");
        logEvent("Trader arriving");
        return;
    }
    trader& t = *tr;
    if (t.phase == trader::Trading) {
        if (++t.timer >= traderTradeTicks) {
            t.phase = trader::Leaving;
            printf("Trader leaving\n");
            logEvent("Trader leaving");
        }
        return;
    }
    Vector2f goal = t.phase == trader::Arriving ? villageCenter : t.exit;
    Vector2f diff = goal - t.shape.getPosition();
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq < 1.0f) {
        if (t.phase == trader::Leaving) {
            tr.reset();
            return;
        }
        int lots = min(stone / tradeStoneCost, tradeMaxLots + (hasStall ? stallExtraLots : 0));
        stallBonusLots = max(0, lots - tradeMaxLots);
        stone -= lots * tradeStoneCost;
        wood  += lots * tradeWoodGain;
        if (lots > 0 && stallBonusLots > 0)
            snprintf(tradeMsg, sizeof(tradeMsg), "Traded: -%d stone +%d wood (stall +%d)", lots * tradeStoneCost, lots * tradeWoodGain, stallBonusLots);
        else if (lots > 0) snprintf(tradeMsg, sizeof(tradeMsg), "Traded: -%d stone +%d wood", lots * tradeStoneCost, lots * tradeWoodGain);
        else          snprintf(tradeMsg, sizeof(tradeMsg), "Trader: no stone to buy");
        tradeMsgTimer = tradeMsgTicks;
        logEvent(tradeMsg);
        printf("TRADE - %d stone for %d wood - stone: %d wood: %d\n", lots * tradeStoneCost, lots * tradeWoodGain, stone, wood);
        t.phase = trader::Trading;
        t.timer = 0;
    } else if (++t.stepTimer >= traderStepInterval) {
        t.stepTimer = 0;
        t.shape.move(dSq > 1.0f ? diff / sqrt(dSq) : diff);
    }
}

struct hoverHit {string label; optional<RectangleShape> rect; optional<CircleShape> circle;
                 int homeOfBarb = -1;     // hovered barb's home house id -> outline that house
                 int hoveredHouse = -1;}; // hovered house id -> pip over each resident

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
                             const vector<house>& houses, const vector<RectangleShape>& houseshapes) {
    float rSq = radius * radius;
    auto near = [&](const Shape& s) { return rectDistSq(s.getGlobalBounds(), p) <= rSq; };
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++)
        if (near(squares[i])) {
            const barb& b = barbs[i];
            string label = string(b.name) + ", " + traitOf(b).name + ", " + raceOf(b).name + ": " + barbTaskName(b) + (isHungry(b) ? " (hungry)" : "");
            if (b.homeHouse >= 0) {   // kin counted here, on hover only
                int kin = 0;
                for (auto& o : barbs) if (o.id != b.id && o.homeHouse == b.homeHouse) kin++;
                label += ", of house " + to_string(b.homeHouse + 1) + ", " + to_string(kin) + " kin";
            }
            if (isLeader(b) && raceNeed[b.race] != needNone) label += string("\nNeeds: ") + leaderNeedNames[raceNeed[b.race]];   // R30-F2
            hoverHit h{label, squares[i], nullopt};
            h.homeOfBarb = b.homeHouse;
            return h;
        }
    if (tr && near(tr->shape)) {
        const char* ph = tr->phase == trader::Arriving ? "arriving" : tr->phase == trader::Trading ? "trading" : "leaving";
        return hoverHit{string("Trader: ") + ph, tr->shape, nullopt};
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
            hoverHit h{"House " + to_string(houses[i].id + 1), houseshapes[i], nullopt};
            h.hoveredHouse = houses[i].id;
            return h;
        }
    return nullopt;
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
            if (barbs[i].homeHouse != h.hoveredHouse) continue;
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

// Outline-only copy of the hit shape, drawn above the overlays so it stays readable at night
void drawHoverOutline(RenderWindow& window, const View& worldView, hoverHit& h, float zoom) {
    window.setView(worldView);
    float th = tooltipOutlinePx / zoom;
    if (h.rect) {
        h.rect->setFillColor(Color::Transparent);
        h.rect->setOutlineThickness(th);
        h.rect->setOutlineColor(tooltipOutlineColor);
        drawSnapped(window, *h.rect);
    }
    if (h.circle) {
        h.circle->setFillColor(Color::Transparent);
        h.circle->setOutlineThickness(th);
        h.circle->setOutlineColor(tooltipOutlineColor);
        drawSnapped(window, *h.circle);
    }
}

// Places the label beside the cursor, flipping left/above to stay on screen and off the HUD and ticker
void drawTooltip(RenderWindow& window, const Font& font, const string& label, Vector2f mouse, const FloatRect& hudRect, const FloatRect& tickerRect) {
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
    t.setFillColor(tooltipTextColor);
    t.setPosition(pos + tooltipPad - lb.position);
    window.draw(t);
}

// Everything the HUD shows, gathered once per frame in main
struct hudValues {
    int wood, stone, food, hungry, people, houses, hour, speed;
    int gentry, commoners;   // R24-F1: founders vs house-born
    bool night, paused, raining;
    const char* trader;      // Away / Arriving / Trading / Leaving
    const char* tradeLine;   // last trade message, "" when expired
    const char* era;         // settlement rank: Camp / Hamlet / Town
    bool learned[techCount]; // know-how pips (R20-F1)
    int pulse[techCount];    // frames of white pulse left per pip
};

// Draws one table icon at pos (top-left, screen px), one quad per filled cell
void drawIcon(RenderWindow& window, hudIconId id, Vector2f pos) {
    const iconDef& d = hudIcons[id];
    VertexArray va(PrimitiveType::Triangles);
    pos = snapPx(pos);
    for (int y = 0; y < hudIconGrid; y++)
        for (int x = 0; x < hudIconGrid && d.rows[y][x]; x++) {
            char ch = d.rows[y][x];
            if (ch < 'a' || ch > 'd') continue;
            pushQuad(va, pos + Vector2f(x * hudIconPx, y * hudIconPx), Vector2f(hudIconPx, hudIconPx), d.key[ch - 'a']);
        }
    window.draw(va);
}

// The old full text block, now shown as a tooltip while hovering the strip
string hudDetailText(const hudValues& v) {
    char buf[380];
    snprintf(buf, sizeof(buf), "Settlement: %s\nWood: %d\nStone: %d\nFood: %d\nHungry: %d\nPopulation: %d (Gentry %d, Commoners %d)\nHouses: %d\nTime: %02d:00 (%s)\nSpeed: %dx%s\nWeather: %s\nTrader: %s%s%s",
             v.era, v.wood, v.stone, v.food, v.hungry, v.people, v.gentry, v.commoners, v.houses, v.hour, v.night ? "Night" : "Day", v.speed, v.paused ? " PAUSED" : "",
             v.raining ? "Rain" : "Clear", v.trader, v.tradeLine[0] ? "\n" : "", v.tradeLine);
    string s = buf;
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
    vector<cell> cells = {
        {iconWood,   to_string(v.wood),   pal.hudText},
        {iconStone,  to_string(v.stone),  pal.hudText},
        {iconFood,   to_string(v.food),   pal.hudText},
        {iconPeople, to_string(v.people), pal.hudText},
        {iconHouse,  to_string(v.houses), pal.hudText},
        {v.night ? iconMoon : iconSun, hour, pal.hudText},
        {v.paused ? iconPause : iconSpeed, v.paused ? string("Paused") : string(speed), v.paused ? pal.hudAlert : pal.hudText},
    };
    if (v.raining) cells.push_back({iconRain, "Rain", pal.hudText}); // last, so it never shifts the other cells

    // Centre on a fixed reference string so every number sits on the same baseline
    Text ref(font, hudMinNumber, hudFontSize);
    FloatRect rb = ref.getLocalBounds();
    float inner = hudBarHeight - hudRimPx;
    float textY = round((inner - rb.size.y) / 2 - rb.position.y);
    float iconY = round((inner - hudIconSize) / 2);
    float x = hudPadX;
    for (auto& c : cells) {
        drawIcon(window, c.icon, {x, iconY});
        x += hudIconSize + hudIconGap;
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

void drawOverlay(RenderWindow& window, RectangleShape& shape, Color c) {
    shape.setFillColor(c);
    window.setView(window.getDefaultView());
    window.draw(shape);
}

struct campfire {int fuel = campfireStartFuel; bool lit = true;};

// Keeps up to one wood of buffer: takes 1 wood only once the fuel drops to one wood's worth,
// so with steady wood the fire stays at full strength and it fades over campfireTicksPerWood once wood runs out
void campfireTick(campfire& f) {
    if (f.fuel <= campfireTicksPerWood && wood > 0) {
        wood--;
        f.fuel += campfireTicksPerWood;
        printf("CAMPFIRE FED - wood: %d\n", wood);
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

// R30-F1: 1px pixel line from race r's crate centre to the nearest house of that race. Called only when a house is built.
void rebuildQuarterRoad(int r, const vector<house>& houses) {
    if (r < 0 || r >= raceCount) return;
    VertexArray& va = quarterRoads[r];
    va.setPrimitiveType(PrimitiveType::Triangles);
    va.clear();
    Vector2f a = quarterCentre(r), best;
    float bestDSq = -1.0f;
    for (auto& h : houses) {
        if (h.race != r) continue;
        Vector2f c((float)h.x + 2.0f, (float)h.y + 2.0f);   // centre of the 4px house
        float dSq = (c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y);
        if (bestDSq < 0.0f || dSq < bestDSq) { bestDSq = dSq; best = c; }
    }
    if (bestDSq < 0.0f) return;
    Vector2f d = best - a;
    int n = max(1, (int)ceil(max(fabs(d.x), fabs(d.y))));   // one dot per step along the long axis: an unbroken 1px line
    for (int i = 0; i <= n; i++) appendDot(va, a + d * ((float)i / n), quarterRoadColor);
}

void buildhouse(vector<barb>& barbs,vector<RectangleShape>& squares, vector<house>& houses, vector<RectangleShape>& houseshapes, const plot& fp) {
    bool canclaim = (int)houses.size() < (int)barbs.size();
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (b.jobKind != jobNone) continue;   // buildshrine() / buildsmokehouse() / buildstall() / buildforge() / buildfield() own this barb
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (!canclaim || wood < houseWoodCost) continue;
            if (b.race < 0 || b.race >= raceCount) continue;
            int dx = quarterSpotD(gen), dy = quarterSpotD(gen);
            if (dx * dx + dy * dy > quarterHouseRadius * quarterHouseRadius) continue; // R30-F1: inside the own quarter's disc
            Vector2f qc = quarterCentre(b.race);
            int tx = (int)qc.x + dx, ty = (int)qc.y + dy;
            if (!houseSpotFree(barbs, houses, tx, ty)) continue;  // packed quarter: no overlapping houses
            if (!forgeClear(FloatRect(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize))) continue; // house + roof + slab; reroll next tick
            if (!lakeClear(FloatRect(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize))) continue;  // same footprint, no houses in the water
            if (!fieldClear(fp, FloatRect(Vector2f(tx - 3.0f, ty - 3.0f), houseSlabSize))) continue; // nor on the wheat plot
            wood -= houseWoodCost;
            b.reservedwood = true;
            b.reservedstone = stone >= houseStoneCost ? houseStoneCost : 0; // 0 stone -> plain house
            stone -= b.reservedstone;
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
                    if (b.reservedwood) wood += houseWoodCost;
                    stone += b.reservedstone;
                    b.reservedwood = false;
                    b.reservedstone = 0;
                    b.moving = b.movingbspot = b.building = false;
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
                    rebuildQuarterRoad(h.race, houses);   // R30-F1: only here, never per tick
                    b.building = b.moving = b.movingbspot = false;
                    b.timer = 0;
		            printf("---HOUSE BUILT---\n");
                    logEvent("House built");
                }
            } else {
                if (++b.stepTimer >= stepLimit(b, pos)) {
                    b.stepTimer = 0;
                    sq.move(diff / sqrt(dSq));
                    trailStep(sq.getPosition());
                }
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
void buildshrine(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, vector<shrine>& shrines, const plot& fp) {
    int pending = 0;
    for (auto& b : barbs) if (b.jobKind == jobShrine) pending++;
    int planned = (int)shrines.size() + pending;
    bool canclaim = planned < maxShrines
                 && (int)houses.size() >= (planned + 1) * shrineHousesPerShrine
                 && stone >= shrineStoneCost + houseStoneCost;
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (b.jobKind != jobNone && b.jobKind != jobShrine) continue; // buildsmokehouse() / buildstall() / buildforge() own this barb
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (!canclaim) continue;
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
                FloatRect hutClear(smokehousePos + smokehouseChimneyPos - Vector2f(smokehouseClearPx, smokehouseClearPx),
                                   Vector2f(smokehouseSize.x, smokehouseSize.y - smokehouseChimneyPos.y) + Vector2f(2 * smokehouseClearPx, 2 * smokehouseClearPx));
                FloatRect stoneRect(p - Vector2f(0.0f, shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH));
                if (stoneRect.findIntersection(hutClear).has_value()) found = false; // keep the smokehouse spot free
                if (!forgeClear(stoneRect)) found = false;                          // and the forge spot
                if (!lakeClear(stoneRect)) found = false;
                if (!fieldClear(fp, stoneRect)) found = false;
                for (auto& o : barbs) {
                    if (o.jobKind != jobShrine) continue;
                    Vector2f d = p - shrineSlabPos(o.targetx, o.targety);
                    if (d.x * d.x + d.y * d.y < shrineMinSpacing * shrineMinSpacing) found = false;
                }
            }
            canclaim = false;   // one claim attempt per tick; keep looping so builders already en route still progress
            if (!found) continue;
            stone -= shrineStoneCost;
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
                    stone += b.reservedstone;
                    b.reservedstone = 0;
                    b.jobKind = jobNone;
                    b.moving = b.movingbspot = b.building = false;
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
                    b.building = b.moving = b.movingbspot = false;
                    b.timer = 0;
                    printf("---SHRINE BUILT--- id %d (%d/%d)\n", s.id, (int)shrines.size(), maxShrines);
                    logEvent("Shrine stone raised");
                }
            } else {
                if (++b.stepTimer >= stepLimit(b, pos)) {
                    b.stepTimer = 0;
                    sq.move(diff / sqrt(dSq));
                    trailStep(sq.getPosition());
                }
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
template <class ReadyFn>
bool buildOneOff(vector<barb>& barbs, vector<RectangleShape>& squares, bool& claimed, buildJob kind,
                 Vector2f stand, const char* name, ReadyFn ready) {
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
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
            printf("%s CLAIMED - barb %d wood: %d stone: %d\n", name, b.id, wood, stone);
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
                b.building = b.moving = b.movingbspot = false;
                b.timer = 0;
                return true;
            }
        } else if (++b.stepTimer >= stepLimit(b, pos)) {
            b.stepTimer = 0;
            sq.move(pathStep(pos, diff));   // R29-F2
            trailStep(sq.getPosition());
        }
        return false;
    }
    return false;
}

// Runs after buildshrine(). One builder ever: claimed is set on the claim and stays set once the hut stands.
// Houses keep first call on stone; wood is whatever buildhouse() left this tick.
void buildsmokehouse(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, smokehouse& sh) {
    if (sh.boostTicks > 0) sh.boostTicks--;
    if (sh.built) return;
    bool done = buildOneOff(barbs, squares, sh.claimed, jobSmoke, smokehousePos + smokehouseStandOffset, "SMOKEHOUSE",
        [&](const barb&) {
            if ((int)houses.size() < smokehouseMinHouses || wood < smokehouseWoodCost
                || stone < smokehouseStoneCost + houseStoneCost) return -1;
            if (!forgeClear(FloatRect(smokehousePos + smokehouseChimneyPos, Vector2f(smokehouseSize.x, smokehouseSize.y - smokehouseChimneyPos.y))))
                return -1;   // fixed spots: only trips if the layout constants are moved onto the forge
            wood -= smokehouseWoodCost;
            stone -= smokehouseStoneCost;
            return 1;
        });
    if (!done) return;
    sh.built = true;
    printf("---SMOKEHOUSE BUILT--- +%d food per deer/rabbit kill\n", smokehouseFoodBonus);
    logEvent("Smokehouse built (+" + to_string(smokehouseFoodBonus) + " food per kill)");
}

// Race with strictly the most houses; a tie for first (or no houses) gives raceHuman, the default style
barbRace majorityRace(const vector<house>& houses) {
    int n[raceCount] = {};
    for (auto& h : houses) n[h.race]++;
    int best = -1, bestN = 0;
    bool tie = false;
    for (int r = 0; r < raceCount; r++) {
        if (n[r] > bestN) { best = r; bestN = n[r]; tie = false; }
        else if (n[r] == bestN && bestN > 0) tie = true;
    }
    return (best < 0 || tie) ? raceHuman : (barbRace)best;
}

// Dark 6x5 hut: 2 roof rows over 3 wall rows, a 1x2 door and a grey 1x2 chimney. Drawn with the shrines.
// Only the roof is tinted to the majority race's colour.
void drawSmokehouse(RenderWindow& window, const smokehouse& sh, const vector<house>& houses) {
    if (!sh.built) return;
    RectangleShape r(smokehouseSize);
    r.setPosition(smokehousePos);
    r.setFillColor(smokehouseWallColor);
    window.draw(r);
    r.setSize(Vector2f(smokehouseSize.x, smokehouseRoofH));
    r.setFillColor(smokehouseRoofRaceColors[majorityRace(houses)]);
    window.draw(r);
    r.setSize(smokehouseDoorSize);
    r.setPosition(smokehousePos + smokehouseDoorPos);
    r.setFillColor(smokehouseDoorColor);
    window.draw(r);
    r.setSize(smokehouseChimneySize);
    r.setPosition(smokehousePos + smokehouseChimneyPos);
    r.setFillColor(smokehouseChimneyColor);
    window.draw(r);
}

// Sparse puffs rising from the chimney and fading out; a short burst of extra puffs after a bonus kill.
// Drawn above the trees and under the day/night overlay.
void drawSmoke(RenderWindow& window, const smokehouse& sh, int frame) {
    if (!sh.built) return;
    int puffs = sh.boostTicks > 0 ? smokePuffsBoost : smokePuffsIdle;
    Vector2f top = smokehousePos + smokehouseChimneyPos;   // chimney top-left
    RectangleShape p;
    for (int k = 0; k < puffs; k++) {
        float t = (float)((frame + k * smokeCycleFrames / puffs) % smokeCycleFrames) / smokeCycleFrames;
        float sz = t < 0.5f ? smokePuffPx : 1.0f;
        Vector2f at(top.x - (sz - 1.0f) * 0.5f + smokeDriftPx * t, top.y - sz - smokeRisePx * t);
        p.setSize(Vector2f(sz, sz));
        p.setPosition(Vector2f(floor(at.x), floor(at.y)));
        Color c = smokeColor;
        c.a = (uint8_t)(smokeAlpha * (1.0f - t));
        p.setFillColor(c);
        window.draw(p);
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
            if (dwarfHouses < forgeMinDwarfHouses || wood < forgeWoodCost || stone < forgeStoneCost + houseStoneCost) return -1;
            if (b.race != raceDwarf) return 0;
            wood -= forgeWoodCost;
            stone -= forgeStoneCost;
            return 1;
        });
    if (!done) return;
    fg.built = true;
    printf("---FORGE BUILT--- (%d wood, %d stone)\n", forgeWoodCost, forgeStoneCost);
    logEvent("Dwarves light a forge");
}

// 7x5 slate forge: darker roof row, hearth mouth with a 1px breathing ember, 2x2 iron anvil.
// The ember glows hotter for ~2s after a dwarf mines a rock. Drawn with the shrines, under barbs and trees.
void drawForge(RenderWindow& window, const forge& fg, int frame) {
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
    if (forgeEmberFrames > 0) { lo = forgeEmberLit; hi = forgeEmberHot; }
    auto mix = [t](uint8_t a, uint8_t c) { return (uint8_t)(a + (c - a) * t); };
    r.setSize(Vector2f(1.0f, 1.0f));
    r.setPosition(forgePos + forgeEmberPos);
    r.setFillColor(Color(mix(lo.r, hi.r), mix(lo.g, hi.g), mix(lo.b, hi.b)));
    window.draw(r);
}

// R29-F1: runs after buildforge(). One storehouse per race, built through buildOneOff() by a barb of that race once the
// race has a house of its own. All share jobStore, so only one is under construction at a time; if its builder is lost
// before finishing (no barb holds jobStore), the claim is released.
void buildstorehouses(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, storehouse (&stores)[raceCount]) {
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
                if (raceHouses < storehouseMinRaceHouses || wood < storehouseWoodCost) return -1;
                if (b.race != r) return 0;
                wood -= storehouseWoodCost;
                return 1;
            });
        if (done) {
            st.built = true;
            st.claimed = false;
            printf("---STOREHOUSE BUILT--- %s (%d wood)\n", raceDefs[r].name, storehouseWoodCost);
            logEvent(string(racePlurals[r]) + " build a storehouse");
        }
        if (st.claimed) return;   // one under construction at a time
    }
}

// R29-F1: 10x10 race-coloured crate, dark inset rim and one dark lid slat across it. The fill eases toward white
// for a moment after each deposit. Drawn with the forge, under barbs and trees.
// R30-F1: ground layer. Each standing crate's 1px road to its race's nearest house (prebuilt on house build), then the
// faint dotted quarter rings at zoom >= quarterRingMinZoom (at 1x a 1px dot ring is just noise).
void drawQuarters(RenderWindow& window, const storehouse (&stores)[raceCount], float zoom) {
    for (int r = 0; r < raceCount; r++)
        if (stores[r].built && quarterRoads[r].getVertexCount() > 0) window.draw(quarterRoads[r]);
    if (zoom >= quarterRingMinZoom && quarterRingVerts.getVertexCount() > 0) window.draw(quarterRingVerts);
}

void drawStorehouses(RenderWindow& window, const storehouse (&stores)[raceCount]) {
    RectangleShape crate(storehouseSize), slat(Vector2f(storehouseSize.x, storehouseOutlinePx));
    crate.setOutlineThickness(-storehouseOutlinePx);
    crate.setOutlineColor(storehouseOutlineColor);
    slat.setFillColor(storehouseOutlineColor);
    for (int r = 0; r < raceCount; r++) {
        const storehouse& st = stores[r];
        if (!st.built) continue;
        float t = storehouseFlashMix * (float)st.flashFrames / storehouseFlashFrames;
        crate.setFillColor(lerpColor(storehouseColors[r], Color::White, t));
        crate.setPosition(storehousePos[r]);
        window.draw(crate);
        slat.setPosition(storehousePos[r] + Vector2f(0.0f, floor(storehouseSize.y / 2.0f) - 1.0f));
        window.draw(slat);
    }
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

// Picks the plot spot once, in a ring around the campfire: clear of the forge, lakes, campfire, smokehouse, stall,
// houses and shrines (standing or claimed), and startup trees/rocks. Reserved from then on via fieldClear().
bool placeField(plot& fp, const vector<barb>& barbs, const vector<house>& houses, const vector<shrine>& shrines,
                const vector<CircleShape>& tris, const vector<CircleShape>& octs) {
    uniform_real_distribution<float> ang(0.0f, 6.2831853f), rad(fieldMinRadius, fieldMaxRadius);
    float fireR = campfireStoneRadius + 1.0f;
    FloatRect fire(villageCenter - Vector2f(fireR, fireR), Vector2f(2 * fireR, 2 * fireR));
    FloatRect hut(smokehousePos + smokehouseChimneyPos - Vector2f(smokehouseClearPx, smokehouseClearPx),
                  Vector2f(smokehouseSize.x, smokehouseSize.y - smokehouseChimneyPos.y) + Vector2f(2 * smokehouseClearPx, 2 * smokehouseClearPx));
    FloatRect stall(stallPos + Vector2f(stallStandOffset.x, 0.0f), stallSize + Vector2f(-stallStandOffset.x, fieldZoneBelowPx)); // incl. stand spot
    for (int k = 0; k < fieldPlaceTries; k++) {
        float a = ang(gen), r = rad(gen);
        Vector2f pos(round(villageCenter.x + cos(a) * r - fieldSize.x / 2), round(villageCenter.y + sin(a) * r - fieldSize.y / 2));
        FloatRect zone = fieldZone(pos);
        if (!forgeClear(zone) || !lakeClear(zone)) continue;
        bool ok = !zone.findIntersection(fire) && !zone.findIntersection(hut) && !zone.findIntersection(stall);
        for (int i = 0; ok && i < (int)houses.size(); i++)
            if (zone.findIntersection(FloatRect(Vector2f(houses[i].x - 3.0f, houses[i].y - 3.0f), houseSlabSize))) ok = false;
        for (auto& b : barbs) {   // houses and shrines still being walked to / built
            if (!ok) break;
            if (b.movingbspot && b.jobKind == jobNone
                && zone.findIntersection(FloatRect(Vector2f(b.targetx - 3.0f, b.targety - 3.0f), houseSlabSize))) ok = false;
            if (b.jobKind == jobShrine
                && zone.findIntersection(FloatRect(shrineSlabPos(b.targetx, b.targety) - Vector2f(0.0f, shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH)))) ok = false;
        }
        for (auto& s : shrines)
            if (ok && zone.findIntersection(FloatRect(Vector2f((float)s.x, (float)s.y - shrineCapH), shrineSlabSize + Vector2f(0.0f, shrineCapH)))) ok = false;
        for (int i = 0; ok && i < (int)tris.size(); i++)
            if (zone.findIntersection(FloatRect(tris[i].getPosition(), Vector2f(treeFootprint, treeFootprint)))) ok = false;
        for (int i = 0; ok && i < (int)octs.size(); i++)
            if (zone.findIntersection(octs[i].getGlobalBounds())) ok = false;
        if (!ok) continue;
        fp.placed = true;
        fp.rect = FloatRect(pos, fieldSize);
        fp.shape.setSize(fieldSize);
        fp.shape.setPosition(pos);
        fp.shape.setFillColor(fieldSoilColor);
        fp.shape.setOutlineColor(fieldBorderColor);
        fp.shape.setOutlineThickness(fieldBorderPx);
        printf("FIELD SPOT chosen at %d,%d\n", (int)pos.x, (int)pos.y);
        return true;
    }
    return false;
}

// Runs after buildforge(). One sowing ever: an idle barb takes jobField through buildOneOff(), which clears it on completion.
// If the sower is lost before finishing (no barb holds jobField), the claim is released so another idle barb can take it.
void buildfield(vector<barb>& barbs, vector<RectangleShape>& squares, const vector<house>& houses, const vector<shrine>& shrines,
                const vector<CircleShape>& tris, const vector<CircleShape>& octs, plot& fp) {
    if (fp.built) return;
    if (fp.claimed) {
        bool held = false;
        for (auto& b : barbs) if (b.jobKind == jobField) held = true;
        if (!held) fp.claimed = false;
    }
    if (!fp.placed) {
        if ((int)houses.size() < fieldMinHouses || wood < fieldWoodCost) return;
        if (!placeField(fp, barbs, houses, shrines, tris, octs)) return;
    }
    bool done = buildOneOff(barbs, squares, fp.claimed, jobField, fieldWorkSpot(fp), "FIELD",
        [&](const barb&) {
            if ((int)houses.size() < fieldMinHouses || wood < fieldWoodCost) return -1;
            wood -= fieldWoodCost;
            return 1;
        });
    if (!done) return;
    fp.built = true;
    fp.claimed = false;
    fp.growth = 0;
    printf("---FIELD SOWN--- ripe in %d ticks, +%d food per harvest\n", fieldGrowTicks, fieldFoodYield);
    logEvent("Wheat field sown");
}

// R20-F3: every way out of a spear craft (finished, unaffordable, interrupted) clears the same barb fields
void clearSpearCraft(barb& b) {
    b.jobKind = jobNone;
    b.moving = b.movingbspot = b.building = false;
    b.spearTimer = 0;
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
    dropCarry(b);   // R29-F1
}

// Runs after buildfield(). One crafter at a time: an idle barb without a spear walks to the campfire and knaps for
// spearCraftTicks (in spearTimer, never timer). Wood and stone are checked on claim but paid only on completion;
// if they ran out meanwhile the craft is dropped. Spears (held + in progress) are capped at 1 per spearBarbsPerSpear barbs.
void craftspear(vector<barb>& barbs, vector<RectangleShape>& squares) {
    int spears = 0, crafter = -1;
    for (int i = 0; i < (int)barbs.size(); i++) {
        if (barbs[i].hasSpear) spears++;
        if (barbs[i].jobKind == jobSpear) crafter = i;
    }
    Vector2f stand = villageCenter + spearStandOffset;
    if (crafter < 0) {
        if ((spears + 1) * spearBarbsPerSpear > (int)barbs.size()) return;
        if (wood < spearWoodCost || stone < spearStoneCost) return;
        for (auto& b : barbs) {
            if (b.hasSpear || b.jobKind != jobNone || b.deertargeting || b.chopping || b.timer > 0
                || b.moving || b.movingbspot || b.building) continue;
            b.jobKind = jobSpear;
            b.target = targetNone;
            b.targetx = (int)stand.x;
            b.targety = (int)stand.y;
            b.moving = b.movingbspot = true;
            b.spearTimer = 0;
            printf("SPEAR CLAIMED - barb %d (%d/%d spears)\n", b.id, spears, (int)barbs.size() / spearBarbsPerSpear);
            return;
        }
        return;
    }
    barb& b = barbs[crafter];
    auto& sq = squares[crafter];
    if (!b.movingbspot) { clearSpearCraft(b); return; }   // something else took the barb: release the job
    Vector2f pos  = sq.getPosition();
    Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq >= 1.0f) {
        if (++b.stepTimer >= stepLimit(b, pos)) {
            b.stepTimer = 0;
            sq.move(diff / sqrt(dSq));
            trailStep(sq.getPosition());
        }
        return;
    }
    b.moving = false;   // standing at the fire: no walking bob while knapping
    if (workTick(b)) b.spearTimer++;
    if (b.spearTimer < spearCraftTicks) return;
    if (wood < spearWoodCost || stone < spearStoneCost) {
        printf("SPEAR ABANDONED - barb %d, no materials (wood: %d stone: %d)\n", b.id, wood, stone);
        clearSpearCraft(b);
        return;
    }
    wood -= spearWoodCost;
    stone -= spearStoneCost;
    b.hasSpear = true;
    clearSpearCraft(b);
    printf("---SPEAR MADE--- barb %d (%d wood, %d stone)\n", b.id, spearWoodCost, spearStoneCost);
    logEvent("Flint spear made");
}

void fieldTick(plot& fp) {
    if (fp.built && fp.growth < wheatGrowTicks()) {
        fp.growth++;
        if (fp.growth == wheatGrowTicks()) printf("WHEAT RIPE\n");
    }
}

// Soil with a 1px gold frame; inside, three 6px crop rows (1px soil margin all round). Stalks sit on y = 2/4/6,
// heads on y = 1/3/5. Sprouts -> green rows -> yellowing heads -> ripe gold with one ear bobbing above the frame.
// Drawn with the village structures, under barbs and trees.
void drawField(RenderWindow& window, const plot& fp, int frame) {
    if (!fp.built) return;
    drawSnapped(window, fp.shape);
    Vector2f o = snapPx(fp.rect.position);
    int stage = fieldStage(fp);
    const float rowW = fieldSize.x - 2.0f;
    RectangleShape r;
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
    b.trait = (barbTrait)uniform_int_distribution<int>(0, traitCount - 1)(gen);   // R25-F1
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
        if (b.moving || b.chopping || b.building || b.movingbspot || b.deertargeting || b.timer > 0 || b.jobKind != jobNone) continue;
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
    b.moving = b.movingbspot = b.building = false;
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
            if (++b.stepTimer >= stepLimit(b, pos)) {
                b.stepTimer = 0;
                sq.move(diff / sqrt(dSq));
                trailStep(sq.getPosition());
            }
            return;
        }
        b.moving = false;     // on the cell: no walking bob, just the generic build tool swing
        b.building = true;
        if (workTick(b)) b.timer++;
        if (b.timer < paveTicks) return;
        if (stone < paveStoneCost) {
            printf("PAVING ABANDONED - barb %d, no stone\n", b.id);
            clearPave(b);
            return;
        }
        int c = paveClaim;
        stone -= paveStoneCost;
        trailPaved[c] = 1;
        trailWear[c] = max(trailWear[c], trailRoadThreshold);   // locked at road tier (keeps roadStepBonus)
        trailSetTier(c);
        pavedCount++;
        paveDirty = true;
        printf("---CELL PAVED--- barb %d at %d,%d (%d/%d) stone: %d\n", b.id, (int)((c % trailGridN) * trailCellSize),
               (int)((c / trailGridN) * trailCellSize), pavedCount, paveMaxCells, stone);
        if (pavedCount == 1) logEvent("First cobbles laid by the fire");
        clearPave(b);
        return;
    }
    if (++scanTimer < paveScanTicks) return;
    scanTimer = 0;
    if (currentEra < eraHamlet || pavedCount >= paveMaxCells || stone < paveStoneCost + houseStoneCost) return;
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
        if (b.jobKind != jobNone || b.deertargeting || b.chopping || b.timer > 0
            || b.moving || b.movingbspot || b.building) continue;
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
    score[decreeTimber]  += councilNeedLean * clamp((float)(houseWoodCost - wood) / (float)houseWoodCost, 0.0f, 1.0f);
    score[decreeStone]   += councilNeedLean * clamp((float)(houseStoneCost - stone) / (float)houseStoneCost, 0.0f, 1.0f);
    score[decreeBerries] += (isHungry(b) || food <= 0) ? councilNeedLean : 0.0f;
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
        if (barbs[i].lastVote == decreeNone) continue;   // born after the vote
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

// R30-F2: leader ranks the colony's wood/stone/food shortage (0..1 each); worst wins, none if nothing is short
void rankRaceNeed(const vector<barb>& barbs, int r) {
    int members = 0;
    for (auto& b : barbs) members += b.race == r ? 1 : 0;
    float s[needCount];
    s[needWood]  = clamp((float)(houseWoodCost - wood) / (float)houseWoodCost, 0.0f, 1.0f);
    s[needStone] = clamp((float)(houseStoneCost - stone) / (float)houseStoneCost, 0.0f, 1.0f);
    s[needFood]  = clamp((float)(members - food) / (float)max(1, members), 0.0f, 1.0f);   // one food per member
    int best = needNone;
    for (int n = 0; n < needCount; n++) if (s[n] > 0.0f && (best == needNone || s[n] > s[best])) best = n;
    raceNeed[r] = (leaderNeed)best;
    printf("LEADER NEED - %s: wood %.2f, stone %.2f, food %.2f -> %s\n", racePlurals[r], s[needWood], s[needStone], s[needFood],
           best == needNone ? "none" : leaderNeedNames[best]);
}

void leaderTick(vector<barb>& barbs) {
    for (int r = 0; r < raceCount; r++) {
        if (raceElected[r] && findBarbById(barbs, raceLeaderId[r]) < 0) loseLeader(r);   // leader gone: no leader, may elect again
        if (raceElected[r]) { rankRaceNeed(barbs, r); continue; }
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

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, const vector<house>& houses, vector<RectangleShape>& houseshapes, vector<birthRing>& rings) {
    static int deferCooldown = 0;
    if (deferCooldown > 0) deferCooldown--;
    int cap = popCap(houses);
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        if (spawnChance(gen) != 0) continue;
        if ((int)barbs.size() >= cap) continue;
        bool fed = (int)barbs.size() < birthFreeFloor || food >= max(0, (int)barbs.size());
        if (!fed) {
            if (deferCooldown == 0) {
                printf("Birth deferred: larder low (food %d, pop %d)\n", food, (int)barbs.size());
                logEvent("Birth deferred: larder low");
                deferCooldown = birthDeferCooldownTicks;
            }
            continue;
        }
        int paid = min(food, birthFoodCost);
        food = max(0, food - birthFoodCost);
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
        if (b.chopping || b.timer > 0 || b.movingbspot || b.building || b.carryKind != carryNone) continue;   // R29-F1: haulTick() walks haulers
        Vector2f pos = sq.getPosition();
        if (!b.moving) {
            if (wanderChance(gen) != 0) continue;
            int tx = (int)pos.x + wanderDX(gen);
            int ty = (int)pos.y + wanderDY(gen);
            if (tx > 0 && tx < 800 && ty > 0 && ty < 800 && lakeClear(FloatRect(Vector2f(tx, ty), sq.getSize()), &pos)) {
                b.targetx = b.wanderX = tx;
                b.targety = b.wanderY = ty;
                b.moving  = true;
            }
        } else {
            Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;
            bool wandering = b.targetx == b.wanderX && b.targety == b.wanderY;
            if (dSq < 1.0f) {
                b.moving    = false;
                b.stepTimer = 0;
                b.wanderX = b.wanderY = -1;
            } else if (++b.stepTimer >= stepLimit(b, pos)) {
                b.stepTimer = 0;
                sq.move(wandering ? diff / sqrt(dSq) : pathStep(pos, diff));   // R29-F2: wander walks stay straight
                trailStep(sq.getPosition());
            }
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
        if (b.targetx == -1 || b.target == targetBush || b.target == targetLake || b.target == targetField || b.target == targetDepot || b.deertargeting || b.movingbspot || b.building) continue;
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

// Every way out of a crop harvest (berry bush or wheat field) clears the same barb fields
void clearHarvest(barb& b) {
    b.moving = b.chopping = false;
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
    food += yield;
    clearHarvest(b);
    return true;
}

// R29-F1: where a hauler drops its load: its own race's storehouse if standing, else the campfire. A lookup, no search.
Vector2f depotSpot(const barb& b, const storehouse (&stores)[raceCount]) {
    if (b.race >= 0 && b.race < raceCount && stores[b.race].built) return storehousePos[b.race] + storehouseStandOffset;
    return villageCenter + campfireDepotOffset;
}

// R29-F1: a felled tree / mined rock becomes a load; the barb walks it to the depot (haulTick) before it counts
void startHaul(barb& b, carryLoad load, const storehouse (&stores)[raceCount]) {
    Vector2f spot = depotSpot(b, stores);
    b.chopping = false;
    b.timer = 0;
    b.carryKind = load;
    b.target = targetDepot;
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    b.stepTimer = 0;
}

// R29-F1: one hauler step. The depot is re-resolved every tick, so a storehouse that is gone sends the load to the
// campfire. On arrival the load is added to the stock, the storehouse flashes, and the barb is idle again.
void haulTick(barb& b, RectangleShape& sq, storehouse (&stores)[raceCount]) {
    Vector2f spot = depotSpot(b, stores);
    b.targetx = (int)spot.x;
    b.targety = (int)spot.y;
    b.moving = true;
    Vector2f pos  = sq.getPosition();
    Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
    float dSq = diff.x * diff.x + diff.y * diff.y;
    if (dSq >= 1.0f) {
        if (++b.stepTimer >= stepLimit(b, pos)) {
            b.stepTimer = 0;
            sq.move(pathStep(pos, diff));
            trailStep(sq.getPosition());
        }
        return;
    }
    bool isWood = b.carryKind == carryWood;
    (isWood ? wood : stone)++;
    storehouse* st = (b.race >= 0 && b.race < raceCount && stores[b.race].built) ? &stores[b.race] : nullptr;
    if (st) { st->stored++; st->flashFrames = storehouseFlashFrames; }
    printf("%s DEPOSITED at %s - wood: %d stone: %d\n", isWood ? "WOOD" : "STONE",
           st ? "storehouse" : "campfire", wood, stone);
    b.carryKind = carryNone;
    b.target = targetNone;
    b.moving = false;
    b.stepTimer = 0;
    b.targetx = b.targety = -1;
}

void minematerials(vector<RectangleShape>& squares, vector<barb>& barbs, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<tree>& trees, vector<CircleShape>& octs, vector<rock>& rocks,
                   vector<bush>& bushes, vector<CircleShape>& bushshapes, vector<birthRing>& catchRings, plot& fp, storehouse (&stores)[raceCount]) {
    vector<int> treeClaim, rockClaim;   // parallel to trees/tris and rocks/octs for this tick
    buildWorkClaims(barbs, tris, octs, treeClaim, rockClaim);
    vector<int> shoreClaim(shorePoints.size(), -1);   // rebuilt every tick like the tree/rock claims, so no stale anglers
    for (int j = 0; j < (int)barbs.size(); j++) {
        int k = barbs[j].targetShore;
        if (barbs[j].target == targetLake && k >= 0 && k < (int)shoreClaim.size() && shoreClaim[k] == -1) shoreClaim[k] = j;
    }
    bool fieldTaken = false;   // one reaper at a time, derived from targets every tick (no stored claim)
    for (auto& o : barbs) if (o.target == targetField) fieldTaken = true;
    leaderNeed liveNeed[raceCount];   // R30-F2: a race's need counts only while its leader is alive (checked once per tick)
    for (int r = 0; r < raceCount; r++) liveNeed[r] = findBarbById(barbs, raceLeaderId[r]) >= 0 ? raceNeed[r] : needNone;
    for (int j = 0; j < (int)squares.size(); j++) {
        auto& sq = squares[j];
        auto& b  = barbs[j];
        if (b.carryKind != carryNone) {   // R29-F1: any other state taken over the haul drops the load
            if (b.target != targetDepot || b.movingbspot || b.building || b.deertargeting || b.chopping || b.jobKind != jobNone) dropCarry(b);
            else { haulTick(b, sq, stores); continue; }
        }
        if (b.movingbspot || b.building) continue;
        Vector2f bpos = sq.getPosition();
        bool bestIsRock = false;
        if (b.timer == 0 && !b.chopping) {
            float treeDSq, rockDSq, bushDSq;
            int bestTree = findNearest(bpos, barbWorkSearchRadius, (int)trees.size(),
                                       [&](int i) { return tris[i].getPosition() + treeWorkOffset; },
                                       [&](int i) { return trees[i].dying || (treeClaim[i] != -1 && treeClaim[i] != j); }, &treeDSq);
            int bestRock = findNearest(bpos, barbWorkSearchRadius, (int)rocks.size(),
                                       [&](int k) { return octs[k].getPosition() + rockWorkOffset; },
                                       [&](int k) { return rocks[k].dying || (rockClaim[k] != -1 && rockClaim[k] != j); }, &rockDSq);
            int bestBush = findNearest(bpos, barbWorkSearchRadius, (int)bushes.size(),
                                       [&](int k) { return bushWorkSpot(bushshapes[k]); },
                                       [&](int k) { return !bushes[k].ripe || bushes[k].picking; }, &bushDSq);
            // need weighting: wood short of a house makes trees count as nearer. Scores are squared
            // distances divided by weight^2; not-found stays infinite so a boosted miss never wins.
            float woodShort = clamp((float)(houseWoodCost - wood) / (float)houseWoodCost, 0.0f, 1.0f);
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
            float rockScore = bestRock != -1 ? rockDSq / (rockW * rockW) : notFound;
            float bushScore = bestBush != -1 ? bushDSq / (bushW * bushW) : notFound;
            // a rock wins only when strictly better, as when trees were scanned first; a bush likewise
            bestIsRock = bestRock != -1 && rockScore < treeScore;
            bool bestIsBush = bestBush != -1 && bushScore < (bestIsRock ? rockScore : treeScore);
            int best = bestIsRock ? bestRock : bestTree;
            // ripe wheat: weighted like berries (same food decree), wins only when strictly better than everything else
            float fieldScore = notFound;
            if (fieldRipe(fp) && !fieldTaken) {
                Vector2f d = fieldWorkSpot(fp) - bpos;
                float dSq = d.x * d.x + d.y * d.y;
                if (dSq < fieldSearchRadius * fieldSearchRadius) fieldScore = dSq / (bushW * bushW);
            }
            if (fieldScore < min(min(treeScore, rockScore), bushScore) && b.moving==false) {
                Vector2f wp = fieldWorkSpot(fp);
                b.targetx = (int)wp.x;
                b.targety = (int)wp.y;
                b.moving  = true;   // later branches all need !moving, so nothing else claims this barb this tick
                b.target = targetField;
                fieldTaken = true;
            }
            if (bestIsBush && b.moving==false) {
                Vector2f wp = bushWorkSpot(bushshapes[bestBush]);
                b.targetx = (int)wp.x;
                b.targety = (int)wp.y;
                b.moving  = true;
                b.target = targetBush;
                b.targetBush = bestBush;
            }
            if (best != -1 && !bestIsBush && !bestIsRock && b.moving==false) {
                Vector2f tp = tris[best].getPosition();
                b.targetx = (int)(tp.x + 8);
                b.targety = (int)(tp.y + 15);
                b.moving  = true;
                b.target = targetNone;
                treeClaim[best] = j;
            }
	    if (best != -1 && !bestIsBush && bestIsRock && b.moving==false) {
		    Vector2f rp = octs[best].getPosition();
		    b.targetx = (int)(rp.x + 6);
		    b.targety = (int)(rp.y + 10);
		    b.moving = true;
		    b.target = targetRock;
		    rockClaim[best] = j;
        }
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
        if (b.targetx == -1) continue;
	}
	if (b.target == targetLake) {
            int k = b.targetShore;
            auto quit = [&]() {   // every way out of fishing clears the same fields
                b.moving = b.chopping = false;
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
                food += fishFoodYield;
                catchRings.push_back({fishPoseOf(b, sq).lineEnd + Vector2f(0.5f, 0.5f)});
                quit();
                printf("FISH CAUGHT - food: %d\n", food);
            }
            continue;
	}
	if (b.target == targetField) {
            Vector2f wp = fieldWorkSpot(fp);
            if (!fp.built || b.targetx != (int)wp.x || b.targety != (int)wp.y || (!fieldRipe(fp) && !b.chopping)) {
                clearHarvest(b);   // retargeted or nothing to reap
                continue;
            }
            int yield = fieldFoodYield + forageBonus(b);   // R25-F1: Forager +1
            if (harvestTick(b, wp, bpos, fieldHarvestTicks, yield)) {
                fp.growth = 0;     // replanted at once
                printf("WHEAT HARVESTED - food: %d\n", food);
                logEvent("Wheat harvested (+" + to_string(yield) + " food, " + to_string(food) + ")");
                practice(techSeedSelection);   // after the harvest line so a discovery reads as its consequence
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
                printf("BERRIES PICKED - food: %d\n", food);
            }
            continue;
	}
	if (b.target != targetRock) {
        for (int i = 0; i < (int)trees.size(); i++) {
            Vector2f tp = tris[i].getPosition();
            float tix = tp.x + 8, tiy = tp.y + 15;
            if (b.targetx != (int)tix || b.targety != (int)tiy) continue;
            if (trees[i].dying && !b.chopping) {
                b.moving  = false;
                b.targetx = b.targety = -1;
                break;
            }
	   
            float dx = tix - bpos.x, dy = tiy - bpos.y;
            if (dx * dx + dy * dy >= 4.0f) break;
            b.chopping = true;
            trees[i].dying = true;
            if (workTick(b)) b.timer++;
            if (b.timer >= chopTicks()) {
                practice(techHandAxe);
                swapRemove(trees,  i);
                swapRemove(tris,   i);
                swapRemove(stumps, i);
                swapRemove(treeClaim, i);
                startHaul(b, carryWood, stores);   // R29-F1: wood counts once it reaches the depot
                printf("TREE DESTROYED - hauling wood (stock %d)\n", wood);
           		}
            break;
        	}
	    }
	else
		for(int i = 0; i < (int)rocks.size(); i++) {
			Vector2f rp = octs[i].getPosition();
			float rix = rp.x + 6, riy = rp.y + 10;
			if (b.targetx != (int)rix || b.targety != (int)riy) continue;
			if (rocks[i].dying && !b.chopping) {
				b.moving = false;
				b.targetx = b.targety = -1;
				break;
			}
			float dx = rix - bpos.x, dy = riy - bpos.y;
			if (dx * dx + dy * dy >= 4.0f) break;
			b.chopping = true;
			rocks[i].dying = true;
			if (workTick(b)) b.timer++;
			if (b.timer >= mineTicks(b)) {           // dwarves finish rocks faster
				practice(techPickwork);
				if (b.race == raceDwarf) forgeEmberFrames = forgeEmberBoostFrames; // forge ember flares (drawn only once built)
				swapRemove(rocks, i);
				swapRemove(octs, i);
				swapRemove(rockClaim, i);
				startHaul(b, carryStone, stores);  // R29-F1: stone counts once it reaches the depot
				printf("ROCK DESTROYED - hauling stone (stock %d)\n", stone);
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

void treespawn(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, const plot& fp) {
    if (treespawnChance(gen) != 0) return;
    tree t{distrib(gen), distrib(gen), false};
    if (!lakeClear(FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return; // no trees in the water
    if (!fieldClear(fp, FloatRect(Vector2f(t.x, t.y), Vector2f(treeFootprint, treeFootprint)))) return; // nor on the wheat plot
    trees.push_back(t);

    tris.push_back(makeTreeTri(t.x, t.y));

    RectangleShape stump(Vector2f(4, 4));
    stump.setFillColor(stumpColor);
    stump.setPosition(Vector2f(t.x + 6, t.y + 12));
    stumps.push_back(stump);
}

// 2-3 ponds away from the village, apart from each other. Startup trees (trees/tris/stumps) and rocks (rocks/octs)
// in or touching a lake are removed together. Fixed village spots (smokehouse, stall, forge) are inside the
// lakeVillageMinDist ring, so they never need a lake check. Shore points are cached here for angling.
void spawnLakes(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<rock>& rocks, vector<CircleShape>& octs) {
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
        swapRemove(rocks, i); swapRemove(octs, i);
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
    uniform_int_distribution<int> centre(60, 740);
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
                   vector<rock>& rocks, vector<CircleShape>& octs, const vector<CircleShape>& bushshapes) {
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
                addR++;
            }
        }
        bool shortNow = nt + addT < quarterMinTrees || nr + addR < quarterMinRocks;
        printf("Race quarter %s: %d trees, %d rocks nearby (+%d/+%d re-seeded)%s\n", raceDefs[q].name, nt, nr, addT, addR,
               shortNow ? " - STILL SHORT" : "");
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
    RenderWindow window(VideoMode({800, 800}), "sim");
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
    }

    vector<tree> trees(numtrees);
    for (auto& t : trees) {t.x = distrib(gen); t.y = distrib(gen);}

    vector<rock> rocks(numrocks);
    for (auto& r : rocks) {r.x = distrib(gen); r.y = distrib(gen);}

    vector<house> houses;
    vector<RectangleShape> houseshapes;
    vector<shrine> shrines;
    shrines.reserve(maxShrines);
    smokehouse smoke;
    forge dwarfForge;
    storehouse stores[raceCount];   // R29-F1: one per race, indexed by barbRace
    plot field;   // the one wheat plot (R19-F3)

    vector<deer> deers;
    vector<RectangleShape> deershape;

    vector<CircleShape> tris;
    vector<RectangleShape> stumps;
    tris.reserve(numtrees);
    stumps.reserve(numtrees);
    for (auto& t : trees) {
        tris.push_back(makeTreeTri(t.x, t.y));

        RectangleShape stump(Vector2f(4, 4));
        stump.setFillColor(stumpColor);
        stump.setPosition(Vector2f(t.x + 6, t.y + 12));
        stumps.push_back(stump);
    }

    vector<CircleShape> octs;
    octs.reserve(numrocks);
    for (auto& r : rocks) {
        CircleShape oct;
        oct.setPointCount(5);
        oct.setPosition(Vector2f(r.x, r.y));
        oct.setRadius(6);
        oct.setFillColor(Color(125, 125, 125));
        octs.push_back(oct);
    }

    spawnLakes(trees, tris, stumps, rocks, octs); // before the bushes so clumps can avoid the water

    vector<bush> bushes;
    vector<CircleShape> bushshapes;
    spawnBushes(bushes, bushshapes, trees, tris, stumps);
    checkQuarters(trees, tris, stumps, rocks, octs, bushshapes);   // R30-F1: no quarter short of wood/stone

    VertexArray treeVerts(PrimitiveType::Triangles);
    VertexArray rainVerts(PrimitiveType::Triangles); // scratch buffer for drawRain, rebuilt every frame

    RectangleShape dnc({800, 800});

    Font hudFont;
    bool hudFontOk = hudFont.openFromFile(hudFontPath);
    if (!hudFontOk) printf("HUD font failed to load: %s\n", hudFontPath);
    else printf("HUD font loaded: %s\n", hudFontPath);

    bool paused = false;
    int simSpeed = 1;
    float zoom = minZoom;
    Vector2f camCenter(worldSize / 2, worldSize / 2);
    bool panning = false;
    Vector2i lastMouse;
    View worldView(FloatRect({0.0f, 0.0f}, {worldSize, worldSize}));
    optional<trader> activeTrader;
    FloatRect tickerRect;                 // last frame's ticker strip, used to suppress hover over it
    FloatRect hudRect;                    // last frame's HUD strip, same use
    VertexArray trailVerts(PrimitiveType::Triangles);
    VertexArray paveVerts(PrimitiveType::Triangles);   // cobbled cells (R20-F2), rebuilt only when a cell is paved
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
            }
            else if (const auto* ms = event->getIf<Event::MouseWheelScrolled>()) {
                float nz = clamp(zoom * (ms->delta > 0 ? zoomStep : 1.0f / zoomStep), minZoom, maxZoom);
                Vector2f before = window.mapPixelToCoords(ms->position, worldView);
                worldView.setSize({worldSize / nz, worldSize / nz});
                Vector2f after = window.mapPixelToCoords(ms->position, worldView);
                camCenter = worldView.getCenter() + (before - after);
                zoom = nz;
            }
            else if (const auto* mp = event->getIf<Event::MouseButtonPressed>()) {
                if (mp->button == Mouse::Button::Middle) { panning = true; lastMouse = mp->position; }
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
            float half = worldSize / zoom / 2;
            camCenter.x = clamp(camCenter.x, half, worldSize - half);
            camCenter.y = clamp(camCenter.y, half, worldSize - half);
            worldView.setSize({worldSize / zoom, worldSize / zoom});
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
                buildhouse(barbs, squares, houses, houseshapes, field);
                buildshrine(barbs, squares, houses, shrines, field);
                buildsmokehouse(barbs, squares, houses, smoke);
                buildstall(barbs, squares);
                buildforge(barbs, squares, houses, dwarfForge);
                buildstorehouses(barbs, squares, houses, stores);
                buildfield(barbs, squares, houses, shrines, tris, octs, field);
                craftspear(barbs, squares);
                minematerials(squares, barbs, tris, stumps, trees, octs, rocks, bushes, bushshapes, catchRings, field, stores);
                barbmove(squares, barbs);
                shinobiTick(barbs, squares, houseshapes, octs, shinobiPuffs);
                treespawn(trees, tris, stumps, field);
                barbspawner(squares, barbs, houses, houseshapes, birthRings);
                birthRingTick(birthRings);
                catchRingTick(catchRings);
                deerspawn(deers, deershape);
                deerwander(deershape, deers, squares);
                deerkilling(deers, deershape, barbs, squares, smoke);
                wolfTick(wolves, deers, deershape, barbs);
                clockk();
                weatherTick();
                traderTick(activeTrader);
                hungerTick(barbs);
                trailTick();
                birdTick(birds, birdHeading, squares);
                bushTick(bushes, bushshapes);
                fieldTick(field);
                campfireTick(fire);
                eraTick(barbs, houses);
                paveTick(barbs, squares);
                leaderWatchTick(barbs);   // R30-F2: leader death -> global weights + ticker line, same tick
                councilTick(barbs);
            }
        }

        for (int i = (int)tris.size() - 1; i >= 0; i--) {
            Vector2f p = tris[i].getPosition();
            if (inClearing(p)) {   // village clearing or a race quarter (R30-F1)
                int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);
                for (auto& b : barbs) {
                    if (b.target != targetBush && b.targetx == tix && b.targety == tiy) {
                        b.moving = b.chopping = false;
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
                    if (b.target == targetRock && b.targetx == rix && b.targety == riy) {
                        b.moving = b.chopping = false;
                        b.timer  = 0;
                        b.targetx = b.targety = -1;
                        printf("Rock cleared near the village - miner %d dropped its target\n", b.id);
                    }
                }
                swapRemove(octs, i); swapRemove(rocks, i);
            }
        }

        window.clear(Color(4, 20, 14)); //bg color
        window.setView(worldView);
        drawTrails(window, trailVerts);
        drawPaving(window, paveVerts);   // over the trail tint, under everything else
        drawQuarters(window, stores, zoom);   // R30-F1: ground marks, under lakes, houses and crates
        drawLakes(window);
	
	for (auto& oct : octs)
		window.draw(rockDrawShape(oct));

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
	drawShrines(window, shrines);
	drawSmokehouse(window, smoke, houses);
	drawStall(window, activeTrader);
	drawForge(window, dwarfForge, frameCount);
	if (forgeEmberFrames > 0 && !paused) forgeEmberFrames--;
	drawStorehouses(window, stores);
	for (auto& st : stores) if (st.flashFrames > 0 && !paused) st.flashFrames--;
	drawField(window, field, frameCount);

	drawCampfire(window, fire, frameCount);

	drawDeer(window, deers, deershape, frameCount);

	for (auto& w : wolves)
		drawSnapped(window, w.shape);

        for (int i = 0; i < (int)tris.size(); i++) {
        	drawSnapped(window, stumps[i]);
        }

        drawBarbs(window, barbs, squares, frameCount, zoom);
	if (activeTrader) drawSnapped(window, activeTrader->shape);
	drawTrees(window, tris, treeVerts);
	drawSmoke(window, smoke, frameCount);
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
	    if (b.chopping && b.timer > 0 && b.target == targetRock)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 16), (float)b.timer / mineTicks(b), pal.barMine);
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
        if (isNightHour(t)) drawCampfireGlow(window, worldView, fire, frameCount);
        if (isNightHour(t)) drawHouseLights(window, worldView, houseshapes);
        drawBallots(window, worldView, zoom, barbs, squares); // above the overlays so the pips read true; under hover marks
        if (ballotFramesLeft > 0 && !paused) ballotFramesLeft--;
        for (int k = 0; k < techCount; k++)
            if (know.pulseFrames[k] > 0 && !paused) know.pulseFrames[k]--;   // know-how pip flash (R20-F1)

        optional<hoverHit> hover;
        bool overHud = false;
        {
            Vector2i mpx = Mouse::getPosition(window);
            Vector2u wsz = window.getSize();
            Vector2f mscreen = window.mapPixelToCoords(mpx, window.getDefaultView());
            overHud = hudFontOk && window.hasFocus() && !panning && hudRect.contains(mscreen);
            bool overUi = overHud || tickerRect.contains(mscreen);
            if (window.hasFocus() && !panning && !overUi && mpx.x >= 0 && mpx.y >= 0 && mpx.x < (int)wsz.x && mpx.y < (int)wsz.y) {
                Vector2f mworld = window.mapPixelToCoords(mpx, worldView);
                hover = pickHover(mworld, tooltipPickRadius / zoom, barbs, squares, activeTrader, wolves, deers, deershape, rocks, octs, bushes, bushshapes, houses, houseshapes);
                if (hover) drawHoverOutline(window, worldView, *hover, zoom);
                if (hover) drawHousehold(window, worldView, *hover, zoom, barbs, squares, houses, houseshapes);
            }
        }

        if (hudFontOk) {
            window.setView(window.getDefaultView());
            hudValues hv{};
            hv.wood = wood; hv.stone = stone; hv.food = food;
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
            for (int k = 0; k < techCount; k++) { hv.learned[k] = know.learned[k]; hv.pulse[k] = know.pulseFrames[k]; }
            hudRect = drawHud(window, hudFont, hv);
            tickerRect = drawEvents(window, hudFont);
            Vector2f mouse = window.mapPixelToCoords(Mouse::getPosition(window), window.getDefaultView());
            if (overHud) // full details hang just under the strip, beside the cursor
                drawTooltip(window, hudFont, hudDetailText(hv),
                            Vector2f(mouse.x, hudRect.position.y + hudRect.size.y + tooltipEdgeMargin - tooltipOffset.y), hudRect, tickerRect);
            else if (hover) drawTooltip(window, hudFont, hover->label, mouse, hudRect, tickerRect);
        }
        eventTick();
        window.display();
        frameCount++;
    }
    return 0;
}
