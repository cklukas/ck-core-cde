#!/usr/bin/env python3
"""Write the seed corpus of the fuzz targets to cde/fuzz/corpus/NAME/.

The seeds are small valid inputs in the layout each fuzz-NAME.c documents at
its top.  Run it again to regenerate them; it only writes files below
corpus/ next to this script.
"""
from pathlib import Path
import struct

root = Path(__file__).resolve().parent / "corpus"


def put(target, name, data):
    d = root / target
    d.mkdir(parents=True, exist_ok=True)
    (d / name).write_bytes(data if isinstance(data, bytes) else data.encode())


# ---- spc-proto: byte 0 selects the entry point ------------------------------------
def counted(*strings):
    """"<count in hex> NUL string NUL ..." as sprint_counted_string writes it."""
    out = b"%x \0" % len(strings)
    for s in strings:
        out += s.encode() + b"\0"
    return out


def spc_request(cid, rtype, payload, seq=1, length=None):
    length = len(payload) if length is None else length
    return b"%08x%02x%04x%04x  " % (cid, rtype, length, seq) + payload


put("spc-proto", "header", b"\x00" + spc_request(1, 1, b"")[:20])
put("spc-proto", "application", b"\x01" + counted("/bin/ls") + counted("/tmp") + counted("ls", "-l") + counted("A=b", "C=d"))
put("spc-proto", "register", b"\x04" + counted("user", "pass", "1000", "host:sys:1"))
put("spc-proto", "whole-request", b"\x06" + spc_request(0, 1, counted("/bin/ls") + counted("/tmp") + counted("ls") + counted()))
put("spc-proto", "multi-packet",
    b"\x07" + spc_request(0, 3, b"2 a=b\0c=d\0"))

# ---- dtwm-session: the content of dtwmfp.session ------------------------------------
put("dtwm-session", "posted", "1\nsub1 5 6 0\n0\n0\n1\n")
put("dtwm-session", "default-control", "0\n1\nsub2 ctl3\n0\n1\n")
put("dtwm-session", "dynamic", "1\nsub1 5 6 1\n1\nsub1 ctl1\n3\ndynfile ctl1 1 box1 1 0\ndynfile sub1 2 par 1 0\ndynfile x 1 y 2 1\n0\n")

# ---- dtsession-client: byte 0 even: client file, odd: quoting parser ---------------------
put("dtsession-client", "local", b'\x00dtsmcmd -cmd "dtterm -e ls" -screen 0\n')
put("dtsession-client", "hints", b'\x00dtsmcmd -hints "1 2 3 4" -cmd "xclock -geometry 100x100" -screen 1\n')
put("dtsession-client", "remote", b'\x00dtsmcmd -host far -cmd "xterm -display far:0" -screen 0\nnot a command\n')
put("dtsession-client", "quoting", b'\x01plain "quoted words" back\\ slash "nested \\"inner\\"" tail')

# ---- dtsession-palette: byte 0 setup (see the top of the harness), then the data ---------
palette = b"#101010\n#202020\n#303030\n#404040\n#505050\n#606060\n#707070\n#808080\n!end\n"
put("dtsession-palette", "high-color", b"\x03" + palette)
put("dtsession-palette", "low-color", b"\x01" + palette)
put("dtsession-palette", "black-white", b"\x00" + palette)
put("dtsession-palette", "no-final-newline", b"\x03#ffffff")
put("dtsession-palette", "directories", b"\x80/nonexistent/palettes  /usr/dt/palettes ")

# ---- dtwm-property: reader, flags, count (2), left, data ----------------------------------
FORMAT32 = 0x10
MATCH = 0x04
ONE_BYTE_ITEMS = 0x40


def prop(reader, flags, count, data=b"", left=0):
    return bytes([reader, flags]) + struct.pack("<H", count) + bytes([left]) + data


