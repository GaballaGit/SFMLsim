#!/bin/bash

SFML_LIBS=$(pkg-config --libs sfml-graphics sfml-window sfml-system)

COMPILE_ERRORS=$(g++ sim.cpp -o sim.run $SFML_LIBS 2>&1 >/dev/null)

if [ $? -ne 0 ]; then
	echo "$COMPILE_ERRORS"
	exit 1
else
	./sim.run
fi
