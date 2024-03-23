/**
 * Copyright (c) 2024 Adrian Siekierka
 *
 * swanshell is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * swanshell is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with swanshell. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ws.h>
#include <ws/hardware.h>
#include <ws/system.h>
#include <wsx/lzsa.h>
#include "launch.h"
#include "nileswan/nileswan.h"
#include "bootstub.h"
#include "../../build/menu/build/bootstub_bin.h"
#include "../../build/menu/assets/menu/bootstub_tiles.h"
#include "fatfs/ff.h"
#include "ui/ui.h"
#include "util/file.h"
#include "util/ini.h"

__attribute__((section(".iramx_0040")))
uint16_t ipl0_initial_regs[16];
__attribute__((section(".iramC.launch.sector_buffer")))
uint8_t sector_buffer[1024];

extern FATFS fs;

/*
void ui_boot(const char *path) {
    FIL fp;
    
	uint8_t result = f_open(&fp, path, FA_READ);
	if (result != FR_OK) {
        // TODO
        return;
	}

    outportw(IO_DISPLAY_CTRL, 0);
	outportb(IO_CART_FLASH, CART_FLASH_ENABLE);

    uint32_t size = f_size(&fp);
    if (size > 4L*1024*1024) {
        return;
    }
    if (size < 65536L) {
        size = 65536L;
    }
    uint32_t real_size = round2(size);
    uint16_t offset = (real_size - size);
    uint16_t bank = (real_size - size) >> 16;
    uint16_t total_banks = real_size >> 16;

	while (bank < total_banks) {
		outportw(IO_BANK_2003_RAM, bank);
		if (offset < 0x8000) {
			if ((result = f_read(&fp, MK_FP(0x1000, offset), 0x8000 - offset, NULL)) != FR_OK) {
                ui_init();
				return;
			}
			offset = 0x8000;
		}
		if ((result = f_read(&fp, MK_FP(0x1000, offset), -offset, NULL)) != FR_OK) {
            ui_init();
			return;
		}
		offset = 0x0000;
		bank++;
	}
    
    clear_registers(true);
    launch_ram_asm(MK_FP(0xFFFF, 0x0000), );
} */

static const uint8_t __far elisa_font_string[] = {'E', 'L', 'I', 'S', 'A'};
static const uint16_t __far sram_sizes[] = {
    0, 8, 32, 128, 256, 512, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};