put("dtwm-property", "wm-state", prop(0, MATCH | FORMAT32 | ONE_BYTE_ITEMS, 2, bytes([1, 0])))
put("dtwm-property", "utf8-string", prop(2, MATCH | 0x00, 6, b"hello\0"))
put("dtwm-property", "normal-hints", prop(4, MATCH | FORMAT32 | ONE_BYTE_ITEMS, 18, bytes(range(18))))
put("dtwm-property", "colormap-windows", prop(5, MATCH | FORMAT32 | ONE_BYTE_ITEMS | 0x80, 3, bytes([1, 2, 3])))
put("dtwm-property", "wm-protocols", prop(6, MATCH | FORMAT32 | ONE_BYTE_ITEMS, 3, bytes([18, 19, 20])))


# ---- dtlogin-xdmcp: byte 0 state, then the XDMCP datagram -------------------------------------
def a8(b):
    return struct.pack(">H", len(b)) + b


def aa8(items):
    return bytes([len(items)]) + b"".join(a8(i) for i in items)


def xdmcp(opcode, body):
    return struct.pack(">HHH", 1, opcode, len(body)) + body


auth = [b"MIT-MAGIC-COOKIE-1"]
put("dtlogin-xdmcp", "query", b"\x00" + xdmcp(2, aa8(auth)))
put("dtlogin-xdmcp", "forward-query", b"\x00" + xdmcp(4, a8(bytes([10, 9, 8, 7])) + a8(struct.pack(">H", 1234)) + aa8(auth)))
request = (struct.pack(">H", 0) + bytes([1]) + struct.pack(">H", 0) + aa8([bytes([10, 1, 2, 3])]) + a8(b"")
           + a8(b"") + aa8(auth) + a8(b"fuzz display"))
put("dtlogin-xdmcp", "request", b"\x00" + xdmcp(7, request))
put("dtlogin-xdmcp", "manage", b"\x04" + xdmcp(10, struct.pack(">IH", 1, 0) + a8(b"MyDisplay")))
put("dtlogin-xdmcp", "keepalive", b"\x24" + xdmcp(13, struct.pack(">HI", 0, 1)))
# ---- dtsvc-dbreader: byte 0 flags (bit 0 multibyte locale, bit 1 version fixed), then the file ----
put("dtsvc-dbreader", "attributes",
    b"\x00DATA_ATTRIBUTES TextFile\n{\n\tICON\t\ttext\n\tDESCRIPTION\tA text \\\n\t\t\tfile\n}\n")
put("dtsvc-dbreader", "criteria",
    b"\x00set DtDbVersion=1.0\nset T=/usr/dt\nDATA_CRITERIA TextFileCriteria1\n{\n\tDATA_ATTRIBUTES_NAME\tTextFile\n"
    b"\tMODE\t\t!d\n\tNAME_PATTERN\t*.txt\n}\n")
put("dtsvc-dbreader", "action",
    b"\x00# comment\nACTION Open\n{\n\tLABEL\tOpen\n\tTYPE\tCOMMAND\n"
    b"\tEXEC_STRING\t${HOME}/bin/open %(File)Arg_1\"Name:\"%\n}\n")
put("dtsvc-dbreader", "multibyte",
    b"\x01set A=caf\xc3\xa9\nACTION \xc3\xa9x\n{\n\tLABEL ${A} $A ${FUZZVAR}\n}\n")

