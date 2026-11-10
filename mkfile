# Build every component that has a mkfile: mk, mk test, mk clean, mk nuke.
# Run inside 9front or h9 (h9 --rc 'mk').

DIRS=rio libux uxd uxmeta resfs finder apps/sample clipd

all:V: dirs-all
test:V: dirs-test
clean:V: dirs-clean
nuke:V: dirs-nuke

dirs-%:V:
	for(d in $DIRS) if(test -e $d/mkfile) @{
		echo mk $stem in $d
		cd $d
		mk $MKFLAGS $stem
	}
	status=''
