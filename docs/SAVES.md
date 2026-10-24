# Moving your save between a 3DS and the PC

The port keeps the game's save and its extdata in the user folder, in Azahar's layout. The Azahar build and
`mh4u-native` share that folder, so **both see the same save**. You can bring your 3DS save in and take it back out
with the folders Checkpoint, JKSM and GodMode9 write. Raw save images (DISA, DIFF) are not supported.

The extdata holds your guild cards, Palico and downloaded quests, so move it together with the save.

**Close the game (and the emulator) first.** An import into a save that a running game has open is not safe.

## From your 3DS to the PC

1. On the 3DS, open **Checkpoint** (cartridge or digital copy of Monster Hunter 4 Ultimate). Back up the **save**,
   and back up the **extdata** as well (Checkpoint has a separate extdata list).
2. Copy the two backup folders from the SD card (under `/3ds/Checkpoint/`) to the PC. The save folder holds files such
   as `system` and `user1`; the extdata folder holds `card1`, `quest1` and so on. Point the tool at the folder that
   holds those files directly: if it says a sub-folder is in the way, open the folder and pick the one inside that
   holds the files.
3. Import them, with the launcher's Saves button once the launcher has it (N3-19), or with the command, from the
   repository folder (Windows: `py -3`; Linux and macOS: `python3`):

       py -3 tools\native\saves.py import-save    "C:\path\to\save folder"    --user "C:\path\to\user"
       py -3 tools\native\saves.py import-extdata "C:\path\to\extdata folder" --user "C:\path\to\user"

   `--user` is the user folder both executables use (the `user` folder next to `mh4u.exe`, or `%APPDATA%\Azahar`;
   on Linux `./user` or `~/.local/share/azahar-emu`). Add `--json` for a machine-readable answer.

`saves.py list --user "..."` shows what the user folder holds now (and says so when a folder holds something that is
not a save file).

## From the PC back to your 3DS

    py -3 tools\native\saves.py export-save    --user "C:\path\to\user" "C:\path\to\a new folder for the save"
    py -3 tools\native\saves.py export-extdata --user "C:\path\to\user" "C:\path\to\a new folder for the extdata"

The destination must be a new or empty folder: if it says the folder is not empty, use a new folder (or add `--force`,
which writes over files of the same names and leaves the others alone, and tells you which it left).

**Before you restore over the save on your console, take a Checkpoint backup of the 3DS save (and extdata) as it is
now**, so you can go back. Then copy the exported folders into Checkpoint's backup folders on the SD card (under a
name you pick) and restore them in Checkpoint.

## What an import does to what was there

- It first copies the current save (or extdata) to `<user>/mh4u-save-backups/<year><month><day>-<hour><minute><second>/`
  (UTC), in the same layout as the user folder, so putting a file back is a copy. A second import in the same second
  gets `-2`. Nothing is backed up when there was nothing there.
- Every new file is written completely to a temporary name (`<name>.mh4n-tmp`) and flushed to disk first; only then
  are they renamed over the old ones. If a file cannot be written (disk full, a file in use), the save is left as it
  was and the message names the backup. A crash leaves the old file or the new one; leftover `.mh4n-tmp` files are
  ignored and removed by the next import.
- Files the old save had and the imported one has not are removed after the import (they are in the backup): the
  archive becomes what you imported. The archive's `metadata` file is kept when it exists.
- The backups are never deleted by the tool; delete old ones yourself when you no longer need them.
