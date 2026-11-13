	rem {{NAME}} - VCSIDE The Game in batari Basic (CC0: use and change it freely).
{{TITLE}}
	rem The title screen: the cartridge and THE GAME; the button starts a game.
{{/TITLE}}
	rem The joystick moves the hero; the button shoots the way it faces (left or
	rem right). The walls and the blocks stop the hero and the shot; the two
	rem enemies walk diagonally and bounce off them. A shot enemy scores a point;
	rem when both are gone, two more come. Touching one costs a life (the dots
	rem beside the score); after the last one,
{{TITLE}}
	rem back to the title. RESET goes back to it too.
{{/TITLE}}
{{NO_TITLE}}
	rem a new game. RESET starts one too.
{{/NO_TITLE}}
	rem
	rem Graphics: hero.vcsgfx, enemy.vcsgfx and room.vcsgfx{{TITLE_FILE}} (open them
	rem from the project panel). Saving one rewrites the lines between its
	rem `rem vcsgfx` and `rem vcsgfx end` below.
	rem Music and sound: theme.vcsmusic is the song and its three effects (shot,
	rem hit, lose); saving it writes music.asm, the player and the song,
	rem included at the end with inline. MusicInit starts the song, MusicFrame
	rem plays it once per frame, and MusicEffect plays an effect; while an
	rem effect sounds, the song's notes on its channel wait.

	set romsize 4k
	set smartbranching on
	rem the lives: dots in the playfield bytes beside the score (pfscore1)
	const pfscore = 1
	rem player1colors and playercolors: a colour per row for both players (the
	rem hero's and the enemies' from their .vcsgfx). They take the missiles'
	rem time, so the shot is the ball, which has the playfield's colour.
	rem pfheights: batari Basic takes playercolors only with pfcolors or
	rem pfheights; here pfheights, with every row as tall as the .vcsgfx's.
	rem The playfield has one colour, COLUPF, and the lives' dots below the
	rem screen leave theirs there, pfscorecolor: so pfscorecolor is the
	rem playfield's colour too (the title's, then the room's), written when the
	rem project was made; after changing the colour in the .vcsgfx, change it too.
	set kernel_options player1colors playercolors pfheights

	rem RAM. The music player uses h to z; the game a to g and var44 to var47
	rem (the playfield's twelfth row, which is never drawn: only vertical
	rem scrolling uses it).
	dim frame = a
	rem flags: bit 0 the button is held, 1 the hero faces left, 2 and 3 enemy 0
	rem and enemy 1 are alive, 4 the enemy drawn on this frame, 5 the hero
	rem walks, 6 the shot flies, 7 the hero touched a wall on the last frame
	dim flags = b
	rem the enemies, as two-byte arrays: enemyX[0] and enemyX[1], and so on
	dim enemyX = c
	dim enemyY = e
	rem their direction: bit 0 left (or right), bit 1 up (or down)
	dim enemyDirection = var44
	rem where the hero was before this frame's move, for the walls
	dim oldHeroX = g
	dim oldHeroY = var46
	rem the shot's step: 3 to the right, 253 (-3) to the left
	dim shotSpeed = var47

start
	rem the playfield's rows: 8 double lines each (16 lines, as in the .vcsgfx)
	pfheights:
	8
	8
	8
	8
	8
	8
	8
	8
	8
	8
	8
	8
end
	asm
	jsr MusicInit
end
{{TITLE}}

	rem ---- the title screen ----
title
	rem vcsgfx title.vcsgfx title
	rem vcsgfx end
	pfscorecolor = {{TITLE_TOP_COLOUR}} : pfscore1 = 0
	rem no score on the title: black on black
	scorecolor = 0
	rem the cartridge: both players, four times as wide, side by side; no shot
	player0x = 49 : player1x = 81 : player0y = 47 : player1y = 47
	bally = 0
	flags = 1
titleLoop
	frame = frame + 1
	rem vcsgfx title.vcsgfx cart
	rem vcsgfx end
	REFP0 = 0
	asm
	jsr MusicFrame
end
	drawscreen
	if switchreset then goto start
	rem a new press of the button: a game
	if !joy0fire then flags{0} = 0 : goto titleLoop
	if flags{0} then goto titleLoop
{{/TITLE}}

	rem ---- a new game ----
