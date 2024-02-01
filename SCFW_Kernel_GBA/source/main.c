#include <gba.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

void tryAgain() {
	iprintf("Critical failure.\nPress A to restart.");
	for (;;) {
		scanKeys();
		if (keysDown() & KEY_A)
			for (;;) {
				scanKeys();
				if (keysUp() & KEY_A)
					((void(*)()) 0x02000000)();
			}
		VBlankIntrWait();
	}
}

__attribute__((packed)) struct settings {
};
struct settings settings;
FILE *settings_file;

#define GBA_ROM ((vu32*) 0x08000000)
#define GBA_BUS ((vu16*) 0x08000000)

#define SC_FLASH_MAGIC_ADDR_1 (*(vu16*) 0x08000b92)
#define SC_FLASH_MAGIC_ADDR_2 (*(vu16*) 0x0800046c)
#define SC_FLASH_MAGIC_1 ((u16) 0xaa)
#define SC_FLASH_MAGIC_2 ((u16) 0x55)
#define SC_FLASH_ERASE ((u16) 0x80)
#define SC_FLASH_ERASE_BLOCK ((u16) 0x30)
#define SC_FLASH_ERASE_CHIP ((u16) 0x10)
#define SC_FLASH_PROGRAM ((u16) 0xA0)
#define SC_FLASH_IDLE ((u16) 0xF0)
#define SC_FLASH_IDENTIFY ((u16) 0x90)

enum
{
	SC_RAM_RO = 0x1,
	SC_MEDIA = 0x3,
	SC_FLASH_RW,
	SC_RAM_RW = 0x5,
};

void sc_mode(u32 mode)
{
    u32 ime = REG_IME;
    REG_IME = 0;
    *(vu16*)0x9FFFFFE = 0xA55A;
    *(vu16*)0x9FFFFFE = 0xA55A;
    *(vu16*)0x9FFFFFE = mode;
    *(vu16*)0x9FFFFFE = mode;
    REG_IME = ime;
}

EWRAM_DATA u8 filebuf[0x4000];

u32 pressed;

void setLastPlayed(char *path) {
	FILE *lastPlayed = fopen("/scfw/lastplayed.txt", "ab+");
	char old_path[PATH_MAX];
	fseek(lastPlayed, 0, SEEK_SET);
	fread(old_path, PATH_MAX, 1, lastPlayed);
	if (strcmp(path, old_path)) {
		ftruncate(settings_file->_file, 0);
		fwrite(path, strlen(path), 1, lastPlayed);
	}
	fclose(lastPlayed);
}

void selectFile(char *path) {
	u32 pathlen = strlen(path);
	if (pathlen > 4 && !strcmp(path + pathlen - 4, ".gba")) {
		FILE *rom = fopen(path, "rb");
		fseek(rom, 0, SEEK_END);
		u32 romsize = ftell(rom);
		fseek(rom, 0, SEEK_SET);

		u32 total_bytes = 0;
		u32 bytes = 0;
		iprintf("Loading ROM:\n\n");
		do {
			bytes = fread(filebuf, 1, sizeof filebuf, rom);
			sc_mode(SC_RAM_RW);
			for (u32 i = 0; i < bytes; i += 4) {
				GBA_ROM[(i + total_bytes) >> 2] = *(vu32*) &filebuf[i];
				if (GBA_ROM[(i + total_bytes) >> 2] != *(vu32*) &filebuf[i]) {
					iprintf("SDRAM write failed!\n");
					tryAgain();
				}
			}
			sc_mode(SC_MEDIA);
			total_bytes += bytes;
			iprintf("\x1b[1A\x1b[K0x%x/0x%x\n", total_bytes, romsize);
		} while (bytes);

		iprintf("Let's go.\n");
		setLastPlayed(path);

		sc_mode(SC_RAM_RO);
		SoftReset(ROM_RESTART);
	} else if (pathlen > 4 && !strcmp(path + pathlen - 4, ".frm")) {
		u32 ime = REG_IME;
		REG_IME = 0;

		iprintf("Probing flash ID.\n");
		sc_mode(SC_FLASH_RW);
		SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_MAGIC_1;
		SC_FLASH_MAGIC_ADDR_2 = SC_FLASH_MAGIC_2;
		SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_IDENTIFY;
		u32 flash_id = SC_FLASH_MAGIC_ADDR_1;
		flash_id |= *GBA_BUS << 16;
		*GBA_BUS = SC_FLASH_IDLE;
		iprintf("Flash ID is 0x%x\n", flash_id);
		if (((flash_id >> 8) & 0xff) != 0x22) {
			iprintf("Unrecognised flash ID.");
			goto fw_end;
		}
		REG_IME = ime;

		iprintf("Flash the Supercard firmware?\n"
		        "It may brick your Supercard!\n"
		        "Press A to flash.\n"
		        "Press any other key to cancel.\n");
		do {
			scanKeys();
			pressed = keysDownRepeat();
			VBlankIntrWait();
		} while (!pressed);
		if (pressed & KEY_A) {
			sc_mode(SC_MEDIA);
			iprintf("Opening firmware\n");
			FILE *fw = fopen(path, "rb");
			fseek(fw, 0, SEEK_END);
			u32 fwsize = ftell(fw);
			fseek(fw, 0, SEEK_SET);
			if (fwsize > 0x80000) {
				iprintf("Firmware too large!\n");
				goto fw_flash_end;
			}

			ime = 0;
			iprintf("Erasing flash.\n");
			sc_mode(SC_FLASH_RW);
			SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_MAGIC_1;
			SC_FLASH_MAGIC_ADDR_2 = SC_FLASH_MAGIC_2;
			SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_ERASE;
			SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_MAGIC_1;
			SC_FLASH_MAGIC_ADDR_2 = SC_FLASH_MAGIC_2;
			SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_ERASE_CHIP;

			while (*GBA_BUS != *GBA_BUS) {
			}
			*GBA_BUS = SC_FLASH_IDLE;

			u32 total_bytes = 0;
			u32 bytes = 0;
			iprintf("Programming flash.\n\n");
			do {
				sc_mode(SC_MEDIA);
				bytes = fread(filebuf, 1, sizeof filebuf, fw);
				if (ferror(fw)) {
					iprintf("Error reading file!\n");
					goto fw_flash_end;
				}
				sc_mode(SC_FLASH_RW);
				for (u32 i = 0; i < bytes; i += 2) {
					SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_MAGIC_1;
					SC_FLASH_MAGIC_ADDR_2 = SC_FLASH_MAGIC_2;
					SC_FLASH_MAGIC_ADDR_1 = SC_FLASH_PROGRAM;
					GBA_BUS[(total_bytes + i)>>1] = filebuf[i] | (filebuf[i+1] << 8);

					while (*GBA_BUS != *GBA_BUS) {
					}
					*GBA_BUS = SC_FLASH_IDLE;
				}
				sc_mode(SC_MEDIA);
				total_bytes += bytes;
				iprintf("\x1b[1A\x1b[K0x%x/0x%x\n", total_bytes, fwsize);
			} while (bytes);

			iprintf("Done!\n");
			fw_flash_end:
			if (fw)
				fclose(fw);
		}
		fw_end:
		REG_IME = ime;
		iprintf("Press A to continue.\n");
		do {
			scanKeys();
			pressed = keysDownRepeat();
			VBlankIntrWait();
		} while (!(pressed & KEY_A));
	} else {
		iprintf("Unrecognised file extension!\n");
		do {
			scanKeys();
			pressed = keysDownRepeat();
			VBlankIntrWait();
		} while (!(pressed & KEY_A));
	}
}

