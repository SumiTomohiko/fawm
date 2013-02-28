
BUILD_CMD = ./blow

all: build

install:
	@$(BUILD_CMD) install

install-dev:
	@$(BUILD_CMD) install-dev

clean:
	@$(BUILD_CMD) clean

build:
	@$(BUILD_CMD) build

.PHONY: build

# vim: tabstop=8 shiftwidth=8 noexpandtab
