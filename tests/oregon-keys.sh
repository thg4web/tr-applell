# oregon-keys.sh: the Oregon Trail play-through as a player types it (sourced by test-oregon.sh and
# test-sound.sh). Defines w N (wait N Apple seconds) and KEYS. See test-oregon.sh for what the keys do.
w() { printf '\\w%.0s' $(seq 1 "$1"); }    # wait N Apple seconds
KEYS="$(w 28)1\r$(w 8)1\r$(w 6)OSCAR\r$(w 3)ANN\r$(w 3)RUTH\r$(w 3)BOB\r$(w 3)JANE\r$(w 4)Y\r$(w 8)3\r"
KEYS+="$(w 6) $(w 6) $(w 6) $(w 6) $(w 6)1\r$(w 4)3\r$(w 5)2\r$(w 4)800\r$(w 5)3\r$(w 4)10\r$(w 5)4\r$(w 4)10\r"
KEYS+="$(w 5)5\r$(w 4)2\r$(w 3)2\r$(w 3)2\r$(w 5) $(w 8) $(w 10) $(w 10)1\r$(w 20) $(w 60) $(w 60)"
KEYS+="N\r$(w 10) $(w 6)3\r$(w 6)Y\r$(w 30) $(w 15)\r$(w 14)Y\r$(w 8) $(w 6)1\r$(w 8) $(w 6)"
KEYS+="1\r$(w 25) $(w 10)\r$(w 8)$(w 5)Y\r$(w 8)\r$(w 8)8\r$(w 10) $(w 6)\r$(w 3)\r$(w 1)P$(w 1) $(w 2)O$(w 1) $(w 2)K$(w 1) $(w 3)"

