# docs/superpowers/plans/refs/dedup_ref.py
WINDOW_MS = 150
_LOW  = {"Jeep V0", "Hitag2"}
_HIGH = {"Hyundai TL", "KeeLoq", "Security+ 1.0", "Security+ 2.0"}

def score(name, bit_count):
    tier = 1000 if name in _LOW else 3000 if name in _HIGH else 2000
    return tier + min(bit_count, 999)   # bit-count tiebreak, never crosses a tier

def new_state():
    return {"last_protocol": None, "last_score": 0, "last_tick": 0, "last_idx": -1}

def decide(st, name, sc, now):
    if st["last_idx"] >= 0 and (now - st["last_tick"]) <= WINDOW_MS:
        return "replace" if sc > st["last_score"] else "suppress"
    return "add"

def commit(st, name, sc, now, idx):
    st.update(last_protocol=name, last_score=sc, last_tick=now, last_idx=idx)

if __name__ == "__main__":
    # 1) Hyundai then jeep inside window -> jeep suppressed
    st = new_state()
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 1000) == "add"
    commit(st, "Hyundai TL", s_h, 1000, 0)
    s_j = score("Jeep V0", 24);   assert decide(st, "Jeep V0", s_j, 1050) == "suppress"

    # 2) jeep then Hyundai inside window -> replace (specific wins even arriving 2nd)
    st = new_state()
    s_j = score("Jeep V0", 24); assert decide(st, "Jeep V0", s_j, 2000) == "add"
    commit(st, "Jeep V0", s_j, 2000, 5)
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 2040) == "replace"

    # 3) two low-tier inside window -> second suppressed (equal score, not >)
    st = new_state()
    s_j = score("Jeep V0", 24); assert decide(st, "Jeep V0", s_j, 100) == "add"
    commit(st, "Jeep V0", s_j, 100, 1)
    s_hit = score("Hitag2", 24); assert decide(st, "Hitag2", s_hit, 120) == "suppress"

    # 4) same protocol, repeat OUTSIDE window -> add (real 2nd press)
    st = new_state()
    s_h = score("Hyundai TL", 40); assert decide(st, "Hyundai TL", s_h, 500) == "add"
    commit(st, "Hyundai TL", s_h, 500, 2)
    assert decide(st, "Hyundai TL", s_h, 500 + WINDOW_MS + 1) == "add"

    # 5) unrelated medium-tier protocols far apart -> both add
    st = new_state()
    s_a = score("CAME", 24); assert decide(st, "CAME", s_a, 0) == "add"
    commit(st, "CAME", s_a, 0, 3)
    s_b = score("Nice FLO", 12); assert decide(st, "Nice FLO", s_b, 5000) == "add"

    # tier ordering sanity
    assert score("Hyundai TL", 0) > score("CAME", 999) > score("Jeep V0", 999)
    print("ALL DEDUP ALGORITHM TESTS PASSED")
