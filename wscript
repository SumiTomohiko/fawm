
APPNAME = "UnnamedWindowManager"
VERSION = "0.1.0"

def options(opt):
    opt.load("compiler_c")

def configure(conf):
    conf.check_tool("compiler_c")

def build(bld):
    bld.recurse("src")

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4 filetype=python
