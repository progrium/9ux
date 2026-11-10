# Build every component that has a mkfile: mk, mk clean, mk nuke.
# Run inside 9front or h9 (h9 --rc 'mk').

DIRS=rio libux uxd uxmeta resfs finder

all clean nuke:V:
	for(d in $DIRS) if(test -e $d/mkfile) @{
		echo mk $target in $d
		cd $d
		mk $MKFLAGS $target
	}
