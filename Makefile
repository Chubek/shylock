MULIB ?= third_party/mulib
ON1X := $(MULIB)/on1x
BUILD ?= build
ON1XC ?= $(BUILD)/on1xc
PYTHON ?= python3
MULIB_REVISION := $(shell git -C "$(MULIB)" rev-parse HEAD 2>/dev/null)
PLUGINS ?= dragnet poindexter
# Normalize order/duplicates; reject unknown selections instead of silently
# producing a different executable. Each plugin owns its build dependencies.
ENABLED_PLUGINS := $(sort $(PLUGINS))
AVAILABLE_PLUGINS := $(patsubst plugins/%/plugin.mk,%,$(wildcard plugins/*/plugin.mk))
UNKNOWN_PLUGINS := $(filter-out $(AVAILABLE_PLUGINS),$(ENABLED_PLUGINS))
ifneq ($(strip $(UNKNOWN_PLUGINS)),)
$(error Unknown plugins: $(UNKNOWN_PLUGINS))
endif
PLUGIN_MANIFESTS := $(addsuffix /plugin.mk,$(addprefix plugins/,$(ENABLED_PLUGINS)))
include $(PLUGIN_MANIFESTS)
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

FRONTEND := source lexer lexer_fallback type parser loader infer sema codegen \
 sexpr ppx_schema ppx_ast macros macro_rules ppx primitives staging cli \
 backend/mir backend/lower backend/serialize backend/evaluate backend/emit_c backend/passes
OBJECTS := $(addprefix $(BUILD)/compiler/,$(addsuffix .o,$(FRONTEND)))

.PHONY: all test test-plugins clean FORCE
all: $(BUILD)/shylock

$(BUILD):
	mkdir -p "$@"

$(BUILD)/fixed-price.so: sdk/examples/fixed-price.c include/shylock-sdk.h | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC -shared $(CFLAGS) "$<" -o "$@"

$(BUILD)/ppx_schema.hpp: $(ON1X)/grammar/PPX.schema.json $(ON1X)/cmake/ppx_schema.hpp.in tools/bootstrap.py
	$(PYTHON) tools/bootstrap.py "$(ON1X)" "$@"

$(BUILD)/compiler/%.o: $(ON1X)/src/%.cpp $(BUILD)/ppx_schema.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -I"$(ON1X)/include" -I"$(BUILD)" -DON1X_STDLIB_ROOT='"$(abspath $(ON1X)/stdlib)"' -c "$<" -o "$@"

$(BUILD)/on1xc: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

CORE_KERNELS := kvdb uri json mune money statsprice histprice priceopt cryptohash latex
KERNELS := $(sort $(CORE_KERNELS) $(PLUGIN_KERNELS))
VM_NATIVE := src/record.c src/mune_vm.c src/sandbox.c src/sandbox_os.c
CORE_NATIVE := src/bridge.c src/document.c src/engine.c src/context.c src/latex.c src/evidence.c src/series.c src/registry.c src/extensions.c $(VM_NATIVE)
NATIVE := $(CORE_NATIVE) $(PLUGIN_NATIVE) $(BUILD)/builtin_plugins.c
CORE_HEADERS := $(wildcard src/*.h) include/shylock-sdk.h
HEADERS := $(CORE_HEADERS) $(PLUGIN_HEADERS)
ON1X_SOURCES := $(wildcard src/*.on1x) src/ppx/fields_runtime.on1x $(PLUGIN_ON1X)
NATIVE_FLAGS := -ldl $(PLUGIN_FLAGS)
$(BUILD)/native-config.json: FORCE tools/native_config.py $(PLUGIN_MANIFESTS)
	$(PYTHON) tools/native_config.py "$@" "$(CC)" "$(CFLAGS)" "$(NATIVE_FLAGS)" "$(abspath $(MULIB))" "$(MULIB_REVISION)" "$(ENABLED_PLUGINS)"

$(BUILD)/main.on1x $(BUILD)/builtin_plugins.c &: FORCE tools/plugin_config.py Makefile $(PLUGIN_MANIFESTS)
	$(PYTHON) tools/plugin_config.py "$(BUILD)" $(foreach plugin,$(ENABLED_PLUGINS),--plugin $(plugin)) \
	  $(foreach initializer,$(PLUGIN_INITIALIZERS),--init $(initializer)) \
	  $(foreach event,$(PLUGIN_EVENTS),--event $(event))

PPX_LIB := $(wildcard $(ON1X)/stdlib/ppx/*.on1x) $(ON1X)/stdlib/sexpr/sexpr.on1x $(ON1X)/stdlib/visitor/visitor.on1x
# The complete command catalog is part of the AST processed by the fields PPX.
PPX_TIMEOUT_MS ?= 60000
$(BUILD)/ppx-fields: src/ppx/fields.on1x $(PPX_LIB) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --cflag=-O2 $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

$(BUILD)/shylock: $(BUILD)/main.on1x $(ON1X_SOURCES) $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/ppx-fields $(BUILD)/native-config.json Makefile $(PLUGIN_MANIFESTS) | $(BUILD)/shylock-worker
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	  $(PYTHON) "$(ON1X)/tools/on1x-build.py" "$(BUILD)/main.on1x" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --ppx fields "$(abspath $(BUILD)/ppx-fields)" \
	  --ppx-timeout-ms "$(PPX_TIMEOUT_MS)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(NATIVE_FLAGS),--cflag=$(flag)) \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE),--extra-c $(file)) -o "$@"

# A strict worker cannot run ASan's large virtual mappings or sanitizer runtime
# syscalls. The parent and in-process VM remain instrumented by CFLAGS; the
# isolated image and policy probes use the production syscall/memory profile.
WORKER_CFLAGS := $(filter-out -fsanitize=%,$(CFLAGS))
$(BUILD)/shylock-worker: src/worker.on1x src/worker.c $(VM_NATIVE) $(CORE_HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(WORKER_CFLAGS),--cflag=$(flag)) \
	  --mulib mune --mulib json --mulib cryptohash \
	  $(foreach file,$(VM_NATIVE) src/worker.c,--extra-c $(file)) -o "$@"

$(BUILD)/native-tests: tests/native.on1x tests/naive_policy.on1x src/ppx/fields_runtime.on1x $(PLUGIN_ON1X) tests/native.c tests/intelligence.c tests/naive.c tests/fixtures/fresh.lua $(NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/ppx-fields $(BUILD)/native-config.json Makefile
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --ppx fields "$(abspath $(BUILD)/ppx-fields)" --ppx-timeout-ms "$(PPX_TIMEOUT_MS)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) \
	  $(foreach flag,$(NATIVE_FLAGS),--cflag=$(flag)) \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE) tests/native.c tests/intelligence.c tests/naive.c,--extra-c $(file)) -o "$@"

$(BUILD)/sandbox-probes: tests/sandbox_probe.c src/sandbox_os.c src/sandbox.h src/mune_vm.h $(BUILD)/native-config.json Makefile
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror $(WORKER_CFLAGS) -I"$(MULIB)/include" \
	  tests/sandbox_probe.c src/sandbox_os.c -o "$@"

$(BUILD)/metaprogramming-tests: tests/metaprogramming.on1x src/demand_macros.on1x src/ppx/fields_runtime.on1x $(BUILD)/ppx-fields $(ON1XC) Makefile
	ON1X_PPX_SCHEMA="$(abspath $(ON1X)/grammar/PPX.schema.json)" \
	  $(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --ppx fields "$(abspath $(BUILD)/ppx-fields)" \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

ALLOCATION_NATIVE := src/document.c src/engine.c src/context.c src/series.c src/latex.c src/record.c src/registry.c src/evidence.c $(PLUGIN_ALLOCATION_NATIVE) $(BUILD)/builtin_plugins.c
$(BUILD)/document-allocation-tests: tests/document_alloc.on1x tests/document_alloc.c $(ALLOCATION_NATIVE) $(HEADERS) $(ON1XC) $(BUILD)/native-config.json Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  --cflag='-DSHY_MULIB_REVISION="$(MULIB_REVISION)"' \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach flag,$(NATIVE_FLAGS),--cflag=$(flag)) \
	  $(foreach file,$(ALLOCATION_NATIVE),--extra-c $(file)) --extra-c tests/document_alloc.c \
	  --cflag=-Wl,--wrap=malloc --cflag=-Wl,--wrap=calloc --cflag=-Wl,--wrap=realloc \
	  $(foreach flag,$(CFLAGS),--cflag=$(flag)) -o "$@"

test: $(BUILD)/shylock $(BUILD)/native-tests $(BUILD)/sandbox-probes $(BUILD)/metaprogramming-tests $(BUILD)/document-allocation-tests
	$(PYTHON) tests/plugins.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/help.py
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
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/poindexter_finish.py
	SHY_BINARY="$(abspath $(BUILD)/shylock)" $(PYTHON) tests/sandbox.py
	SDL_VIDEODRIVER=dummy "$(BUILD)/native-tests"
	$(PYTHON) tests/metaprogramming.py "$(ON1XC)" "$(BUILD)/ppx-fields" "$(ON1X)" "$(BUILD)/metaprogramming-tests"
	$(PYTHON) tests/document_alloc.py "$(BUILD)/document-allocation-tests" "$(BUILD)/shylock"

test-plugins: $(ON1XC)
	CC="$(CC)" CFLAGS="$(CFLAGS)" $(PYTHON) tests/plugins.py --profiles --on1xc "$(ON1XC)" --mulib "$(MULIB)"

clean:
	rm -rf "$(BUILD)"

-include $(OBJECTS:.o=.d)
