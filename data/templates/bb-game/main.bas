	rem {{NAME}} - VCSIDE The Game in batari Basic (CC0: use and change it freely).
{{TITLE}}
	rem The title screen: the button on PLAY starts a game.
{{/TITLE}}
	rem The joystick moves the hero; the button shoots the way it faces. The walls
	rem and the blocks stop the hero, the shot and the two enemies, which bounce
	rem off them. A shot enemy scores a point; when both are gone, two more come.
	rem Touching one costs a life (the dots beside the score); after the last one,
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
	rem hit, lose); saving it writes theme.music.asm, the player and the song,
	rem included at the end with inline. MusicInit starts the song, MusicFrame
	rem plays it once per frame, and MusicEffect plays an effect; while an
	rem effect sounds, the song's notes on its channel wait.

	set romsize 4k
	set smartbranching on
	rem the lives: dots in the playfield bytes beside the score (pfscore1)
	const pfscore = 1
	rem a colour per playfield row (pfcolors:), set by the kernel on every
	rem frame, except the top row's: that one keeps the colour of the lives'
	rem dots below the screen, pfscorecolor. So pfscorecolor is the top row's
	rem colour (the title's, then the room's); change it with the .vcsgfx.
	set kernel_options pfcolors

	rem RAM. The music player uses h to z; the game a to g and var44 to var47
	rem (the playfield's twelfth row, which is never drawn: only vertical
	rem scrolling uses it).
	dim frame = a
	rem flags: bit 0 the button is held, 1 the hero faces left, 2 and 3 enemy 0
	rem and enemy 1 are alive, 4 the enemy drawn on this frame, 5 the hero
	rem walks, 6 the shot flies
	dim flags = b
	rem the enemies, as two-byte arrays: enemyX[0] and enemyX[1], and so on
	dim enemyX = c
	dim enemyY = e
	rem their direction: bit 0 left (or right), bit 1 up (or down)
	dim enemyDirection = var44
	dim shotDx = var46
	dim shotDy = var47
	rem where the hero faces, for the shot: 0 right, 1 left, 2 up, 3 down
	dim facing = g

start
	scorecolor = $0E
	asm
	jsr MusicInit
end
{{TITLE}}

	rem ---- the title screen ----
title
	rem vcsgfx title.vcsgfx title
	rem vcsgfx end
	pfscorecolor = {{TITLE_TOP_COLOUR}} : pfscore1 = 0
	rem the hero beside PLAY; no enemy, no shot
	player0x = 44 : player0y = 80
	player1y = 0 : missile0y = 0
	flags = 1
titleLoop
	frame = frame + 1
	gosub heroLook
	REFP0 = 0
	asm
	jsr MusicFrame
end
	drawscreen
	if switchreset then goto start
	rem a new press of the button: PLAY
	if !joy0fire then flags{0} = 0 : goto titleLoop
	if flags{0} then goto titleLoop
{{/TITLE}}

	rem ---- a new game ----
newGame
	rem vcsgfx room.vcsgfx room
	rem vcsgfx end
	score = 0
	rem three lives: three dots
	pfscore1 = %00010101
	pfscorecolor = {{ROOM_TOP_COLOUR}}
	missile0height = 2
	flags = 1
	gosub placeHero
	gosub newWave

	rem ---- the game, one frame per pass ----
main
	frame = frame + 1

	rem the hero: the joystick moves and turns it
	flags{5} = 0
	if joy0left then player0x = player0x - 1 : facing = 1 : flags{1} = 1 : flags{5} = 1
	if joy0right then player0x = player0x + 1 : facing = 0 : flags{1} = 0 : flags{5} = 1
	if joy0up then player0y = player0y - 1 : facing = 2 : flags{5} = 1
	if joy0down then player0y = player0y + 1 : facing = 3 : flags{5} = 1

	rem the shot: a new press of the button, if it is not flying already
	if !joy0fire then flags{0} = 0 : goto noShot
	if flags{0} then goto noShot
	flags{0} = 1
	if flags{6} then goto noShot
	flags{6} = 1
	missile0x = player0x + 4 : missile0y = player0y - 6
	shotDx = 0 : shotDy = 0
	on facing goto shootRight shootLeft shootUp shootDown
shootRight
	shotDx = 3 : goto shoot
shootLeft
	shotDx = 253 : goto shoot
shootUp
	shotDy = 254 : goto shoot
shootDown
	shotDy = 2
shoot
	asm
	lda #MUSIC_EFFECT_SHOT
	jsr MusicEffect
end
noShot
	if flags{6} then missile0x = missile0x + shotDx : missile0y = missile0y + shotDy else missile0y = 0

	rem the enemy of this frame: with both alive they take turns on player1
	rem (each is drawn every other frame), one alone is drawn on every frame
	temp1 = frame & 1
	if !flags{2} then temp1 = 1
	if !flags{3} then temp1 = 0
	flags{4} = 0
	if temp1 then flags{4} = 1
	gosub moveEnemy
	player1x = enemyX[temp1] : player1y = enemyY[temp1]
	temp2 = frame & 8
	if temp2 then gosub enemyCrawl2 else gosub enemyCrawl1

	gosub heroLook
	REFP0 = 0
	if flags{1} then REFP0 = 8
	rem the score's digits change NUSIZ0: the shot's width (4 pixels) on every frame
	NUSIZ0 = $20
	asm
	jsr MusicFrame
end
	drawscreen

	rem what touched what in the frame just drawn
	temp1 = 0
	if flags{4} then temp1 = 1

	rem a wall or a block: the hero steps back
	if !collision(player0, playfield) then goto heroFree
	if joy0left then player0x = player0x + 1
	if joy0right then player0x = player0x - 1
	if joy0up then player0y = player0y + 1
	if joy0down then player0y = player0y - 1
heroFree

	rem the enemy: it bounces (turns on the axis it moved on, and steps back)
	if !collision(player1, playfield) then goto enemyFree
	temp3 = enemyDirection[temp1]
	temp4 = frame & 2
	if temp4 then temp3 = temp3 ^ 2 else temp3 = temp3 ^ 1
	enemyDirection[temp1] = temp3
	gosub moveEnemy
enemyFree

	rem the shot: a wall stops it, an enemy is shot
	if !flags{6} then goto shotDone
	if collision(missile0, playfield) then flags{6} = 0
	if !collision(missile0, player1) then goto shotDone
	flags{6} = 0
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
	player0x = 77 : player0y = 80 : facing = 0
	flags{1} = 0 : flags{6} = 0
	return

	rem two enemies at the top corners, walking down and inwards
newWave
	enemyX[0] = 30 : enemyY[0] = 22 : enemyDirection[0] = 0
	enemyX[1] = 124 : enemyY[1] = 22 : enemyDirection[1] = 1
	flags{2} = 1 : flags{3} = 1
	return

	rem enemy temp1 moves on x on two frames and on y on the next two, so a
	rem wall it reaches is always on the axis it moved on
moveEnemy
	temp3 = enemyDirection[temp1]
	temp4 = frame & 2
	if temp4 then goto moveEnemyY
	temp5 = enemyX[temp1]
	if temp3{0} then temp5 = temp5 - 2 else temp5 = temp5 + 2
	enemyX[temp1] = temp5
	return
moveEnemyY
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

	inline theme.music.asm
