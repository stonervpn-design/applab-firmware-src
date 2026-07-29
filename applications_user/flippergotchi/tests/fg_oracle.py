#!/usr/bin/env python3
"""Pure-logic oracle mirroring flippergotchi v2.0 C modules (no host C compiler here).
Run: python fg_oracle.py  -> asserts the algorithms behave as the C ports must."""

MAXN = 96
HAS_PMKID, HAS_M2, HAS_M3, HAS_M4, FULL, SOLICITED = 1, 2, 4, 8, 16, 32

# ---------------- net table ----------------
class Net:
    def __init__(s): s.bssid=None; s.ssid=""; s.ch=0; s.rssi=0; s.flags=0; s.last=0
class NetTable:
    def __init__(s): s.nets=[];
    def find(s,b):
        for i,n in enumerate(s.nets):
            if n.bssid==b: return i
        return -1
    def observe(s,b,ch,rssi,now):
        i=s.find(b)
        if i>=0:
            s.nets[i].ch=ch; s.nets[i].rssi=rssi; s.nets[i].last=now; return i
        if len(s.nets)<MAXN:
            n=Net(); n.bssid=b; n.ch=ch; n.rssi=rssi; n.last=now; s.nets.append(n); return len(s.nets)-1
        victim=-1; oldest=1<<32
        for j,n in enumerate(s.nets):
            if n.flags&(HAS_PMKID|HAS_M2): continue
            if n.last<=oldest: oldest=n.last; victim=j
        if victim<0: return -1
        n=s.nets[victim]; n.bssid=b; n.ssid=""; n.ch=ch; n.rssi=rssi; n.flags=0; n.last=now
        return victim
    def add_flag(s,i,f):
        was=bool(s.nets[i].flags&f); s.nets[i].flags|=f; return not was
    def is_pwned(s,i): return bool(s.nets[i].flags&(HAS_PMKID|HAS_M2))
    def pwned_count(s): return sum(1 for n in s.nets if n.flags&(HAS_PMKID|HAS_M2))

def test_net():
    t=NetTable()
    a,b,c=[1]*6,[2]*6,[3]*6
    assert t.observe(a,6,-40,1)==0 and t.observe(b,6,-50,1)==1 and t.observe(c,1,-60,1)==2
    assert len(t.nets)==3
    assert t.observe(a,11,-30,5)==0 and len(t.nets)==3 and t.nets[0].ch==11  # re-observe updates
    assert t.add_flag(0,HAS_M2) is True and t.add_flag(0,HAS_M2) is False
    assert t.is_pwned(0) and t.pwned_count()==1
    # fill to 96, then a new unpwned observation evicts the oldest unpwned (never idx0=pwned)
    t2=NetTable()
    for k in range(MAXN): t2.observe([k&0xff,0,0,0,0,1],1,-40,k)  # last = k
    t2.add_flag(0,HAS_PMKID)  # oldest slot is now pwned -> must NOT be evicted
    v=t2.observe([9,9,9,9,9,9],1,-40,1000)
    assert v==1, f"evicted {v}, expected the oldest UNPWNED (idx1)"
    assert t2.nets[0].flags&HAS_PMKID  # pwned survivor untouched
    print("net_table OK")

# ---------------- AI channel engine ----------------
def xorshift(x):
    x^= (x<<13)&0xffffffff; x^= x>>17; x^= (x<<5)&0xffffffff; return x&0xffffffff
class Ai:
    def __init__(s,seed): s.ap=[0]*14; s.cl=[0]*14; s.cr=[0]*14; s.rng=seed or 1
    def score(s,ch): return 3*s.ap[ch]+2*s.cl[ch]+5*s.cr[ch]+1
    def next_ch(s):
        s.rng=xorshift(s.rng)
        if s.rng%8==0:  # epsilon explore
            s.rng=xorshift(s.rng); return 1+s.rng%13
        total=sum(s.score(c) for c in range(1,14)); s.rng=xorshift(s.rng); r=s.rng%total; acc=0
        for c in range(1,14):
            acc+=s.score(c)
            if r<acc: return c
        return 13
    def dwell(s,ch):
        sc=s.score(ch); d=400+sc*100
        if d>2000: d=2000
        if sc==1: d=150
        return d
    def reward(s,ch): s.cr[ch]+=1
    def decay(s):
        # subtract at least 1 so a channel's score actually reaches 0 (plain >>3
        # floors at 7 because 7>>3==0 and the score would stick forever).
        for c in range(14):
            if s.cr[c]>0: s.cr[c]-=(s.cr[c]>>3)+1

