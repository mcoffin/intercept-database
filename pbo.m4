define(`directory_target', `dnl
$1:
	mkdir -p $1
')dnl
define(`directory_deps', `ifelse(`$#', `1', ``$(shell find $1 -type f)'', ``directory_deps($1) directory_deps(shift($@))'')')dnl
define(`and_suffix_helper', `ifelse(`$#', `1', , `$#', `2', ``$1$2'', `and_suffix_helper(`$1', `$2') and_suffix_helper(`$1', shift(shift($@)))')')dnl
define(`and_suffix', `$1 and_suffix_helper($@)')dnl
define(`start_addons', `')dnl
define(`add_addon', `dnl
ifelse(`$#', `1', , `$#', `2', `dnl
`$1 += ./addons/$2
./addons/$2.pbo: 'directory_deps(./addons/$2)`
	armake2 build 'and_suffix(./addons/$2, .pbo)`
'', `dnl
`add_addon($1, $2)
add_addon($1, shift(shift($@)))
'')')dnl
define(`sign_target', `dnl
%.pbo.$1.bisign: %.pbo $2
	armake2 sign $2 $<
')dnl
define(`finish_addons', `dnl
$2 := $(addsuffix .pbo,$($1))
ifelse(`$#', `4', ``$3 := $(addsuffix .$4.bisign,$($2))'', ``$3 := '')dnl
')dnl
