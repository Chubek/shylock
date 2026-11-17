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
OPENAI_TLS_FLAGS := $(shell pkg-config --cflags --libs openssl 2>/dev/null)
ifeq ($(strip $(OPENAI_TLS_FLAGS)),)
OPENAI_TLS_FLAGS := -lssl -lcrypto
endif
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

FRONTEND := source lexer lexer_fallback type parser loader infer sema codegen \
 sexpr ppx_schema ppx_ast macros macro_rules ppx primitives staging cli \
 backend/mir backend/lower backend/serialize backend/evaluate backend/emit_c backend/passes
OBJECTS := $(addprefix $(BUILD)/compiler/,$(addsuffix .o,$(FRONTEND)))

.PHONY: all test clean FORCE
all: $(BUILD)/shylock

$(BUILD)/fixed-price.so: sdk/examples/fixed-price.c include/shylock-sdk.h | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -shared $(CFLAGS) "$<" -o "$@"

$(BUILD)/ppx_schema.hpp: $(ON1X)/grammar/PPX.schema.json $(ON1X)/cmake/ppx_schema.hpp.in tools/bootstrap.py
	$(PYTHON) tools/bootstrap.py "$(ON1X)" "$@"

$(BUILD)/compiler/%.o: $(ON1X)/src/%.cpp $(BUILD)/ppx_schema.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -I"$(ON1X)/include" -I"$(BUILD)" -DON1X_STDLIB_ROOT='"$(abspath $(ON1X)/stdlib)"' -c "$<" -o "$@"

$(BUILD)/on1xc: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

KERNELS := kvdb elvi-scrape uri json fetch html-jsonld html-textstream html-tokenizer mune money statsprice histprice priceopt cryptohash tui termkey immgtk fontdraw latex readability browser-engine browserlocator dom-query openai-client regex scrape-rule
VM_NATIVE := src/record.c src/mune_vm.c src/sandbox.c src/sandbox_os.c
NAIVE_NATIVE := plugins/dragnet/naive.c plugins/dragnet/naive-dom.c plugins/dragnet/naive-regex.c plugins/dragnet/naive-money.c plugins/dragnet/naive-span.c
INTELLIGENCE_NATIVE := plugins/dragnet/flat.c plugins/dragnet/browser.c plugins/dragnet/openai.c plugins/dragnet/openai_tls.c plugins/dragnet/collection.c $(NAIVE_NATIVE)
NATIVE := plugins/poindexter/store.c plugins/poindexter/sheet.c plugins/poindexter/ui.c plugins/poindexter/tui.c plugins/poindexter/gui.c src/bridge.c src/document.c src/engine.c src/context.c src/latex.c src/evidence.c src/series.c src/registry.c src/extensions.c plugins/dragnet/dragnet.c plugins/dragnet/workflow.c plugins/dragnet/transport.c $(INTELLIGENCE_NATIVE) $(VM_NATIVE)
HEADERS := src/shylock.h include/shylock-sdk.h plugins/dragnet/dragnet.h plugins/dragnet/workflow.h plugins/dragnet/intelligence.h src/document.h src/engine.h src/latex.h src/extensions.h src/poindexter.h src/mune_vm.h src/sandbox.h plugins/poindexter/sheet.h plugins/poindexter/ui.h
HEADERS += plugins/dragnet/naive.h plugins/dragnet/naive-rules.h
HEADERS += plugins/dragnet/collection.h
# Elvi's shared scanner is part of its implementation contract. Rebuild the
# generated one-TU kernel closure even when a checkout edits these in place.
HEADERS += $(MULIB)/include/mulib-elvi-scrape.h $(MULIB)/include/detail/mulib-webscan.h
$(BUILD)/native-config.json: FORCE tools/native_config.py
	$(PYTHON) tools/native_config.py "$@" "$(CC)" "$(CFLAGS)" "$(GUI_FLAGS) $(OPENAI_TLS_FLAGS)" "$(abspath $(MULIB))" "$(MULIB_REVISION)"

