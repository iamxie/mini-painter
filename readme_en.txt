mpainter - a lightweight image viewer and annotator
===================================================

Portable. The program is the single file mpainter.exe; run it from anywhere.
No Visual C++ Redistributable needed. The 7z.dll next to it is only used for viewing
archives; delete it if you do not need that.

Usage
-----
- Drop an image onto the window, use File > Open, or pass a path on the command line:
  mpainter.exe <image or folder>
- Supports BMP, TIFF, JPEG, PNG, WebP.
- Also opens zip / 7z / rar archives (including .cbz / .cb7 / .cbr) and pages through the
  images inside as if they were a folder; see "Archives" below.
- Left / Right arrows go to the previous / next image in the same folder. Opening a folder
  starts at its first image. At the end you get a "This is the last image" hint.
- For reading comics, turn on View > "Continue to the next folder or archive at the end"
  (off by default; the built-in "Comic mode" has it on): at the end, the first
  press tells you where you would go, and a second press within 3 seconds jumps to the first
  page of the next folder or archive in the same parent folder; going backwards is symmetric.
  Only that one level is searched: subfolders and archives inside the current folder are not
  part of it, and the parent's siblings are never visited. Opening a folder that only holds
  archives or subfolders opens its first one.
- The markup toolbar (pen button at the right of the toolbar) has sketch, line / arrow,
  shapes, text, adjust color and adjust size.
- Ctrl+S flattens the annotations into the image and overwrites the file.
  File > Export As (Ctrl+Shift+S) writes the current view (annotations included) to a new
  file, optionally in another format; the file you are editing stays the original. There
  is no "Save As". Saving keeps the original EXIF, XMP and ICC (Orientation becomes 1
  because the pixels are already upright). Exporting to a format that cannot hold that
  data (PNG, BMP, ...) asks first.
- Up / Down scroll. The View menu picks a view mode: fit to window, fit to width (webtoons),
  fit to height, actual size, two pages side by side (comics: wide images get a screen of
  their own; right-to-left, first page alone and shift-by-one are switches). The mode is
  remembered across pages.
- Zoom quality (fast / high quality Lanczos3), "Up/Down at the end of a page turns the page"
  and the two-page switches are part of a "mode"; see the next section.

Modes (settings sets)
---------------------
- View > Mode lists the modes you have; picking one applies a whole set of viewing settings:
  the view mode images open in, enlarge small images, zoom quality, Up/Down turns the page,
  continue to the next folder or archive, two pages right to left, first page alone, and the
  metadata warning when saving. Two are built in: "General mode" (actual size, no continuing,
  no two-page, warning on) and "Comic mode" (two pages, continue, high quality, no warning).
  Rename, edit and add as many as you like.
- Once a mode is picked, anything you change in the View menu is a temporary adjustment: it
  applies now, the mode itself is untouched. The title bar adds * after the mode name and the
  menu shows "(adjusted)". To make the adjustments the new baseline, use View > Mode > "Save
  Current Settings to ...". Restarting keeps the state you closed with, adjustments included.
- A mode changes only when you pick one: in View > Mode or in the Modes tab of Settings. It
  never switches by itself because of what you opened. The title bar always ends with the
  current mode.
- Edit > Settings has four tabs: General (language, hints, settings file), Modes (new / copy /
  rename / delete / use this mode, with the mode's contents on the right), Archives (unpack
  folder with its current usage, show-first names, passwords) and Windows Integration
  (context menu, default app). The buttons are Cancel / OK / Apply; Apply takes effect at once
  and keeps the window open.

Archives (zip / 7z / rar)
-------------------------
- Open an archive the same way as an image: drop it on the window, File > Open, the command
  line, or "Open with mpainter" in the Explorer context menu. .cbz / .cb7 / .cbr can also be
  set to open with mpainter by default; .zip / .7z / .rar only get the context menu entry so
  your archiver keeps its association.
- The whole archive is unpacked to a temp folder first; every supported image inside is then
  flattened into one list sorted by path (natural order, chapters in subfolders follow one
  another). __MACOSX, Thumbs.db and dot-files are skipped. Names in the "Show first" list
  of Edit > Settings (default cover.jpg / cover.png / cover.webp) go to the front; the same
  applies when opening a plain folder.
- Unpacked files are numbered in the temp folder (00001.png, ...); the window shows the name
  inside the archive. So names with characters Windows does not allow, or two names that the
  wrong code page maps to the same string, never make unpacking fail or lose a page. Names in
  old zips without the UTF-8 flag may show as mojibake on another locale, but every page is
  there. A damaged page (CRC error) is skipped and the rest opens; you are told how many are
  missing first.
- Encrypted archives: the passwords in Edit > Settings > "Passwords to try" are tried one by
  one first (the window shows the attempt); you are asked only if none fits. A password that
  works is added to that list. Note that the list is stored unencrypted in mpainter.ini.
