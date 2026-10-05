CXX = g++
AR = ar
ARFLAGS = rcs

TARGET = libuthreads.a
SRC = uthreads.cpp
OBJ = $(SRC:.cpp=.o)
HEADER = uthreads.h

CXXFLAGS = -Wall -Wextra -std=c++11 -I.

.PHONY: all clean

all: $(TARGET)

$(OBJ): $(SRC) $(HEADER)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

clean:
	$(RM) $(TARGET) $(OBJ)