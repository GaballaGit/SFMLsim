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

const int numbarbs = 1;
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
const Color barbSkinColor(196, 158, 122);   // muted tan (HUD people icon)

// Kin lines: every barb rolls a race at birth. Body colour stays shared so task stripes read the same;
// races differ by body size (which is also the hitbox) and a hat mark drawn above the head.
const int rockMineTicks = 480;              // base work ticks to mine one rock
enum barbRace { raceHuman, raceDwarf, raceElf, raceCount };
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

int wood = 0;
int stone = 0;
int food = 0;
int game_time = 12;

const int stepInterval = 10;
const int rainStepInterval = 16;          // slower barb stepping in rain (~0.6x speed)
const int clearWeatherTicks = 108000;     // ticks of clear weather between rains
const int rainWeatherTicks = 36000;       // ticks a rain lasts
const Color rainOverlayColor(70, 90, 120, 70);
// Falling rain streaks: draw-only, positions hashed from (frame cycle, index) so there is no stored state
const int rainStreakCount = 150;           // sparse over the 800x800 map
const int rainStreakPixels = 3;            // 1x3 streak, bottom pixel stepped 1px left for the wind slant
const int rainStreakLifeFrames = 40;       // frames a streak falls before it respawns at a new hashed spot
const float rainStreakFallPx = 4.0f;       // px fallen per frame (~240px/s)
const float rainStreakDriftPx = 1.0f;      // px drifted left per frame, matches the slant
const Color rainStreakColor(170, 195, 225, 64); // pale blue, ~25% alpha
bool raining = false;

const int traderIntervalTicks = 216000;   // one in-game hour between trader visits
const int traderStepInterval = 6;         // ticks per 1px step while walking
const int traderTradeTicks = 300;         // ticks spent at the village center
const int tradeStoneCost = 2;             // stone paid per lot
const int tradeWoodGain = 1;              // wood received per lot
const int tradeMaxLots = 5;               // max lots per visit (caps at 5 wood/hour)
const int tradeMsgTicks = 600;            // ticks the last-trade line stays on the HUD
const Vector2f traderSize(3.0f, 3.0f);
const Color traderColor(190, 140, 230);
const Vector2f villageCenter(400.0f, 400.0f);

const int hungerThreshold = 54000;        // ticks between meals (~quarter in-game hour); deer are the only food for now
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
const Color trailPathColor(150, 125, 85, 26);
const Color trailRoadColor(150, 125, 85, 52);

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
const int deerStepInterval = 3;            // ticks per 1px step while wandering
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
// Deer sprite, offsets relative to the 6x3 logic shape (hitbox unchanged), written for a deer facing right
const Vector2f deerHeadPos(5.0f, -1.0f);   // 2x2 head overhangs the front-top corner
const Vector2f deerHeadSize(2.0f, 2.0f);
const int deerAntlerPixels = 2;            // tiny swept-back tine above the back of the head
const Vector2f deerAntlerPos[deerAntlerPixels] = {{5.0f, -2.0f}, {4.0f, -3.0f}};
const int deerLegCount = 4;
const float deerLegX[deerLegCount] = {0.0f, 1.0f, 4.0f, 5.0f}; // back pair, front pair
const float deerLegPx = 2.0f;              // planted leg length below the body
const float deerLegLiftedPx = 1.0f;        // lifted leg while walking
const int deerLegFrames = 6;               // diagonal leg pairs swap every this many frames when moving
const Color deerBodyColor(192, 133, 82);
const Color deerHeadColor(176, 120, 74);   // a shade darker so the head reads against the body
const Color deerLegColor(112, 78, 48);
const Color deerAntlerColor(178, 162, 132); // dull bone

// Campfire at villageCenter: burns wood slowly, glows at night (cosmetic only, no barb behaviour yet)
const int campfireTicksPerWood = 18000;    // 1 wood per ~5 min at 1x (12 per in-game hour): a sliver of one chopper's output
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
uniform_int_distribution<int> distrib(0, 800);
uniform_int_distribution<int> wanderChance(0, 149);
uniform_int_distribution<int> wanderDX(-50, 50);
uniform_int_distribution<int> wanderDY(-50, 50);
uniform_int_distribution<int> spawnChance(0, 7500);
uniform_int_distribution<int> treespawnChance(0, 99999);
uniform_int_distribution<int> buildSpotDX(-250, 250); //-25, 25
uniform_int_distribution<int> buildSpotDY(-250, 250); //-25, 25
uniform_int_distribution<int> DeerSpawnY(20, 780); 
uniform_int_distribution<int> DeerSpawnRL(0, 1);
uniform_int_distribution<int> DeerSpawnChance(0, 100);
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

