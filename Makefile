# Heavy Gear II port
#   make hg2      build the game
#   make tools    build the asset tools (hg2extract)
# SDL3 is found with pkg-config (set PKG_CONFIG_PATH for a local SDL3 build).

CXX      ?= c++
CXXFLAGS ?= -O2 -g
CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c99 -w
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS   := $(shell pkg-config --libs sdl3)
LIBS       := $(SDL_LIBS) -lz

BUILD := build
CORE_SRC  := src/core/fs.cpp src/core/zfs.cpp src/assets/assets.cpp src/assets/mission.cpp src/script/vm.cpp src/script/system.cpp src/assets/gear.cpp src/assets/weapon.cpp
GAME_SRC  := $(CORE_SRC) src/render/renderer.cpp src/game/scene.cpp src/game/player.cpp src/game/combat.cpp src/game/ai.cpp src/audio/audio.cpp src/game/hud.cpp src/game/particles.cpp src/game/fountains.cpp src/game/session.cpp src/shell/ldl.cpp src/shell/ui.cpp src/shell/gamedata.cpp src/shell/avi.cpp src/shell/movie.cpp src/shell/shell.cpp src/net/net.cpp src/main.cpp
INDEO_SRC := third_party/indeo5/indeo5.c third_party/indeo5/ivi.c third_party/indeo5/ivi_dsp.c third_party/indeo5/ffshim.c
TOOL_SRC  := $(CORE_SRC) tools/hg2extract.cpp

GAME_OBJ := $(GAME_SRC:%.cpp=$(BUILD)/%.o) $(INDEO_SRC:%.c=$(BUILD)/%.o)
TOOL_OBJ := $(TOOL_SRC:%.cpp=$(BUILD)/%.o)

.PHONY: all hg2 tools clean shaders
all: tools
hg2: $(BUILD)/hg2
tools: $(BUILD)/hg2extract

$(BUILD)/hg2: $(GAME_OBJ)
	$(CXX) -o $@ $^ $(LIBS)

$(BUILD)/hg2extract: $(TOOL_OBJ)
	$(CXX) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

shaders:
	shaders/gen.sh

clean:
	rm -rf $(BUILD)

-include $(GAME_OBJ:.o=.d) $(TOOL_OBJ:.o=.d)
