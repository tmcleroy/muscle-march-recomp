# Muscle March (WiiWare) — top-level build
WAD ?= Muscle March (USA) (WiiWare).wad
DOLPHIN ?= /Applications/Dolphin.app/Contents/MacOS/Dolphin
DTK_VERSION := v1.8.4
DTK := build/tools/dtk

.PHONY: all nand extract split ref clean
all: extract

# 1. Let Dolphin decrypt the WAD into our private NAND (booting a WAD installs it).
nand: dolphin_user/Wii/title/00010001/574d4d45/content/title.tmd
dolphin_user/Wii/title/00010001/574d4d45/content/title.tmd:
	@test -f "$(WAD)" || { echo "missing $(WAD)"; exit 1; }
	mkdir -p dolphin_user/Config && python3 tools/dolphin_config.py dolphin_user
	"$(DOLPHIN)" -u "$(CURDIR)/dolphin_user" -b -e "$(CURDIR)/$(WAD)" >/dev/null 2>&1 & pid=$$!; \
	  while [ ! -f $@ ]; do sleep 1; done; sleep 3; kill $$pid

# 2. Verify contents against the TMD, unpack DOL / U8 / LZ11.
extract: extracted/main.dol
extracted/main.dol: tools/extract.py | nand
	python3 tools/extract.py --wad "$(WAD)"

# 3. dtk analysis + split (asm in build/WMME01, not committed).
$(DTK):
	@mkdir -p build/tools
	gh release download $(DTK_VERSION) -R encounter/decomp-toolkit -p dtk-macos-arm64 -O $@ --clobber
	chmod +x $@
split: extracted/main.dol $(DTK)
	@touch config/WMME01/symbols.txt config/WMME01/splits.txt
	$(DTK) dol split config/WMME01/config.yml build/WMME01

# Reference run: boot in Dolphin headless for SECS seconds, dump frames.
SECS ?= 40
ref: nand
	tools/dolphin_run.sh $(SECS)

clean:
	rm -rf build extracted

# --- native port (static recompilation, engine in port/) ---------------------
PY := .venv/bin/python
PORT_BUILD := port/build/out
ANALYSIS := port/games/musclemarch/analysis
.PHONY: venv analysis recomp port run
venv: .venv/bin/python
.venv/bin/python:
	/opt/homebrew/bin/python3.13 -m venv .venv && .venv/bin/pip -q install numpy
analysis: split
	python3 tools/apply_dsy.py > build/dsy_names.csv
	python3 port/tools/dtk_functions.py config/WMME01/symbols.txt $(ANALYSIS) --dsy build/dsy_names.csv
recomp: analysis venv
	cd port && WP_GAME=musclemarch ../$(PY) recompiler/recomp.py
	cd port && WP_GAME=musclemarch ../$(PY) recompiler/dsp/recomp_dsp.py
