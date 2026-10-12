#!/usr/bin/env python3
"""A text for make test-convert: about 3000 characters (seven LisaWrite
pages) in paragraphs of many lengths -- empty ones, short ones, and one longer than a LisaWrite page -- so
that the conversion fills many pages, continues a paragraph across pages and
ends pages on paragraph ends."""
import sys

words = ('the Lisa talks to an AppleShare server over LocalTalk and copies '
         'files both ways while the network runs in a tool').split()
out = []
for i in range(40):
    n = (i * 7) % 23 if i != 13 else 120          # paragraph 13: about 700 characters
    out.append(' '.join(words[(i + j) % len(words)] for j in range(n)))
sys.stdout.write('A Long Text\n\n' + '\n'.join(out) + '\nThe end.\n')
