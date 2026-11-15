MULIB ?= third_party/mulib
ON1X := $(MULIB)/on1x
BUILD ?= build
ON1XC ?= $(BUILD)/on1xc
PYTHON ?= python3
MULIB_REVISION := $(shell git -C "$(MULIB)" rev-parse HEAD 2>/dev/null)
GUI ?= auto
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ifeq ($(GUI),0)
SDL_CFLAGS :=
SDL_LIBS :=
endif
ifeq ($(GUI),1)
ifeq ($(strip $(SDL_LIBS)),)
$(error GUI=1 requires SDL2 development files and pkg-config)
endif
endif
GUI_FLAGS := $(if $(strip $(SDL_LIBS)),-DSHY_HAVE_SDL2=1,-DSHY_HAVE_SDL2=0) $(SDL_CFLAGS) $(SDL_LIBS)
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

FRONTEND := source lexer lexer_fallback type parser loader infer sema codegen \
 sexpr ppx_schema ppx_ast macros macro_rules ppx primitives staging cli \
 backend/mir backend/lower backend/serialize backend/evaluate backend/emit_c backend/passes
OBJECTS := $(addprefix $(BUILD)/compiler/,$(addsuffix .o,$(FRONTEND)))

.PHONY: all test clean FORCE
all: $(BUILD)/shylock

$(BUILD)/ppx_schema.hpp: $(ON1X)/grammar/PPX.schema.json $(ON1X)/cmake/ppx_schema.hpp.in tools/bootstrap.py
	$(PYTHON) tools/bootstrap.py "$(ON1X)" "$@"

$(BUILD)/compiler/%.o: $(ON1X)/src/%.cpp $(BUILD)/ppx_schema.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -I"$(ON1X)/include" -I"$(BUILD)" -DON1X_STDLIB_ROOT='"$(abspath $(ON1X)/stdlib)"' -c "$<" -o "$@"

$(BUILD)/on1xc: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

KERNELS := kvdb elvi-scrape uri json fetch html-jsonld html-textstream html-tokenizer mune money statsprice histprice priceopt cryptohash tui termkey immgtk fontdraw
VM_NATIVE := src/record.c src/mune_vm.c src/sandbox.c src/sandbox_os.c
NATIVE := plugins/poindexter/store.c plugins/poindexter/sheet.c plugins/poindexter/ui.c plugins/poindexter/tui.c plugins/poindexter/gui.c src/bridge.c src/evidence.c src/series.c src/registry.c src/extensions.c plugins/dragnet/dragnet.c $(VM_NATIVE)
HEADERS := src/shylock.h src/extensions.h src/poindexter.h src/mune_vm.h src/sandbox.h plugins/poindexter/sheet.h plugins/poindexter/ui.h
$(BUILD)/native-config.json: FORCE tools/native_config.py
	$(PYTHON) tools/native_config.py "$@" "$(CC)" "$(CFLAGS)" "$(GUI_FLAGS)" "$(abspath $(MULIB))" "$(MULIB_REVISION)"

$(BUILD)/shylock: src/main.on1x src/poindexter.on1x src/worksheet.on1x $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile | $(BUILD)/shylock-worker
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(GUI_FLAGS),--cflag=$(flag)) \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE),--extra-c $(file)) -o "$@"

# A strict worker cannot run ASan's large virtual mappings or sanitizer runtime
# syscalls. The parent and in-process VM remain instrumented by CFLAGS; the
# isolated image and policy probes use the production syscall/memory profile.
WORKER_CFLAGS := $(filter-out -fsanitize=%,$(CFLAGS))
$(BUILD)/shylock-worker: src/worker.on1x src/worker.c $(VM_NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(WORKER_CFLAGS),--cflag=$(flag)) \
	  --mulib mune --mulib json --mulib cryptohash \
	  $(foreach file,$(VM_NATIVE) src/worker.c,--extra-c $(file)) -o "$@"

$(BUILD)/native-tests: tests/native.on1x tests/native.c tests/fixtures/fresh.lua $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(GUI_FLAGS),--cflag=$(flag)) \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE) tests/native.c,--extra-c $(file)) -o "$@"

$(BUILD)/sandbox-probes: tests/sandbox_probe.c src/sandbox_os.c src/sandbox.h src/mune_vm.h $(BUILD)/native-config.json Makefile
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror $(WORKER_CFLAGS) -I"$(MULIB)/include" \
	  tests/sandbox_probe.c src/sandbox_os.c -o "$@"

test: $(BUILD)/shylock $(BUILD)/native-tests $(BUILD)/sandbox-probes
	$(PYTHON) tests/test.py "$(BUILD)/shylock" "$(MULIB)"
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/poindexter.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/pipeline.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/extensions.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/worksheet.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/sandbox.py
	SDL_VIDEODRIVER=dummy "$(BUILD)/native-tests"

clean:
	rm -rf "$(BUILD)"

-include $(OBJECTS:.o=.d)
