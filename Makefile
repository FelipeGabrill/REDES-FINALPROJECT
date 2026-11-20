CXX = g++
CXXFLAGS = -g -Wall -Wextra -Wpedantic -std=c++17 -pthread -Iinclude

TARGET = user
SOURCES = src/app/user.cpp src/network/network.cpp src/protocol/protocol.cpp src/commands/commands.cpp src/peer/peer.cpp
OBJECTS = $(SOURCES:.cpp=.o)
DEPENDENCIES = $(OBJECTS:.o=.d)
HEADERS = $(wildcard include/*.h)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

$(OBJECTS): $(HEADERS)

src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TARGET)
	python3 -m unittest discover -s tests -v

clean:
	rm -f $(TARGET) $(OBJECTS) $(DEPENDENCIES)
