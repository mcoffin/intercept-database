define(`concatlist', `ifelse(`$#', `1', ``$1'', ``$1' concatlist(shift($@))')')dnl
