#include <iostream>

struct numbers {
int x;
int y;
};

int main() {
	numbers number = {10, 1};
	numbers* ptr = &number;
	std::cout << number.x << std::endl;
	std::cout << ptr->y <<std::endl;
return 0;
}
