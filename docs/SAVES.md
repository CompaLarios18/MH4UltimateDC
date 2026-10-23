# Moving your save between a 3DS and the PC

The port keeps the game's save and its extdata (guild cards, Palico, downloaded quests) in the user folder, in
Azahar's layout. The Azahar build and `mh4u-native` share that folder, so **both see the same save**. You can bring
your 3DS save in and take it back out with the folders Checkpoint, JKSM and GodMode9 write. Raw save images (DISA,
DIFF) are not supported.

**Close the game (and the emulator) first.** An import into a save that a running game has open is not safe.

## From your 3DS to the PC

1. On the 3DS, open **Checkpoint** (cartridge or digital copy of Monster Hunter 4 Ultimate). Back up the **save**,
   and, with the extdata tab (press X), back up the **extdata** as well. Without the extdata you lose your guild
   cards, Palico and downloaded quests.
2. Copy the two backup folders from the SD card (`/3ds/Checkpoint/saves/...` and `/3ds/Checkpoint/extdata/...`) to
   the PC. The save folder holds files such as `system` and `user1`; the extdata folder holds `card1`, `quest1` and
   so on. Folders with sub-folders are refused.
3. Import them, with the launcher's Saves button, or with the command (from the repository folder):

       python3 tools/native/saves.py import-save    "C:\path\to\save folder"    --user "C:\path\to\user"
       python3 tools/native/saves.py import-extdata "C:\path\to\extdata folder" --user "C:\path\to\user"

   `--user` is the user folder both executables use (the `user` folder next to `mh4u.exe`, or `%APPDATA%\Azahar`;
   on Linux `./user` or `~/.local/share/azahar-emu`). Add `--json` for a machine-readable answer.

`saves.py list --user "..."` shows what the user folder holds now.

## From the PC back to your 3DS

    python3 tools/native/saves.py export-save    --user "C:\path\to\user" "C:\path\to\new empty folder"
    python3 tools/native/saves.py export-extdata --user "C:\path\to\user" "C:\path\to\another empty folder"

Copy the folders into Checkpoint's backup folders on the SD card (the same place as above, under a name you pick),
then restore them in Checkpoint. An export refuses a folder that is not empty unless you add `--force`, which writes
over files of the same names and leaves the others alone.

## What an import does to what was there

- It first copies the current save (or extdata) to `<user>/mh4u-save-backups/<year><month><day>-<hour><minute><second>/`
  (UTC), in the same layout as the user folder, so putting a file back is a copy. A second import in the same second
  gets `-2`. Nothing is backed up when there was nothing there.
- Every file is written to a temporary name (`<name>.mh4n-tmp`), flushed to disk and then renamed over the old one: a
  crash leaves the old file or the new one, never half of one. The temporary file of a crash can be deleted.
- Files the old save had and the imported one has not are removed after the import (they are in the backup): the
  archive becomes what you imported. The archive's `metadata` file is kept when it exists.
- The backups are never deleted by the tool; delete old ones yourself when you no longer need them.
