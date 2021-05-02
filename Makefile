
INCLUDES = -I/usr/include/GLFW\
	-I/usr/include/freetype2\
	-I/usr/include/GL\
	-Iinclude

LIBS = -lfreetype -lglfw -lGL -lGLEW

CC=g++
CPPFLAGS=-Wall -Wextra -g -std=c++14  $(INCLUDES) $(LIBS)

SOURCES = $(shell find -name "*.cpp")

run: demo
	./demo

demo: $(SOURCES)
	$(CC) $^ $(CPPFLAGS) -o $@

