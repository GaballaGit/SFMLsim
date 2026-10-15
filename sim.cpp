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

using namespace sf;
using namespace std;

const int numbarbs = 1;
const int numtrees = 2000;
const int numrocks = 100;

const char* const hudFontPath = "assets/DejaVuSans.ttf";
const unsigned int hudFontSize = 14;
const Vector2f hudPos(6.0f, 6.0f);
const Vector2f hudPanelSize(150.0f, 148.0f);
const Vector2f hudTextOffset(6.0f, 4.0f);
const int maxTicksPerFrame = 20;
const float worldSize = 800.0f;
const float minZoom = 1.0f;
const float maxZoom = 4.0f;
const float zoomStep = 1.1f;
const Color hudPanelColor(0, 0, 0, 160);

const Color barbIdleColor(255, 255, 255);
const Color barbChopColor(120, 220, 90);
const Color barbMineColor(255, 165, 0);
const Color barbBuildColor(80, 170, 255);
const Color barbHuntColor(255, 70, 70);

int wood = 0;
int stone = 0;
int food = 0;
int game_time = 12;

const int stepInterval = 10;
const int rainStepInterval = 16;          // slower barb stepping in rain (~0.6x speed)
const int clearWeatherTicks = 108000;     // ticks of clear weather between rains
const int rainWeatherTicks = 36000;       // ticks a rain lasts
const Color rainOverlayColor(70, 90, 120, 70);
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
const int trailMaxWear = 240;              // cap so abandoned roads fade within ~8 min
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

const float barbWorkSearchRadius = 25.0f;  // barbs only notice trees/rocks within this many px (findNearest)
const float noSearchRadius = numeric_limits<float>::infinity(); // findNearest with no distance limit
const Vector2f treeWorkOffset(8.0f, 15.0f); // where a barb stands to chop a tree, relative to its shape
const Vector2f rockWorkOffset(6.0f, 10.0f); // where a barb stands to mine a rock, relative to its shape

const int numBushClumps = 4;
const int bushesPerClump = 5;
const float bushClumpSpread = 14.0f;       // max px from the clump centre on each axis
const float bushClumpMinCenterDist = 120.0f; // clumps stay clear of the village center (and its tree clearing)
const float bushTreeClearance = 3.0f;      // startup trees overlapping a bush (plus this margin) are removed
const float bushRadius = 3.0f;             // 6px bush: bigger than a barb, well under a 16px tree
const Vector2f bushWorkOffset(3.0f, 7.0f); // where a barb stands to pick a bush (just below it)
const int bushPickTicks = 180;             // ~3s to pick, +1 food
const int bushRegrowTicks = 7200;          // ~2 min for a bare bush to ripen again
const Color bushRipeColor(150, 55, 95);    // muted berry red-violet, darker than every barb task colour
const Color bushBareColor(62, 78, 46);     // dull olive: readable on the dark ground, distinct from tree green
const Color barbPickColor(255, 140, 190);  // pink: distinct from the other task colours and the trader

const int birthRingTicks = 40;             // ~0.7s lifetime
const float birthRingStartRadius = 1.5f;   // starts just around the 2px barb
const float birthRingEndRadius = 8.0f;     // ends about half a tree wide
const float birthRingThickness = 1.0f;
const Color birthRingColor(225, 220, 190); // pale warm cream, matches ticker text
const uint8_t birthRingMaxAlpha = 70;      // faint at birth, fades linearly to 0

const int houseWoodCost = 30;
const int houseStoneCost = 2;              // small; with less stone a plain house is built instead
const Vector2f houseSlabSize(10.0f, 10.0f); // centred under the 4px house (6px with outline): 2px rim shows
const Color houseSlabColor(50, 56, 52);    // dim grey, low contrast on the dark ground, darker than rocks

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

struct house {int x, y, health; bool hasFoundation = false;};

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
};