- Images inside an archive are read-only: no annotation, color or size adjustment, or save.
  To edit, extract the archive with your archiver and open the file. Rotation (Ctrl+R) still
  works as a viewing aid and resets when you turn the page. Export As works too: it writes
  this one image (rotation included) next to the archive, metadata copied over.
- The temp folder defaults to "temp" next to mpainter.exe (%TEMP%\mpainter\unzip when the exe
  sits somewhere read-only) and can be changed in Edit > Settings > Archives, which also shows
  the folder in use and its current size. Note that files unpacked from an encrypted archive
  sit there unencrypted. They are deleted when mpainter exits; after a crash, the next start of
  any mpainter cleans them up. If mpainter.exe lives in a synced folder (OneDrive, Dropbox),
  the unpacked images get synced too; move the temp folder elsewhere in that case.
- This feature relies on 7z.dll (a 7-Zip component) next to mpainter.exe. Without it archives
  cannot be opened; everything else works. You may replace 7z.dll with another build that has
  the same interface, e.g. from a newer 7-Zip.

Updates
-------
- Each time it starts, mpainter checks GitHub for a newer version in the background; opening the
  image is not slowed down. If there is no connection it stays quiet.
- When a new version exists, a "Software Update" window shows the version and its release notes,
  with three choices: Skip This Version (automatic checks stop asking about it), Remind Me Later
  (asks again next start), Install Update.
- Install Update: if you have unsaved edits it first asks whether to save (Cancel stops the update),
  then downloads, verifies the file, replaces the program files next to mpainter.exe, and closes
  and reopens itself on the same image (or archive). mpainter.ini, the temp folder and language
  files you added yourself are left alone. Files that ship in the package (including 7z.dll,
  lang\ja-JP.txt and the readmes) are replaced; if you edited one of them, save it under another
  name first.
- If mpainter cannot write to its own folder (e.g. it lives in Program Files) or 7z.dll is missing,
  it cannot update itself; it tells you why and offers to open the download page.
- To stop the automatic check, untick "Check for updates when mpainter starts" in Edit > Settings >
  General. You can still check by hand: Help > About mpainter > Check for Updates.

Language
--------
The "語言 / Language" menu switches the UI language. Traditional Chinese and English are
built in; the default follows Windows. The bundled lang\ja-JP.txt is Japanese and, with the
format notes in its header, the template for new languages. To add one, copy lang\ja-JP.txt to
lang\<code>.txt (in a lang folder next to mpainter.exe), edit _code and _name in the
header, and translate the right-hand side of each line. Missing lines fall back to English.

Settings file mpainter.ini
--------------------------
Settings live in mpainter.ini next to mpainter.exe, created on first run. The window position,
size and maximized state are kept there too (window=), and each mode is a [profile] section.
Leading and trailing spaces in a password= line are part of the password. Move the exe and
the ini together and the settings move too. If the exe sits in a location that cannot be
written (optical disc, folder without write permission), settings are not saved and every
start returns to defaults; Edit > Settings says so.

"Open with mpainter" in the Explorer context menu
--------------------------------------------------
On first run mpainter adds "Open with mpainter" to the Explorer context menu for image
files and folders. This only writes the current user's registry (HKEY_CURRENT_USER), needs
no administrator rights and does not affect other programs. It can be turned off in
Edit > Settings, which removes the entry. After moving mpainter.exe to another folder, the
next run updates the path. After replacing the exe with a new version the icon in the
context menu may stay old: that is Explorer's icon cache; sign out and in again (or restart
Explorer) to refresh it.

  Windows 11: the context menu has two levels and this item is on the second one. Press
  "Show more options" (or Shift+F10 with the file selected). Being on the first level
  requires an MSIX-packaged install, which conflicts with the portable design, so it is
  not done. Windows 10 does not have this issue.

Default image app
-----------------
On the first image opened, mpainter asks whether to become the default image app.
Since Windows 8 a program cannot set itself as default; you choose it in Settings:
- Windows 10: Settings > Apps > Default apps > Set defaults by app > mpainter > Manage.
- Windows 11: Settings > Apps > Default apps > search mpainter, set each file type.
mpainter registers itself in the list first and opens that Settings page. Later:
Edit > Settings > Set as default image app...

Removal
-------
1. Untick the context menu option in Edit > Settings.
2. Delete mpainter.exe and mpainter.ini.
3. If it was the default app, Windows falls back to another program. The leftover keys
   HKEY_CURRENT_USER\Software\mpainter and HKEY_CURRENT_USER\Software\Classes\mpainter.Image
   can be deleted by hand; leaving them is harmless.

License
-------
Open source under the Apache License 2.0 (full text in LICENSE, notices in NOTICE).
Uses libwebp (Google, BSD-3-Clause) and bit7z (MPL 2.0), and loads 7-Zip's 7z.dll at run
time (GNU LGPL 2.1; the RAR decoder carries the unRAR restriction). Full license texts are
in THIRD-PARTY-NOTICES.txt. 7z.dll is unmodified; the matching 7-Zip source code is published
with every release and at https://www.7-zip.org.
Source code and build instructions: project_info.md in the repository.