# ---- dtsvc-mmcache: a type database cache file: header, then 32-bit integers ---------------------
def mm_cache(strings, db_name, records):
    """records: [(name, [(field, value), ...])]; one database of that name."""
    sdata, off = b"", {}
    for t in strings:
        off[t] = len(sdata)
        sdata += t.encode() + b"\0"
    sdata += b"\0" * (-len(sdata) % 4)
    count = 8
    entries = []
    for t in strings:
        h, i = 0, 1
        for c in t.encode():
            h += c << i
            i += 1
        h &= 0xffffffff
        entries.append((off[t], h % count))
    buckets = [0xffff] * count
    chain = []
    for n, (key, b) in enumerate(entries):
        chain.append([key, 0xffff, 0xffff])
        if buckets[b] == 0xffff:
            buckets[b] = n
        else:
            j = buckets[b]
            while chain[j][2] != 0xffff:
                j = chain[j][2]
            chain[j][2] = n
    # table: count bucket entries (key unused) then the chained entries; "first" indexes the whole array
    tab = [struct.pack("<IHH", 0, buckets[b] + count if buckets[b] != 0xffff else 0xffff, 0xffff) for b in range(count)]
    for key, _, nxt in chain:
        tab.append(struct.pack("<IHH", key, 0xffff, nxt + count if nxt != 0xffff else 0xffff))
    strtab = struct.pack("<IIII", 16 + len(sdata) + 8 * (count + len(chain)), 16, 16 + len(sdata), count) + sdata + b"".join(tab)
    ints = []

    def add(b):
        b += b"\0" * (-len(b) % 4)
        ints.extend(struct.unpack("<%di" % (len(b) // 4), b))
        return len(ints) - len(b) // 4
    names = [r[0] for r in records]
    fields_at = []
    for _, fl in records:
        fields_at.append(add(b"".join(struct.pack("<ii", off[a], off[v]) for a, v in fl)))
    recs_at = add(b"".join(struct.pack("<iiiii", off[n], off[strings[0]], i, len(fl), fields_at[i])
                           for i, (n, fl) in enumerate(records)))
    # the integer table of the database: an empty one (key 0 in its header)
    name_tab = add(struct.pack("<iiHH", 0, 0, 0, 0))
    db_at = add(struct.pack("<iiii", off[db_name], name_tab, len(records), recs_at))
    size_at = add(struct.pack("<i", 0))               # an empty name list preceded by its size
    str_at = add(strtab)
    files_at = add(struct.pack("<i", 0))
    mtime_at = add(struct.pack("<q", 1))
    header = struct.pack("<iiiiiiiiii", 0x1234, 1, db_at, size_at + 1, -1, 0, str_at, 1, files_at, mtime_at)
    return header + struct.pack("<%di" % len(ints), *ints)


put("dtsvc-mmcache", "one-action",
    mm_cache(["ACTION", "Open", "LABEL", "Open it", "ICON", "x"], "ACTION",
             [("Open", [("LABEL", "Open it"), ("ICON", "x")])]))
put("dtsvc-mmcache", "two-records",
    mm_cache(["DATA_ATTRIBUTES", "TextFile", "ICON", "text", "Binary", "bin"], "DATA_ATTRIBUTES",
             [("TextFile", [("ICON", "text")]), ("Binary", [("ICON", "bin")])]))
put("dtsvc-mmcache", "header-only", struct.pack("<iiiiiiiiii", 0, 0, 0, 0, -1, 0, 0, 0, 0, 0))

# ---- dthelp-sdl: flags, operation count, operations, then the SDL text ---------------------------
def sdl(flags, ops, text):
    return bytes([flags, len(ops)]) + bytes(ops) + text


put("dthelp-sdl", "elements", sdl(0x02, [0], b'<block id="b1" class="table">text <anchor id=a1> more</block>'))
put("dthelp-sdl", "attributes", sdl(0x00, [1, 3, 1, 4, 7], b' id="x1" class=\'a b\' length="12">'))
put("dthelp-sdl", "cdata", sdl(0x08, [2], b"Tom &#38; Jerry\nsecond line &#233; end<"))
put("dthelp-sdl", "multibyte", sdl(0x01, [2, 6], "caf\u00e9 \u65e5\u672c\u8a9e text<".encode()))
put("dthelp-sdl", "declaration", sdl(0x00, [6, 8], b'<!DOCTYPE sdl [ <!ENTITY a "x"> ]> <block>'))

# ---- dtlogin-xaccess: client name length, client name, then the Xaccess file ------------------------
def xaccess(name, text):
    return bytes([len(name)]) + name + text


put("dtlogin-xaccess", "hosts", xaccess(b"pc1", b"# comment\n10.0.0.2 serverA serverB\n*.example.org CHOOSER BROADCAST\n"))
put("dtlogin-xaccess", "alias", xaccess(b"x.example.org", b"%servers serverA serverB \\\n  serverC\n10.0.0.2 %servers\n"))
put("dtlogin-xaccess", "pattern", xaccess(b"term7.lab", b"term?.lab CHOOSER %hosts\n%hosts hostA hostB\n!bad.lab\n"))
put("dtlogin-xaccess", "direct", xaccess(b"pc2", b"pc2\n127.0.0.1 CHOOSER BROADCAST\n"))

print("seeds written below", root)
