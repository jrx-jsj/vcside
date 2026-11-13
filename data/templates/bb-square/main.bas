	rem {{NAME}} - batari Basic game made with VCSIDE (CC0: use and change it freely).
	rem Move the square with the joystick; the button changes the colour.
	rem The playfield wall stops it. RESET starts again.
	rem
	rem Music and sound: resources/music/theme.vcsmusic is the song and its two
	rem sound effects; saving it writes generated/music/music.asm, the player
	rem and the song, included at the end. The player uses
	rem the variables h to z. MusicInit starts the song, MusicFrame plays it once
	rem per frame, and MusicEffect plays an effect: blip on the button, bump on
	rem the wall. While an effect sounds, the song's notes on its channel wait.

	set romsize 4k
	set smartbranching on

	dim colour = a
	dim button = b
	dim touching = c

start
	playfield:
	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
	X..............................X
	X..............................X
	X......XXXX..........XXXX......X
	X..............................X
	X..............................X
	X..............................X
	X......XXXX..........XXXX......X
	X..............................X
	X..............................X
	XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
end

	player0:
	%11111111
	%10000001
	%10111101
	%10100101
	%10100101
	%10111101
	%10000001
	%11111111
end

	player0x = 77 : player0y = 50
	colour = $1E
	score = 0
	button = 1
	asm
	jsr MusicInit
end

main
	COLUBK = $00
	COLUPF = $84
	COLUP0 = colour
	scorecolor = $0E

	rem the button: a new press changes the colour and blips
	if !joy0fire then button = 0 : goto noPress
	if button then goto noPress
	button = 1 : colour = colour + 2 : score = score + 1
	asm
	lda #MUSIC_EFFECT_BLIP
	jsr MusicEffect
end
noPress
	if joy0left then player0x = player0x - 1
	if joy0right then player0x = player0x + 1
	if joy0up then player0y = player0y - 1
	if joy0down then player0y = player0y + 1

	asm
	jsr MusicFrame
end
	drawscreen

	rem the playfield was hit in the frame just drawn: step back, and bump
	rem when it has just been reached
	if !collision(player0, playfield) then touching = 0 : goto moved
	if joy0left then player0x = player0x + 1
	if joy0right then player0x = player0x - 1
	if joy0up then player0y = player0y + 1
	if joy0down then player0y = player0y - 1
	if touching then goto moved
	touching = 1
	asm
	lda #MUSIC_EFFECT_BUMP
	jsr MusicEffect
end
moved

	if switchreset then goto start
	goto main

	rem generated/music/music.asm: asm, since inline takes no folders
	asm
	include "generated/music/music.asm"
end