struct deer {
int x, y;
int id = -1;
int health = 100;
int stepTimer = 0;
bool moving = false;
int targetx = -1, targety = -1;
bool invincible = false;
int invincibleTimer = 0;
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

void deerwander(vector<RectangleShape>& deershape, vector<deer>& deers) {
    for (int i = 0; i < (int)deershape.size(); i++) {
        auto& ds = deershape[i];
        auto& d  = deers[i];
        Vector2f pos = ds.getPosition();
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
                d.moving    = false;
                d.stepTimer = 0;
            } else if (++d.stepTimer >= 3) {
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
        printf(raining ? "Rain starts\n" : "Rain stops\n");
        logEvent(raining ? "Rain starts" : "Rain stops");
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
    if (++dtimer < trailDecayIntervalTicks) return;
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

struct hoverHit {string label; optional<RectangleShape> rect; optional<CircleShape> circle;};

float rectDistSq(const FloatRect& r, Vector2f p) {
    float dx = max(max(r.position.x - p.x, p.x - (r.position.x + r.size.x)), 0.0f);
    float dy = max(max(r.position.y - p.y, p.y - (r.position.y + r.size.y)), 0.0f);
    return dx * dx + dy * dy;
}

// Fresh scan each frame, no stored selection. Returns the first hit: barbs, trader, wolves, deer, rocks, bushes, houses.
optional<hoverHit> pickHover(Vector2f p, float radius, const vector<barb>& barbs, const vector<RectangleShape>& squares,
                             const optional<trader>& tr, const vector<wolf>& wolves, const vector<deer>& deers, const vector<RectangleShape>& deershape,
                             const vector<rock>& rocks, const vector<CircleShape>& octs,
                             const vector<bush>& bushes, const vector<CircleShape>& bushshapes, const vector<RectangleShape>& houseshapes) {
    float rSq = radius * radius;
    auto near = [&](const Shape& s) { return rectDistSq(s.getGlobalBounds(), p) <= rSq; };
    for (int i = 0; i < (int)squares.size() && i < (int)barbs.size(); i++)
        if (near(squares[i]))
            return hoverHit{string("Barb: ") + barbTaskName(barbs[i]) + (isHungry(barbs[i]) ? " (hungry)" : ""), squares[i], nullopt};
    if (tr && near(tr->shape)) {
        const char* ph = tr->phase == trader::Arriving ? "arriving" : tr->phase == trader::Trading ? "trading" : "leaving";
        return hoverHit{string("Trader: ") + ph, tr->shape, nullopt};
    }
    for (auto& w : wolves)
        if (near(w.shape)) return hoverHit{w.phase == wolf::Hunting ? "Wolf: hunting" : "Wolf: leaving", w.shape, nullopt};
    for (int i = 0; i < (int)deershape.size() && i < (int)deers.size(); i++)
        if (near(deershape[i])) return hoverHit{"Deer " + to_string(deers[i].health) + "hp", deershape[i], nullopt};
    for (int i = 0; i < (int)octs.size() && i < (int)rocks.size(); i++)
        if (near(octs[i])) return hoverHit{rocks[i].dying ? "Rock: being mined" : "Rock", nullopt, octs[i]};
    for (int i = 0; i < (int)bushshapes.size() && i < (int)bushes.size(); i++)
        if (near(bushshapes[i])) return hoverHit{bushes[i].ripe ? "Bush: ripe" : "Bush: bare", nullopt, bushshapes[i]};
    for (auto& hs : houseshapes)
        if (near(hs)) return hoverHit{"House", hs, nullopt};
    return nullopt;
}

// Outline-only copy of the hit shape, drawn above the overlays so it stays readable at night
void drawHoverOutline(RenderWindow& window, const View& worldView, hoverHit& h, float zoom) {
    window.setView(worldView);
    float th = tooltipOutlinePx / zoom;
    if (h.rect) {
        h.rect->setFillColor(Color::Transparent);
        h.rect->setOutlineThickness(th);
        h.rect->setOutlineColor(tooltipOutlineColor);
        window.draw(*h.rect);
    }
    if (h.circle) {
        h.circle->setFillColor(Color::Transparent);
        h.circle->setOutlineThickness(th);
        h.circle->setOutlineColor(tooltipOutlineColor);
        window.draw(*h.circle);
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

void drawOverlay(RenderWindow& window, RectangleShape& shape, Color c) {
    shape.setFillColor(c);
    window.setView(window.getDefaultView());
    window.draw(shape);
}

void buildhouse(vector<barb>& barbs, vector<RectangleShape>& squares, vector<house>& houses, vector<RectangleShape>& houseshapes) {
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
                    house h{b.targetx, b.targety, 100, b.reservedstone > 0};
                    b.reservedstone = 0;
                    houses.push_back(h);
                    RectangleShape hs(Vector2f(4, 4));
                    hs.setPosition(Vector2f(h.x, h.y));
                    hs.setFillColor(Color(197, 149, 82));
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
void makeBarb(Vector2i pos, vector<barb>& barbs, vector<RectangleShape>& squares, vector<birthRing>& rings) {
    barb b;
    b.id = nextBarbId++;
    b.x = pos.x;
    b.y = pos.y;
    barbs.push_back(b);
    RectangleShape sq;
    sq.setPosition(Vector2f(b.x, b.y));
    sq.setSize(Vector2f(2, 2));
    sq.setFillColor(Color::White);
    squares.push_back(sq);
    rings.push_back({Vector2f(b.x + 1.0f, b.y + 1.0f)}); // centred on the 2x2 square
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

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, vector<RectangleShape>& houseshapes, vector<birthRing>& rings) {
    for (int i = 0; i < (int)houseshapes.size(); i++) {
        if (spawnChance(gen) != 0) continue;
        Vector2f pos = houseshapes[i].getPosition();
        makeBarb(Vector2i((int)(pos.x + 4), (int)(pos.y + 13)), barbs, squares, rings);
        logEvent("A barb is born (pop " + to_string(barbs.size()) + ")");
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

void minematerials(vector<RectangleShape>& squares, vector<barb>& barbs, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<tree>& trees, vector<CircleShape>& octs, vector<rock>& rocks,
                   vector<bush>& bushes, vector<CircleShape>& bushshapes) {
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
                                       [&](int i) { return trees[i].dying; }, &treeDSq);
            int bestRock = findNearest(bpos, barbWorkSearchRadius, (int)rocks.size(),
                                       [&](int k) { return octs[k].getPosition() + rockWorkOffset; },
                                       [&](int k) { return rocks[k].dying; }, &rockDSq);
            int bestBush = findNearest(bpos, barbWorkSearchRadius, (int)bushes.size(),
                                       [&](int k) { return bushWorkSpot(bushshapes[k]); },
                                       [&](int k) { return !bushes[k].ripe || bushes[k].picking; }, &bushDSq);
            // a rock wins only when strictly closer, as when trees were scanned first; a bush likewise
            bestIsRock = bestRock != -1 && rockDSq < treeDSq;
            bool bestIsBush = bestBush != -1 && bushDSq < (bestIsRock ? rockDSq : treeDSq);
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
            }
	    if (best != -1 && !bestIsBush && bestIsRock && b.moving==false) {
		    Vector2f rp = octs[best].getPosition();
		    b.targetx = (int)(rp.x + 6);
		    b.targety = (int)(rp.y + 10);
		    b.moving = true;
		    b.targetIsRock = true;
		    b.targetIsBush = false;
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
			if (b.timer >= 480) {
				stone++;
				swapRemove(rocks, i);
				swapRemove(octs, i);
				b.moving = b.chopping = false;
				b.timer = 0;
				b.targetx = b.targety = -1;
				printf("ROCK DESTROYED - stone: %d\n", stone);
			}
			break;
		}
	}
}



void treespawn(vector<tree>& trees, vector<CircleShape>& tris, vector<RectangleShape>& stumps) {
    if (treespawnChance(gen) != 0) return;
    tree t{distrib(gen), distrib(gen), 100, false};
    trees.push_back(t);

    CircleShape tri;
    tri.setPointCount(3);
    tri.setPosition(Vector2f(t.x, t.y));
    tri.setRadius(8);
    tri.setFillColor(Color(6, 64, 43));
    tris.push_back(tri);

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

// Bare bushes ripen again after bushRegrowTicks
void bushTick(vector<bush>& bushes, vector<CircleShape>& bushshapes) {
    for (int i = 0; i < (int)bushes.size(); i++) {
        if (bushes[i].ripe || ++bushes[i].regrow < bushRegrowTicks) continue;
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
        CircleShape tri;
        tri.setPointCount(3);
        tri.setPosition(Vector2f(t.x, t.y));
        tri.setRadius(8);
        tri.setFillColor(Color(1, 92, 55));
        tris.push_back(tri);    

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

    RectangleShape bgBar({20, 3});
    bgBar.setFillColor(Color::Black);
    RectangleShape fgBar;

    RectangleShape dnc({800, 800});

    Font hudFont;
    bool hudFontOk = hudFont.openFromFile(hudFontPath);
    if (!hudFontOk) printf("HUD font failed to load: %s\n", hudFontPath);
    else printf("HUD font loaded: %s\n", hudFontPath);
    RectangleShape hudPanel(hudPanelSize);
    hudPanel.setPosition(hudPos);
    hudPanel.setFillColor(hudPanelColor);

    bool paused = false;
    int simSpeed = 1;
    float zoom = minZoom;
    Vector2f camCenter(worldSize / 2, worldSize / 2);
    bool panning = false;
    Vector2i lastMouse;
    View worldView(FloatRect({0.0f, 0.0f}, {worldSize, worldSize}));
    optional<trader> activeTrader;
    FloatRect tickerRect;                 // last frame's ticker strip, used to suppress hover over it
    VertexArray trailVerts(PrimitiveType::Triangles);
    vector<bird> birds;
    Vector2f birdHeading(1.0f, 0.0f);
    RectangleShape birdShape;
    int frameCount = 0;
    vector<wolf> wolves;

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
                barbspawner(squares, barbs, houseshapes, birthRings);
                birthRingTick(birthRings);
                deerspawn(deers, deershape);
                deerwander(deershape, deers);
                deerkilling(deers, deershape, barbs, squares);
                wolfTick(wolves, deers, deershape, barbs);
                clockk();
                weatherTick();
                traderTick(activeTrader);
                hungerTick(barbs);
                trailTick();
                birdTick(birds, birdHeading, squares);
                bushTick(bushes, bushshapes);
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
		window.draw(oct);

	for (auto& bs : bushshapes)
		window.draw(bs);

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
	for (auto& hs : houseshapes)
		window.draw(hs);

	for (auto& ds : deershape)
		window.draw(ds);

	for (auto& w : wolves)
		window.draw(w.shape);

        for (int i = 0; i < (int)tris.size(); i++) {
        	window.draw(stumps[i]);
        }
	
        for (int j = 0; j < (int)squares.size(); j++) {
            Color bc = barbColor(barbs[j]);
            squares[j].setFillColor(isHungry(barbs[j]) ? hungryTint(bc) : bc);
            window.draw(squares[j]);
	}
	if (activeTrader) window.draw(activeTrader->shape);
	for (int k = 0; k < (int)tris.size(); ++k)
		window.draw(tris[k]);
	drawBirds(window, birds, birdShape, frameCount++);
	for (int q = 0; q < (int)squares.size(); ++q) {
            auto& b = barbs[q];
            if (b.chopping && b.timer > 0 && !b.targetIsRock && !b.targetIsBush) {
                bgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 20));
                window.draw(bgBar);
                fgBar.setSize(Vector2f(b.timer * 20.0f / 180.0f, 3));
                fgBar.setFillColor(Color(1, 121, 111));
                fgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 20));
                window.draw(fgBar);
            }
            if (b.chopping && b.timer > 0 && b.targetIsBush) {
                bgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 14));
                window.draw(bgBar);
                fgBar.setSize(Vector2f(b.timer * 20.0f / bushPickTicks, 3));
                fgBar.setFillColor(barbPickColor);
                fgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 14));
                window.draw(fgBar);
            }
            if (b.building &&  b.timer > 0) {
                bgBar.setPosition(Vector2f(b.targetx - 9, b.targety - 10));
                window.draw(bgBar);
                fgBar.setSize(Vector2f(b.timer / 600.0f * 20.0f, 3));
                fgBar.setFillColor(Color(0, 255, 175));
                fgBar.setPosition(Vector2f(b.targetx - 9, b.targety - 10));
                window.draw(fgBar);
            }
	    if (b.chopping && b.timer > 0 && b.targetIsRock) {
                bgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 16));
                window.draw(bgBar);
                fgBar.setSize(Vector2f(b.timer * 20.0f / 480.0f, 3));
                fgBar.setFillColor(Color(255, 165, 0));
                fgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 16));
                window.draw(fgBar);
	    }
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

        optional<hoverHit> hover;
        {
            Vector2i mpx = Mouse::getPosition(window);
            Vector2u wsz = window.getSize();
            Vector2f mscreen = window.mapPixelToCoords(mpx, window.getDefaultView());
            bool overUi = (hudFontOk && hudPanel.getGlobalBounds().contains(mscreen)) || tickerRect.contains(mscreen);
            if (window.hasFocus() && !panning && !overUi && mpx.x >= 0 && mpx.y >= 0 && mpx.x < (int)wsz.x && mpx.y < (int)wsz.y) {
                Vector2f mworld = window.mapPixelToCoords(mpx, worldView);
                hover = pickHover(mworld, tooltipPickRadius / zoom, barbs, squares, activeTrader, wolves, deers, deershape, rocks, octs, bushes, bushshapes, houseshapes);
                if (hover) drawHoverOutline(window, worldView, *hover, zoom);
            }
        }

        if (hudFontOk) {
            window.setView(window.getDefaultView());
            const char* phase = (t >= 21 || t < 6) ? "Night" : "Day";
            const char* traderState = !activeTrader ? "Away"
                : activeTrader->phase == trader::Arriving ? "Arriving"
                : activeTrader->phase == trader::Trading ? "Trading" : "Leaving";
            int hungry = 0;
            for (auto& b : barbs) if (isHungry(b)) hungry++;
            char buf[320];
            snprintf(buf, sizeof(buf), "Wood: %d\nStone: %d\nFood: %d\nHungry: %d\nPopulation: %d\nHouses: %d\nTime: %02d:00 (%s)\nSpeed: %dx%s\nWeather: %s\nTrader: %s%s%s",
                     wood, stone, food, hungry, (int)barbs.size(), (int)houses.size(), t < 0 ? 0 : t, phase, simSpeed, paused ? " PAUSED" : "", raining ? "Rain" : "Clear",
                     traderState, tradeMsgTimer > 0 ? "\n" : "", tradeMsgTimer > 0 ? tradeMsg : "");
            Text hudText(hudFont, buf, hudFontSize);
            FloatRect tb = hudText.getLocalBounds();
            hudPanel.setSize(Vector2f(max(hudPanelSize.x, tb.position.x + tb.size.x + 2 * hudTextOffset.x),
                                      max(hudPanelSize.y, tb.position.y + tb.size.y + 2 * hudTextOffset.y)));
            window.draw(hudPanel);
            hudText.setFillColor(Color::White);
            hudText.setPosition(hudPos + hudTextOffset);
            window.draw(hudText);
            tickerRect = drawEvents(window, hudFont);
            if (hover) drawTooltip(window, hudFont, hover->label, window.mapPixelToCoords(Mouse::getPosition(window), window.getDefaultView()),
                                   hudPanel.getGlobalBounds(), tickerRect);
        }
        eventTick();
        window.display();
    }
    return 0;
}
