
APPNAME = "UnnamedWindowManager"
VERSION = "0.1.0"

def options(ctx):
    ctx.load("compiler_c")

configure = options

def build(ctx):
    ctx.recurse("src")

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4 filetype=python
