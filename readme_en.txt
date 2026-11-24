mpainter - a lightweight image viewer and annotator
===================================================

Portable. The program is the single file mpainter.exe; run it from anywhere.
No Visual C++ Redistributable needed. The 7z.dll next to it is only used for viewing
archives, and plugins\jxl and plugins\dng only for opening JPEG XL and DNG; delete them if you
do not need that.

Usage
-----
- Drop an image onto the window, use File > Open, or pass a path on the command line:
  mpainter.exe <image or folder>
- Supports BMP, TIFF, JPEG, PNG, WebP; the bundled plugins add JPEG XL (.jxl) and DNG (.dng,
  camera raw), which you can view and export but not edit. See "Image plugins" below.
- HEIC / HEIF (.heic, .heif, .hif, e.g. iPhone photos) are decoded by Windows once you install
  extensions from Microsoft Store; view and export only. See "HEIC / HEIF" below.
- Also opens zip / 7z / rar archives (including .cbz / .cb7 / .cbr) and pages through the
  images inside as if they were a folder; see "Archives" below.
- Left / Right arrows go to the previous / next image in the same folder. At the end you get
  a "This is the last image" hint. Opening a folder starts Browse Folder; see the next section.
- For reading comics, turn on View > "Continue to the next folder or archive at the end"
  (off by default; the built-in "Comic mode" has it on): at the end, the first
  press tells you where you would go, and a second press within 3 seconds jumps to the first
  page of the next folder or archive in the same parent folder; going backwards is symmetric.
  Only that one level is searched: subfolders and archives inside the current folder are not
  part of it, and the parent's siblings are never visited.
- The markup toolbar (pen button at the right of the toolbar) has sketch, line / arrow,
  shapes, text, adjust color, adjust size and crop. To crop, click the crop button, drag the
  corners, the edges or the inside of the frame, then click Crop at the right end of the
  markup toolbar; Esc cancels.
- Ctrl+S flattens the annotations into the image and overwrites the file.
  File > Export As (Ctrl+Shift+S) writes the current view (annotations included) to a new
  file, optionally in another format; the file you are editing stays the original. There
  is no "Save As". Saving keeps the original EXIF, XMP and ICC (Orientation becomes 1
  because the pixels are already upright). Exporting to a format that cannot hold that
  data (PNG, BMP, ...) asks first. JPEG and BMP cannot keep transparency: exporting an image
  with transparent areas to them asks first, and those areas become white (semi-transparent
  pixels are blended with white).
- Up / Down scroll. The View menu picks a view mode: fit to window, fit to width (webtoons),
  fit to height, actual size, two pages side by side (comics: wide images get a screen of
  their own; right-to-left, first page alone and shift-by-one are switches). The mode is
  remembered across pages.
- Zoom quality (fast / high quality Lanczos3), "Up/Down at the end of a page turns the page"
  and the two-page switches are part of a "mode"; see "Modes (setting presets)".

Browse Folder
-------------
- The grid button to the right of Rotate (or View > Browse Folder) switches to Browse Folder:
  the folder tree at the top left (the same one as on the left of File Explorer), a preview at
  the bottom left and thumbnails of the folder on the right. Press the button again or Esc to
  go back to the image you were viewing. An image (or archive) must be open to enter; with an
  archive open you get the folder that holds the archive.
- The right side lists subfolders, then archives, then images: this level only, images in
  formats mpainter can open, in the same order as paging. A click selects and shows a preview
  with size, bit depth, file size and modified date below it (the info panel shows the same
  image). Double-click or Enter: an image opens in the normal view, and Left / Right then page
  through this folder; a subfolder is entered; an archive opens in the current mode.
  Backspace goes up one level; Tab switches between the folder tree and the thumbnails. When
  another program adds or deletes files in the folder, the thumbnails follow.
- Thumbnails come in small, medium and large: click the small triangle at the right of the
  button, or View > Thumbnail Size. The dividers between the three parts can be dragged and
  are remembered.
- The file name bar shows the current path, which you can select and copy.
- With unsaved changes, mpainter asks first: save and enter, discard and enter, or stay. Entering
  clears the undo history of the image.
- A folder given on the command line, dropped on the window, or opened with "Open with mpainter"
  in File Explorer starts Browse Folder.
- Thumbnails: ones Windows has already made are used as they are; mpainter makes the rest and
  keeps them in thumbs.mpainter.db in the unpack folder (by default "temp" next to the exe),
  leaving nothing in your picture folders. The limit is 500 MB by default; over it, the ones
  unused for the longest are dropped. Edit > Settings > Archives changes the limit, shows the
  usage and clears them. If the unpack folder is on a network drive, no thumbnails are kept.
