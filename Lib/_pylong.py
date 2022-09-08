import sys
import decimal

def long_to_decimal_string(n):
    # Proof-of-concept implementation, authored by Tim Peters. See the
    # todecstr.py file attached to
    # https://github.com/python/cpython/issues/90716

    #print('long_to_decimal_string', file=sys.stderr)
    D = decimal.Decimal
    D2 = D(2)

    BITLIM = 128

    def inner(n):
        w = n.bit_length()
        if w <= BITLIM:
            return D(n)
        w >>= 1
        hi = n >> w
        lo = n - (hi << w)
        return inner(hi) * w2pow[w] + inner(lo)

    with decimal.localcontext() as ctx:
        ctx.prec = decimal.MAX_PREC
        ctx.Emax = decimal.MAX_EMAX
        ctx.Emin = decimal.MIN_EMIN
        ctx.traps[decimal.Inexact] = 1

        w2pow = {}
        w = n.bit_length()
        while w >= BITLIM:
            w2 = w >> 1
            if w & 1:
                w2pow[w2 + 1] = None
            w2pow[w2] = None
            w = w2
        if w2pow:
            it = reversed(w2pow.keys())
            w = next(it)
            w2pow[w] = D2 ** w
            for w in it:
                if w - 1 in w2pow:
                    val = w2pow[w - 1] * D2
                else:
                    w2 = w >> 1
                    val = w2pow[w2] * w2pow[w - w2]
                w2pow[w] = val
            #print(w2pow.keys())
        return str(inner(n))
