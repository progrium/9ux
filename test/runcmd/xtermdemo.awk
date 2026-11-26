# A full-screen program's screen, for Run Command's xterm test and its
# screenshot: the alternate screen, DEC line drawing, the 16 and 256
# colours, text styles, and a meter and status line as top(1) draws them.
function at(r, c) { printf "\033[%d;%dH", r, c }
function hline(n,   s) { s = ""; while(n-- > 0) s = s "q"; return s }
function box(r, c, h, w, title,   i) {
	at(r, c); printf "\033(0l%sk\033(B", hline(w-2)
	for(i = 1; i < h-1; i++){
		at(r+i, c); printf "\033(0x\033(B"
		at(r+i, c+w-1); printf "\033(0x\033(B"
	}
	at(r+h-1, c); printf "\033(0m%sj\033(B", hline(w-2)
	at(r, c+2); printf " \033[1m%s\033[22m ", title
}
BEGIN {
	cols = ENVIRON["COLS"] + 0; if(cols < 60) cols = 60
	lines = ENVIRON["LINES"] + 0; if(lines < 20) lines = 20
	printf "\033[?1049h\033[?25l\033[H\033[2J"
	box(1, 1, 8, cols, "9/UX Run Command: xterm-256color")
	at(2, 3); printf "16 colours  "
	for(i = 0; i < 8; i++) printf "\033[4%dm  ", i
	printf "\033[0m "
	for(i = 0; i < 8; i++) printf "\033[10%dm  ", i
	printf "\033[0m"
	at(3, 3); printf "256 colours "
	for(i = 16; i < 232; i += 4) printf "\033[48;5;%dm ", i
	printf "\033[0m"
	at(4, 3); printf "grey ramp   "
	for(i = 232; i < 256; i++) printf "\033[48;5;%dm  ", i
	printf "\033[0m"
	at(5, 3); printf "true colour "
	for(i = 0; i < 48; i++) printf "\033[48;2;%d;%d;%dm ", 255-i*5, 60+i*3, i*5
	printf "\033[0m"
	at(6, 3); printf "styles      \033[1mbold\033[0m \033[4munderline\033[0m \033[7minverse\033[0m \033[2mfaint\033[0m \033[9mstruck\033[0m \033[1;34mbold blue\033[0m \033[38;5;208morange\033[0m"
	at(7, 3); printf "line drawing \033(0lqwqk tqnqu mqvqj ` a\033(B   wide: \344\270\255\346\226\207"

	box(10, 1, 7, cols, "top")
	split("41 73 18 92", load, " ")
	for(i = 1; i <= 4; i++){
		at(10+i, 3)
		w = cols - 22
		n = int(load[i] * w / 100)
		bar = ""; for(j = 0; j < n; j++) bar = bar "|"
		color = load[i] > 80 ? 31 : load[i] > 50 ? 33 : 32
		printf "\033[1mcpu%d\033[0m [\033[%dm" sprintf("%-" w "s", bar) "\033[0m%3d%%]", i-1, color, load[i]
	}
	at(15, 3); printf "\033[30;46m%s\033[0m", sprintf("%-" (cols-4) "s", "  PID USER     %CPU COMMAND")

	at(lines, 1); printf "\033[7m q \033[0m Quit  \033[7m arrows \033[0m Move   \033[2m(a 9front awk script, drawn by libux's vt)\033[0m"
	at(16, 3); printf "\033[?25h"
}
