	rem {{NAME}} - VCSIDE The Game in batari Basic for the PXE kernel (CC0: use and change it freely).
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
	rem hit, lose); saving it writes music.h, a player in C. pxe.c plays it:
	rem VCSIDE builds this program with batari Basic, then links pxe.c into the
	rem kernel's ELF, where it runs once per frame. The game asks for music with
	rem musicCommand (see below); pxe.c reads it and sets it back to 0.

	set kernel PXE
	set smartbranching on
	rem the lives: dots in the playfield bytes beside the score (pfscore1)
	const pfscore = 1

	rem RAM: the game uses a to k; z is for the music.
	dim frame = a
	rem flags: bit 0 the button is held, 1 the hero faces left, 2 and 3 enemy 0
	rem and enemy 1 are alive, 5 the hero walks, 6 the shot flies, 7 the hero
	rem touched a wall on the last frame
	dim flags = b
	rem the enemies: enemy 0 and enemy 1 (this kernel's batari Basic does not
	rem build arrays, so each has its own variables)
	dim enemy0X = c
	dim enemy1X = d
	dim enemy0Y = e
	dim enemy1Y = f
	rem their direction: bit 0 left (or right), bit 1 up (or down)
	dim enemy0Direction = g
	dim enemy1Direction = h
	rem where the hero was before this frame's move, for the walls
	dim oldHeroX = i
	dim oldHeroY = j
	rem the shot's step: 3 to the right, 253 (-3) to the left
	dim shotSpeed = k
	rem what the music does on the next frame, read by pxe.c: 0 nothing,
	rem 1 + an effect (the order of theme.vcsmusic's effects), 128 + a song
	rem (MUSIC_SONG_x in music.h), 255 stop
	dim musicCommand = z
	const musicShot = 1
	const musicHit = 2
	const musicLose = 3
	const musicTheme = 128

	rem The PXE kernel: 176 lines, y counts lines from the top (playerNy is the
	rem sprite's top row), and players 1 to 16 share the TIA's player 1, so the
	rem two enemies are player1 and player2, both drawn on every frame.

start
	musicCommand = musicTheme
{{TITLE}}

	rem ---- the title screen ----
title
	rem vcsgfx title.vcsgfx title
	rem vcsgfx end
	pfscore1 = 0
	rem no score on the title: black on black
	gosub scoreBlack
	rem the cartridge: both players, four times as wide, side by side; no shot
	player0x = 49 : player1x = 81 : player0y = 8 : player1y = 8
	player2y = 200 : bally = 200
	flags = 1
titleLoop
	frame = frame + 1
	rem vcsgfx title.vcsgfx cart
	rem vcsgfx end
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
	gosub scoreWhite
	rem three lives: three dots, in the room's colour
	pfscore1 = %00010101
	pfscorecolor = {{ROOM_TOP_COLOUR}}
	rem the ball (the shot): 4 pixels wide (bits 4 and 5), in the room's colour
	CTRLPF = $21
	COLUBL = {{ROOM_TOP_COLOUR}}
	ballheight = 2
	_NUSIZ1 = 0 : NUSIZ2 = 0
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
	rem two lines a frame: the same pace as in the kernels of two-line rows
	if joy0up then player0y = player0y - 2
	if joy0down then player0y = player0y + 2
heroMovedY

	rem the shot: a new press of the button, if it is not flying already
	if !joy0fire then flags{0} = 0 : goto noShot
	if flags{0} then goto noShot
	flags{0} = 1
	if flags{6} then goto noShot
	flags{6} = 1
	rem from the hero's middle, on the side it faces
	bally = player0y + 12
	shotSpeed = 3 : ballx = player0x + 8
	if flags{1} then shotSpeed = 253 : ballx = player0x - 2
	musicCommand = musicShot
noShot
	if !flags{6} then bally = 200 : goto shotMoved
	ballx = ballx + shotSpeed
	rem off the screen (on the left it wraps above 158)
	if ballx > 158 then flags{6} = 0 : bally = 200
shotMoved

	rem the enemies: each takes a step on every other frame, enemy 0 on even
	rem frames and enemy 1 on odd ones, so a wall touched after the frame is
	rem the one that moved (both are drawn by the TIA's player 1)
	temp1 = frame & 1
	gosub moveEnemy
	player1x = enemy0X : player1y = enemy0Y
	player2x = enemy1X : player2y = enemy1Y
	if !flags{2} then player1y = 200
	if !flags{3} then player2y = 200
	rem their two frames, 16 frames each
	temp2 = frame & 16
	if temp2 then gosub enemyCrawl2 else gosub enemyCrawl1
	rem the second enemy in another colour: the first one's plus $40 (another
	rem hue), as in the other versions of the game. Change it here: it is not
	rem in enemy.vcsgfx.
	player2color:
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
	{{SECOND_ENEMY_COLOUR}}
end

	gosub heroLook
	rem facing left: NUSIZ0 bit 3 reflects it in this kernel
	NUSIZ0 = 0
	if flags{1} then NUSIZ0 = 8
	drawscreen

	rem what touched what in the frame just drawn
	rem a wall or a block: the hero goes back where it was
	flags{7} = 0
	if collision(player0, playfield) then player0x = oldHeroX : player0y = oldHeroY : flags{7} = 1

	rem the enemy that moved: back where it was, and the other way on one axis.
	rem The TIA does not say which side it touched, so the axis is chosen at
	rem random; a wrong guess touches again and turns the other one (a fixed
	rem rule traps it in a loop).
	temp1 = frame & 1
	if !collision(player1, playfield) then goto enemyFree
	rem a step the other way takes it back
	temp3 = 3
	gosub turnEnemy
	gosub moveEnemy
	gosub turnEnemy
	rem then it turns on one axis
	temp4 = rand & 1
	temp3 = 1
	if temp4 then temp3 = 2
	gosub turnEnemy
enemyFree

	rem the shot: a wall stops it, an enemy is shot (the one it is closer to)
	if !flags{6} then goto shotDone
	if collision(ball, playfield) then flags{6} = 0 : bally = 200
	if !collision(ball, player1) then goto shotDone
	flags{6} = 0 : bally = 200
	gosub nearestEnemy
	if temp1 then flags{3} = 0 else flags{2} = 0
	score = score + 1
	musicCommand = musicHit
	rem both gone: two more
	if flags{2} then goto shotDone
	if flags{3} then goto shotDone
	gosub newWave
shotDone

	rem an enemy touches the hero: a life less, and everyone back to the start
	if !collision(player0, player1) then goto heroSafe
	musicCommand = musicLose
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
	player0x = 76 : player0y = 136
	oldHeroX = 76 : oldHeroY = 136
	rem flag bits 1 and 6 off
	flags = flags & %10111101 : bally = 200
	return

	rem two enemies near the top of the room, walking down and inwards
newWave
	enemy0X = 40 : enemy0Y = 20 : enemy0Direction = 0
	enemy1X = 112 : enemy1Y = 20 : enemy1Direction = 1
	rem flag bits 2 and 3 on
	flags = flags | %00001100
	return

	rem enemy temp1 takes one diagonal step, the way its direction says: a pixel
	rem across and two lines down or up
moveEnemy
	if temp1 then goto moveEnemy1
	if enemy0Direction{0} then enemy0X = enemy0X - 1 else enemy0X = enemy0X + 1
	if enemy0Direction{1} then enemy0Y = enemy0Y - 2 else enemy0Y = enemy0Y + 2
	return
moveEnemy1
	if enemy1Direction{0} then enemy1X = enemy1X - 1 else enemy1X = enemy1X + 1
	if enemy1Direction{1} then enemy1Y = enemy1Y - 2 else enemy1Y = enemy1Y + 2
	return

	rem enemy temp1 turns: its direction bits in temp3 change (3: it goes back)
turnEnemy
	if temp1 then enemy1Direction = enemy1Direction ^ temp3 else enemy0Direction = enemy0Direction ^ temp3
	return

	rem the living enemy closer to the shot, in temp1 (0 or 1)
nearestEnemy
	if !flags{3} then temp1 = 0 : return
	if !flags{2} then temp1 = 1 : return
	temp2 = ballx - enemy0X
	if temp2 > 127 then temp2 = 0 - temp2
	temp3 = ballx - enemy1X
	if temp3 > 127 then temp3 = 0 - temp3
	temp1 = 0
	if temp3 < temp2 then temp1 = 1
	return

	rem the score's colour: one per row of its digits
scoreWhite
	scorecolors:
	$0E
	$0E
	$0E
	$0E
	$0E
	$0E
	$0E
	$0E
end
	return
{{TITLE}}
scoreBlack
	scorecolors:
	$00
	$00
	$00
	$00
	$00
	$00
	$00
	$00
end
	return
{{/TITLE}}

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

	rem the enemies' frames: the same art for both (player2's colour is in main)
enemyCrawl1
	rem vcsgfx enemy.vcsgfx enemy frame 1
	rem vcsgfx end
	rem vcsgfx enemy.vcsgfx enemy frame 1 as player2
	rem vcsgfx end
	return
enemyCrawl2
	rem vcsgfx enemy.vcsgfx enemy frame 2
	rem vcsgfx end
	rem vcsgfx enemy.vcsgfx enemy frame 2 as player2
	rem vcsgfx end
	return
