CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

all: vn

vn: vn.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f vn

.PHONY: all clean
