# arexx_IBrowse.py - Test script for arexx.Port with IBrowse

import arexx

print("=== ARexx Test ===")
print()

# Test 1: Basic Port usage with explicit close
print("--- Test 1: Port with close() ---")
if arexx.exists("IBROWSE"):
    port = arexx.Port("IBROWSE")
    print(f"  Port created for '{port.portname}', closed={port.closed}")
    
    rc = port.send("SHOW")
    print(f"  send SHOW: rc={rc}")
    
    rc, url = port.send("QUERY ITEM=URL", result=True)
    print(f"  send QUERY ITEM=URL: rc={rc}, result={url}")
    
    rc, title = port.send("QUERY ITEM=TITLE", result=True)
    print(f"  send QUERY ITEM=TITLE: rc={rc}, result={title}")
    
    port.close()
    print(f"  After close: closed={port.closed}")
    
    # Verify send after close raises error
    try:
        port.send("SHOW")
        print("  ERROR: should have raised OSError!")
    except OSError as e:
        print(f"  Correctly raised OSError after close: {e}")
else:
    print("  IBrowse not running, skipping")

print()

# Test 2: Context manager (with statement)
print("--- Test 2: Port with context manager ---")
if arexx.exists("IBROWSE"):
    with arexx.Port("IBROWSE") as ib:
        print(f"  Inside 'with': closed={ib.closed}")
        rc, url = ib.send("QUERY ITEM=URL", result=True)
        print(f"  send QUERY ITEM=URL: rc={rc}, result={url}")
    
    # After with block, port should be closed
    print(f"  After 'with': closed={ib.closed}")
else:
    print("  IBrowse not running, skipping")

print()

# Test 3: Multiple rapid commands (performance benefit of reusing port)
print("--- Test 3: Multiple commands via persistent Port ---")
if arexx.exists("IBROWSE"):
    with arexx.Port("IBROWSE") as ib:
        items = ["URL", "TITLE", "ACTIVEBROWSERNR", "ACTIVEWINDOWNR"]
        for item in items:
            rc, val = ib.send(f"QUERY ITEM={item}", result=True)
            print(f"  QUERY ITEM={item}: rc={rc}, val={val}")
else:
    print("  IBrowse not running, skipping")

print()

# Test 4: __del__ safety net (port not explicitly closed)
print("--- Test 4: GC safety net ---")
if arexx.exists("IBROWSE"):
    port = arexx.Port("IBROWSE")
    port.send("SHOW")
    print("  Port created and used, NOT calling close()")
    port = None  # drop reference, GC should clean up
    import gc
    gc.collect()
    print("  gc.collect() done, port should be cleaned up")
else:
    print("  IBrowse not running, skipping")

print()
print("=== All tests complete ===")