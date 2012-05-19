
WAF = ./waf -v

all: build

configure:
	@$(WAF) configure

install:
	@$(WAF) install

clean:
	@$(WAF) clean

build:
	@$(WAF) build

.PHONY: build

# vim: tabstop=8 shiftwidth=8 noexpandtab
