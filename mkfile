</$objtype/mkfile

BIN=/$objtype/bin/git
TARG=\
	conf\
	get\
	fs\
	log\
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
	compat\
	diff\
	export\
	import\
	init\
	merge\
	pull\
	push\
	rebase\
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

%.rcinstall:V:
	cp $stem $BIN/$stem
	chmod +x $BIN/$stem