def test_ai():
    a=Ai(0xC0FFEE); a.ap[6]=10; a.cr[6]=5  # hot channel 6
    counts=[0]*14
    for _ in range(2000): counts[a.next_ch()]+=1
    assert counts[6]>counts[2]*3, (counts[6],counts[2])
    assert a.dwell(6)==2000 and a.dwell(2)==150
    a.cr[6]=64
    for _ in range(30): a.decay()
    assert a.cr[6]<5, a.cr[6]
    print("ai OK")

# ---------------- progression ----------------
def xp_for(level): return 10*level*(level+1)//2
def level_of(xp):
    L=0
    while xp_for(L+1)<=xp: L+=1
    return L
NAMES=["Pixel","Nibble","Byte","Sparky","Gizmo","Waffle","Mochi","Bolt"]
def default_name(seed): return NAMES[xorshift(seed)%len(NAMES)]
def award(ach,hs,pmkid,friends,full,secs,chhit):
    new=0
    def maybe(bit,cond):
        nonlocal new,ach
        if cond and not (ach&bit): ach|=bit; new|=bit
    maybe(1, hs+pmkid>=1); maybe(2, hs>=10); maybe(4, pmkid>=25)
    maybe(8, friends>=1); maybe(16, friends>=5); maybe(0x20, full>=1)
    maybe(0x40, secs>=1800); maybe(0x80, chhit>=8)
    return ach,new
def test_prog():
    assert xp_for(1)==10 and xp_for(2)==30 and xp_for(3)==60
    assert level_of(59)==2 and level_of(60)==3 and level_of(0)==0
    ach,new=award(0,1,0,0,0,0,0); assert new==1
    ach,new=award(ach,10,0,0,0,0,0); assert new==2 and (award(ach,10,0,0,0,0,0)[1]==0)
    assert award(0,0,0,0,0,1800,0)[1]==0x40 and award(0,0,0,0,0,0,8)[1]==0x80
    assert default_name(1)==default_name(1)
    print("progress OK")

# ---------------- pwngrid parse ----------------
def build_beacon(name,pwnd,face):
    hdr=[0x80,0]+[0,0]+[0xff]*6+[0xde,0xad,0xbe,0xef,0xde,0xad]*2  # note: SA+BSSID both sig
    hdr=[0x80,0,0,0]+[0xff]*6+[0xde,0xad,0xbe,0xef,0xde,0xad]+[0xde,0xad,0xbe,0xef,0xde,0xad]
    hdr+=[0,0]+[0]*8+[0x64,0]+[0,0]
    j=('{"name":"%s","pwnd_tot":%d,"face":"%s"}'%(name,pwnd,face)).encode()
    return bytes(hdr+[0xde,len(j)]+list(j))
def parse(fr):
    # scan from offset 38 for the JSON keys, bounds-checked
    def sfind(key,conv):
        k=key.encode()
        for i in range(38,len(fr)-len(k)):
            if fr[i:i+len(k)]==k:
                s=i+len(k); out=b""
                while s<len(fr) and fr[s] not in (ord('"'),ord(',')) and len(out)<19:
                    out+=bytes([fr[s]]); s+=1
                return conv(out)
        return None
    name=sfind('name":"',lambda b:b.decode('latin1'))
    if not name: return None
    pwnd=sfind('pwnd_tot":',lambda b:int(b or 0))
    face=sfind('face":"',lambda b:b.decode('latin1'))
    return (name,pwnd,face)
def test_pwngrid():
    fr=build_beacon("Brute",7,"(^^)")
    assert fr[10:16]==bytes([0xde,0xad,0xbe,0xef,0xde,0xad])
    assert parse(fr)==("Brute",7,"(^^)")
    assert parse(fr[:40]) is None  # truncated -> no over-read, no name
    print("pwngrid OK")

# ---------------- wigle csv ----------------
def wigle_row(bssid,ssid,ch,rssi,lat,lon):
    mac=":".join("%02X"%b for b in bssid)
    return f"{mac},{ssid},[WPA2],,{ch},{rssi},{lat:.6f},{lon:.6f}"
def test_geo():
    assert wigle_row([0xde,0xad,0xbe,0xef,0,1],"Home",6,-42,51.5,-0.12)==\
        "DE:AD:BE:EF:00:01,Home,[WPA2],,6,-42,51.500000,-0.120000"
    print("geo OK")

if __name__=="__main__":
    test_net(); test_ai(); test_prog(); test_pwngrid(); test_geo()
    print("ALL ORACLE TESTS PASS")
