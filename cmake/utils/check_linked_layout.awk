# Checks a GNU ld link map (-Map) for the layout VM_ResetLinked relies on:
# everything the linker placed between a linked module's data markers, and
# between its bss markers, comes from the module's own objects. Anything
# else there would be zeroed or overwritten each time the module loads.
#
#   awk -v modules="cgame qagame ui" -f check_linked_layout.awk <map>
#
# Prints each stray input section and exits 1, or exits 0.

# mawk has no strtonum
function hexvalue(s,    i, c, v) {
	sub(/^0x/, "", s)
	v = 0
	for (i = 1; i <= length(s); i++) {
		c = index("0123456789abcdef", tolower(substr(s, i, 1))) - 1
		v = v * 16 + c
	}
	return v
}

function record(name, addr, size, file) {
	n++
	secname[n] = name
	secaddr[n] = hexvalue(addr)
	secsize[n] = hexvalue(size)
	secfile[n] = file
	secout[n] = outsec
}

# an output section, or another line the map starts at the margin; the
# debug sections, and every other one the program doesn't load, number
# their input sections from 0, so only the markers' own output section
# is compared with them
/^[^ ]/ {
	outsec = $1
	pending = ""
	next
}

# an input section on one line: " .data  0x... 0x... file.o"
/^ [^ *]/ && NF == 4 && $2 ~ /^0x/ && $3 ~ /^0x/ {
	record($1, $2, $3, $4)
	pending = ""
	next
}
# a long section name alone, its address, size and file on the next line
/^ [^ *]/ && NF == 1 {
	pending = $1
	next
}
pending != "" && /^  +0x/ && NF == 3 && $2 ~ /^0x/ {
	record(pending, $1, $2, $3)
	pending = ""
	next
}
{
	pending = ""
}

END {
	count = split(modules, mods, " ")
	failed = 0
	for (m = 1; m <= count; m++) {
		mod = mods[m]
		for (k = 1; k <= 2; k++) {
			kind = k == 1 ? "data" : "bss"
			begin = end = -1
			for (i = 1; i <= n; i++) {
				if (secsize[i] == 0 || secname[i] !~ ("^\\." kind)) {
					continue
				}
				if (index(secfile[i], mod "_linkedBegin.c")) {
					begin = secaddr[i]
					out = secout[i]
				} else if (index(secfile[i], mod "_linkedEnd.c")) {
					end = secaddr[i]
					endout = secout[i]
				}
			}
			if (begin < 0 || end < 0 || end <= begin) {
				printf "%s: no %s markers in the link map\n", mod, kind
				failed = 1
				continue
			}
			if (endout != out) {
				printf "%s: the %s markers are in %s and %s\n", mod, kind, out, endout
				failed = 1
				continue
			}
			for (i = 1; i <= n; i++) {
				if (secsize[i] > 0 && secout[i] == out && secaddr[i] >= begin && secaddr[i] < end &&
					!index(secfile[i], mod "_linked.dir/")) {
					printf "%s: %s at %.0f from %s lies between the module's %s markers\n", mod, secname[i], secaddr[i], secfile[i], kind
					failed = 1
				}
			}
		}
	}
	exit failed
}
