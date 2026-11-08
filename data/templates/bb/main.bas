  rem {{NAME}} - batari Basic game made with VCSIDE (CC0: use and change it freely).
  rem Move the square with the joystick; the button changes the colour.
  rem The playfield wall stops it. RESET starts again.

  set romsize 4k
  set smartbranching on

  dim colour = a

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

main
  COLUBK = $00
  COLUPF = $84
  COLUP0 = colour
  scorecolor = $0E

  if joy0fire then colour = colour + 2 : score = score + 1
  if joy0left then player0x = player0x - 1
  if joy0right then player0x = player0x + 1
  if joy0up then player0y = player0y - 1
  if joy0down then player0y = player0y + 1

  drawscreen

  rem the playfield was hit in the frame just drawn: step back
  if !collision(player0, playfield) then goto moved
  if joy0left then player0x = player0x + 1
  if joy0right then player0x = player0x - 1
  if joy0up then player0y = player0y + 1
  if joy0down then player0y = player0y - 1
moved

  if switchreset then goto start
  goto main
