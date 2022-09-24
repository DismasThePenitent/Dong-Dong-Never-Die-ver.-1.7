LunaPort r39 Readme


LunaPort is an original netplay application for the free game Vanguard Princess,
made by Suge9. The game can be downloaded via the author's blog at:
http://suge9.blog58.fc2.com/
LunaPort might also work with other games, which use the same engine, as long as
the game exe is exactly identical to the one used by Vanguard Princess.

Please note that LunaPort is still in early developement, so some versions might
not work at all. Only testing can tell.

It is released under the GPLv3 or later. Feel free to tinker, improve, etc.
Magic in the header file is "explained" in src/gore.txt

The netcode seems to be reasonable these days.

If you want to send me a bug report, patch or anything, just drop it into:
http://lunaport.pastebin.com/
If you want to get older versions of LunaPort, you might find them at:
www.mediafire.com/?sharekey=1536dbd02f0809f3e62ea590dc5e5dbbe04e75f6e8ebb871

IMPORTANT NOTES:
0) Do not touch the Game menu. If you already touched it, get a pristine copy
   of game.ini before even trying this.
1) If you have customized P2 controls, make a backup of your game.ini, might
   get overwritten with P1 controls, although that shouldn't happen.
2) Also, don't try to change any controls in the menu while playing with
   LunaPort. Bad things will happen and the changes won't be saved anyway.

Less important notes:
3) I only have a single PC this game runs on, so I couldn't really test
   LunaPort. Please help me out here.
4) As long as the game exe is the same, other games might work too, but you have
   to set the correct Stage in lunaport.ini
5) Delay is counted in input requests, not frames. There are 100 input requests
   per sec, so 1 delay is 10ms.
6) If the standard filename for the game exe is not found, game.exe is tried.
   You can also specify the filename for the game exe in lunaport.ini

Usage:
Pretty much like caster, even though LunaPort is not based on it. By default,
it uses port 7500 UDP. Use option one to host a game, option two to join a
game, and option three to play a local VS games with random stage selection.
Don't forget to put LunaPort.exe into the game folder.
You can use the --local commandline switch to start local VS mode with random
stages directly. You can give a .rpy file as a commandline argument to watch it
directly.

Known working: r11, r35, r36, r38

Please excuse the desync problems from before. Things should be better now.

Now, let's hope this works. Once it does, I might even get around to cleaning
up the code.

Links:
http://vp.mizuumi.net/index.php/Main_Page
http://vp.mizuumi.net/index.php/Lunaport
http://vanpri.wikiwiki.jp/
http://vanpri.wikiwiki.jp/?%C4%CC%BF%AE%C2%D0%C0%EF
http://jbbs.livedoor.jp/otaku/12852/

-- 37f3b1e05fb90ddbb7d834314d6e562f251b49f7
   d61b6b204f454a9c52c23f766fe3df4c705bd5d6
   377d97b2a687444bbf9c58c70f8214268a00a9b5
   f5044b8ecb3632ef663a8749aa5be47dec9a488d
   0eb701b3a21d7194188e5a8cde88e3c5e40b645a
   481adab3e68722c73bf99cb49f7dc85d3203ccfa

PS: About http://vanpri.wikiwiki.jp/?%C4%CC%BF%AE%C2%D0%C0%EF
    I am very glad to see that LunaPort seems to work for you.
