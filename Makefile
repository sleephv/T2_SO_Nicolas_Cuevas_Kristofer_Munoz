CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
TARGET = torneo
SRC = torneo.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) *.o *.out
	