PPX_LIB := $(wildcard $(ON1X)/stdlib/ppx/*.on1x) $(ON1X)/stdlib/sexpr/sexpr.on1x $(ON1X)/stdlib/visitor/visitor.on1x
PPX_TIMEOUT_MS ?= 30000
$(BUILD)/ppx-fields: src/ppx/fields.on1x $(PPX_LIB) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --cflag=-O2 $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

$(BUILD)/shylock: $(wildcard src/*.on1x) $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/ppx-fields $(BUILD)/native-config.json Makefile | $(BUILD)/shylock-worker
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	  $(PYTHON) "$(ON1X)/tools/on1x-build.py" src/main.on1x --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --ppx fields "$(abspath $(BUILD)/ppx-fields)" \
	  --ppx-timeout-ms "$(PPX_TIMEOUT_MS)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(GUI_FLAGS),--cflag=$(flag)) \
	  --cflag=-ldl \
	  $(foreach flag,$(OPENAI_TLS_FLAGS),--cflag=$(flag)) \
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

$(BUILD)/native-tests: tests/native.on1x tests/naive_policy.on1x $(wildcard src/dragnet_*.on1x) tests/native.c tests/intelligence.c tests/naive.c tests/fixtures/fresh.lua $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/ppx-fields $(BUILD)/native-config.json Makefile
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --ppx fields "$(abspath $(BUILD)/ppx-fields)" --ppx-timeout-ms "$(PPX_TIMEOUT_MS)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(GUI_FLAGS),--cflag=$(flag)) \
	  --cflag=-ldl \
	  $(foreach flag,$(OPENAI_TLS_FLAGS),--cflag=$(flag)) \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE) tests/native.c tests/intelligence.c tests/naive.c,--extra-c $(file)) -o "$@"

$(BUILD)/sandbox-probes: tests/sandbox_probe.c src/sandbox_os.c src/sandbox.h src/mune_vm.h $(BUILD)/native-config.json Makefile
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror $(WORKER_CFLAGS) -I"$(MULIB)/include" \
	  tests/sandbox_probe.c src/sandbox_os.c -o "$@"

$(BUILD)/metaprogramming-tests: tests/metaprogramming.on1x src/demand_macros.on1x $(BUILD)/ppx-fields $(ON1XC) Makefile
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	  $(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --ppx fields "$(abspath $(BUILD)/ppx-fields)" \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

DRAGNET_NATIVE := plugins/dragnet/dragnet.c plugins/dragnet/workflow.c plugins/dragnet/transport.c src/registry.c src/evidence.c
$(BUILD)/document-allocation-tests: tests/document_alloc.on1x tests/document_alloc.c src/document.c src/engine.c src/context.c src/series.c src/latex.c src/record.c $(DRAGNET_NATIVE) $(INTELLIGENCE_NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" --mulib json --mulib cryptohash --mulib latex \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  --mulib money --mulib histprice --mulib statsprice --mulib priceopt \
	  --mulib elvi-scrape --mulib html-jsonld --mulib html-tokenizer --cflag=-ldl \
	  --mulib readability --mulib browser-engine --mulib browserlocator --mulib dom-query --mulib openai-client --mulib regex --mulib scrape-rule \
	  $(foreach flag,$(OPENAI_TLS_FLAGS),--cflag=$(flag)) \
	  $(foreach file,$(DRAGNET_NATIVE) $(INTELLIGENCE_NATIVE),--extra-c $(file)) \
	  --extra-c tests/document_alloc.c --extra-c src/document.c --extra-c src/engine.c --extra-c src/context.c --extra-c src/series.c --extra-c src/latex.c --extra-c src/record.c \
	  --cflag=-Wl,--wrap=malloc --cflag=-Wl,--wrap=calloc --cflag=-Wl,--wrap=realloc \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

test: $(BUILD)/shylock $(BUILD)/native-tests $(BUILD)/sandbox-probes $(BUILD)/metaprogramming-tests $(BUILD)/document-allocation-tests
	$(PYTHON) tests/test.py "$(BUILD)/shylock" "$(MULIB)"
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/poindexter.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/pipeline.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/demand.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/engine.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" CC="$(CC)" SDK_TEST_CFLAGS="$(CFLAGS)" $(PYTHON) tests/sdk.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/dragnet.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/intelligence.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/naive.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/collection.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/extensions.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/calculation.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/reports.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/worksheet.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/sandbox.py
	SDL_VIDEODRIVER=dummy "$(BUILD)/native-tests"
	$(PYTHON) tests/metaprogramming.py "$(ON1XC)" "$(BUILD)/ppx-fields" "$(ON1X)" "$(BUILD)/metaprogramming-tests"
	$(PYTHON) tests/document_alloc.py "$(BUILD)/document-allocation-tests" "$(BUILD)/shylock"

clean:
	rm -rf "$(BUILD)"

-include $(OBJECTS:.o=.d)
