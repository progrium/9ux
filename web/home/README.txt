Welcome to 9/UX, running in your browser.

This is 9front's own userland and the 9/UX desktop, compiled for 386 and run by h9 on the v86 emulator, inside your browser tab. Nothing leaves the tab: the files are an archive in memory, and what you change goes away when you close it.

Some things to try:

- Double-click a folder to open it, and a .txt file to open it in acme, through the plumber (you just did).
- Drag icons between folder windows, or onto the sample app's text field.
- Select an icon and choose Get Info, Rename or Duplicate from the File menu.
- Every app's windows and controls are files too. Choose New from the Glenda menu at the left of the menu bar, sweep out a window, and type in it:

	ls /mnt/ux/apps
	cd /mnt/ux/apps/sample-1/windows/*/controls
	ls
	echo 'set ''hello, world''' >name/ctl
	echo activate >save/ctl

Mouse: button 2 is Alt-click (Option-click), button 3 is right-click or Ctrl-click (Cmd-click on a Mac). To paste text from outside, press Ctrl-V (Cmd-V) in the screen, then paste with acme's or rio's own paste.