struct house {int x, y, health; bool hasFoundation = false; int id = -1;}; // id: stable, from nextHouseId
int nextHouseId = 0;   // assigned only in buildhouse(); never reused, so ids survive any swapRemove

struct barb {
    int id = -1;                 // stable, unique; assigned only by makeBarb()
    int x, y, health = 100;
    bool moving = false;
    int targetx = -1, targety = -1;
    int timer = 0;
    bool chopping = false;
    bool movingbspot = false;
    bool building = false;
    bool reservedwood = false;
    int reservedstone = 0;       // stone held for a house foundation; refunded with the wood on cancel
    int stepTimer = 0;
    bool targetIsRock = false;
    bool targetIsBush = false;   // chopping + targetIsBush = picking; shares timer/chopping with chop and mine
    int targetBush = -1;         // bush index (bushes are never removed, so indices are stable)
    bool deertargeting = false;
    int targetDeerId = -1;
    int hunger = 0;
    int slowTick = 0;
    barbRace race = raceHuman;   // rolled once in makeBarb()
    int homeHouse = -1;          // house.id of the birth house (stable id, not an index); -1 = founder
};

const raceDef& raceOf(const barb& b) { return raceDefs[b.race]; }
int mineTicks(const barb& b) { return (int)(rockMineTicks / raceOf(b).mineSpeed); }

struct deer {
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

struct gameEvent {string text; int age = 0;};
deque<gameEvent> events;

void logEvent(const string& text) {
    events.push_back({text, 0});
    while ((int)events.size() > eventMaxLines) events.pop_front();
}

// Ages ticker lines once per rendered frame so fade time is real-time regardless of sim speed
void eventTick() {
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
        Color c = eventTextColor;
        c.a = eventFade(events[i], eventTextAlpha);
        t.setFillColor(c);
        t.setPosition(topLeft + eventPad + Vector2f(0.0f, i * lineH));
        window.draw(t);
    }
    return FloatRect(topLeft, {width, height});
}

struct tree  {int x, y, health; bool dying = false;};
struct rock  {int x, y, health; bool dying = false;};
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

// Priority when flags overlap: hunting > building > picking > mining > chopping > idle
Color barbColor(const barb& b) {
    if (b.deertargeting) return barbHuntColor;
    if (b.building) return barbBuildColor;
    if (b.chopping && b.targetIsBush) return barbPickColor;
    if (b.chopping && b.targetIsRock) return barbMineColor;
    if (b.chopping) return barbChopColor;
    return barbIdleColor;
}

// Same priority as barbColor() so the tooltip always matches the colour on screen
const char* barbTaskName(const barb& b) {
    if (b.deertargeting) return "hunting";
    if (b.building) return "building";
    if (b.chopping && b.targetIsBush) return "picking";
    if (b.chopping && b.targetIsRock) return "mining";
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
}

// Any barb whose target deer no longer exists stops hunting. Safe to call after any deer removal.
void validateDeerTargets(vector<barb>& barbs, const vector<deer>& deers) {
	for (auto& b : barbs)
		if (b.deertargeting && findDeerById(deers, b.targetDeerId) == -1) clearHunt(b);
}

