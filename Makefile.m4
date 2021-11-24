include(`pbo.m4')dnl
include(`utils.m4')dnl
define(`faketarget', `dnl
`fake_targets += $1
$1'')dnl
define(`xargs_rm', ``xargs --no-run-if-empty $@ rm'')dnl
define(`findn', ``find "$1" -mindepth $2 -maxdepth $3'')dnl
define(`find1', `findn(`$1', 1, 1)')dnl
define(`sbasename', `$(shell basename "`$1'")')dnl
.DEFAULT_GOAL := all
PRIVATE_KEY ?= $(HOME)/.secrets/codeYeTi.biprivatekey
private_key_filename := sbasename(`$(PRIVATE_KEY)')
private_key_name := $(private_key_filename:.biprivatekey=)

start_addons()
add_addon(all_addons, a3sanitize)
finish_addons(all_addons, all_pbos, all_signatures, `$(private_key_name)')

sign_target(`$(private_key_name)', `$(PRIVATE_KEY)')

faketarget(all): $(all_pbos)

faketarget(sign): $(all_signatures)

faketarget(clean):
	$(foreach f,$(all_pbos),[ ! -e "$(f)" ] || rm "$(f)";)
	$(foreach f,$(all_signatures),[ ! -e "$(f)" ] || rm "$(f)";)

faketarget(clean-all):
	find1(addons) -name '*.pbo' | xargs_rm
	find1(addons) -type f -name "*.pbo.$(private_key_name).bisign" | xargs_rm

.PHONY: $(fake_targets)
