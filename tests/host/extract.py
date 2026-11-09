# Builds gen_tts.h from the firmware's real tts.h: drops #includes and the network-only functions
# (request, synthesize), keeps everything else verbatim. ui::asciify comes from the real text.h.
import sys
src = sys.argv[1]
tts = open(f"{src}/tts.h").read().splitlines()

def cut(lines, starts):
    out, skip = [], False
    for l in lines:
        if not skip and any(l.startswith(s) for s in starts):
            skip = True
            while out and out[-1].startswith("//"):  # its leading comment
                out.pop()
        if skip:
            if l == "}":
                skip = False
            continue
        out.append(l)
    return out

body = [l for l in tts if not l.startswith("#include") and not l.startswith("#pragma")]
body = cut(body, ["inline Synth request(", "inline Synth synthesize("])
out = ["#pragma once", '#include "brand.h"', '#include "text.h"'] + body
open("gen_tts.h", "w").write("\n".join(out) + "\n")
