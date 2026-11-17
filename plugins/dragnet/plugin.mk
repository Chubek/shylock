# All discovery, transport, extraction, browser and model dependencies belong here.
PLUGIN_ON1X += $(wildcard plugins/dragnet/*.on1x)
DRAGNET_NATIVE := $(wildcard plugins/dragnet/*.c)
PLUGIN_NATIVE += $(DRAGNET_NATIVE)
PLUGIN_ALLOCATION_NATIVE += $(DRAGNET_NATIVE)
PLUGIN_HEADERS += $(wildcard plugins/dragnet/*.h)
PLUGIN_INITIALIZERS += plugins/dragnet/dragnet.h:shy_dragnet_init
PLUGIN_KERNELS += elvi-scrape fetch html-jsonld html-textstream html-tokenizer readability browser-engine browserlocator dom-query openai-client regex scrape-rule
# Rebuild the generated one-TU closure on in-place upstream scanner changes.
PLUGIN_HEADERS += $(MULIB)/include/mulib-elvi-scrape.h $(MULIB)/include/detail/mulib-webscan.h
OPENAI_TLS_FLAGS := $(shell pkg-config --cflags --libs openssl 2>/dev/null)
ifeq ($(strip $(OPENAI_TLS_FLAGS)),)
OPENAI_TLS_FLAGS := -lssl -lcrypto
endif
PLUGIN_FLAGS += $(OPENAI_TLS_FLAGS)
