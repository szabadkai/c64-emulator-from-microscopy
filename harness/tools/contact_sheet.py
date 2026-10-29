# contact_sheet.py OUT.png COLS SCALE f1.png f2.png ... - grid of indexed PNGs written by c64 (stdlib only)
import sys, zlib, struct
def read_png(path):
    d=open(path,'rb').read(); p=8; idat=b''; pal=None
    while p < len(d):
        n=struct.unpack('>I',d[p:p+4])[0]; t=d[p+4:p+8]; c=d[p+8:p+8+n]; p+=12+n
        if t==b'IHDR': w,h=struct.unpack('>II',c[:8])
        elif t==b'PLTE': pal=[tuple(c[i:i+3]) for i in range(0,len(c),3)]
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat)
    return w,h,[raw[y*(w+1)+1:(y+1)*(w+1)] for y in range(h)],pal
out, cols, sc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
imgs=[read_png(f) for f in sys.argv[4:]]
w,h=imgs[0][0]//sc, imgs[0][1]//sc; gap=4
rows=(len(imgs)+cols-1)//cols
W=cols*w+(cols-1)*gap; H=rows*h+(rows-1)*gap
canvas=[bytearray(b'\x80'*(W*3)) for _ in range(H)]
for k,(iw,ih,px,pal) in enumerate(imgs):
    ox=(k%cols)*(w+gap); oy=(k//cols)*(h+gap)
    for y in range(h):
        row=px[y*sc]; line=canvas[oy+y]
        for x in range(w):
            r,g,b=pal[row[x*sc]]; o=(ox+x)*3; line[o]=r; line[o+1]=g; line[o+2]=b
raw=b''.join(b'\x00'+bytes(l) for l in canvas)
def chunk(t,c): return struct.pack('>I',len(c))+t+c+struct.pack('>I',zlib.crc32(t+c)&0xffffffff)
open(out,'wb').write(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw,6))+chunk(b'IEND',b''))