newGame
	rem vcsgfx room.vcsgfx room
	rem vcsgfx end
	score = 0
	scorecolor = $0E
	rem three lives: three dots
	pfscore1 = %00010101
	pfscorecolor = {{ROOM_TOP_COLOUR}}
	rem the ball (the shot): 4 pixels wide (bits 4 and 5) on a mirrored
	rem playfield (bit 0, as batari Basic leaves it)
	CTRLPF = $21
	ballheight = 1
	flags = 1
	gosub placeHero
	gosub newWave

	rem ---- the game, one frame per pass ----
main
	frame = frame + 1

	rem the hero: the joystick moves and turns it
	oldHeroX = player0x : oldHeroY = player0y
	flags{5} = 0
	if joy0left then flags{1} = 1 : flags{5} = 1
	if joy0right then flags{1} = 0 : flags{5} = 1
	if joy0up || joy0down then flags{5} = 1
	rem held diagonally against a wall, it moves on one axis a frame (x on even
	rem frames, y on odd ones): the wall stops one and it slides along it
	temp1 = 0
	if !flags{7} then goto heroMoves
	if !joy0up && !joy0down then goto heroMoves
	if !joy0left && !joy0right then goto heroMoves
	temp1 = frame & 1
	temp1 = temp1 + 1
heroMoves
	if temp1 = 2 then goto heroMovedX
	if joy0left then player0x = player0x - 1
	if joy0right then player0x = player0x + 1
heroMovedX
	if temp1 = 1 then goto heroMovedY
	if joy0up then player0y = player0y - 1
	if joy0down then player0y = player0y + 1
heroMovedY

	rem the shot: a new press of the button, if it is not flying already
	if !joy0fire then flags{0} = 0 : goto noShot
	if flags{0} then goto noShot
	flags{0} = 1
	if flags{6} then goto noShot
	flags{6} = 1
	rem from the hero's middle, on the side it faces
	bally = player0y - 6
	shotSpeed = 3 : ballx = player0x + 8
	if flags{1} then shotSpeed = 253 : ballx = player0x - 2
	asm
	lda #MUSIC_EFFECT_SHOT
	jsr MusicEffect
end
noShot
	if !flags{6} then bally = 0 : goto shotMoved
	ballx = ballx + shotSpeed
	rem off the screen (on the left it wraps above 158)
	if ballx > 158 then flags{6} = 0 : bally = 0
shotMoved

	rem the enemy of this frame: with both alive they take turns on player1
	rem (each is drawn every other frame), one alone is drawn on every frame.
	rem Either way each moves on every other frame: enemy 0 on even frames,
	rem enemy 1 on odd ones.
	temp1 = frame & 1
	if !flags{2} then temp1 = 1
	if !flags{3} then temp1 = 0
	flags{4} = 0
	if temp1 then flags{4} = 1
	temp2 = frame & 1
	if temp1 = temp2 then gosub moveEnemy
	player1x = enemyX[temp1] : player1y = enemyY[temp1]
	rem its two frames, 16 frames each
	temp2 = frame & 16
	if temp2 then gosub enemyCrawl2 else gosub enemyCrawl1
	if !temp1 then goto enemyDrawn
	rem the second enemy in another colour: the first one's plus $40 (another
	rem hue), as in the other versions of the game. Change it here: it is not
	rem in enemy.vcsgfx.
	player1color:
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
end
enemyDrawn

	gosub heroLook
	REFP0 = 0
	if flags{1} then REFP0 = 8
	rem the score's digits change NUSIZ0 and NUSIZ1: one copy of each player
	NUSIZ0 = 0 : NUSIZ1 = 0
	asm
	jsr MusicFrame
