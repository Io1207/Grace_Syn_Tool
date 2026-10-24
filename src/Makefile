CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET   := main
SRCS     := main.cpp game.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) game.h player.h hoa_automaton.h fairsyn_writer.h subgame.h opt_lib.h dynamic_policy.h
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)