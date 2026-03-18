# platform module for AmigaOS
import sys
import uos


def system():
    return "AmigaOS"


def machine():
    return uos._cpu()


def processor():
    return uos._cpu()


def version():
    return "Kickstart " + uos._kickstart()


def release():
    return uos._kickstart()


def python_implementation():
    return "MicroPython"


def python_version():
    v = sys.implementation.version
    return "%d.%d.%d" % (v[0], v[1], v[2])


def platform():
    return "AmigaOS-%s-%s-MicroPython_%s" % (uos._kickstart(), uos._cpu(), python_version())


def node():
    return "Amiga"


def fpu():
    return uos._fpu()


def chipset():
    return uos._chipset()


def amiga_chipmem():
    return uos._chipmem()


def amiga_fastmem():
    return uos._fastmem()


def amiga_info():
    chip = uos._chipmem()
    fast = uos._fastmem()
    return "CPU: %s | FPU: %s | Chipset: %s | Kickstart: %s | Chip: %dKB | Fast: %dKB" % (
        uos._cpu(), uos._fpu(), uos._chipset(), uos._kickstart(),
        chip // 1024, fast // 1024)
