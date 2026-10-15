#include <SFML/Graphics.hpp>
#include <random>
#include <vector>
#include <cstdio>

using namespace sf;
using namespace std;

const int numbarbs = 1;
const int numtrees = 2000;
const int numrocks = 100;

const char* const hudFontPath = "assets/DejaVuSans.ttf";
const unsigned int hudFontSize = 14;
const Vector2f hudPos(6.0f, 6.0f);
const Vector2f hudPanelSize(150.0f, 130.0f);
const Vector2f hudTextOffset(6.0f, 4.0f);
const int maxTicksPerFrame = 20;
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

struct house {int x, y, health;};

struct barb {
    int x, y, health = 100;
    bool moving = false;
    int targetx = -1, targety = -1;
    int timer = 0;
    bool chopping = false;
    bool movingbspot = false;
    bool building = false;
    bool reservedwood = false;
    int stepTimer = 0;
    bool targetIsRock = false;
    bool deertargeting = false;
    int targetDeerId = -1;
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

struct tree  {int x, y, health; bool dying = false;};
struct rock  {int x, y, health; bool dying = false;};

template<typename T>
void swapRemove(vector<T>& v, int i) {
    v[i] = move(v.back());
    v.pop_back();
}

// Priority when flags overlap: hunting > building > mining > chopping > idle
Color barbColor(const barb& b) {
    if (b.deertargeting) return barbHuntColor;
    if (b.building) return barbBuildColor;
    if (b.chopping && b.targetIsRock) return barbMineColor;
    if (b.chopping) return barbChopColor;
    return barbIdleColor;
}

void deerkilling(vector<deer>& deers, vector<RectangleShape>& deershape, vector<barb>& barbs, vector<RectangleShape>& squares) {
	const float deerInfluenceDSq = 625.0f;
	for (int i = 0; i < (int)deers.size(); ++i) {
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
					swapRemove(deers, i);
					swapRemove(deershape, i);
					b.deertargeting = false;
					b.targetDeerId = -1;
					b.moving = false;
					food++;
					printf("DEER KILLED - Food: %d\n", food);
					break;
				}
			}
		}
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

void buildhouse(vector<barb>& barbs, vector<RectangleShape>& squares, vector<house>& houses, vector<RectangleShape>& houseshapes) {
    bool canclaim = (int)houses.size() < (int)barbs.size();
    for (int i = 0; i < (int)squares.size(); i++) {
        auto& sq = squares[i];
        auto& b  = barbs[i];
        if (b.chopping || (b.timer > 0 && !b.building)) continue;
        if (!b.movingbspot && !b.moving) {
            if (!canclaim || wood < 30) continue;
            wood -= 30;
            b.reservedwood = true;
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
                    if (b.reservedwood) wood += 30;
                    b.reservedwood = false;
                    b.moving = b.movingbspot = b.building = false;
                    b.timer = 0;
                    continue;
                }
                b.building = true;
                b.timer++;
                if (b.timer >= 600) {
                    b.reservedwood = false;
                    house h{b.targetx, b.targety, 100};
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
                }
            } else {
                if (++b.stepTimer >= 10) {
                    b.stepTimer = 0;
                    sq.move(diff / sqrt(dSq));
                }
            }
        }
    }
}

void barbspawner(vector<RectangleShape>& squares, vector<barb>& barbs, vector<RectangleShape>& houseshapes) {
    for (int i = 0; i < (int)houseshapes.size(); i++) {
        if (spawnChance(gen) != 0) continue;
        Vector2f pos = houseshapes[i].getPosition();
        barb b;
        b.x = (int)(pos.x + 4);
        b.y = (int)(pos.y + 13);
        barbs.push_back(b);
        RectangleShape sq;
        sq.setPosition(Vector2f(b.x, b.y));
        sq.setSize(Vector2f(2, 2));
        sq.setFillColor(Color::White);
        squares.push_back(sq);
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
            } else if (++b.stepTimer >= 10) {
                b.stepTimer = 0;
                sq.move(diff / sqrt(dSq));
		sq.getGlobalBounds();
            }
        }
    }
}

