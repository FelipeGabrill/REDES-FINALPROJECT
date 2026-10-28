CXX = g++
CXXFLAGS = -g -Wall -Wextra -Wpedantic -std=c++17 -pthread -Iinclude

TARGET = user
SOURCES = src/app/user.cpp src/network/network.cpp src/protocol/protocol.cpp src/commands/commands.cpp src/peer/peer.cpp
OBJECTS = $(SOURCES:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)
