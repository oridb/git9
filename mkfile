</$objtype/mkfile

BIN=/$objtype/bin/git
TARG=\
	conf\
	fetch\
	fs\
	query\
	repack\
	save\
	send\
	serve\
	walk

RC=\
	add\
	branch\
	clone\
	commit\
	diff\
	export\
	import\
	init\
	log\
	merge\
	pull\
	push\
	revert\
	rm

OFILES=\
	delta.$O\
	objset.$O\
	ols.$O\
	pack.$O\
	proto.$O\
	util.$O\
	ref.$O

HFILES=git.h

</sys/src/cmd/mkmany

# Override install target to install rc.
install:V:
	mkdir -p $BIN
	mkdir -p /sys/lib/git
	for (i in $TARG)
		mk $MKFLAGS $i.install
	for (i in $RC)
		mk $MKFLAGS $i.rcinstall
	cp git.1.man /sys/man/1/git
	cp gitfs.4.man /sys/man/4/gitfs
	cp common.rc /sys/lib/git/common.rc
	mk $MKFLAGS /sys/lib/git/template

uninstall:V:
	rm -rf $BIN /sys/lib/git

%.rcinstall:V:
	cp $stem $BIN/$stem
	chmod +x $BIN/$stem

/sys/lib/git/template: template
	mkdir -p /sys/lib/git/template
	dircp template /sys/lib/git/template
