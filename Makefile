
INCLUDES = -I/usr/include/GLFW\
	-I/usr/include/freetype2\
	-I/usr/include/GL\
	-Iinclude

LIBS = -lfreetype -lglfw -lGL -lGLEW

CC=g++
CPPFLAGS=-Wall -Wextra -g -std=c++14  $(INCLUDES) $(LIBS)

run: demo
	./demo

demo: demo.cpp lib/gllabel.cpp lib/types.cpp lib/vgrid.cpp lib/cubic2quad.cpp lib/outline.cpp
	$(CC) $^ $(CPPFLAGS) -o $@
