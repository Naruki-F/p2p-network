CXX = g++
CXXFLAGS = -Wall -std=c++17
TARGETS = registry peer

all: $(TARGETS)

registry: registry.cpp
	$(CXX) $(CXXFLAGS) -o registry registry.cpp

peer: peer.cpp
	$(CXX) $(CXXFLAGS) -o peer peer.cpp

clean:
	rm -f $(TARGETS)