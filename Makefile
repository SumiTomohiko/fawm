
WAF = ./waf -v

all:
	$(WAF) build

configure:
	$(WAF) configure

install:
	$(WAF) install

# vim: tabstop=8 shiftwidth=8 noexpandtab
