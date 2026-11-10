# A hand-written resource source exercising every form rescomp knows.
# Paths are relative to this file.

icon 128 name=Sample large=../golden/icon128.image large1=../golden/ICN-128.image small=../golden/icon128.small

resource CURS 128 name=Cross attr=purgeable image=../golden/CURS128.image hotspot=8,7

resource STR%20 128
Hello, 9/UX café — résumé
end

resource STR# 128 name=Messages
First
Two\nlines

tab\there \\ back
en\x64
end

resource MENU 128
menu id=128 title=File enabled=on
item title=New key=N
item 'title=Open…' key=O
item title=- enabled=off
item title=Recent submenu=130
item 'title=Bold item' mark=✓ icon=3 style=bold,underline
item title=Quit key=Q
end

resource DLOG 128 name=Find
dlog rect=30,40,330,140 proc=1 visible=on goaway=off refcon=305419896 ditl=128 'title=Find File' position=12298
end

resource DITL 128
item type=button rect=220,70,290,90 text=OK
item type=button rect=130,70,200,90 text=Cancel
item type=text rect=10,10,100,26 enabled=off 'text=Find what:'
item type=edit rect=110,10,290,26
item type=icon rect=10,70,42,102 enabled=off id=128
end

resource ALRT 129
alrt rect=80,60,400,160 ditl=129 stages=21845
end

resource zzzz -16455 'name=Opaque é' hex=000102ff

# opaque, so it compiles to exactly the bytes of fixtures/sample.rsrc
resource icl8 130 image=../golden/icl8-130.image
