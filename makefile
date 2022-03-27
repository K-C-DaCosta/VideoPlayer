COMPILER=gcc

STATIC_LIBS=$(wildcard ./libs/*.a)
STATIC_LIBS:=$(patsubst ./libs/lib%.a,-l%,$(STATIC_LIBS))

FLAGS= -Wall -O3 -g3  -I'./include' 
LIBS= -lavfilter -lavdevice -lavformat -lavcodec -lavutil -lswscale  -lswresample -lvpx -lz -lpng  -lm -lpthread -lSDL2
SRC_DIR=./src
BIN_DIR=./bin

SRC_FILES=$(wildcard ./src/*.c)

all:  video-player

build: $(SRC_FILES)
	echo $(SRC_FILES)
	
video-player:
	$(COMPILER)     $(FLAGS) $(FFMPEG_LIBS)  ./src/main.c $(LIBS)    -o ./bin/video_player 

%.c %.bin:
	echo asdasd $< and $@ 

.PHONY:all build
