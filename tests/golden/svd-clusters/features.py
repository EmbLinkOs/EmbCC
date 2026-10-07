"""What --json must say about features.svd, beyond where the registers
are (jsoncheck.py and svdref.py see to that): the cpu, the memories, the
interrupts, derivedFrom, the index of each array element, and the fields
with their enumerated values.

    python3 features.py features.json
"""
import json
import sys

d = json.load(open(sys.argv[1]))
bad = []


def want(what, got, exp):
    if got != exp:
        bad.append("%s: %r, not %r" % (what, got, exp))


want("schema", d["schema"], 1)
want("device", (d["device"]["name"], d["device"]["vendor"], d["device"]["version"]),
     ("FEATURES", "EmbLinkOs", "1.0"))
c = d["cpu"]
want("cpu", (c["name"], c["revision"], c["endian"], c["mpuPresent"], c["fpuPresent"],
             c["nvicPrioBits"], c["vendorSystickConfig"]),
     ("CM4", "r0p1", "little", True, True, 3, False))
want("memories", d["memories"],
     [{"name": "FLASH", "origin": 0x08000000, "length": 0x40000, "access": "rx"},
      {"name": "RAM", "origin": 0x20000000, "length": 0x10000, "access": "rwx"}])
want("interrupts", [(q["name"], q["value"]) for q in d["interrupts"]],
     [("DMA", 5), ("NET", 6), ("NET2", 7)])

P = {p["name"]: p for p in d["peripherals"]}
want("peripherals", list(P), ["DMA", "NET", "NET2", "TIMER0", "TIMER1", "TIMER2"])
want("DMA interrupts", P["DMA"]["interrupts"],
     [{"name": "DMA", "value": 5, "description": "DMA channel done"}])
want("DMA addressBlocks", P["DMA"]["addressBlocks"],
     [{"offset": 0, "address": 0x40001000, "size": 0x400, "usage": "registers"}])
want("NET2", (P["NET2"]["derivedFrom"], P["NET2"]["typeName"], P["NET2"]["baseAddress"],
              [q["name"] for q in P["NET2"]["interrupts"]]),
     ("NET", "NETIF_Type", 0x40003000, ["NET2"]))
want("TIMER2", (P["TIMER2"]["derivedFrom"], P["TIMER2"]["typeName"],
                P["TIMER2"]["baseAddress"]), ("TIMER0", "TIMER_Type", 0x40004800))


def reg(p, path):
    for r in P[p]["registers"]:
        if r["path"] == path:
            return r
    bad.append("%s has no register %s" % (p, path))
    return None


def check(p, path, **kw):
    r = reg(p, path)
    if r:
        for k, v in kw.items():
            want("%s.%s %s" % (p, path, k), r[k], v)
    return r


check("DMA", "CH[3].CFG", name="CFG", index=[3], address=0x4000116c)
check("DMA", "DATA[2]", name="DATA[2]", index=[2], address=0x40001208)
check("DMA", "PRIO1", name="PRIO[1]", index=[1], address=0x40001218)
check("DMA", "GPIOC_CTRL", name="GPIOC_CTRL", index=[2], address=0x40001238)
check("DMA", "IRQ9", name="IRQ9", index=[1])
check("DMA", "MODEB", name="MODEB", index=[1], size=16, resetMask=0xffff)
check("DMA", "STATUS", access="read-only", address=0x40001004)
check("DMA", "CLEAR", access="write-only", address=0x40001004)
check("DMA", "DATAX", index=[], address=0x40001280)
check("NET", "PORT[1].QUEUE[2].DESC[1].FLAGS", name="FLAGS", index=[1, 2, 1],
      address=0x40002266, size=16)
check("NET", "PORT[0].STAT.TX", index=[0], size=16, access="read-only")
check("NET", "LANER.LVL", name="LVL", index=[1], address=0x40002414)
check("NET", "SPARE.LVL", index=[], address=0x40002504)
check("NET", "MODE_B.Z", address=0x40002608)
check("NET2", "PORT[1].QUEUE[3].TAIL", address=0x40003274, index=[1, 3])
check("TIMER1", "CC[2].VALUE", address=0x40004418, index=[2])
check("TIMER2", "LANEX.LVL", address=0x40004834)
check("TIMER0", "VAL", access="read-only")

ctrl = check("DMA", "CTRL", resetValue=0x10, resetMask=0xffffffff)
ctrl2 = check("DMA", "CTRL2", description="Control, again", resetValue=0x10)
if ctrl and ctrl2:
    want("CTRL2's fields are CTRL's", ctrl2["fields"], ctrl["fields"])
if ctrl:
    F = {f["name"]: f for f in ctrl["fields"]}
    want("CTRL fields", list(F),
         ["EN", "MODE", "PRI", "EN2", "CH0_IE", "CH1_IE", "CH2_IE", "CH3_IE"])
    want("EN", (F["EN"]["bitOffset"], F["EN"]["bitWidth"], F["EN"]["access"]),
         (0, 1, "read-write"))
    want("EN values", [(e["name"], e["value"]) for e in F["EN"]["enumeratedValues"]],
         [("Disabled", 0), ("Enabled", 1)])
    want("EN2 derivedFrom EN", (F["EN2"]["bitOffset"], F["EN2"]["bitWidth"],
                                F["EN2"]["description"], F["EN2"]["enumeratedValues"]),
         (8, 1, "Second enable", F["EN"]["enumeratedValues"]))
    want("PRI's enumeratedValues derivedFrom ENV",
         [(e["name"], e["value"]) for e in F["PRI"]["enumeratedValues"]],
         [("Disabled", 0), ("Enabled", 1)])
    want("MODE", (F["MODE"]["bitOffset"], F["MODE"]["bitWidth"], F["MODE"]["access"]),
         (1, 2, "read-only"))
    want("MODE values", F["MODE"]["enumeratedValues"],
         [{"name": "Single", "value": 0, "isDefault": False, "usage": "read",
           "description": None},
          {"name": "Burst", "value": 2, "care": 2, "isDefault": False, "usage": "read",
           "description": None},
          {"name": "Other", "value": None, "isDefault": True, "usage": "read",
           "description": None}])
    want("CH3_IE", (F["CH3_IE"]["bitOffset"], F["CH3_IE"]["bitWidth"]), (19, 1))
idr = check("NET", "ID", access="read-only", resetValue=0x4e01)
if idr:
    v = idr["fields"][1]
    want("VENDOR derivedFrom DMA.CTRL.MODE",
         (v["name"], v["bitOffset"], v["bitWidth"], v["access"], len(v["enumeratedValues"])),
         ("VENDOR", 8, 2, "read-only", 3))
    want("REV inherits the register's access", idr["fields"][0]["access"], "read-only")

if bad:
    print("\n".join("FAIL: " + b for b in bad))
    sys.exit(1)
print("features.json says what features.svd says")
