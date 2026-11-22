# Run Command's resources: its menus, the About alert and its icon.
# mk compiles this to runcmd.rsrc with resfs's rescomp; libux reads it
# through resfs (uxres). Item IDs come from the titles: "New Window" is
# the action new-window.

icon 128 name=runcmd large=../../assets/gen/icons/app-rc.32.img large1=../../assets/gen/icons/app-rc.32.1.img small=../../assets/gen/icons/app-rc.16.img small1=../../assets/gen/icons/app-rc.16.1.img

resource MENU 128
menu id=128 title=File
item 'title=About Run Command…'
item title=-
item 'title=New Window' key=N
item title=Close key=W
item title=-
item title=Quit key=Q
end

resource MENU 129
menu id=129 title=Edit
item title=Undo key=Z enabled=off
item title=-
item title=Cut key=X
item title=Copy key=C
item title=Paste key=V
item 'title=Select All' key=A
end

resource MENU 130
menu id=130 title=Shell
item title=Interrupt key=.
item 'title=Send End of File' key=D
item title=-
item 'title=Clear Window' key=K
end

# About: the rc application icon from the system's resources (ICN# 140)
resource ALRT 128
alrt rect=160,160,500,280 ditl=128
end

resource DITL 128
item type=button rect=246,80,316,104 text=OK
item type=icon rect=16,16,48,48 enabled=off id=140
item type=text rect=64,16,320,34 enabled=off 'text=Run Command'
item type=text rect=64,40,320,58 enabled=off 'text=An rc shell in a 9/UX window.'
end
