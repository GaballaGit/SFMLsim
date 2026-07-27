#include <SFML/Graphics.hpp>
#include <vector>
#include <random>
#include <SFML/Window/Keyboard.hpp>
#include <cstdio>
#include <iostream>
#define PI acos(-1.0)
using namespace sf;
using namespace std;
const int numstars = 500;

template<typename T>
void swapremove(vector<T>& v, int i) {
	v[i] = move(v.back());
	v.pop_back();       
}

void updateLasers(vector<RectangleShape>& lasers) {
	for (int i = 0; i < (int)lasers.size(); i++) {
		auto& l = lasers[i];
		float angle = l.getRotation();
		float radians = angle * (PI / 180.0f) + (PI / 2);
		l.move(Vector2f(-cos(radians), -sin(radians)) * 8.f);
		Vector2f laserpos = l.getPosition();
		if (laserpos.x > 600 || laserpos.x < -80 || laserpos.y > 600 || laserpos.y < -80) {
			swapremove(lasers, i);
		}
	}
}

bool controls(CircleShape& ship, bool& booster) {
	booster = false;
	if (Keyboard::isKeyPressed(Keyboard::Right)) {
		ship.rotate(3.f);
	}
	if (Keyboard::isKeyPressed(Keyboard::Left)) {
		ship.rotate(-3.f);
	}
	if (Keyboard::isKeyPressed(Keyboard::Up)) {
		float angle = ship.getRotation();
		float radians = angle * (PI / 180.0f) + (PI/2);
		ship.move(Vector2f(-cos(radians), -sin(radians)));
		booster = true;
	}
	static bool wasFiring = false;
	bool isFiring = Keyboard::isKeyPressed(Keyboard::Z);
	bool justPressed = isFiring && !wasFiring;
	wasFiring = isFiring;
	return justPressed;
}

mt19937 gen(random_device{}());
uniform_int_distribution<int> r(0, 520);

int main() {
RenderWindow window(VideoMode(520, 520), "asteroids");
window.setFramerateLimit(60);
Clock clock;
Time timesincelastupdate = Time::Zero;
const Time timepertick = seconds(1.0f / 60.0f);
float totalTime = 0.f;

vector<RectangleShape> stars;
stars.reserve(numstars);
for (int i = 0; i < numstars; i++) {
	RectangleShape str;
	str.setPosition(Vector2f(r(gen), r(gen)));
	str.setSize(Vector2f(1, 1));
	str.setFillColor(Color::White);
	stars.push_back(str);
}

CircleShape ship;
	ship.setPointCount(3);
	ship.setRadius(5);
	ship.setOrigin(ship.getRadius(), ship.getRadius());
	ship.setPosition(260, 260);
	ship.setFillColor(Color(95, 102, 112));

vector<RectangleShape> laser;

while (window.isOpen()) {
	Event event;
	while (window.pollEvent(event)) {
		if (event.type == Event::Closed)
			window.close();
	}
	bool firing = false;
	bool booster = false;
	Time frameTime = clock.restart();
	totalTime += frameTime.asSeconds();
	timesincelastupdate += frameTime;
	while (timesincelastupdate > timepertick) {
		timesincelastupdate -= timepertick;
		bool boosterTick = false;
		firing = firing || controls(ship, boosterTick);
		booster = booster || boosterTick;
		updateLasers(laser);
	}
	window.clear(Color(12, 2, 23));
	for (auto& s : stars)
		window.draw(s);
	if (firing) {
		RectangleShape lsr;
		lsr.setSize(Vector2f(1, 4));
		lsr.setOrigin(0.5f, 2.f);
		lsr.setPosition(ship.getPosition());
		lsr.setRotation(ship.getRotation());
		lsr.setFillColor(Color::White);
		laser.push_back(lsr);
	}
	if (booster) {
		float angle = ship.getRotation();
		float radians = angle * (PI / 180.0f) + (PI / 2);
		Vector2f forward(-cos(radians), -sin(radians));
		float flicker = 0.5f + 0.5f * sin(totalTime * 25.f);
		float jitter = 0.5f + 0.5f * cos(totalTime * 47.f + 1.7f);

		float outerRadius = 3.5f + 1.5f * flicker;
		CircleShape outerFlame;
		outerFlame.setRadius(outerRadius);
		outerFlame.setOrigin(outerRadius, outerRadius);
		outerFlame.setPosition(ship.getPosition() - forward * (5.f + outerRadius * 0.6f));
		outerFlame.setFillColor(Color(0, 255, 255, 200));
		window.draw(outerFlame);

		float innerRadius = 1.8f + 1.2f * jitter;
		CircleShape innerFlame;
		innerFlame.setRadius(innerRadius);
		innerFlame.setOrigin(innerRadius, innerRadius);
		innerFlame.setPosition(ship.getPosition() - forward * (5.f + innerRadius * 0.4f));
		innerFlame.setFillColor(Color::White);
		window.draw(innerFlame);
	}
	for (auto& l : laser)
		window.draw(l);
	window.draw(ship);
	window.display();
}
return 0;
}