- No file operations (delete, rename, copy, move), and the folder tree has no context menu.

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
- Edit > Settings has five tabs: General (language, hints, undo steps, prefetch, settings file), Modes (new / copy /
  rename / delete / use this mode, with the mode's contents on the right), Archives (unpack
  folder with its current usage, show-first names, passwords, Browse Folder's thumbnail cache), Windows Integration
  (context menu, default app) and Plugins (which image plugins to use). The buttons are
  Cancel / OK / Apply; Apply takes effect at once and keeps the window open. The language and
  plugins take effect after a restart; mpainter asks when you close the window.
- Undo (Ctrl+Z) keeps up to 99 steps; change it to anything from 1 to 999 in Edit > Settings >
  General. Rotating, cropping, resizing and color adjustment each keep a full copy of the image
  per step. When those copies add up to more than the memory limit (512 MB by default, on the
  same page), the oldest steps are dropped; the latest step is always kept. With the defaults a
  24-megapixel photo keeps about 5 such steps. Steps that only add markup do not count against
  the limit. When the history is almost full (one more step may drop the earliest one), a note
  appears once in the middle of the window. The undo history of an image is dropped when you
  move to another image, so mpainter asks first: whether to save unsaved changes, or, after
  saving, whether to drop the history (Cancel stays on the image). Rotating inside an
  archive does not ask.
- While you read, the next page (the next pair in two-page view) is decoded in the background and
  the page you just left is kept, so turning pages either way does not wait. With "Continue to
  the next folder or archive at the end" on, the next book is prepared when you reach the last
  page of a book; an archive is unpacked in advance, so the temp folder holds two books for a
  while. Prefetched pages use at most the prefetch memory limit (512 MB by default; Edit >
  Settings > General, 0 turns it off).

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
- Images inside an archive are read-only: no annotation, color or size adjustment, cropping,
  or save.
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
- If several windows of the same mpainter.exe are open, it first tells you the others will be
  closed. Each window with unsaved edits asks whether to save; Cancel in any of them stops the
  update. Afterwards only the window you installed from opens again.
- If mpainter cannot write to its own folder (e.g. it lives in Program Files) or 7z.dll is missing,
  it cannot update itself; it tells you why and offers to open the download page.
- To stop the automatic check, untick "Check for updates when mpainter starts" in Edit > Settings >
  General. You can still check by hand: Help > About mpainter > Check for Updates.

HEIC / HEIF
-----------
- mpainter does not ship a HEIC decoder (HEVC has patent licensing issues); Windows decodes it.
  Install from Microsoft Store: "HEIF Image Extension" (free) plus "HEVC Video Extensions" (paid).
  Files with AV1 content (AVIF) need "AV1 Video Extension" (free) instead.
- When one is missing, the canvas says which, with a link to its Microsoft Store web page.
- A HEIC image is read-only: you can view it and use Export As (one of the five formats above;
  exporting to JPEG keeps the EXIF), but not mark it up, adjust or rotate it.
- Transparency becomes opaque (the Windows decoder gives no alpha). 10-bit HEIC may not open (it
  does not on our test PC, which has no graphics card).
- If someone makes a HEIC plugin with its own decoder, enabling it makes .heic open through it.

Image plugins
-------------
- Plugins let mpainter open more image formats. An image opened through a plugin is read-only:
  you can view it and use Export As (one of the five formats above), but not mark it up, adjust,
  rotate or save it (trying says so).
- The JPEG XL plugin (plugins\jxl\jxl.dll, using libjxl) is bundled and on by default.
  Animations show the first frame only.
- The DNG plugin (plugins\dng\dng.dll, using the Adobe DNG SDK) is bundled and on by default.
  It renders the raw data with the camera settings stored in the file, to 8-bit sRGB, including
  the lens corrections stored in the file; when the file has an image enhanced by Lightroom, that
  one is shown. A 20-odd-megapixel file takes about 1 to 2 seconds on a 4-core computer. XMP in
  the file is not read; 3:1 lossy compressed DNG from Blackmagic cameras does not open.
- Plugins made by others: put them in plugins\<name>\<name>.dll, tick them in
  Edit > Settings > Plugins and restart mpainter. New ones start disabled.
- Plugins run inside mpainter. If a plugin crashes mpainter, the next start disables it and
  tells you; tick it again on the Plugins page to use it again.
- Two plugins that open the same file extension cannot be on together: ticking the second one
  on the Plugins page asks whether to switch to it. If the ini is edited by hand to turn both
  on, the next start turns both off and tells you; then pick one on the Plugins page.
- The extensions of the enabled plugins are added to the context menu and the default-app list
  too, and removed on the next start after a plugin is turned off.
- How to write a plugin: src/plugins/README.md in the source code; src/plugins/jxl is the example.

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
Each plugin's state is one line (plugin=<name>|on / off / crashed / conflict); without a line,
the bundled plugin is on and the others are off.
Leading and trailing spaces in a password= line are part of the password. Move the exe and
the ini together and the settings move too. If the exe sits in a location that cannot be
written (optical disc, folder without write permission), settings are not saved and every
start returns to defaults. mpainter says so once, with the reason, the first time saving
fails, and Edit > Settings shows it too.

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
time (GNU LGPL 2.1; the RAR decoder carries the unRAR restriction). The JPEG XL plugin uses
libjxl (BSD-3-Clause, with Google's patent grant), Highway (BSD-3-Clause), Brotli and Little CMS
(MIT). The DNG plugin uses the Adobe DNG SDK (Adobe's DNG SDK License Agreement), the libjpeg
that comes with it (Independent JPEG Group) and zlib, plus the same libjxl, Highway, Brotli and
Little CMS. Full license texts are in THIRD-PARTY-NOTICES.txt.
This product includes DNG technology under license by Adobe.
This software is based in part on the work of the Independent JPEG Group. 7z.dll is unmodified; the matching 7-Zip source code is published
with every release and at https://www.7-zip.org.
Source code and build instructions: project_info.md in the repository.
