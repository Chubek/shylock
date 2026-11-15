MULIB ?= third_party/mulib
ON1X := $(MULIB)/on1x
BUILD ?= build
ON1XC ?= $(BUILD)/on1xc
PYTHON ?= python3
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

FRONTEND := source lexer lexer_fallback type parser loader infer sema codegen \
 sexpr ppx_schema ppx_ast macros macro_rules ppx primitives staging cli \
 backend/mir backend/lower backend/serialize backend/evaluate backend/emit_c backend/passes
OBJECTS := $(addprefix $(BUILD)/compiler/,$(addsuffix .o,$(FRONTEND)))

.PHONY: all test clean
all: $(BUILD)/shylock

$(BUILD)/ppx_schema.hpp: $(ON1X)/grammar/PPX.schema.json $(ON1X)/cmake/ppx_schema.hpp.in tools/bootstrap.py
	$(PYTHON) tools/bootstrap.py "$(ON1X)" "$@"

$(BUILD)/compiler/%.o: $(ON1X)/src/%.cpp $(BUILD)/ppx_schema.hpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -MMD -MP -I"$(ON1X)/include" -I"$(BUILD)" -DON1X_STDLIB_ROOT='"$(abspath $(ON1X)/stdlib)"' -c "$<" -o "$@"

$(BUILD)/on1xc: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o "$@"

KERNELS := elvi-scrape uri json fetch html-jsonld html-textstream html-tokenizer mune money statsprice histprice priceopt cryptohash
NATIVE := src/bridge.c plugins/dragnet/dragnet.c
$(BUILD)/shylock: src/main.on1x $(NATIVE) src/shylock.h $(ON1XC) Makefile
	$(PYTHON) "$(ON1X)/tools/on1x-build.py" "$<" --on1xc "$(ON1XC)" --cc "$(CC)" \
	  --stdlib "$(ON1X)/stdlib" --mulib-root "$(MULIB)" \
	  $(foreach kernel,$(KERNELS),--mulib $(kernel)) \
	  $(foreach file,$(NATIVE),--extra-c $(file)) -o "$@"

test: $(BUILD)/shylock
	$(PYTHON) tests/test.py "$(BUILD)/shylock" "$(MULIB)"

clean:
	rm -rf "$(BUILD)"

-include $(OBJECTS:.o=.d)
