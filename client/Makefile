# Legacy stock/trading client disabled. Delegate to the network-only benchmark.
ifeq ($(OS),Windows_NT)
EXE = .exe
else
EXE =
endif
.PHONY: all client test
all client test:
	$(MAKE) -C .. net_client$(EXE)