void deerkilling(vector<deer>& deers, vector<RectangleShape>& deershape, vector<barb>& barbs, vector<RectangleShape>& squares) {
	const float deerInfluenceDSq = 625.0f;
	validateDeerTargets(barbs, deers);
	for (int i = 0; i < (int)deers.size(); ) {
		auto& ds = deershape[i];
		auto& d = deers[i];
		Vector2f deerpos = ds.getPosition();
		if (d.invincible) {
			ds.setFillColor(d.invincibleTimer > 50 ? Color::Red : Color(192, 133, 82));
			if (--d.invincibleTimer <= 0) {
				d.invincible = false;
				ds.setFillColor(Color(192, 133, 82));
			}
		}
		bool removed = false;
		for (int j = 0; j < (int)barbs.size(); ++j) {
			auto& b = barbs[j];
			auto& sq = squares[j];
			if (b.building || b.movingbspot || b.chopping || b.timer > 0) continue;
			if (!b.deertargeting) {
				Vector2f bpos = sq.getPosition();
				float dx = deerpos.x - bpos.x, dy = deerpos.y - bpos.y;
				if (dx * dx + dy * dy >= deerInfluenceDSq) continue;
				b.deertargeting = true;
				b.targetDeerId = d.id;
			} else if (b.targetDeerId != d.id) {
				continue;
			}
			b.targetx = (int)deerpos.x;
			b.targety = (int)deerpos.y;
			b.moving = true;
			if (!d.invincible && sq.getGlobalBounds().findIntersection(ds.getGlobalBounds()).has_value()) {
				d.health -= 5;
				d.invincible = true;
				d.invincibleTimer = 60;
				ds.setFillColor(Color::Red);
				printf("Deer Health: %d\n", d.health);
				if (d.health <= 0) {
					swapRemove(deers, i);      // d and ds are invalid from here on
					swapRemove(deershape, i);
					removed = true;
					food++;
					printf("DEER KILLED - Food: %d\n", food);
					logEvent("Deer killed (food " + to_string(food) + ")");
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
	if ((int)wolves.size() < maxWolves && (int)deers.size() > wolfMinDeer
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
		if (w.phase == wolf::Hunting && (int)deers.size() <= wolfMinDeer) w.phase = wolf::Leaving;
		Vector2f goal = w.exit;
		int prey = -1;
		if (w.phase == wolf::Hunting) {
			prey = findNearest(pos, noSearchRadius, (int)deershape.size(),
			                   [&](int k) { return deershape[k].getPosition(); },
			                   [](int) { return false; });
			if (prey != -1) goal = deershape[prey].getPosition();
		}
		if (prey != -1 && w.shape.getGlobalBounds().findIntersection(deershape[prey].getGlobalBounds()).has_value()) {
			swapRemove(deers, prey);
			swapRemove(deershape, prey);
			validateDeerTargets(barbs, deers);   // no barb keeps hunting the dead deer
			w.phase = wolf::Leaving;
			printf("Wolf killed a deer - deer left: %zu\n", deers.size());
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

void deerspawn(vector<deer>& deers, vector<RectangleShape>& deershape) {
	if (deers.size() >= 10) return;
	deer d;
	if (DeerSpawnChance(gen) != 0) return;
	d.id = nextDeerId++;
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
	deers.push_back(d);
	RectangleShape ds;
	ds.setPosition(Vector2f(d.x, d.y));
	ds.setSize(Vector2f(6, 3));
	FloatRect deerHitbox = ds.getGlobalBounds();
	ds.setFillColor(Color(192, 133, 82));
	deershape.push_back(ds);
	printf("%zu\n", deers.size());
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
                goal.y = clamp(goal.y, hudBarHeight + deerFleeEdgeMargin, worldSize - size.y - deerLegPx - deerFleeEdgeMargin);
                d.targetx = (int)goal.x;
                d.targety = (int)goal.y;
                d.moving = true;
                d.stepTimer = 0;
                d.fleeTimer = deerFleeTicks;
                printf("Deer %d fleeing\n", d.id);
            }
        }
        if (!d.moving) {
            if (wanderChance(gen) != 0) continue;
            int tx = (int)pos.x + wanderDX(gen);
            int ty = (int)pos.y + wanderDY(gen);
            if (tx > 0 && tx < 800 && ty > 0 && ty < 800) {
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
            } else if (++d.stepTimer >= (d.fleeTimer > 0 ? deerFleeStepInterval : deerStepInterval)) {
                d.stepTimer = 0;
                ds.move(diff / sqrt(dSq));
		ds.getGlobalBounds();
            }
        }
    }
}

 // 216000
int clockk() {
    static int timer = 0;
    if (++timer >= 216000) {
        game_time++;
        printf("The time is now: %d:00\n", game_time);
        if (game_time >=24) {
            game_time = -1;
        }
        timer = 0;
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

bool isHungry(const barb& b) { return b.hunger >= hungerThreshold; }

int stepLimit(const barb& b) { return stepLimit() * (isHungry(b) ? hungerWorkSlowdown : 1); }

// Worn trails: per-cell wear from barb steps, decaying over time. Tiers: 0 none, 1 path, 2 road.
vector<int> trailWear(trailGridN * trailGridN, 0);
vector<uint8_t> trailTier(trailGridN * trailGridN, 0);
bool trailDirty = false;

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
        if (trailWear[c] == 0) continue;
        trailWear[c]--;
        trailSetTier(c);
    }
}

bool onRoad(Vector2f p) {
    int c = trailCell(p);
    return c >= 0 && trailTier[c] == 2;
}

int stepLimit(const barb& b, Vector2f pos) { return max(1, stepLimit(b) - (onRoad(pos) ? roadStepBonus : 0)); }

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
    for (auto& b : barbs) {
        if (!isHungry(b)) {
            b.hunger++;
            if (isHungry(b) && food <= 0 && hungryLogCooldown == 0) {
                logEvent("A barb is hungry - no food");
                hungryLogCooldown = eventHungryCooldownTicks;
            }
            continue;
        }
        if (food <= 0) continue;
        food--;
        b.hunger = 0;
        b.slowTick = 0;
        printf("BARB ATE - Food: %d\n", food);
    }
}

Color hungryTint(Color c) {
    auto mix = [](uint8_t a, uint8_t t) { return (uint8_t)(a + (t - a) * hungryTintAmount); };
    return Color(mix(c.r, hungryTintColor.r), mix(c.g, hungryTintColor.g), mix(c.b, hungryTintColor.b));
}

// Read-only on game state: squares stay the hitbox, the sprite is built from temporary shapes.
// Plain white 2x4 rectangle (no head/hair); the task shows as a 2x1 stripe and a tinted tool pixel.
// Pose comes from existing flags only: walking bob, chop/mine/build tool tick, pick arm dip, hungry droop.
void drawBarbs(RenderWindow& window, const vector<barb>& barbs, const vector<RectangleShape>& squares, int frame) {
    RectangleShape body(barbBodySize), stripe(Vector2f(barbBodySize.x, barbStripeH)), tool(barbToolSize), hat, tip;
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
        body.setSize(Vector2f(bodySize.x, bodyH));
        body.setFillColor(hungry ? hungryTint(barbBodyColor) : barbBodyColor);
        body.setPosition(Vector2f(base.x, bodyTop));
        window.draw(body);
        if (!idle) {
            stripe.setSize(Vector2f(bodySize.x, barbStripeH));
            stripe.setFillColor(tc);
            stripe.setPosition(Vector2f(base.x, bodyTop + barbStripeTop));
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

        float side = base.x + bodySize.x;                // tool/arm sits just right of the body
        tool.setFillColor(tc);
        if (b.chopping && b.targetIsBush) {
            bool dip = (phase / barbPickFrames) % 2 == 1;
            tool.setPosition(Vector2f(side, dip ? feet - barbToolSize.y : bodyTop + 1.0f));
            window.draw(tool);
        } else if (working && b.timer > 0) {
            bool raised = (phase / barbToolFrames) % 2 == 0;
            tool.setPosition(Vector2f(side, raised ? bodyTop - barbToolSize.y : bodyTop + 1.0f));
            window.draw(tool);
        }
    }
}

// Roofed houses: slab is drawn separately underneath. Window is dark glass here; drawHouseLights lights it at night.
void drawHouses(RenderWindow& window, const vector<RectangleShape>& houseshapes) {
    RectangleShape r;
    for (auto& hs : houseshapes) {
        Vector2f o = snapPx(hs.getPosition());
        r.setFillColor(houseRoofColor);
        for (int k = 0; k < houseRoofRows; k++) {
            r.setSize(Vector2f(houseRoofWidth[k], 1.0f));
            r.setPosition(o + Vector2f(houseRoofLeft[k], houseRoofTop + k));
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

// Deer with head, antler and legs over the unchanged 6x3 hitbox. Facing and leg swap come from existing state + frame.
void drawDeer(RenderWindow& window, const vector<deer>& deers, const vector<RectangleShape>& deershape, int frame) {
    RectangleShape r;
    for (int i = 0; i < (int)deershape.size() && i < (int)deers.size(); i++) {
        const deer& d = deers[i];
        const RectangleShape& ds = deershape[i];
        Vector2f o = snapPx(ds.getPosition());
        float bw = ds.getSize().x, bh = ds.getSize().y;
        bool left = d.moving ? d.targetx < ds.getPosition().x : d.id % 2 == 1;
        // mirror a right-facing x offset (and width) across the body
        auto at = [&](Vector2f p, float w) { return o + Vector2f(left ? bw - p.x - w : p.x, p.y); };
        Color body = ds.getFillColor();                  // keeps the red hit flash from deerkilling
        bool flashing = body != deerBodyColor;

        int legPhase = ((frame + d.id * 4) / deerLegFrames) % 2;
        r.setFillColor(deerLegColor);
        for (int k = 0; k < deerLegCount; k++) {
            bool lifted = d.moving && (k % 2) == legPhase; // diagonal pairs (0+2, 1+3) alternate
            r.setSize(Vector2f(1.0f, lifted ? deerLegLiftedPx : deerLegPx));
            r.setPosition(at(Vector2f(deerLegX[k], bh), 1.0f));
            window.draw(r);
        }
        r.setSize(Vector2f(bw, bh));
        r.setPosition(o);
        r.setFillColor(body);
        window.draw(r);
        r.setSize(deerHeadSize);
        r.setPosition(at(deerHeadPos, deerHeadSize.x));
        r.setFillColor(flashing ? body : deerHeadColor);
        window.draw(r);
        r.setSize(Vector2f(1.0f, 1.0f));
        r.setFillColor(deerAntlerColor);
        for (int k = 0; k < deerAntlerPixels; k++) {
            r.setPosition(at(deerAntlerPos[k], 1.0f));
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
        int lots = min(stone / tradeStoneCost, tradeMaxLots);
        stone -= lots * tradeStoneCost;
        wood  += lots * tradeWoodGain;
        if (lots > 0) snprintf(tradeMsg, sizeof(tradeMsg), "Traded: -%d stone +%d wood", lots * tradeStoneCost, lots * tradeWoodGain);
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
            string label = string(raceOf(b).name) + ": " + barbTaskName(b) + (isHungry(b) ? " (hungry)" : "");
            if (b.homeHouse >= 0) {   // kin counted here, on hover only
                int kin = 0;
                for (auto& o : barbs) if (o.id != b.id && o.homeHouse == b.homeHouse) kin++;
                label += ", of house " + to_string(b.homeHouse + 1) + ", " + to_string(kin) + " kin";
            }
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
        if (near(deershape[i])) return hoverHit{"Deer " + to_string(deers[i].health) + "hp", deershape[i], nullopt};
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
            Vector2f tl(o.x + houseRoofLeft[houseRoofRows - 1], o.y + houseRoofTop);
            Vector2f br(o.x + houseRoofLeft[houseRoofRows - 1] + houseRoofWidth[houseRoofRows - 1], o.y + houseWallPos.y + houseWallSize.y);
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
    bool night, paused, raining;
    const char* trader;      // Away / Arriving / Trading / Leaving
    const char* tradeLine;   // last trade message, "" when expired
    const char* era;         // settlement rank: Camp / Hamlet / Town
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
    char buf[340];
    snprintf(buf, sizeof(buf), "Settlement: %s\nWood: %d\nStone: %d\nFood: %d\nHungry: %d\nPopulation: %d\nHouses: %d\nTime: %02d:00 (%s)\nSpeed: %dx%s\nWeather: %s\nTrader: %s%s%s",
             v.era, v.wood, v.stone, v.food, v.hungry, v.people, v.houses, v.hour, v.night ? "Night" : "Day", v.speed, v.paused ? " PAUSED" : "",
             v.raining ? "Rain" : "Clear", v.trader, v.tradeLine[0] ? "\n" : "", v.tradeLine);
    return buf;
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

void buildhouse(vector<barb>& barbs,vector<RectangleShape>& squares, vector<house>& houses, vector<RectangleShape>& houseshapes) {
    bool canclaim = (int)houses.size() < (int)barbs.size();
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (!canclaim || wood < houseWoodCost) continue;
            wood -= houseWoodCost;
            b.reservedwood = true;
            b.reservedstone = stone >= houseStoneCost ? houseStoneCost : 0; // 0 stone -> plain house
            stone -= b.reservedstone;
            b.targetx = 400 + buildSpotDX(gen);
            b.targety = 400 + buildSpotDY(gen);
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
                if (b.timer >= 600) {
                    b.reservedwood = false;
                    house h{b.targetx, b.targety, 100, b.reservedstone > 0, nextHouseId++};
                    b.reservedstone = 0;
                    houses.push_back(h);
                    RectangleShape hs(Vector2f(4, 4));
                    hs.setPosition(Vector2f(h.x, h.y));
                    hs.setFillColor(houseWallColor);
                    hs.setOutlineColor(Color(54, 35, 16));
                    hs.setOutlineThickness(1);
                    houseshapes.push_back(hs);
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

struct birthRing {Vector2f pos; int age = 0;};

int nextBarbId = 0;

// The only place a barb is created: keeps barbs/squares in sync and owns nextBarbId
void makeBarb(Vector2i pos, vector<barb>& barbs, vector<RectangleShape>& squares, vector<birthRing>& rings, int homeHouse = -1) {
    barb b;
    b.id = nextBarbId++;
    b.homeHouse = homeHouse;
    b.x = pos.x;
    b.y = pos.y;
    b.race = (barbRace)uniform_int_distribution<int>(0, raceCount - 1)(gen);
    barbs.push_back(b);
    Vector2f bodySize = raceOf(b).bodySize;   // hitbox = drawn body, so hover/collision match what you see
    RectangleShape sq;
    sq.setPosition(Vector2f(b.x, b.y));
    sq.setSize(bodySize);
    sq.setFillColor(Color::White);
    squares.push_back(sq);
    rings.push_back({Vector2f(b.x, b.y) + bodySize / 2.0f}); // centred on the body
    printf("BARB BORN - %s\n", raceOf(b).name);
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

councilDecree currentDecree = decreeNone;   // none until the first dawn

// Ballot: race lean plus colony needs (wood short of a house, stone short of a foundation, this barb hungry or no food)
councilDecree castBallot(const barb& b) {
    float score[decreeCount] = {0.0f, 0.0f, 0.0f};
    score[raceLeans[b.race]] += councilRaceLean;
    score[decreeTimber]  += councilNeedLean * clamp((float)(houseWoodCost - wood) / (float)houseWoodCost, 0.0f, 1.0f);
    score[decreeStone]   += councilNeedLean * clamp((float)(houseStoneCost - stone) / (float)houseStoneCost, 0.0f, 1.0f);
    score[decreeBerries] += (isHungry(b) || food <= 0) ? councilNeedLean : 0.0f;
    int best = raceLeans[b.race];               // ties go to the barb's own lean
    for (int d = 0; d < decreeCount; d++) if (score[d] > score[best]) best = d;
    return (councilDecree)best;
}

void councilTick(const vector<barb>& barbs) {
    static int lastHour = game_time;
    if (game_time == lastHour) return;
    lastHour = game_time;
    if (game_time != councilDawnHour || barbs.empty()) return;
    int votes[decreeCount] = {0, 0, 0};
    for (auto& b : barbs) votes[castBallot(b)]++;
    int top = *max_element(votes, votes + decreeCount);
    vector<int> tied;
    for (int d = 0; d < decreeCount; d++) if (votes[d] == top) tied.push_back(d);
    currentDecree = (councilDecree)tied[uniform_int_distribution<int>(0, (int)tied.size() - 1)(gen)];
    printf("DAWN COUNCIL - Timber %d, Stone %d, Berries %d -> %s\n", votes[decreeTimber], votes[decreeStone], votes[decreeBerries], decreeNames[currentDecree]);
    logEvent(string("Council decrees ") + decreeNames[currentDecree] + " (" + to_string(votes[decreeTimber]) + "-" +
             to_string(votes[decreeStone]) + "-" + to_string(votes[decreeBerries]) + ")");
}

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, const vector<house>& houses, vector<RectangleShape>& houseshapes, vector<birthRing>& rings) {
    for (int i = 0; i < (int)houseshapes.size() && i < (int)houses.size(); i++) {
        if (spawnChance(gen) != 0) continue;
        Vector2f pos = houseshapes[i].getPosition();
        makeBarb(Vector2i((int)(pos.x + 4), (int)(pos.y + 13)), barbs, squares, rings, houses[i].id);
        logEvent(string(raceOf(barbs.back()).birthName) + " is born (pop " + to_string(barbs.size()) + ")");
    }
}

void barbmove(vector<RectangleShape>& squares, vector<barb>& barbs) {
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (b.chopping || b.timer > 0 || b.movingbspot || b.building) continue;
        Vector2f pos = sq.getPosition();
        if (!b.moving) {
            if (wanderChance(gen) != 0) continue;
            int tx = (int)pos.x + wanderDX(gen);
            int ty = (int)pos.y + wanderDY(gen);
            if (tx > 0 && tx < 800 && ty > 0 && ty < 800) {
                b.targetx = tx;
                b.targety = ty;
                b.moving  = true;
            }
        } else {
            Vector2f diff = Vector2f(b.targetx, b.targety) - pos;
            float dSq = diff.x * diff.x + diff.y * diff.y;
            if (dSq < 1.0f) {
                b.moving    = false;
                b.stepTimer = 0;
            } else if (++b.stepTimer >= stepLimit(b, pos)) {
                b.stepTimer = 0;
                sq.move(diff / sqrt(dSq));
                trailStep(sq.getPosition());
		sq.getGlobalBounds();
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
        if (b.targetx == -1 || b.targetIsBush || b.deertargeting || b.movingbspot || b.building) continue;
        (b.targetIsRock ? rockSpots : treeSpots).emplace(workSpotKey(b.targetx, b.targety), j); // first claimant kept
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

void minematerials(vector<RectangleShape>& squares, vector<barb>& barbs, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<tree>& trees, vector<CircleShape>& octs, vector<rock>& rocks,
                   vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    vector<int> treeClaim, rockClaim;   // parallel to trees/tris and rocks/octs for this tick
    buildWorkClaims(barbs, tris, octs, treeClaim, rockClaim);
    for (int j = 0; j < (int)squares.size(); j++) {
        auto& sq = squares[j];
        auto& b  = barbs[j];
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
            if (bestIsBush && b.moving==false) {
                Vector2f wp = bushWorkSpot(bushshapes[bestBush]);
                b.targetx = (int)wp.x;
                b.targety = (int)wp.y;
                b.moving  = true;
                b.targetIsRock = false;
                b.targetIsBush = true;
                b.targetBush = bestBush;
            }
            if (best != -1 && !bestIsBush && !bestIsRock && b.moving==false) {
                Vector2f tp = tris[best].getPosition();
                b.targetx = (int)(tp.x + 8);
                b.targety = (int)(tp.y + 15);
                b.moving  = true;
                b.targetIsRock = false;
                b.targetIsBush = false;
                treeClaim[best] = j;
            }
	    if (best != -1 && !bestIsBush && bestIsRock && b.moving==false) {
		    Vector2f rp = octs[best].getPosition();
		    b.targetx = (int)(rp.x + 6);
		    b.targety = (int)(rp.y + 10);
		    b.moving = true;
		    b.targetIsRock = true;
		    b.targetIsBush = false;
		    rockClaim[best] = j;
        }
        if (b.targetx == -1) continue;
	}
	if (b.targetIsBush) {
            int i = b.targetBush;
            if (i < 0 || i >= (int)bushes.size()) continue;
            Vector2f wp = bushWorkSpot(bushshapes[i]);
            if (b.targetx != (int)wp.x || b.targety != (int)wp.y) continue; // retargeted (e.g. a hunt)
            if ((!bushes[i].ripe || bushes[i].picking) && !b.chopping) {   // someone else got there first
                b.moving = false;
                b.targetx = b.targety = -1;
                b.targetIsBush = false;
                b.targetBush = -1;
                continue;
            }
            float dx = wp.x - bpos.x, dy = wp.y - bpos.y;
            if (dx * dx + dy * dy >= 4.0f) continue;
            b.chopping = true;
            bushes[i].picking = true;
            if (workTick(b)) b.timer++;
            if (b.timer >= bushPickTicks) {
                food++;
                bushes[i].ripe = bushes[i].picking = false;
                bushes[i].regrow = 0;
                bushshapes[i].setFillColor(bushBareColor);
                b.moving = b.chopping = false;
                b.timer = 0;
                b.targetx = b.targety = -1;
                b.targetIsBush = false;
                b.targetBush = -1;
                printf("BERRIES PICKED - food: %d\n", food);
            }
            continue;
	}
	if (!b.targetIsRock) {
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
            if (b.timer >= 180) {
                wood++;
                swapRemove(trees,  i);
                swapRemove(tris,   i);
                swapRemove(stumps, i);
                swapRemove(treeClaim, i);
                b.moving = b.chopping = false;
                b.timer  = 0;
                b.targetx = b.targety = -1;
                printf("TREE DESTROYED - wood: %d\n", wood);
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
				stone++;
				swapRemove(rocks, i);
				swapRemove(octs, i);
				swapRemove(rockClaim, i);
				b.moving = b.chopping = false;
				b.timer = 0;
				b.targetx = b.targety = -1;
				printf("ROCK DESTROYED - stone: %d\n", stone);
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

void treespawn(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps) {
    if (treespawnChance(gen) != 0) return;
    tree t{distrib(gen), distrib(gen), 100, false};
    trees.push_back(t);

    tris.push_back(makeTreeTri(t.x, t.y));

    RectangleShape stump(Vector2f(4, 4));
    stump.setFillColor(Color(128, 26, 0));
    stump.setPosition(Vector2f(t.x + 6, t.y + 12));
    stumps.push_back(stump);
}

// A few clumps of bushes, away from the village center. Startup trees overlapping a bush are
// cleared (trees, tris and stumps together) so the clumps sit in small clearings and stay visible.
void spawnBushes(vector<bush>& bushes, vector<CircleShape>& bushshapes, vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps) {
    uniform_int_distribution<int> centre(60, 740);
    uniform_real_distribution<float> spread(-bushClumpSpread, bushClumpSpread);
    for (int c = 0; c < numBushClumps; c++) {
        Vector2f cp;
        do cp = Vector2f((float)centre(gen), (float)centre(gen));
        while (hypot(cp.x - villageCenter.x, cp.y - villageCenter.y) < bushClumpMinCenterDist);
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

    vector<barb> barbs;
    vector<RectangleShape> squares;
    vector<birthRing> birthRings;
    barbs.reserve(numbarbs);
    squares.reserve(numbarbs);
    {
        uniform_int_distribution<int> d(350, 450);
        for (int i = 0; i < numbarbs; i++) {
            int x = d(gen), y = d(gen);
            makeBarb(Vector2i(x, y), barbs, squares, birthRings);
        }
    }

    vector<tree> trees(numtrees);
    for (auto& t : trees) {t.x = distrib(gen); t.y = distrib(gen); t.health = 100;}

    vector<rock> rocks(numrocks);
    for (auto& r : rocks) {r.x = distrib(gen); r.y = distrib(gen); r.health = 100;}

    vector<house> houses;
    vector<RectangleShape> houseshapes;

    vector<deer> deers;
    vector<RectangleShape> deershape;

    vector<CircleShape> tris;
    vector<RectangleShape> stumps;
    tris.reserve(numtrees);
    stumps.reserve(numtrees);
    for (auto& t : trees) {
        tris.push_back(makeTreeTri(t.x, t.y));

        RectangleShape stump(Vector2f(4, 4));
        stump.setFillColor(Color(115, 30, 2));
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

    vector<bush> bushes;
    vector<CircleShape> bushshapes;
    spawnBushes(bushes, bushshapes, trees, tris, stumps);

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
                buildhouse(barbs, squares, houses, houseshapes);
                minematerials(squares, barbs, tris, stumps, trees, octs, rocks, bushes, bushshapes);
                barbmove(squares, barbs);
                treespawn(trees, tris, stumps);
                barbspawner(squares, barbs, houses, houseshapes, birthRings);
                birthRingTick(birthRings);
                deerspawn(deers, deershape);
                deerwander(deershape, deers, squares);
                deerkilling(deers, deershape, barbs, squares);
                wolfTick(wolves, deers, deershape, barbs);
                clockk();
                weatherTick();
                traderTick(activeTrader);
                hungerTick(barbs);
                trailTick();
                birdTick(birds, birdHeading, squares);
                bushTick(bushes, bushshapes);
                campfireTick(fire);
                eraTick(barbs, houses);
                councilTick(barbs);
            }
        }

        for (int i = (int)tris.size() - 1; i >= 0; i--) {
            Vector2f p = tris[i].getPosition();
            float dx = p.x - 400, dy = p.y - 400;
            if (dx*dx + dy*dy < 2500.0f) { // 50^2
                int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);
                for (auto& b : barbs) {
                    if (!b.targetIsBush && b.targetx == tix && b.targety == tiy) {
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
            float dx = p.x - 400, dy = p.y - 400;
            if (dx*dx + dy*dy < 2500.0f) { // 100^2
                swapRemove(octs, i); swapRemove(rocks, i);
            }
        }

        window.clear(Color(4, 20, 14)); //bg color
        window.setView(worldView);
        drawTrails(window, trailVerts);
	
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
	drawHouses(window, houseshapes);

	drawCampfire(window, fire, frameCount);

	drawDeer(window, deers, deershape, frameCount);

	for (auto& w : wolves)
		drawSnapped(window, w.shape);

        for (int i = 0; i < (int)tris.size(); i++) {
        	drawSnapped(window, stumps[i]);
        }

        drawBarbs(window, barbs, squares, frameCount);
	if (activeTrader) drawSnapped(window, activeTrader->shape);
	drawTrees(window, tris, treeVerts);
	drawBirds(window, birds, birdShape, frameCount++);
	if (raining) drawRain(window, rainVerts, frameCount);
	for (int q = 0; q < (int)squares.size(); ++q) {
            auto& b = barbs[q];
            if (b.chopping && b.timer > 0 && !b.targetIsRock && !b.targetIsBush)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 20), b.timer / 180.0f, pal.barChop);
            if (b.chopping && b.timer > 0 && b.targetIsBush)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 14), (float)b.timer / bushPickTicks, pal.barPick);
            if (b.building &&  b.timer > 0)
                drawBar(window, Vector2f(b.targetx - 9, b.targety - 10), b.timer / 600.0f, pal.barBuild);
	    if (b.chopping && b.timer > 0 && b.targetIsRock)
                drawBar(window, Vector2f(b.targetx - 10, b.targety - 16), (float)b.timer / mineTicks(b), pal.barMine);
	}
	drawBirthRings(window, birthRings);

        int t = game_time;
        Color dncColor;
        if (t >= 21 || t < 6)      dncColor = Color(4, 7, 32, 150);      // Night
        else if (t < 12)           dncColor = Color(250, 231, 190, 30);  // Morning
        else if (t < 18)           dncColor = Color(233, 255, 253, 30);  // Midday
        else                       dncColor = Color(255, 178, 127, 50);  // Evening
        drawOverlay(window, dnc, dncColor);
        if (raining) drawOverlay(window, dnc, rainOverlayColor);
        if (t >= 21 || t < 6) drawCampfireGlow(window, worldView, fire, frameCount);
        if (t >= 21 || t < 6) drawHouseLights(window, worldView, houseshapes);

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
            hv.people = (int)barbs.size();
            hv.houses = (int)houses.size();
            hv.hour = t < 0 ? 0 : t;
            hv.speed = simSpeed;
            hv.night = (t >= 21 || t < 6);
            hv.paused = paused;
            hv.raining = raining;
            hv.trader = !activeTrader ? "Away"
                : activeTrader->phase == trader::Arriving ? "Arriving"
                : activeTrader->phase == trader::Trading ? "Trading" : "Leaving";
            hv.tradeLine = tradeMsgTimer > 0 ? tradeMsg : "";
            hv.era = eraNames[currentEra];
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
    }
    return 0;
}