end
	drawscreen

	rem what touched what in the frame just drawn
	temp1 = 0
	if flags{4} then temp1 = 1

	rem a wall or a block: the hero goes back where it was
	flags{7} = 0
	if collision(player0, playfield) then player0x = oldHeroX : player0y = oldHeroY : flags{7} = 1

	rem the enemy: back where it was, and the other way on one axis. The TIA
	rem does not say which side it touched, so the axis is chosen at random; a
	rem wrong guess touches again and turns the other one (a fixed rule traps
	rem it in a loop).
	if !collision(player1, playfield) then goto enemyFree
	temp3 = enemyDirection[temp1] ^ 3
	enemyDirection[temp1] = temp3
	rem it moved on this frame if it was its turn: the other way undoes that
	temp2 = frame & 1
	if temp1 = temp2 then gosub moveEnemy
	temp3 = enemyDirection[temp1] ^ 3
	temp4 = rand & 1
	if temp4 then temp3 = temp3 ^ 1 else temp3 = temp3 ^ 2
	enemyDirection[temp1] = temp3
enemyFree

	rem the shot: a wall stops it, an enemy is shot
	if !flags{6} then goto shotDone
	if collision(ball, playfield) then flags{6} = 0 : bally = 0
	if !collision(ball, player1) then goto shotDone
	flags{6} = 0 : bally = 0
	if temp1 then flags{3} = 0 else flags{2} = 0
	score = score + 1
	asm
	lda #MUSIC_EFFECT_HIT
	jsr MusicEffect
end
	rem both gone: two more
	if flags{2} then goto shotDone
	if flags{3} then goto shotDone
	gosub newWave
shotDone

	rem an enemy touches the hero: a life less, and everyone back to the start
	if !collision(player0, player1) then goto heroSafe
	asm
	lda #MUSIC_EFFECT_LOSE
	jsr MusicEffect
end
	gosub placeHero
	gosub newWave
	pfscore1 = pfscore1 / 4
	if pfscore1 then goto heroSafe
{{TITLE}}
	rem no lives left: the title
	goto title
{{/TITLE}}
{{NO_TITLE}}
	rem no lives left: a new game
	goto newGame
{{/NO_TITLE}}
heroSafe

{{TITLE}}
	if switchreset then goto start
{{/TITLE}}
{{NO_TITLE}}
	if switchreset then goto newGame
{{/NO_TITLE}}
	goto main

	rem ---- subroutines ----

	rem the hero at the bottom of the room, facing right, with no shot
placeHero
	player0x = 76 : player0y = 80
	oldHeroX = 76 : oldHeroY = 80
	rem flag bits 1 and 6 off
	flags = flags & %10111101 : bally = 0
	return

	rem two enemies near the top of the room, walking down and inwards
newWave
	enemyX[0] = 40 : enemyY[0] = 18 : enemyDirection[0] = 0
	enemyX[1] = 112 : enemyY[1] = 18 : enemyDirection[1] = 1
	rem flag bits 2 and 3 on
	flags = flags | %00001100
	return

	rem enemy temp1 takes one diagonal step, the way enemyDirection says
moveEnemy
	temp3 = enemyDirection[temp1]
	temp5 = enemyX[temp1]
	if temp3{0} then temp5 = temp5 - 1 else temp5 = temp5 + 1
	enemyX[temp1] = temp5
	temp5 = enemyY[temp1]
	if temp3{1} then temp5 = temp5 - 1 else temp5 = temp5 + 1
	enemyY[temp1] = temp5
	return

	rem the hero's frame: the walk while it moves; standing still, a blink now
	rem and then
heroLook
	if flags{5} then goto heroWalks
	temp6 = frame & 127
	if temp6 < 6 then goto heroBlink
	goto heroStand
heroWalks
	temp6 = frame / 8
	temp6 = temp6 & 3
	on temp6 goto heroStep1 heroStand heroStep2 heroStand

heroStand
	rem vcsgfx hero.vcsgfx hero frame 1
	rem vcsgfx end
	return
heroStep1
	rem vcsgfx hero.vcsgfx hero frame 2
	rem vcsgfx end
	return
heroStep2
	rem vcsgfx hero.vcsgfx hero frame 3
	rem vcsgfx end
	return
heroBlink
	rem vcsgfx hero.vcsgfx hero frame 4
	rem vcsgfx end
	return

enemyCrawl1
	rem vcsgfx enemy.vcsgfx enemy frame 1
	rem vcsgfx end
	return
enemyCrawl2
	rem vcsgfx enemy.vcsgfx enemy frame 2
	rem vcsgfx end
	return

	inline music.asm