$(PORT_BUILD)/build.ninja:
	cmake -S port -B $(PORT_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DWP_GAME=musclemarch -DPython3_EXECUTABLE=$(CURDIR)/$(PY)
port: recomp $(PORT_BUILD)/build.ninja
	cmake --build $(PORT_BUILD) --target musclemarch
# run: TIMEOUT=0 for no watchdog; HEADLESS=1 for no window
TIMEOUT ?= 0
run:
	@mkdir -p build/run
	cd build/run && $(if $(HEADLESS),WP_HEADLESS=1) ../../$(PORT_BUILD)/musclemarch ../../extracted $(TIMEOUT) nand

# The launcher (game file, features, controllers; starts the game).
.PHONY: launcher
launcher:
	cmake --build $(PORT_BUILD) --target musclemarch_launcher
	$(PORT_BUILD)/musclemarch-launcher

# Input movies (deterministic mode, fresh save each time): play and record, watch, or test.
#   make record NAME=race1    play normally; input goes to movies/race1.wpm
#   make replay NAME=race1    watch it again (identical run)
#   make replay-test          replay every movie headless and compare with build/golden (BLESS=1 to update)
MOVIE_RUN = build/movies/$(NAME)
.PHONY: record replay replay-test
record replay:
	@test -n "$(NAME)" || (echo "usage: make $@ NAME=..." && false)
	@mkdir -p movies && rm -rf $(MOVIE_RUN) && mkdir -p $(MOVIE_RUN)/nand/shared2/sys
	@cp dolphin_user/Wii/shared2/sys/SYSCONF $(MOVIE_RUN)/nand/shared2/sys/ 2>/dev/null || true
	cd $(MOVIE_RUN) && $(if $(filter record,$@),WP_RECORD=$(CURDIR)/movies/$(NAME).wpm,WP_REPLAY=$(CURDIR)/movies/$(NAME).wpm) \
		../../../$(PORT_BUILD)/musclemarch ../../../extracted 0 nand
replay-test: venv
	$(PY) tools/replay_test.py $(if $(BLESS),--bless) $(if $(FRAMES),--frames)

# Lockstep: same sync function in Dolphin (GDB stub) and the port, compare
# per-page hashes of game memory. Saves are cleared on both sides first.
HITS ?= 300
LOCKSTEP_ARGS ?=
.PHONY: lockstep
lockstep:
	@mkdir -p build/lockstep build/run
	rm -rf dolphin_user/Wii/title/00010001/574d4d45/data build/run/lockstep_nand
	mkdir -p build/run/lockstep_nand/shared2/sys && cp dolphin_user/Wii/shared2/sys/SYSCONF build/run/lockstep_nand/shared2/sys/
	$(PY) tools/lockstep.py dolphin --hits $(HITS) $(LOCKSTEP_ARGS) -o build/lockstep/dolphin.txt
	cd build/run && WP_HEADLESS=1 WP_SYNC_OUT=../lockstep/port.txt WP_SYNC_LIMIT=$(HITS) $(if $(DUMP),WP_SYNC_DUMP=$(DUMP)) ../../$(PORT_BUILD)/musclemarch ../../extracted 0 lockstep_nand > /dev/null 2>&1 || true
	$(PY) tools/lockstep.py compare build/lockstep/dolphin.txt build/lockstep/port.txt $(if $(DUMP),--bytes $(DUMP))

# Test build with instruction patches (infinite stamina etc.) for unattended smoke runs.
TEST_GEN := port/build/musclemarch-test/generated
.PHONY: port-test
port-test: recomp
	cd port && WP_GAME=musclemarch ../$(PY) recompiler/recomp.py --patches games/musclemarch/analysis/test_patches.csv --output ../$(TEST_GEN)/dol
	mkdir -p $(TEST_GEN)/dsp && cp port/build/musclemarch/generated/dsp/* $(TEST_GEN)/dsp/
	cmake -S port -B port/build/test -G Ninja -DCMAKE_BUILD_TYPE=Release -DWP_GAME=musclemarch -DPython3_EXECUTABLE=$(CURDIR)/$(PY) -DWP_GENERATED=$(CURDIR)/$(TEST_GEN)
	cmake --build port/build/test --target musclemarch

# macOS app bundle (no game data inside; it reads ./extracted of this checkout).
APP := build/MuscleMarch.app
.PHONY: app
app: port
	rm -rf $(APP) && mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp tools/app/Info.plist $(APP)/Contents/
	cp $(PORT_BUILD)/musclemarch $(APP)/Contents/MacOS/musclemarch-bin
	sed 's|@EXTRACTED@|$(CURDIR)/extracted|' tools/app/launcher.sh > $(APP)/Contents/MacOS/MuscleMarch
	chmod +x $(APP)/Contents/MacOS/MuscleMarch
	-python3 tools/app/make_icns.py $(APP)/Contents/Resources/icon.icns
	codesign --force --deep -s - $(APP) 2>/dev/null || true
	@echo "built $(APP)"

# Android APK with this checkout's game files packed in, for your own phone (never to be shared;
# docs/BUILDING.md, "Android").
.PHONY: android
android: port
	GAME_FILES=$(CURDIR)/extracted tools/release/android.sh