void minematerials(vector<RectangleShape>& squares, vector<barb>& barbs, vector<CircleShape>& tris, vector<RectangleShape>& stumps, vector<tree>& trees, vector<CircleShape>& octs, vector<rock>& rocks) {
    for (int j = 0; j < (int)squares.size(); j++) {
        auto& sq = squares[j];
        auto& b  = barbs[j];
        if (b.movingbspot || b.building) continue;
        Vector2f bpos = sq.getPosition();
        bool bestIsRock = false;
        if (b.timer == 0 && !b.chopping) {
            int best = -1;
            float bestDSq = 625.0f;
            for (int i = 0; i < (int)trees.size(); i++) {
                if (trees[i].dying) continue;
                Vector2f tp = tris[i].getPosition();
                float dx = tp.x + 8  - bpos.x;
                float dy = tp.y + 15 - bpos.y;
                float dSq = dx * dx + dy * dy;
                if (dSq < bestDSq) {bestDSq = dSq; best = i; bestIsRock = false;}
            }
	    for (int k = 0; k < (int)rocks.size(); k++) {
	    if (rocks[k].dying) continue;
	    Vector2f rp = octs[k].getPosition();
	    float dx = rp.x + 6 - bpos.x;
	    float dy = rp.y + 10 - bpos.y;
	    float dSq = dx * dx + dy * dy;
	    if (dSq < bestDSq) {bestDSq = dSq; best = k; bestIsRock = true;}
	    }
            if (best != -1 && !bestIsRock && b.moving==false) {
                Vector2f tp = tris[best].getPosition();
                b.targetx = (int)(tp.x + 8);
                b.targety = (int)(tp.y + 15);
                b.moving  = true;
                b.targetIsRock = false;
            }
	    if (best != -1 && bestIsRock && b.moving==false) {
		    Vector2f rp = octs[best].getPosition();
		    b.targetx = (int)(rp.x + 6);
		    b.targety = (int)(rp.y + 10);
		    b.moving = true;
		    b.targetIsRock = true;
        }
        if (b.targetx == -1) continue;
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
            b.timer++;
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
			b.timer++;
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

int main() {
    RenderWindow window(VideoMode({800, 800}), "sim");
    window.setFramerateLimit(60);

    Clock clock;
    Time timesincelastupdate = Time::Zero;
    const Time Timepertick = seconds(1.0f / 60.0f);

    vector<barb> barbs(numbarbs);
    {
        uniform_int_distribution<int> d(350, 450);
        for (auto& b : barbs) {b.x = d(gen); b.y = d(gen);}
    }

    vector<tree> trees(numtrees);
    for (auto& t : trees) {t.x = distrib(gen); t.y = distrib(gen); t.health = 100;}

    vector<rock> rocks(numrocks);
    for (auto& r : rocks) {r.x = distrib(gen); r.y = distrib(gen); r.health = 100;}

    vector<house> houses;
    vector<RectangleShape> houseshapes;

    vector<RectangleShape> squares;
    squares.reserve(numbarbs);
    for (auto& b : barbs) {
        RectangleShape sq;
        sq.setPosition(Vector2f(b.x, b.y));
	sq.getGlobalBounds();
        sq.setSize(Vector2f(2, 2));
        sq.setFillColor(Color::White);
        squares.push_back(sq);
    }

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

    while (window.isOpen()) {
        while (const optional event = window.pollEvent())
            if (event->is<Event::Closed>()) window.close();
            else if (const auto* kp = event->getIf<Event::KeyPressed>()) {
                if (kp->code == Keyboard::Key::Space) paused = !paused;
                else if (kp->code == Keyboard::Key::Num1) simSpeed = 1;
                else if (kp->code == Keyboard::Key::Num2) simSpeed = 2;
                else if (kp->code == Keyboard::Key::Num3) simSpeed = 4;
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
            minematerials(squares, barbs, tris, stumps, trees, octs, rocks);
            barbmove(squares, barbs);
            treespawn(trees, tris, stumps);
            barbspawner(squares, barbs, houseshapes);
	    deerspawn(deers, deershape);
	    deerwander(deershape, deers);
	    deerkilling(deers, deershape, barbs, squares);
            clockk();
            }
        }

        for (int i = (int)tris.size() - 1; i >= 0; i--) {
            Vector2f p = tris[i].getPosition();
            float dx = p.x - 400, dy = p.y - 400;
            if (dx*dx + dy*dy < 2500.0f) { // 50^2
                int tix = (int)(p.x + 8), tiy = (int)(p.y + 15);
                for (auto& b : barbs) {
                    if (b.targetx == tix && b.targety == tiy) {
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
	
	for (auto& oct : octs)
		window.draw(oct);

	for (auto& hs : houseshapes)
		window.draw(hs);

	for (auto& ds : deershape)
		window.draw(ds);

        for (int i = 0; i < (int)tris.size(); i++) {
        	window.draw(stumps[i]);
        }
	
        for (int j = 0; j < (int)squares.size(); j++) {
            squares[j].setFillColor(barbColor(barbs[j]));
            window.draw(squares[j]);
	}
	for (int k = 0; k < (int)tris.size(); ++k)
		window.draw(tris[k]);
	for (int q = 0; q < (int)squares.size(); ++q) {
            auto& b = barbs[q];
            if (b.chopping && b.timer > 0 && !b.targetIsRock) {
                bgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 20));
                window.draw(bgBar);
                fgBar.setSize(Vector2f(b.timer * 20.0f / 180.0f, 3));
                fgBar.setFillColor(Color(1, 121, 111));
                fgBar.setPosition(Vector2f(b.targetx - 10, b.targety - 20));
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

        int t = game_time;
        if (t >= 21 || t < 6)      {dnc.setFillColor(Color(4, 7, 32, 150));}      // Night
        else if (t < 12)           {dnc.setFillColor(Color(250, 231, 190, 30));}  // Morning
        else if (t < 18)           {dnc.setFillColor(Color(233, 255, 253, 30));}  // Midday
        else                       {dnc.setFillColor(Color(255, 178, 127, 50));}  // Evening
        window.draw(dnc);

        if (hudFontOk) {
            window.setView(window.getDefaultView());
            window.draw(hudPanel);
            const char* phase = (t >= 21 || t < 6) ? "Night" : "Day";
            char buf[200];
            snprintf(buf, sizeof(buf), "Wood: %d\nStone: %d\nFood: %d\nPopulation: %d\nHouses: %d\nTime: %02d:00 (%s)\nSpeed: %dx%s",
                     wood, stone, food, (int)barbs.size(), (int)houses.size(), t < 0 ? 0 : t, phase, simSpeed, paused ? " PAUSED" : "");
            Text hudText(hudFont, buf, hudFontSize);
            hudText.setFillColor(Color::White);
            hudText.setPosition(hudPos + hudTextOffset);
            window.draw(hudText);
        }
        window.display();
    }
    return 0;
}
