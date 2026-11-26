#!/usr/bin/env python3
"""Build the T096 flash table for readable MT_NOTICE literals using byte-pair encoding."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def messages(source):
    strings = re.findall(r'MT_NOTICE\(("(?:[^"\\]|\\.)*")\)',source.read_text())
    return sorted({json.loads(s).encode('ascii') for s in strings})


def key(text):
    digest=2166136261
    for byte in text:
        digest=((digest^byte)*16777619)&0xffffffff
    return digest


def generate(source):
    raw=messages(source)
    if len({key(s) for s in raw})!=len(raw):
        raise ValueError('notice ID collision')
    if any(not s or any(c<32 or c>=128 for c in s) for s in raw):
        raise ValueError('expected a printable ASCII notice')
    encoded=raw.copy()
    pairs=[]
    for token in range(128,256):
        counts=Counter((s[i],s[i+1]) for s in encoded for i in range(len(s)-1))
        if not counts:break
        pair,frequency=counts.most_common(1)[0]
        if frequency<3:break
        pairs.append(pair)
        encoded=[s.replace(bytes(pair),bytes([token])) for s in encoded]
    lines=['// Generated from MT_NOTICE literals by pack-notices.py.','#pragma once',
           'namespace mt::companion::notice {','inline constexpr unsigned char pairs[][2]={',
           *('    {%d,%d},'%p for p in pairs),'};','template<unsigned Id> struct Text;']
    for original,packed in zip(raw,encoded):
        # The marker is outside the printable ASCII range. Token 0 terminates each entry.
        escaped=''.join('\\%03o'%n for n in b'\x01'+packed)
        lines.append(f'template<> struct Text<{key(original)}u> {{static constexpr char bytes[]="{escaped}";}};')
    return '\n'.join([*lines,'}',''])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path)
    p.add_argument('output',type=Path)
    args=p.parse_args()
    args.output.write_text(generate(args.source))