static const uint16_t __far eeprom_sizes[] = {
    0, 128, 2048, 0, 0, 1024, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

uint8_t launch_get_rom_metadata(const char *path, launch_rom_metadata_t *meta) {
    uint8_t tmp[5];
    uint16_t br;
    bool elisa_found = false;

    FIL f;
    uint8_t result = f_open(&f, path, FA_OPEN_EXISTING | FA_READ);
    if (result != FR_OK)
        return result;

    uint32_t size = f_size(&f);
    if (size == 0x80000) {
        result = f_lseek(&f, 0x70000);
        if (result != FR_OK)
            return result;

        result = f_read(&f, tmp, sizeof(elisa_font_string), &br);
        if (result != FR_OK)
            return result;

        elisa_found = !_fmemcmp(elisa_font_string, tmp, sizeof(elisa_font_string));
    }

    result = f_lseek(&f, size - 16);
    if (result != FR_OK)
        return result;

    result = f_read(&f, &(meta->footer), sizeof(rom_footer_t), &br);
    if (result != FR_OK)
        return result;

    meta->sram_size = sram_sizes[meta->footer.save_type & 0xF] * 1024L;
    meta->eeprom_size = eeprom_sizes[meta->footer.save_type >> 4];
    meta->flash_size = 0;
    if (elisa_found
        && meta->footer.publisher_id == 0x00
        && meta->footer.game_id == 0x00
        && meta->footer.save_type == 0x04
        && meta->footer.mapper == 0x01) {

        meta->flash_size = 0x80000;
    }

    return FR_OK;
}

static const char __far save_ini_location[] = "/NILESWAN/SAVE.INI";

// Follow ares convention on save filenames.
static const char __far ext_sram[] = ".ram";
static const char __far ext_eeprom[] = ".eeprom";
static const char __far ext_flash[] = ".flash";
static const char __far ext_rtc[] = ".rtc";

static uint8_t preallocate_file(const char *path, FIL *fp, uint8_t fill_byte, uint32_t file_size, const char *src_path) {
    uint8_t stack_buffer[16];
    uint8_t *buffer;
    uint16_t buffer_size;
    uint8_t result, result2;
    uint16_t bw;

    if (ws_system_color_active()) {
        buffer = sector_buffer;
        buffer_size = sizeof(sector_buffer);
    } else {
        buffer = stack_buffer;
        buffer_size = sizeof(stack_buffer);
    }

    result = f_open(fp, path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
    if (result != FR_OK)
        return result;

    // Do not overwrite the file if it already has the right size.
    if (f_size(fp) >= file_size)
        goto preallocate_file_end;

    // Try to ensure a contiguous area for the file.
    result = f_expand(fp, file_size, 0);

    // Write the remaining data.
    if (src_path != NULL) {
        // Copy from src_path to path.
        FIL src_fp;

        result = f_open(&src_fp, src_path, FA_READ | FA_OPEN_EXISTING);
        if (result != FR_OK)
            return result;

        if (f_size(&src_fp) != file_size) {
            result = FR_INT_ERR;
            goto preallocate_file_copy_end;
        }

        for (uint32_t i = 0; i < file_size; i += buffer_size) {
            uint16_t to_write = buffer_size;
            if ((file_size - i) < to_write)
                to_write = file_size - i;
            result = f_read(&src_fp, buffer, to_write, &bw);
            if (result != FR_OK)
                goto preallocate_file_copy_end;
            result = f_write(fp, buffer, to_write, &bw);
            if (result != FR_OK)
                goto preallocate_file_copy_end;
        }
preallocate_file_copy_end:
        f_close(&src_fp);
    } else {
        // Fill bytes.
        memset(buffer, fill_byte, buffer_size);
        result = f_lseek(fp, f_size(fp));
        if (result != FR_OK)
            goto preallocate_file_end;

        for (uint32_t i = f_tell(fp); i < file_size; i += buffer_size) {
            uint16_t to_write = buffer_size;
            if ((file_size - i) < to_write)
                to_write = file_size - i;
            result = f_write(fp, buffer, to_write, &bw);
            if (result != FR_OK)
                goto preallocate_file_end;
        }
    }

preallocate_file_end:
    result2 = f_lseek(fp, 0);
    if (result == FR_OK && result2 != FR_OK)
        return result2;
    return result;
}

static const char __far save_ini_start[] = "[save]\n";
static const char __far save_ini_sram[] = "sram";
static const char __far save_ini_eeprom[] = "eeprom";
static const char __far save_ini_flash[] = "flash";
static const char __far save_ini_entry[] = "%s=%ld|%s%s\n";

uint8_t launch_backup_save_data(void) {
    FIL fp, save_fp;
    char buffer[FF_LFN_BUF + 4];
    char *key, *value;
    ini_next_result_t ini_result;
    uint8_t result;

    memcpy(buffer, save_ini_location, sizeof(save_ini_location));
    result = f_open(&fp, buffer, FA_OPEN_EXISTING | FA_READ);
    // If the .ini file doesn't exist, skip.
    if (result == FR_NO_FILE)
        return FR_OK;
    if (result != FR_OK)
        return result;

    ui_layout_clear(0);
    ui_show();
    ui_draw_centered_status("Save -> Card");

    while (true) {
        ini_result = ini_next(&fp, buffer, sizeof(buffer), &key, &value);
        if (ini_result == INI_NEXT_ERROR) {
            result = FR_INT_ERR;
            f_close(&fp);
            return result;
        } else if (ini_result == INI_NEXT_FINISHED) {
            break;
        } else if (ini_result == INI_NEXT_CATEGORY) {
            // TODO: Pay attention to this.
        } else if (ini_result == INI_NEXT_KEY_VALUE) {
            uint8_t file_type = 0;
            if (!strcasecmp(key, save_ini_sram)) file_type = 1;
            else if (!strcasecmp(key, save_ini_eeprom)) file_type = 2;
            else if (!strcasecmp(key, save_ini_flash)) file_type = 3;
            if (file_type != 0) {
                key = (char*) strchr(value, '|');
                if (key != NULL) value = key + 1;

                result = f_open(&save_fp, value, FA_OPEN_EXISTING | FA_WRITE);
                if (result != FR_OK) {
                    // TODO: Handle FR_NO_FILE by preallocating a new file.
                    f_close(&fp);
                    return result;
                }

                if (file_type == 1) {
                    outportb(IO_CART_FLASH, 0);
                    result = f_write_sram_banked(&save_fp, 0, f_size(&save_fp), NULL);
                } else if (file_type == 2) {
                    // TODO: EEPROM restore          
                    result = FR_OK;          
                } else if (file_type == 3) {
                    // Flash restore
                    result = f_write_rom_banked(&save_fp, 0, f_size(&save_fp), NULL);
                }

                f_close(&save_fp);
                if (result != FR_OK) {
                    f_close(&fp);
                    return result;
                }
            }
        }
    }

    f_close(&fp);
    memcpy(buffer, save_ini_location, sizeof(save_ini_location));
    f_unlink(buffer);
    return result;
}

uint8_t launch_restore_save_data(char *path, const launch_rom_metadata_t *meta) {
    char dst_cwd[FF_LFN_BUF + 4];
    char dst_path[FF_LFN_BUF + 4];
    char tmp_buf[20];
    FIL fp;
    uint8_t result, result2;

    bool has_save_data = meta->sram_size || meta->eeprom_size || meta->flash_size;
    if (!has_save_data)
        return FR_OK;

    ui_layout_clear(0);
    ui_show();
    ui_draw_centered_status("Card -> Save");

    // extension-editable version of "path"
    strcpy(dst_path, path);
    char *ext_loc = (char*) strrchr(dst_path, '.');
    if (ext_loc == NULL)
        ext_loc = dst_path + strlen(dst_path);

    // restore or create data
    if (meta->sram_size != 0) {
        strcpy(ext_loc, ext_sram);
        result = preallocate_file(dst_path, &fp, 0xFF, meta->sram_size, NULL);
        if (result != FR_OK)
            return result;

        // copy data to SRAM
        outportb(IO_CART_FLASH, 0);
        result = f_read_sram_banked(&fp, 0, f_size(&fp), NULL);
        if (result != FR_OK) {
            f_close(&fp);
            return result;
        }

        result = f_close(&fp);
        if (result != FR_OK)
            return result;
    }
    if (meta->eeprom_size != 0) {
        strcpy(ext_loc, ext_eeprom);
        result = preallocate_file(dst_path, &fp, 0xFF, meta->eeprom_size, NULL);
        if (result != FR_OK)
            return result;

        // TODO: copy
        result = f_close(&fp);
        if (result != FR_OK)
            return result;
    }
    if (meta->flash_size != 0) {
        strcpy(ext_loc, ext_flash);
        result = preallocate_file(dst_path, &fp, 0xFF, meta->flash_size, path);
        if (result != FR_OK)
            return result;

        result = f_close(&fp);
        if (result != FR_OK)
            return result;

        // Use .flash instead of .ws/.wsc to boot on this platform.
        strcpy(path + (ext_loc - dst_path), ext_flash);
    }

    result = f_getcwd(dst_cwd, sizeof(dst_cwd) - 1);
    if (result != FR_OK)
        goto launch_restore_save_data_ini_end;
    
    char *dst_cwd_end = dst_cwd + strlen(dst_cwd) - 1;
    if (*dst_cwd_end != '/') {
        *(++dst_cwd_end) = '/';
        *(++dst_cwd_end) = 0;
    }

    // generate .INI file
    memcpy(tmp_buf, save_ini_location, sizeof(save_ini_location));
    result = f_open(&fp, tmp_buf, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK)
        return result;
    
    result = f_puts(save_ini_start, &fp) < 0 ? FR_INT_ERR : FR_OK;
    if (result != FR_OK)
        goto launch_restore_save_data_ini_end;

    if (meta->sram_size != 0) {
        strcpy(ext_loc, ext_sram);
        result = f_printf(&fp, save_ini_entry,
            (const char __far*) save_ini_sram,
            (uint32_t) meta->sram_size,
            (const char __far*) dst_cwd,
            (const char __far*) dst_path) < 0 ? FR_INT_ERR : FR_OK;
        if (result != FR_OK)
            goto launch_restore_save_data_ini_end;
    }

    if (meta->eeprom_size != 0) {
        strcpy(ext_loc, ext_eeprom);
        result = f_printf(&fp, save_ini_entry,
            (const char __far*) save_ini_eeprom,
            (uint32_t) meta->eeprom_size,
            (const char __far*) dst_cwd,
            (const char __far*) dst_path) < 0 ? FR_INT_ERR : FR_OK;
        if (result != FR_OK)
            goto launch_restore_save_data_ini_end;
    }

    if (meta->flash_size != 0) {
        strcpy(ext_loc, ext_flash);
        result = f_printf(&fp, save_ini_entry,
            (const char __far*) save_ini_flash,
            (uint32_t) meta->flash_size,
            (const char __far*) dst_cwd,
            (const char __far*) dst_path) < 0 ? FR_INT_ERR : FR_OK;
        if (result != FR_OK)
            goto launch_restore_save_data_ini_end;
    }

launch_restore_save_data_ini_end:
    result2 = f_close(&fp);
    if (result == FR_OK && result2 != FR_OK)
        return result2;
    return result;
}

uint8_t launch_rom_via_bootstub(const char *path, const launch_rom_metadata_t *meta) {
    FILINFO fp;
    
	uint8_t result = f_stat(path, &fp);
	if (result != FR_OK) {
        return result;
	}

    outportw(IO_DISPLAY_CTRL, 0);

    // Disable IRQs - avoid other code interfering/overwriting memory
    cpu_irq_disable();

    // Initialize bootstub graphics
    if (ws_system_color_active()) {
        ws_system_mode_set(WS_MODE_COLOR);
    }
    wsx_lzsa2_decompress((void*) 0x3200, gfx_bootstub_tiles);

    // Populate bootstub data
    bootstub_data->data_base = fs.database;
    bootstub_data->cluster_table_base = fs.fatbase;
    bootstub_data->cluster_size = fs.csize;
    bootstub_data->fat_entry_count = fs.n_fatent;
    bootstub_data->fs_type = fs.fs_type;
    
    bootstub_data->prog_cluster = fp.fclust;
    bootstub_data->prog_size = fp.fsize;
    if (meta != NULL) {
        bootstub_data->prog_sram_mask = meta->sram_size >> 16;
    } else {
        bootstub_data->prog_sram_mask = 7;
    }
    
    // Jump to bootstub
    memcpy((void*) 0x00c0, bootstub, bootstub_size);
    asm volatile("ljmp $0x0000,$0x00c0\n");
    return true;
}
