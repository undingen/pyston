# Test a generator cycle involving an unfinished generator.
def f():
    g = (i in (None, g, g) for i in xrange(2))
    print g.next()
    # map(type(g).next, [g])
f()

def f():
   g = None
   def G():
       c = g
       g
       #c = None
       yield 1
       yield 2
       yield 3
   g = G()
   g.next()

print f()
import gc
gc.collect
print gc.garbage
