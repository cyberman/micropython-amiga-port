import arexx

with arexx.Port("WORKBENCH") as wb:
    # File management
    wb.send("NEWDRAWER 'RAM:MyProject'")
    wb.send("RENAME 'RAM:MyProject' 'NewName'")
    wb.send("DELETE 'RAM:NewName' ALL")

    # Windows management
    wb.send("WINDOW 'Work:' OPEN")
    wb.send("CHANGEWINDOW 'Work:' LEFTEDGE 10 TOPEDGE 30 WIDTH 400 HEIGHT 300")
    wb.send("WINDOWTOFRONT 'Work:'")
    
    # Menus
    wb.send("MENU INVOKE WORKBENCH.ABOUT")
    wb.send("MENU WINDOW root INVOKE WINDOW.SELECTCONTENTS")
    
    # Icons
    wb.send("ICON WINDOW root NAMES Workbench SELECT")
    wb.send("ICON WINDOW root NAMES Workbench OPEN")
    
    # GETATTR
    rc, version = wb.send("GETATTR APPLICATION.VERSION", result=True)
    rc, screen = wb.send("GETATTR APPLICATION.SCREEN", result=True)
    print(f"Workbench {version} on {screen}")
    
    # List opened windows
    rc, count = wb.send("GETATTR WINDOWS.COUNT", result=True)
    print(f"{count} windows open")

    # Adding a custom menu
    wb.send("MENU ADD NAME myscript TITLE 'My Arexx script' CMD 'test.rexx'")