int main() {
	irqInit();
	irqEnable(IRQ_VBLANK);

	scanKeys();
	keysDownRepeat();

	consoleDemoInit();

	iprintf("SCFW Kernel v0.1 GBA-mode\n\n");

	if (fatInitDefault()) {
		iprintf("FAT system initialised\n");
	} else {
		iprintf("FAT initialisation failed!\n");
		tryAgain();
	}

	settings_file = fopen("scfw/settings.bin", "ab+");
	if (settings_file) {
		fseek(settings_file, 0, SEEK_SET);
		struct settings loaded_settings = settings;
		fread(&loaded_settings, sizeof loaded_settings, 1, settings_file);
		if (memcmp(&loaded_settings, &settings, sizeof settings)) {
			settings = loaded_settings;
			ftruncate(settings_file->_file, 0);
			fwrite(&settings, sizeof settings, 1, settings_file);
		}
	} else {
		iprintf("Failed to load settings file!\n"
		        "Press A to continue.\n");
		do {
			scanKeys();
			pressed = keysDownRepeat();
			VBlankIntrWait();
		} while (!(pressed & KEY_A));
	}

	for (;;) {
		char cwd[PATH_MAX];
		getcwd(cwd, PATH_MAX);
		DIR *dir = opendir(".");
		long diroffs[0x200];
		u32 diroffs_len = 0;
		for (;;) {
			long diroff = telldir(dir);
			struct dirent *dirent = readdir(dir);
			if (!dirent)
				break;
			if (!strcmp(dirent->d_name, "."))
				continue;
			diroffs[diroffs_len++] = diroff;
		}
		if (!diroffs_len) {
			iprintf("No directory entries!\n");
			tryAgain();
		}

		for (int i = 0;;) {
			seekdir(dir, diroffs[i]);
			struct dirent *dirent = readdir(dir);

			iprintf("\x1b[2J");
			iprintf("%s\n", cwd);
			iprintf("%ld: %s\n", i, dirent->d_name);

			do {
				scanKeys();
				pressed = keysDownRepeat();
				VBlankIntrWait();
			} while (!(pressed & (KEY_A | KEY_B | KEY_START | KEY_UP | KEY_DOWN)));

			if (pressed & KEY_A) {
				if (dirent->d_type == DT_DIR) {
					chdir(dirent->d_name);
					break;
				} else {
					// inefficient, idc
					char path[PATH_MAX];
					strncpy(path, cwd, PATH_MAX);
					strncpy(path, "/", PATH_MAX);
					strncpy(path, dirent->d_name, PATH_MAX);
					selectFile(path);
				}
			}
			if (pressed & KEY_B) {
				chdir("..");
				break;
			}
			if (pressed & KEY_START) {
				FILE *lastPlayed = fopen("/scfw/lastplayed.txt", "rb");
				if (lastPlayed) {
					char path[PATH_MAX];
					fread(path, PATH_MAX, 1, lastPlayed);
					fclose(lastPlayed);
					selectFile(path);
				} else {
					iprintf("Could not open last played.\n");
					do {
						scanKeys();
						pressed = keysDownRepeat();
						VBlankIntrWait();
					} while (!(pressed & KEY_A));
				}

			}
			if (pressed & KEY_DOWN) {
				++i;
				if (i >= diroffs_len)
					i -= diroffs_len;
			}
			if (pressed & KEY_UP) {
				--i;
				if (i < 0)
					i += diroffs_len;
			}
		}
		closedir(dir);
	}


	tryAgain();
}