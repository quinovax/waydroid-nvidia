#!/usr/bin/env python3
"""Shader publication order in an nvcap capture (push mode): nvcap_publish.py CAP

The proprietary driver publishes a shader program in this order: copy-engine copy
of the program into the (never CPU-mapped) shader heap, then, in the first
submission on the 3D channel after the upload, an acquire of the copy's
completion, then INVALIDATE_SHADER_CACHES_NO_WFI with INSTRUCTION set, then the
draws that use it. This tool checks NVK's captured stream for the same order:

  * every SET_PIPELINE_PROGRAM_ADDRESS uses an address that a copy-engine copy
    (OFFSET_OUT) wrote earlier;
  * between that copy's submission and the use, the using context had a
    submission with at least one wait whose pushes contain the INSTRUCTION
    invalidate (3D or compute class form);
  * the program address is 256 B aligned and, when the prefetch size follows the
    address (Ampere B and later), the prefetch stays inside the copied range.

Exit status 0 when every use passes, 1 otherwise."""
import os
import sys

os.environ.setdefault('NV_CLASS_HEADERS', '/home/forsen/repos/waydroid-nvidia-prolapse/build/mesa-gl-source/src/nouveau/headers/nvidia/classes/')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvcap


def methods(words):
    """(name, values) per method, decoded as nvcap_seq.py does."""
    i, n = 0, len(words)
    while i < n:
        h = words[i]
        if h == 0:
            i += 1
            continue
        op = h >> 29
        cnt = (h >> 16) & 0x1fff
        sc = (h >> 13) & 7
        m = (h & 0xfff) << 2
        if op == 4:
            vals = [cnt]
            i += 1
        elif op in (1, 3, 5):
            vals = list(words[i + 1:i + 1 + cnt])
            i += 1 + cnt
        else:
            i += 1
            continue
        if m < 0x100:
            name = 'HOST.' + nvcap.HOST.get(m, f'{m:#x}')
        else:
            cname, table = nvcap.SUBC.get(sc, (f'sc{sc}', {}))
            name = cname + '.' + table.get(m, f'{m:#06x}')
        yield name, vals


def main(path):
    cap = nvcap.parse(nvcap.read(path))
    copies = {}          # destination -> (size, submit index)
    last_copy = -1       # index of the newest submission carrying copies
    published = {}       # context -> newest copy submission covered by acquire + invalidate
    uses = invalidates = errors = 0
    for si, s in enumerate(cap['submits']):
        named = [nv for (_, _, _, words) in s['pushes'] if words for nv in methods(words)]
        for name, vals in named:
            if name == 'CE.OFFSET_IN_UPPER' and len(vals) >= 5:
                copies[vals[2] << 32 | vals[3]] = (vals[4], si)
                last_copy = si
        if any(name.endswith('INVALIDATE_SHADER_CACHES_NO_WFI') and vals[0] & 1 for name, vals in named):
            invalidates += 1
            if s['waits'] < 1 and last_copy > published.get(s['ctx'], -1):
                print(f"submit {si}: instruction invalidate without an acquire of the uploads")
                errors += 1
            published[s['ctx']] = last_copy
        for name, vals in named:
            if not name.startswith('3D.SET_PIPELINE_PROGRAM_ADDRESS_A(') or len(vals) < 2:
                continue
            uses += 1
            addr = vals[0] << 32 | vals[1]
            if addr not in copies:
                print(f"submit {si}: {name} {addr:#x} was never written by a copy-engine copy")
                errors += 1
                continue
            size, ci = copies[addr]
            if ci > published.get(s['ctx'], -1):
                print(f"submit {si}: {name} {addr:#x} used before acquire + invalidate (copy in submit {ci})")
                errors += 1
            if addr % 256:
                print(f"submit {si}: {name} {addr:#x} not 256 B aligned")
                errors += 1
            if len(vals) >= 3 and vals[2] * 256 > size:
                print(f"submit {si}: {name} {addr:#x} prefetch {vals[2]} blocks beyond the copied {size:#x} B")
                errors += 1
    print(f"submits {len(cap['submits'])}, copies {len(copies)}, instruction invalidates {invalidates}, "
          f"program address uses {uses}, errors {errors}")
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
