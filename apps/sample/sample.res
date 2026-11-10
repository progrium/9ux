# The sample app's resources: menus, its dialogs, strings and its icon.
# mk compiles this to sample.rsrc with resfs's rescomp, and libux reads
# it through resfs (uxres). Item and control IDs come from the titles:
# "Confirm…" is the action confirm.

icon 128 name=sample large=../../assets/gen/icons/app.32.img large1=../../assets/gen/icons/app.32.1.img small=../../assets/gen/icons/app.16.img small1=../../assets/gen/icons/app.16.1.img

resource MENU 128
menu id=128 title=File
item 'title=About Sample…'
item title=New key=N
item 'title=Confirm…' key=K
item title=-
item title=Close key=W
item title=Quit key=Q
end

resource MENU 129
menu id=129 title=Edit
item title=Undo key=Z enabled=off
end

# the list's colours
resource STR# 128 name=Colors
Red
Green
Blue
Cyan
Magenta
Yellow
end

resource DLOG 128 name=Confirm
dlog rect=200,200,520,330 ditl=128 title=Confirm
end

resource DITL 128
item type=button rect=226,80,296,104 text=OK
item type=button rect=140,80,210,104 text=Cancel
item type=text rect=16,16,300,36 enabled=off 'text=Save the document?'
end

# About: ^0 is the number of saves; the note icon is the system's
resource ALRT 129
alrt rect=160,160,480,280 ditl=129
end

resource DITL 129
item type=button rect=226,80,296,104 text=OK
item type=icon rect=16,16,48,48 enabled=off id=1
item type=text rect=64,16,300,34 enabled=off 'text=Sample, built from resources.'
item type=text rect=64,40,300,58 enabled=off 'text=Saved ^0 times.'
end
